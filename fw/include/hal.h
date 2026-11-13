/* Hardware abstraction layer: the only interface between portable code
 * (drivers, dsp, acq, ui, app) and the MCU. Two implementations exist:
 *   fw/bsp    - MSPM0G3507 + TI DriverLib
 *   host/sim  - host simulation with a rail-invariant checker
 *
 * Nothing above this header may include a TI header.
 *
 * Threading model: bare-metal superloop. Every function here is called from
 * the main loop only, unless marked "ISR-safe". */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

typedef enum {
    HAL_OK = 0,
    HAL_ERR_NACK,    /* I2C address or data not acknowledged */
    HAL_ERR_TIMEOUT, /* peripheral did not finish in time */
    HAL_ERR_BUS,     /* arbitration lost / bus stuck */
    HAL_ERR_PARAM
} hal_status_t;

/* ---- Init / core ---------------------------------------------------------
 * hal_init() brings the board to its safe idle state:
 *   - every board_pin_t output low, inputs configured (RADIO_IRQ with the
 *     internal pull-down), wake enabled on buttons, CHG_PG, CW_ALRT;
 *   - LED_BLANK (PA12) driven high (LEDs off);
 *   - OLED bus detached (SCK, MOSI, DC, CS, RESET low);
 *   - radio off-state: UART TX (PA10) GPIO low, RX (PA11) input pull-down,
 *     RADIO_EN low (see hal_radio_power());
 *   - unused pins output low, analog pins analog, PA18 input (never driven);
 *   - MCLK 32 MHz SYSOSC, SysTick 1 ms, RTC on LFOSC, I2C0 400 kHz;
 *   - watchdog started (about 2 s period, no closed window) as its very
 *     first step, so a hang later in start-up resets the MCU; restarted
 *     once more on return. */
void hal_init(void);

typedef enum {
    HAL_RESET_POWER_ON,
    HAL_RESET_PIN,
    HAL_RESET_WATCHDOG,
    HAL_RESET_SOFTWARE,
    HAL_RESET_BROWNOUT,
    HAL_RESET_OTHER
} hal_reset_cause_t;
hal_reset_cause_t hal_reset_cause(void); /* latched at hal_init() */

void hal_wdt_kick(void);
void hal_system_reset(void);

/* Nestable interrupt lock. ISR-safe. */
uint32_t hal_irq_lock(void);
void hal_irq_unlock(uint32_t state);

/* ---- Time ----------------------------------------------------------------
 * hal_millis(): monotonic, 1 ms resolution, does not advance in STANDBY.
 * hal_rtc_seconds(): monotonic seconds from the RTC (LFOSC, a few % accuracy),
 * keeps counting through STANDBY. */
uint32_t hal_millis(void); /* ISR-safe */
uint32_t hal_rtc_seconds(void);
void hal_delay_ms(uint32_t ms); /* busy/sleep wait; kicks nothing; main loop only */
void hal_delay_us(uint32_t us);

/* MCLK: true = 80 MHz SYSPLL (acquisition), false = 32 MHz SYSOSC.
 * All HAL timing (SysTick, baud rates, timers) stays correct across a switch.
 * Must not be called while acquisition is running. */
void hal_clock_fast(bool fast);

/* ---- GPIO ----------------------------------------------------------------
 * Logical levels are electrical levels (true = pin high). Writing an input
 * pin or an out-of-range pin is ignored. */
void hal_pin_write(board_pin_t pin, bool high); /* ISR-safe */
bool hal_pin_read(board_pin_t pin);             /* ISR-safe; outputs read back */

/* ---- Asynchronous events -------------------------------------------------
 * Latched by ISRs, returned and cleared by hal_events_take(). They are also
 * the wake sources for hal_idle() and hal_standby().
 *
 * Safety: on a CHG_PG falling edge (USB plugged in) the ISR itself drives
 * MEASURE_REQ low before latching HAL_EV_USB_PLUGGED. The application then
 * ends the record and runs the full stop sequence. */
#define HAL_EV_BUTTON      (1u << 0) /* edge on BUTTON1 or BUTTON2 */
#define HAL_EV_USB_PLUGGED (1u << 1) /* CHG_PG fell */
#define HAL_EV_USB_REMOVED (1u << 2) /* CHG_PG rose */
#define HAL_EV_CHG_STAT    (1u << 3) /* edge on CHG_STAT1 */
#define HAL_EV_CW_ALRT     (1u << 4) /* CW_ALRT fell */
#define HAL_EV_RTC         (1u << 5) /* periodic RTC wake (1 Hz while awake) */
#define HAL_EV_RADIO_IRQ   (1u << 6) /* RADIO_IRQ rose (unused in this build) */
uint32_t hal_events_take(void);

/* Sleep until the next interrupt (CPU SLEEP; clocks and peripherals run). */
void hal_idle(void);

/* Enter STANDBY. Preconditions (caller's job): acquisition stopped, every
 * switched rail off, OLED bus detached, MCLK slow. SysTick stops. Returns
 * the HAL_EV_* bits that caused the wake (they also stay latched).
 * wake_after_s = 0 means no timed wake; the timed wake has 1 s granularity.
 * A stale HAL_EV_RTC is discarded on entry (the 1 Hz tick is not a wake
 * source); any other event already pending makes the call return at once.
 * HAL_EV_CHG_STAT is not a wake source either (a charger-fault blink would
 * keep the MCU awake): it is reported on exit if CHG_STAT1 changed. */
uint32_t hal_standby(uint32_t wake_after_s);

/* ---- I2C0 (400 kHz controller) -------------------------------------------
 * Write wn bytes, then (repeated start) read rn bytes. Either may be 0.
 * Blocking with an internal timeout of about 10 ms. wn = rn = 0 is an
 * address-only probe (HAL_OK if the device acknowledges). */
hal_status_t hal_i2c_xfer(uint8_t addr, const uint8_t *w, size_t wn,
                          uint8_t *r, size_t rn);
/* Bus-hang recovery: 9 clock pulses, STOP, re-init. */
void hal_i2c_recover(void);

/* ---- OLED bus (SPI1 mode 0, 8 MHz, MSB first) ----------------------------
 * attach(true): SCK/MOSI become SPI pins. DC, CS, RESET stay GPIO and are
 * driven through hal_pin_write() by the display driver.
 * attach(false): SCK and MOSI driven low as GPIO (the driver separately
 * drives DC, CS and RESET low). Required before OLED_EN goes low. */
void hal_oled_bus_attach(bool attached);
/* Blocking write; returns after the last bit has left the shifter. Must not
 * be called while an async write is busy. Bounded: if the SPI (or a DMA
 * transfer still running) does not finish within ~10 ms, a hardware fault,
 * the transfer is aborted (DMA stopped, SPI1 re-initialised) and the call
 * returns; bytes may then be missing. attach() waits the same way. */
void hal_oled_write(const uint8_t *buf, size_t n);
/* DMA write. buf must stay valid and unchanged until hal_oled_busy() is
 * false. Returns false if a transfer is already running. */
bool hal_oled_write_async(const uint8_t *buf, size_t n);
bool hal_oled_busy(void);

/* ---- Acquisition ---------------------------------------------------------
 * One time base. hal_acq_start() starts every requested stream from a single
 * timer event, so with both running:
 *   ECG raw sample n is taken at        t = n * 250 us
 *   PPG frame k: dark sample at         t = k * 10 ms + PPG_T_DARK_US
 *                LED on (LED_BLANK low) t = k * 10 ms + PPG_T_LED_ON_US
 *                                       (until one timer tick before frame end)
 *                on sample at           t = k * 10 ms + PPG_T_ON_SAMPLE_US
 * hal_acq_start() requires that no stream is running; to change the set of
 * streams, stop everything and start again. Sample/frame indices restart
 * at 0. hal_acq_stop() stops all streams and leaves LED_BLANK high.
 *
 * Preconditions (caller's job): rails up, MCLK fast, and for PPG
 * hal_ppg_analog(true). */
#define HAL_ACQ_ECG (1u << 0)
#define HAL_ACQ_PPG (1u << 1)
void hal_acq_start(unsigned streams);
void hal_acq_stop(void);
unsigned hal_acq_running(void);

/* Stream reads. Both report the absolute timeline position, so a caller can
 * neither miscount nor misplace lost data:
 *   - every sample (frame) since hal_acq_start() has a timeline index,
 *     counting lost ones too (ECG sample n is taken at n * 250 us, PPG frame
 *     k at k * 10 ms, see above), modulo 2^32;
 *   - one call returns only samples (frames) that are contiguous on the
 *     timeline, and stores the index of the first one in *first_index
 *     (written only when the return value is > 0; first_index must not be
 *     NULL, else 0 is returned);
 *   - the next call starts at or after (first_index + returned count); a
 *     difference is the number lost in between (reader too slow: the oldest
 *     data is overwritten; a conversion that was missed). The caller fills
 *     gaps by index. Only after a ring-index resync (hal_acq_resyncs()) may
 *     indices repeat; callers drop such an overlap.
 * The internal buffers hold at least 64 ms (ECG) / 320 ms (PPG).
 *
 * hal_*_overruns(): diagnostics only, identical rule in the BSP and the
 * simulator: samples (frames) lost since hal_acq_start() that a read has
 * already skipped, i.e. (first_index + count of the latest read) minus the
 * total delivered since hal_acq_start(). A loss not yet passed by a read is
 * not counted yet. */

/* ECG: raw 12-bit ADC0 codes (hardware-averaged x4) at ECG_RAW_HZ. Returns
 * the number of samples copied (0..max). */
size_t hal_ecg_read(uint16_t *dst, size_t max, uint32_t *first_index);
uint32_t hal_ecg_overruns(void);

/* PPG: one frame per 10 ms, 12-bit ADC1 codes. */
typedef struct {
    uint16_t tia_dark; /* TIA_OUT, LEDs off */
    uint16_t hi_dark;  /* PPG_HI, LEDs off (usually railed) */
    uint16_t tia_on;   /* TIA_OUT, LEDs on */
    uint16_t hi_on;    /* PPG_HI, LEDs on */
} hal_ppg_frame_t;
size_t hal_ppg_read(hal_ppg_frame_t *dst, size_t max, uint32_t *first_index);
uint32_t hal_ppg_overruns(void);

/* Ring-index resynchronisations since hal_acq_start() (ECG + PPG): the
 * hardware write position was found behind the reader, which can only
 * happen if the position source is unreliable; the reader then restarts at
 * the write position instead of wrapping. Should stay 0; diagnostics only
 * (always 0 in the simulator). */
uint32_t hal_acq_resyncs(void);

/* PPG on-chip analog: COMP0 DAC8 photodiode bias, OPA0 (TIA), DAC12, OPA1.
 * The GPAMP is never enabled. The DAC12 offset is undefined after
 * hal_ppg_analog(true) until hal_ppg_set_offset() is called. Must not be
 * turned off while the PPG stream is running. */
void hal_ppg_analog(bool on);
/* DAC12 offset code (0..4095, VDDA reference). Takes effect immediately;
 * call right after hal_ppg_read() returned a frame so the step lands in the
 * dark gap, and treat the next frame as disturbed. */
void hal_ppg_set_offset(uint16_t dac12_code);

/* One-shot conversions for start-up checks and diagnostics: 16-sample
 * average, 12-bit code. Only valid while no acquisition stream is running
 * (returns 0 otherwise). */
typedef enum {
    HAL_ADC_ECG,     /* PA14 */
    HAL_ADC_TIA,     /* PA22 */
    HAL_ADC_PPG_HI,  /* PA16 */
    HAL_ADC_VMID,    /* PB17 */
    HAL_ADC_COUNT
} hal_adc_ch_t;
uint16_t hal_adc_read_once(hal_adc_ch_t ch);

/* ---- Non-volatile settings blob (last flash sector) ----------------------
 * A single blob of up to HAL_NV_MAX bytes. Integrity (CRC, versioning) is
 * the caller's job. hal_nv_read() of never-written storage yields 0xFF.
 * hal_nv_write() blocks for tens of ms with interrupts masked, so it must not
 * be called during acquisition. Errors: HAL_ERR_PARAM (n too large),
 * HAL_ERR_TIMEOUT (erase/program failed), HAL_ERR_BUS (verify mismatch). */
#define HAL_NV_MAX 256u
void hal_nv_read(void *dst, size_t n);
hal_status_t hal_nv_write(const void *src, size_t n);

/* ---- Radio link: CH585 on UART0 (ecg-fw.md "Inter-processor link") -------
 * hal_radio_power(true), in this order:
 *   1. RADIO_EN (PB18) high;
 *   2. UART0 on: LINK_BAUD (460800) 8N1, no flow control, rings emptied;
 *   3. PA11 = UART0_RX (internal pull-up: idle-high until the CH585 drives
 *      it), PA10 = UART0_TX (idles high).
 * hal_radio_power(false), in this order:
 *   1. UART0 off (bytes still in the TX ring or FIFO are discarded);
 *   2. PA10 GPIO output low, PA11 input with pull-down (the off-state of
 *      hal_init());
 *   3. RADIO_EN low.
 * So no pin is high into the CH585 while its supply is off (brief rule 5).
 * The brief's step list sets the UART pins before RADIO_EN; doing it the
 * other way round avoids the idle-high TX pin back-feeding the radio for
 * the few microseconds in between, and the CH585 boots for milliseconds
 * before it listens. PB16 (RADIO_IRQ) stays an input with pull-down
 * throughout (unused in link protocol 1). Both calls are idempotent and take
 * a few microseconds. The radio must be off before hal_standby().
 *
 * hal_uart_write(): non-blocking, all or nothing. Copies n bytes into the
 * TX ring (HAL_UART_TX_RING, drained by the UART0 TX interrupt at line rate)
 * and returns n, or returns 0 if they do not fit or the radio is off.
 * hal_uart_read(): copies up to max received bytes out of the RX ring
 * (HAL_UART_RX_RING, filled by the UART0 RX interrupt) and returns the count.
 * No HAL_EV_* bit for received data: the RX interrupt itself ends
 * hal_idle(), SysTick ends it every 1 ms anyway, and the RX ring holds 5.5 ms
 * of back-to-back bytes at 460800 baud, so polling once per main-loop pass
 * is enough.
 *
 * Clock switches: hal_clock_fast() waits (bounded, ~0.2 ms) for the TX FIFO
 * and shifter to go idle, then reprograms the UART0 divider for the new
 * clock with interrupts masked. TX bytes are never cut; a byte the radio is
 * sending during the switch may be corrupted (one frame then fails its CRC
 * on the MCU side). */
#define HAL_UART_TX_RING 512u
#define HAL_UART_RX_RING 256u
void hal_radio_power(bool on);
bool hal_radio_powered(void);
size_t hal_uart_write(const uint8_t *buf, size_t n);
size_t hal_uart_read(uint8_t *buf, size_t max);

typedef struct {
    uint32_t rx_bytes;    /* received and stored */
    uint32_t tx_bytes;    /* accepted by hal_uart_write() */
    uint32_t rx_overruns; /* bytes lost: RX ring full or hardware FIFO overrun */
    uint32_t rx_errors;   /* framing / parity / break errors (byte kept) */
} hal_uart_stats_t;
/* Counters since hal_init(). */
void hal_uart_stats(hal_uart_stats_t *out);

/* ---- Debug channels (SEGGER RTT compatible, read by the J-Link) ----------
 * Both are non-blocking and drop data when the host is not reading.
 * Channel 0: text log. Channel 1: binary stream frames (see stream.h).
 * Return the number of bytes accepted. */
size_t hal_log_write(const char *s, size_t n);
size_t hal_stream_write(const uint8_t *buf, size_t n);

#endif
