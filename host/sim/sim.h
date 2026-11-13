/* ecg_sim: host implementation of fw/include/hal.h plus a model of the board.
 *
 * The simulator is a single global instance (not thread safe). It implements
 * every hal_* function on a virtual clock, models the pins/rails, the two I2C
 * devices, the SSD1306 display, the ECG and PPG analog chains, the flash blob
 * and the debug channels, and checks the board's electrical safety rules as
 * assertions ("violations").
 *
 * ---------------------------------------------------------------------------
 * HOW A SCENARIO TEST IS WRITTEN
 *
 *     static void press(void *ctx) { (void)ctx; sim_set_button(1, true); }
 *     ...
 *     sim_reset();                              // power-on defaults
 *     sim_set_violation_mode(SIM_VIOL_COLLECT); // or leave default: abort
 *     sim_at(1000, press, NULL);                // at t = 1000 ms
 *     sim_at_button(1100, 1, false);            // canned helper
 *     sim_stop_at(60000);                       // end of run: t = 60 s
 *     app_init();                               // calls hal_init() ...
 *     while (!sim_should_stop())                // <-- how the run terminates
 *         app_step();                           // calls hal_idle() each pass
 *     CHECK(sim_violation_count() == 0);
 *
 * Nothing sleeps: time only moves inside HAL calls that take time
 * (hal_delay_*, hal_idle, hal_standby, I2C/SPI transfers, flash writes, ADC
 * one-shots). While time moves, scripted events (sim_at) and the simulated
 * hardware (sampling, RTC, charger fault toggle, watchdog) fire in time order.
 * Scripted callbacks run "inside the interrupt": they may call sim_* functions
 * but must NOT call hal_* functions that advance time.
 *
 * Termination: the main loop must test sim_should_stop(). The flag becomes true
 * when sim_stop_at() time is reached, sim_stop() is called, hal_system_reset()
 * is called, a simulated watchdog reset happens, or hal_standby() can never
 * wake. Alternatively install sim_set_exit_hook() and exit()/longjmp from it
 * (e.g. so that "reached standby" ends the test). If the hook returns, the
 * stop flag is set and the HAL call returns normally (hal_standby returns 0,
 * hal_system_reset returns).
 *
 * Time base: 1 us resolution (uint64). t = 0 at sim_reset(). hal_millis() does
 * not advance in standby; sim_now_us()/sim_at() times are absolute and do.
 * ---------------------------------------------------------------------------
 */
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "hal.h"

/* ======================= lifecycle and time ============================== */

/* Return everything (time, pins, devices, flash, scripts, violations, logs,
 * stream buffer, analog sources and settings) to power-on defaults. The
 * violation mode and log echo setting are kept. Call between tests. */
void sim_reset(void);

/* MCU reset only (what hal_system_reset/watchdog/pin reset do on a real
 * part): time and scripted events continue; flash, I2C devices (MCP4728,
 * CW2015: they sit on the always-on rail), the analog world and the
 * battery/USB/buttons keep their state. The MCU's pins go to reset state
 * (outputs low, LED_BLANK high, OLED bus detached, rails off - without
 * invariant checks), acquisition stops, MCLK slow, events cleared, stop flag
 * cleared. The next hal_init() sees the reset cause set by the event that
 * caused the reset (SOFTWARE/WATCHDOG), else HAL_RESET_PIN, unless overridden
 * by sim_set_reset_cause(). */
void sim_reboot(void);
void sim_set_reset_cause(hal_reset_cause_t cause); /* for next hal_init() */
/* True after hal_system_reset() or a simulated watchdog reset until the next
 * sim_reboot()/sim_reset(). */
bool sim_reset_requested(void);

uint64_t sim_now_us(void);
uint64_t sim_now_ms(void);
/* RTC (LFOSC) frequency error. The RTC runs at (1 + ppm*1e-6) of real time. */
void sim_set_rtc_error_ppm(int ppm);

typedef void (*sim_cb_t)(void *ctx);
/* Schedule cb(ctx) at absolute virtual time. A time in the past fires at the
 * next time advance. Equal times fire in scheduling order. */
void sim_at(uint64_t time_ms, sim_cb_t cb, void *ctx);
void sim_at_us(uint64_t time_us, sim_cb_t cb, void *ctx);
void sim_after_ms(uint64_t delta_ms, sim_cb_t cb, void *ctx); /* relative to now */
void sim_clear_scheduled(void);
size_t sim_scheduled_count(void);

/* Canned scripted events (allocate their own context; freed by sim_reset). */
void sim_at_button(uint64_t time_ms, int button /*1|2*/, bool pressed);
void sim_at_usb(uint64_t time_ms, bool present);
void sim_at_thumbs(uint64_t time_ms, bool on);
void sim_at_finger(uint64_t time_ms, bool on);
void sim_at_battery_mv(uint64_t time_ms, uint16_t mv);

/* Run control (see header comment). */
void sim_stop_at(uint64_t time_ms); /* sim_should_stop() becomes true at this time */
void sim_stop(void);                /* ... now */
bool sim_should_stop(void);
typedef enum {
    SIM_EXIT_STOP_AT,          /* sim_stop_at() time reached (hook is NOT called for sim_stop()) */
    SIM_EXIT_STANDBY_DEADLOCK, /* hal_standby(): nothing can ever wake the MCU */
    SIM_EXIT_SYSTEM_RESET,     /* hal_system_reset() */
    SIM_EXIT_WATCHDOG          /* simulated watchdog reset (a violation is recorded too) */
} sim_exit_reason_t;
typedef void (*sim_exit_hook_t)(sim_exit_reason_t reason, void *ctx);
void sim_set_exit_hook(sim_exit_hook_t hook, void *ctx);

/* ======================= violations (invariant checker) ================== */

typedef enum {
    SIM_V_NONE = 0,
    SIM_V_GUARDED_PIN_ANA_LOW,    /* MEASURE_REQ/FAST_RESTORE/GAIN_HI/RLD_x high while ANA_EN low */
    SIM_V_ANA_FALL_ACQ_RUNNING,   /* ANA_EN fell while an acquisition stream runs */
    SIM_V_ANA_FALL_LED_BLANK_LOW, /* ANA_EN fell while LED_BLANK low */
    SIM_V_ANA_FALL_DAC_ACTIVE,    /* ANA_EN fell with MCP4728 ch A not powered down */
    SIM_V_VLED_CHANGE_NOT_BLANKED,/* VLED_EN changed while LED_BLANK low */
    SIM_V_OLED_SIGNAL_UNPOWERED,  /* OLED_DC/CS/RESET high or SPI attached while OLED_EN low */
    SIM_V_OLED_SPI_UNPOWERED,     /* SPI write while OLED_EN low */
    SIM_V_OLED_SPI_DETACHED,      /* SPI write while bus not attached */
    SIM_V_OLED_DMA_MISUSE,        /* buffer changed during DMA, detach/blocking write during DMA */
    SIM_V_RADIO_EN,               /* RADIO_EN driven high */
    SIM_V_STANDBY_PRECOND,        /* hal_standby() preconditions not met */
    SIM_V_STANDBY_DEADLOCK,       /* hal_standby() with no possible wake source (no exit hook) */
    SIM_V_CLOCK_WHILE_ACQ,        /* hal_clock_fast() while acquisition runs */
    SIM_V_ACQ_START,              /* bad hal_acq_start(): running, no stream, ANA_EN low, slow clock */
    SIM_V_PPG_START,              /* PPG start without hal_ppg_analog(true), VLED_EN or ANA_EN */
    SIM_V_PPG_ANALOG_OFF,         /* hal_ppg_analog(false) while PPG runs */
    SIM_V_ADC_WHILE_ACQ,          /* hal_adc_read_once() while a stream runs */
    SIM_V_WATCHDOG,               /* no hal_wdt_kick() for > 2 s while awake */
    SIM_V_I2C_PROTOCOL,           /* malformed/ignored MCP4728 or CW2015 transaction */
    SIM_V_LED_OVERCURRENT,        /* LED lit with more than LED_MAX_MA */
    SIM_V_API,                    /* invalid argument / misuse of the simulator API */
    SIM_V_COUNT
} sim_viol_t;

typedef enum { SIM_VIOL_ABORT = 0, SIM_VIOL_COLLECT } sim_viol_mode_t;
/* Default: ABORT (print a message, abort()). Env SIM_VIOLATIONS=collect
 * selects COLLECT at startup. The mode survives sim_reset(). */
void sim_set_violation_mode(sim_viol_mode_t mode);
size_t sim_violation_count(void);
const char *sim_violation_text(size_t i);  /* "[t=12.345 ms] message" */
sim_viol_t sim_violation_id(size_t i);
size_t sim_violation_count_of(sim_viol_t id);
size_t sim_violation_count_containing(const char *substr);
void sim_violations_clear(void);

/* ======================= world: pins, rails, power ======================= */

/* Inputs (these generate HAL_EV_* exactly as hal.h documents). */
void sim_set_button(int button /*1|2*/, bool pressed); /* active-low pin */
void sim_set_usb(bool present);  /* CHG_PG low when present. Falling edge forces MEASURE_REQ low at once. */
typedef enum { SIM_CHG_CHARGING, SIM_CHG_DONE, SIM_CHG_FAULT } sim_charge_t;
/* CHG_STAT1 while USB is present: CHARGING = low, DONE = high, FAULT = toggles
 * at 2 Hz (every 250 ms, edge each time). Default CHARGING. Absent USB: high. */
void sim_set_charge_mode(sim_charge_t mode);
void sim_set_radio_irq(bool level);
void sim_set_battery_mv(uint16_t mv);  /* cell voltage (CW2015 VCELL, SOC derived from it) */
void sim_set_battery_soc(int pct);     /* force SOC (0..100); negative = derive from voltage */
void sim_set_thumbs(bool on);          /* thumbs on both electrodes */
void sim_set_finger(bool on);          /* finger on the PPG sensor */

typedef enum {
    SIM_RAIL_3V3A,     /* +3V3A  = ANA_EN */
    SIM_RAIL_3V3_SW,   /* +3V3_SW = ANA_EN */
    SIM_RAIL_OLED_VCC, /* OLED_EN */
    SIM_RAIL_VLED,     /* VLED_EN */
    SIM_RAIL_RADIO_VCC,/* RADIO_EN */
    SIM_RAIL_BODY_EN   /* MEASURE_REQ && CHG_PG && ANA_EN */
} sim_rail_t;
bool sim_rail(sim_rail_t r);
bool sim_pin(board_pin_t p);        /* level without side effects (works for any pin) */
bool sim_led_blank(void);           /* PA12 level (true = LEDs off) */
bool sim_oled_bus_attached(void);
bool sim_clock_fast(void);
bool sim_in_standby(void);
bool sim_usb_present(void);

/* ======================= counters and facts ============================== */

typedef struct {
    uint32_t i2c_xfers, i2c_faults_injected, i2c_recoveries;
    uint32_t mcp_eeprom_writes;        /* EEPROM channel writes */
    uint32_t mcp_writes_ignored_busy;  /* EEPROM commands sent during BSY */
    uint32_t cw_profile_writes;        /* transactions touching 0x10..0x4F */
    uint32_t cw_profile_bytes;
    uint32_t cw_mode_writes;
    uint32_t oled_cmd_bytes, oled_data_bytes, oled_ignored_bytes; /* ignored: CS high / in reset / unpowered */
    uint32_t oled_async_writes;
    uint32_t nv_writes, nv_erase_cycles;
    uint32_t ppg_offset_changes;
    uint32_t ppg_offset_in_sample_window; /* DAC12 changed between dark and on sample of a frame */
    uint32_t ppg_vled_early_starts;       /* PPG started < PPG_VLED_START_MS after VLED_EN rose */
    uint32_t standby_entries, system_resets, wdt_kicks, wdt_resets;
    uint64_t standby_total_us;
    uint32_t ecg_samples, ppg_frames;     /* produced since sim_reset */
} sim_facts_t;
const sim_facts_t *sim_facts(void);

/* ======================= I2C: fault injection, log, devices ============== */

/* The next n hal_i2c_xfer() calls fail with `status` and have no effect on the
 * devices (NACK takes ~30 us, TIMEOUT/BUS take 10 ms like the real timeout). */
void sim_i2c_fail_next(unsigned n, hal_status_t status);
/* Stuck bus: every transfer fails with HAL_ERR_BUS (10 ms) until hal_i2c_recover(). */
void sim_i2c_stick_bus(bool stuck);
bool sim_i2c_bus_stuck(void);

typedef struct {
    uint64_t t_us;        /* start time */
    uint8_t addr;
    uint8_t w[32];        /* first 32 written bytes */
    size_t wn, rn;
    hal_status_t status;
} sim_i2c_txn_t;
size_t sim_i2c_log_count(void);            /* up to 1024 are kept */
const sim_i2c_txn_t *sim_i2c_log_get(size_t i);
void sim_i2c_log_clear(void);

/* MCP4728 (0x60). ch = 0..3 (A..D). */
unsigned sim_mcp_code(int ch);            /* DAC input register code */
bool sim_mcp_vref_internal(int ch);       /* true = internal 2.048 V, false = VDD */
bool sim_mcp_gain2(int ch);               /* x2 gain (internal reference only) */
unsigned sim_mcp_pd(int ch);              /* 0 normal, 1..3 power-down */
double sim_mcp_vout_mv(int ch);
void sim_mcp_eeprom_get(int ch, unsigned *code, bool *vref_int, unsigned *pd, bool *gain2);
void sim_mcp_eeprom_set(int ch, unsigned code, bool vref_int, unsigned pd, bool gain2); /* test setup */
bool sim_mcp_busy(void);                  /* EEPROM write in progress (~50 ms) */
void sim_mcp_power_cycle(void);           /* reload registers from EEPROM */
/* Factory EEPROM (assumed): VDD reference, gain x1, normal mode, code 0x800. */
/* LED current set by channel A: I = Vout * (10/164) / 8.2 ohm, in mA. */
double sim_led_set_ma(void);
/* Actual LED current right now: set_ma when LED_BLANK low, VLED_EN, ANA_EN and
 * channel A active; else 0. */
double sim_led_now_ma(void);

/* CW2015 (0x62). */
unsigned sim_cw_profile_writes(void);     /* transactions that wrote 0x10..0x4F */
void sim_cw_profile(uint8_t out[64]);
bool sim_cw_ufg(void);                    /* CONFIG bit 1 */
unsigned sim_cw_athd(void);               /* CONFIG bits 7:3 */
bool sim_cw_asleep(void);                 /* MODE sleep bits set */
bool sim_cw_alert_flag(void);             /* RRT_ALRT bit 15 */
uint8_t sim_cw_reg(unsigned addr);        /* raw register file (0..0x4F) */

/* ======================= OLED (SSD1306) ================================== */

typedef struct {
    bool powered;               /* OLED_EN high */
    bool in_reset;              /* RESET pin low while powered */
    bool reset_pulse_seen;      /* since power-up: module was in reset (RESET low at or after
                                   OLED_EN rising) and RESET has been released */
    uint32_t power_ups;
    uint32_t resets_released;
    uint32_t early_reset_releases; /* released < 5 ms after OLED_EN rose */
    uint32_t dirty_power_offs;     /* RESET asserted or OLED_EN dropped with display/charge pump still on */
    bool display_on, charge_pump;
    bool init_complete;         /* powered, out of reset, display on and charge pump on */
    bool image_settled;         /* init_complete for >= settle time (default 100 ms) */
    uint8_t contrast;
    uint32_t cmd_bytes, data_bytes, ignored_bytes;
} sim_oled_info_t;
sim_oled_info_t sim_oled_info(void);
void sim_oled_set_settle_ms(unsigned ms);   /* default 100 */
/* What a viewer sees: 0 = dark, 1 = lit. All 0 when unpowered, in reset,
 * display off, charge pump off or not yet settled. Orientation: with the
 * datasheet init (A1h, C8h) RAM column 0 / bit 0 of page 0 is the top-left
 * pixel. */
void sim_oled_image(uint8_t out[64][128]);
uint8_t sim_oled_ram_byte(int page, int col);  /* raw display RAM */
bool sim_oled_save_pgm(const char *path, int scale);  /* 0/255 binary PGM */
bool sim_oled_save_png(const char *path, int scale);

/* ======================= analog world: ECG ============================== */

/* Differential electrode voltage in microvolts at absolute virtual time t_us.
 * Only used while the thumbs are on and BODY_EN is high. */
typedef int32_t (*sim_ecg_fn)(uint64_t t_us, void *ctx);
void sim_set_ecg_source(sim_ecg_fn fn, void *ctx);
/* Contact transients, applied when BODY_EN and the thumbs are both on:
 *  - offset: DC step at the high-pass node (C34/R36, unity DC gain), in uV at
 *    the node (default 200000 = 200 mV: saturates the output, ~0.5 s to leave
 *    the 25 % band at 558x). Decays with the 0.48 Hz high-pass unless
 *    FAST_RESTORE is pulsed, which snaps it away completely.
 *  - transient: step at the electrode input (default 1000 uV) that goes through
 *    the first stage's 0.32 Hz AC coupling (x21): the 2-3 s "settling" tail that
 *    FAST_RESTORE cannot remove. */
void sim_ecg_set_contact_offset_uv(int32_t uv);
void sim_ecg_set_contact_transient_uv(int32_t uv);
/* Additive mains interference at the electrodes (default off). Amplitude is
 * for RLD both on; x3 with one RLD pin, x10 with none. */
void sim_ecg_set_mains(double hz, double amp_uv);
void sim_ecg_set_noise_uv(double rms_uv);       /* white, at the electrodes (default 0) */
void sim_ecg_set_vmid_error_mv(int mv);         /* VMID target = 1650 + mv (default 0) */
void sim_ecg_set_float_high(bool high);         /* lead-off rail (default true: near 3.25 V) */
/* Offset injected at the output by a GAIN_HI or RLD pin change (defaults 15, 8). */
void sim_ecg_set_injection_mv(double gain_mv, double rld_mv);
void sim_ecg_set_adc_noise_lsb(double rms);     /* default 0 */
double sim_ecg_out_v(void);                     /* ECG_ADC pin voltage now */
double sim_vmid_mv(void);                       /* VMID node now */

/* ======================= analog world: PPG ============================== */

/* LED-induced photocurrent in pA per mA of LED current (finger reflectance
 * incl. the pulsatile part), used only while sim_set_finger(true). */
typedef uint32_t (*sim_ppg_fn)(uint64_t t_us, void *ctx);
void sim_set_ppg_source(sim_ppg_fn fn, void *ctx);
/* Ambient photocurrent: dc + flicker_amp*sin(2 pi f t), pA (default 0). */
void sim_ppg_set_ambient(double dc_pa, double flicker_amp_pa, double flicker_hz);
double sim_ppg_tia_v(void);   /* TIA_OUT now (LED state as it is now) */
double sim_ppg_hi_v(void);    /* PPG_HI now */

/* ======================= non-volatile blob =============================== */

void sim_nv_set(const void *data, size_t n);    /* pre-load / pre-corrupt content (rest 0xFF) */
void sim_nv_get(void *dst, size_t n);
void sim_nv_corrupt(size_t offset, uint8_t xor_mask);
void sim_nv_erase(void);                        /* back to 0xFF, not counted as a cycle */
uint32_t sim_nv_erase_count(void);
void sim_nv_fail_next_write(hal_status_t status); /* next hal_nv_write fails, content unchanged */

/* ======================= debug channels ================================== */

const char *sim_log_text(void);                 /* NUL-terminated captured log */
size_t sim_log_len(void);
void sim_log_clear(void);
void sim_set_log_echo(bool on);                 /* also env SIM_LOG_ECHO=1 */
const uint8_t *sim_stream_data(size_t *len);
void sim_stream_clear(void);
bool sim_stream_save(const char *path);
/* env SIM_STREAM_FILE=path: also append every stream byte to that file live. */

/* Capacities of the sample rings (the real buffers hold "at least" 64 ms). */
#define SIM_ECG_RING_SAMPLES 256u
#define SIM_PPG_RING_FRAMES  32u

#endif
