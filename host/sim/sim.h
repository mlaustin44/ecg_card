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
#include "link_fmt.h"

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
/* Clear the stop flag (after a sim_stop_at() time or sim_stop()) so a test
 * can keep driving the main loop in phases. */
void sim_clear_stop(void);
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
    SIM_V_RADIO_BACKFEED,         /* UART0 on (PA10 idle-high) while RADIO_EN is low, either order */
    SIM_V_UART_RADIO_OFF,         /* hal_uart_write() while the radio is off */
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
    /* MCP4728 internal reference (sim_mcp_int_ref_on()): hal_standby() entries
     * with it on, and total time it ran while VLED_EN was low (no PPG
     * session). Both must stay 0 unless the I2C bus is broken. */
    uint32_t mcp_vref_int_standby;
    uint64_t mcp_vref_int_idle_us;
} sim_facts_t;
const sim_facts_t *sim_facts(void);

/* ======================= I2C: fault injection, log, devices ============== */

/* The next n hal_i2c_xfer() calls fail with `status` and have no effect on the
 * devices (NACK takes ~30 us, TIMEOUT/BUS take 10 ms like the real timeout). */
void sim_i2c_fail_next(unsigned n, hal_status_t status);
/* The next n transfers DO reach the device (their effect lands) but then
 * report `status` (e.g. a timeout on the final STOP): a write the MCU
 * believes failed although the device acted on it. */
void sim_i2c_fail_after_next(unsigned n, hal_status_t status);
/* Stuck bus: every transfer fails with HAL_ERR_BUS (10 ms) until hal_i2c_recover(). */
void sim_i2c_stick_bus(bool stuck);
bool sim_i2c_bus_stuck(void);
/* Remove (false) or refit a device: an absent device NACKs its address
 * (I2C_ADDR_MCP4728 or I2C_ADDR_CW2015). Default: both present. */
void sim_i2c_set_present(uint8_t addr, bool present);

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
/* Internal 2.048 V reference running: some channel's DAC register selects it,
 * powered down or not (DS22187E 4.10; ~45 uA vs 40 nA with all on VDD). */
bool sim_mcp_int_ref_on(void);
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
/* After a wake (MODE sleep -> 0x00) VCELL and SOC read 0 for this long, as
 * the real gauge does until its first conversion. Default 200 ms (restored
 * by sim_reset); 0 = valid at once. */
void sim_cw_set_wake_zero_ms(unsigned ms);

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
/* The chain while the thumbs are on (BODY_EN high):
 *   electrodes: v = source + electrode offset (DC, see below)
 *   U5 (ecg-fw.md ECG chain 2): y1 = v + 20 * HP(v), HP with tau 0.5 s (the
 *     gain network's 50 uF AC coupling): gain 21 for changes, 1 at DC. Its
 *     output clips at VMID +- 1.55 V. FAST_RESTORE does not reach it.
 *   C34/R36 (tau 0.33 s), the node FAST_RESTORE shorts to VMID, then x26.6
 *     (x52.3) and the 159 Hz / 339 Hz low-passes to the rails 0.05..3.25 V.
 * Contact transients, applied when BODY_EN and the thumbs are both on:
 *  - offset: DC step at the high-pass node (C34/R36, unity DC gain), in uV at
 *    the node (default 200000 = 200 mV: saturates the output, ~0.5 s to leave
 *    the 25 % band at 558x). Decays with the 0.48 Hz high-pass unless
 *    FAST_RESTORE is pulsed, which snaps it away completely.
 *  - transient: the electrode offset (half-cell / skin potential difference)
 *    present at contact, in uV at the electrodes (default 1000). It steps
 *    the U5 output by 21x (clipping for more than ~74 mV) and decays to 1x
 *    with tau 0.5 s: the "settling" tail FAST_RESTORE cannot remove. Dry
 *    thumbs: tens to hundreds of mV. */
void sim_ecg_set_contact_offset_uv(int32_t uv);
void sim_ecg_set_contact_transient_uv(int32_t uv);
/* Thumb adjustment: the electrode offset changes by step_uv now (a new step
 * through U5, as at contact), linearly over ramp_ms (0 = at once). Ignored
 * while there is no contact. */
void sim_ecg_thumb_adjust(int32_t step_uv, unsigned ramp_ms);
void sim_at_thumb_adjust(uint64_t time_ms, int32_t step_uv, unsigned ramp_ms);
/* What ECG_ADC does with the electrodes open (thumbs off, or BODY_EN low):
 *  SIM_FLOAT_EXCURSIONS (default, what the first board did): floating inputs
 *    keep the output on a rail (sim_ecg_set_float_high), with brief in-range
 *    excursions at a random level: Poisson starts at rate_hz, durations
 *    min + (max - min) * u^3, u uniform (mostly short: half are under
 *    ~50 ms with the defaults 1 Hz, 20..300 ms, seed 1; deterministic for a
 *    given seed). After a FAST_RESTORE pulse the output is back on the rail
 *    at once unless an excursion is in progress.
 *  SIM_FLOAT_RAIL: always near the rail (slow 60 mV wobble).
 *  SIM_FLOAT_MID: open inputs sit quietly near mid-scale (+-4 mV, 0.3 Hz),
 *    as other boards / conditions may do.
 * FAST_RESTORE high holds the output at VMID in every model (node shorted). */
typedef enum { SIM_FLOAT_EXCURSIONS = 0, SIM_FLOAT_RAIL, SIM_FLOAT_MID } sim_float_model_t;
void sim_ecg_set_float_model(sim_float_model_t model);
void sim_ecg_set_float_excursions(double rate_hz, unsigned min_ms, unsigned max_ms, uint64_t seed);
/* Additive mains interference at the electrodes (default off). Amplitude is
 * for RLD both on; x3 with one RLD pin, x10 with none. */
void sim_ecg_set_mains(double hz, double amp_uv);
void sim_ecg_set_noise_uv(double rms_uv);       /* white, at the electrodes (default 0) */
void sim_ecg_set_vmid_error_mv(int mv);         /* VMID target = 1650 + mv (default 0) */
void sim_ecg_set_float_high(bool high);         /* floating-input rail (default true: near 3.25 V) */
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
#define SIM_BSP_ECG_RING_SAMPLES 512u /* SIM_RING_BSP: fw/bsp ECG ring */

/* ======================= acquisition ring models ========================= */

/* How the HAL sample rings behave (hal.h read contract either way: the
 * reads report timeline indices, losses show as index jumps).
 *   SIM_RING_DEFAULT: SIM_ECG_RING_SAMPLES / SIM_PPG_RING_FRAMES deep.
 *   SIM_RING_BSP:     the fw/bsp ECG ring's failure pattern: 512 samples,
 *                     16 kept away from the write position, so a reader that
 *                     stalls longer than ~124 ms finds the oldest data
 *                     overwritten only when it comes back (drops discovered
 *                     at read time); a read never crosses a 512-sample lap.
 * Kept across sim_reboot(); sim_reset() restores DEFAULT and no misses. */
typedef enum { SIM_RING_DEFAULT = 0, SIM_RING_BSP } sim_ring_model_t;
void sim_acq_set_ring_model(sim_ring_model_t model);
/* Every n-th ECG sample / PPG frame is a missed conversion: its index
 * advances, no data is stored (BSP: a result that landed while the ADC's
 * DMAEN was off at a lap end; a missed ADC1 trigger). 0 = never. */
void sim_acq_set_missed(uint32_t ecg_every, uint32_t ppg_every);

/* ======================= radio link: UART0 + fake CH585 ================= */

/* UART0 model: hal_radio_power() / hal_uart_*() on a byte-timed line at
 * LINK_BAUD (10 bits per byte, 21.7 us), both directions. MCU rings as in
 * the BSP (HAL_UART_TX_RING / HAL_UART_RX_RING). While interrupts are
 * masked (hal_nv_write) the TX line stops after the 4-byte FIFO and the
 * shifter, and received bytes beyond the 4-byte RX FIFO are lost
 * (rx_overruns). A byte arriving while hal_clock_fast() switches the clock
 * is corrupted (rx_errors), as on the part.
 *
 * Invariants (violations): SIM_V_RADIO_BACKFEED - UART0 enabled (PA10
 * idle-high) while RADIO_EN is low, in either order; SIM_V_UART_RADIO_OFF -
 * hal_uart_write() with the radio off; hal_standby() with RADIO_EN high or
 * UART0 on is SIM_V_STANDBY_PRECOND. RADIO_EN high by itself is fine. */
bool sim_uart_enabled(void);
bool sim_radio_tx_pin(void);          /* PA10 level */
#define SIM_UART_BYTE_NS (10u * 1000000000u / 460800u) /* 21701 ns */

/* Ordered record of RADIO_EN and UART0 transitions since sim_reset(). */
typedef enum { SIM_RT_RADIO_EN_HIGH, SIM_RT_RADIO_EN_LOW, SIM_RT_UART_ON, SIM_RT_UART_OFF } sim_rt_t;
size_t sim_radio_trace_count(void);
sim_rt_t sim_radio_trace_get(size_t i, uint64_t *t_us);

/* Fake CH585, powered by RADIO_EN. It speaks fw/include/link_fmt.h with its
 * own codec (no app code). After power-up it sends LINK_HELLO after
 * hello_delay_ms and then boot_log as STREAM_LOG; it ACKs every
 * LINK_RADIO_CTRL after ack_delay_us, runs the advertising schedule (fast
 * for LINK_ADV_FAST_SECONDS, then slow) and sends LINK_CONN_STATE on every
 * change. After LINK_RC_SHUTDOWN (ACK) it sends nothing more. Matches the
 * CH585 firmware: ADV_START while connected -> BUSY, while advertising ->
 * OK without restarting the schedule; extra bytes on an argument-less op ->
 * BAD_ARG; SET_INFO text kept to 20 bytes; mtu 0 when not connected; after
 * a disconnect it advertises (fast) again by itself unless the last
 * advertising command was ADV_STOP. STREAM_LOG from the MCU goes to the
 * phone's Log text (sim_phone_log_text) while subscribed with LINK_SUB_LOG
 * (else it counts as unsolicited); payloads > 32 bytes count as
 * oversize_notify. */
typedef struct {
    bool present;            /* false: never sends anything (dead / unflashed radio) */
    uint32_t hello_delay_ms; /* power-up to HELLO, default 20 */
    uint32_t silent_boots;   /* the next n power-ups send no HELLO; UINT32_MAX = never */
    uint8_t proto;           /* HELLO proto, default LINK_PROTO_VERSION */
    uint8_t fw[3];           /* default 1.2.3 */
    uint8_t addr[6];         /* BLE address, least significant byte first */
    uint8_t reset_reason;
    const char *boot_log;    /* STREAM_LOG after HELLO; NULL = none */
    uint32_t ack_delay_us;   /* default 300 */
    bool ack_shutdown;       /* false: LINK_RC_SHUTDOWN is never ACKed */
    bool reading;            /* false: the radio ignores everything from the MCU */
} sim_radio_cfg_t;
sim_radio_cfg_t sim_radio_defaults(void);
void sim_radio_config(const sim_radio_cfg_t *cfg); /* default restored by sim_reset() */

typedef struct {
    bool powered, hello_sent, shut_down;   /* shut_down: LINK_RC_SHUTDOWN received */
    uint8_t conn_state;                    /* LINK_CS_* */
    uint16_t mtu, interval;
    uint8_t subs;                          /* LINK_SUB_* */
    uint32_t power_ups, hellos;
    /* MCU -> radio */
    uint32_t frames, crc_errors, seq_gaps, unknown_types;
    uint32_t by_type[256];
    uint32_t ctrl_ops[8];                  /* LINK_RADIO_CTRL by op (index < 8) */
    uint32_t ecg_blocks, ppg_blocks, ecg_gaps, ppg_gaps, ecg_runs, ppg_runs;
    uint64_t ecg_first_us, ecg_last_us, ppg_first_us, ppg_last_us;
    uint32_t events, events_by_kind[8];
    uint32_t unsolicited;                  /* data / events with no matching subscription */
    uint32_t oversize_notify;              /* ECG / PPG / EVENT payloads > 32 bytes (CH585 limit) */
    uint32_t after_shutdown;               /* frames received after the SHUTDOWN */
    uint32_t status_frames;
    uint32_t log_frames, log_bytes;        /* STREAM_LOG (device log tee), any subscription */
    uint8_t last_status[LINK_STATUS_LEN];
    char mcu_info[40];                     /* LINK_RC_SET_INFO text */
    /* shutdown checks */
    uint32_t shutdowns;                    /* LINK_RC_SHUTDOWN received */
    uint32_t clean_offs;                   /* power cut after SHUTDOWN, within 50 ms */
    uint32_t unclean_offs;                 /* cut after HELLO without SHUTDOWN, or > 50 ms after it */
    uint64_t last_shutdown_wait_us;        /* SHUTDOWN received -> power cut */
    uint64_t max_shutdown_wait_us;
} sim_radio_info_t;
const sim_radio_info_t *sim_radio_info(void);

/* Device log (Log characteristic): STREAM_LOG payloads the MCU sent while
 * the phone was subscribed with LINK_SUB_LOG (others count as unsolicited),
 * concatenated, plus the fake radio's own sim_radio_log() lines as
 * "radio: <text>\n" (only at an MCU line boundary, as the CH585 does).
 * NUL-terminated; cleared by sim_reset() and sim_phone_log_clear(). */
const char *sim_phone_log_text(size_t *len);
void sim_phone_log_clear(void);

/* Frames the MCU sent (all types except ECG / PPG blocks and STREAM_LOG), last 128. */
typedef struct {
    uint64_t t_us;
    uint8_t type, seq;
    uint16_t len;
    uint8_t p[32];                         /* first 32 payload bytes */
} sim_link_frame_t;
size_t sim_radio_log_count(void);
const sim_link_frame_t *sim_radio_log_get(size_t i);
/* Most recent LINK_ACK the MCU sent for the radio frame with this seq. */
bool sim_radio_ack_of(uint8_t seq, uint8_t *result);
/* Most recent STREAM_EV_CMD_RESULT for cmd (any subscription). */
bool sim_radio_cmd_result(uint8_t cmd, uint8_t *result);

/* The phone. connect: only while advertising (returns false otherwise). */
bool sim_phone_connect(uint16_t mtu, uint16_t interval, uint8_t subs);
void sim_phone_subscribe(uint8_t subs);
void sim_phone_disconnect(void);
/* A Control write: the radio sends LINK_CMD with this payload. Returns the
 * frame's seq, or -1 when no phone is connected. */
int sim_phone_command(const uint8_t *payload, size_t n);

/* Injection on the radio -> MCU line. */
void sim_radio_send_frame(uint8_t type, const uint8_t *payload, size_t n); /* radio seq, any state */
void sim_radio_send_raw(const uint8_t *bytes, size_t n);                    /* as-is */
void sim_radio_log(const char *text);                                       /* STREAM_LOG */
void sim_radio_corrupt_to_mcu(uint32_t n, uint8_t xor_mask); /* next n bytes on the wire */
void sim_radio_drop_to_mcu(uint32_t n);                      /* next n bytes vanish */
void sim_radio_corrupt_to_radio(uint32_t n, uint8_t xor_mask);
/* Freeze the MCU -> radio line (bytes stay in the MCU TX ring), e.g. to
 * build a TX backlog; false resumes. Cleared by sim_reset(). */
void sim_uart_stall_tx(bool stall);
/* The radio resets by itself (watchdog): state lost, HELLO again. */
void sim_radio_self_reset(void);

#endif
