/* BSP smoke test (ecg_bsp_smoke.elf). Never powers a rail on its own.
 *
 * Main loop: kick the watchdog, take events, log once per second over RTT
 * channel 0, sleep. A debugger can poke g_smoke_cmd (see below) to exercise
 * the rest of the HAL; every HAL entry point is reachable from here, so the
 * link proves the whole contract resolves and nothing is dropped by
 * --gc-sections. */
#include <string.h>

#include "hal.h"

/* BSP diagnostics outside hal.h (bsp_internal.h needs the TI headers). */
uint32_t bsp_ecg_pos_mismatches(void);

enum {
    CMD_NONE = 0,
    CMD_ADC_ONCE = 1,   /* one-shot of every channel into g_smoke_val[] */
    CMD_I2C_PROBE = 2,  /* CW2015 VERSION (0x00) -> g_smoke_val[0] */
    CMD_ACQ_ECG = 3,
    CMD_ACQ_PPG = 4,    /* also enables the on-chip PPG analog */
    CMD_ACQ_STOP = 5,
    CMD_CLOCK_FAST = 6,
    CMD_CLOCK_SLOW = 7,
    CMD_OLED_TEST = 8,  /* attach, blocking + DMA write, detach */
    CMD_NV_TEST = 9,
    CMD_STANDBY = 10,   /* 5 s timed STANDBY */
    CMD_I2C_RECOVER = 11,
    CMD_RESET = 12,
};

volatile uint32_t g_smoke_cmd;
volatile uint32_t g_smoke_val[8];

static uint8_t s_buf[64];
static uint16_t s_ecg[64];
static hal_ppg_frame_t s_ppg[4];

static void log_str(const char *s)
{
    (void) hal_log_write(s, strlen(s));
}

/* Tiny formatter (no printf: keeps newlib's stdio out of the image). */
static void log_kv(const char *key, uint32_t v)
{
    char b[24];
    size_t n = sizeof b;
    b[--n] = ' ';
    do {
        b[--n] = (char) ('0' + v % 10u);
        v /= 10u;
    } while (v != 0u && n > 1u);
    b[--n] = '=';
    log_str(key);
    (void) hal_log_write(&b[n], sizeof b - n);
}

static void run_cmd(uint32_t cmd)
{
    size_t i;
    switch (cmd) {
    case CMD_ADC_ONCE:
        for (i = 0; i < (size_t) HAL_ADC_COUNT; i++)
            g_smoke_val[i] = hal_adc_read_once((hal_adc_ch_t) i);
        break;
    case CMD_I2C_PROBE: {
        uint8_t reg = 0x00u, v = 0u;
        g_smoke_val[1] = (uint32_t) hal_i2c_xfer(I2C_ADDR_CW2015, &reg, 1u, &v, 1u);
        g_smoke_val[0] = v;
        break;
    }
    /* Acquisition tests power the analog rail first (no pin or on-chip
     * amplifier may drive an unpowered front end). MEASURE_REQ and VLED_EN
     * stay low: electrodes disconnected, LEDs dark. */
    case CMD_ACQ_ECG:
        hal_pin_write(PIN_ANA_EN, true);
        hal_delay_ms(10u);
        hal_clock_fast(true);
        hal_acq_start(HAL_ACQ_ECG);
        break;
    case CMD_ACQ_PPG:
        hal_pin_write(PIN_ANA_EN, true);
        hal_delay_ms(10u);
        hal_clock_fast(true);
        hal_ppg_analog(true);
        hal_ppg_set_offset(2048u);
        hal_acq_start(HAL_ACQ_PPG | HAL_ACQ_ECG);
        break;
    case CMD_ACQ_STOP:
        hal_acq_stop();
        hal_ppg_analog(false);
        hal_clock_fast(false);
        hal_pin_write(PIN_ANA_EN, false);
        break;
    case CMD_CLOCK_FAST: hal_clock_fast(true); break;
    case CMD_CLOCK_SLOW: hal_clock_fast(false); break;
    case CMD_OLED_TEST:
        /* SPI bus exercise with the panel powered and held in reset, then
         * every OLED signal low again before OLED_EN falls. */
        memset(s_buf, 0xA5, sizeof s_buf);
        hal_pin_write(PIN_OLED_RESET, false);
        hal_pin_write(PIN_OLED_EN, true);
        hal_delay_ms(10u);
        hal_oled_bus_attach(true);
        hal_oled_write(s_buf, sizeof s_buf);
        (void) hal_oled_write_async(s_buf, sizeof s_buf);
        for (i = 0; i < 50u && hal_oled_busy(); i++)
            hal_delay_ms(1u);
        hal_oled_bus_attach(false);
        hal_pin_write(PIN_OLED_DC, false);
        hal_pin_write(PIN_OLED_CS, false);
        hal_pin_write(PIN_OLED_EN, false);
        break;
    case CMD_NV_TEST:
        /* Write a modified blob, then put the original back so stored
         * settings survive the test. */
        hal_nv_read(s_buf, sizeof s_buf);
        s_buf[0]++;
        g_smoke_val[0] = (uint32_t) hal_nv_write(s_buf, sizeof s_buf);
        s_buf[0]--;
        g_smoke_val[1] = (uint32_t) hal_nv_write(s_buf, sizeof s_buf);
        break;
    case CMD_STANDBY:
        g_smoke_val[0] = hal_standby(5u);
        break;
    case CMD_I2C_RECOVER: hal_i2c_recover(); break;
    case CMD_RESET: hal_system_reset(); break;
    default: break;
    }
}

int main(void)
{
    uint32_t last_s = 0u, ev, pm;
    size_t n_ecg = 0u, n_ppg = 0u;
    uint32_t ecg_next = 0u, ppg_next = 0u;

    hal_init();
    log_kv("ecg_bsp_smoke: reset cause", (uint32_t) hal_reset_cause());
    log_str("\n");

    for (;;) {
        hal_wdt_kick();
        ev = hal_events_take();

        pm = hal_irq_lock();
        uint32_t cmd = g_smoke_cmd;
        g_smoke_cmd = CMD_NONE;
        hal_irq_unlock(pm);
        if (cmd != CMD_NONE) run_cmd(cmd);

        if (hal_acq_running() != 0u) {
            /* The timeline index of the next sample must equal what was
             * delivered plus what the BSP reports lost (hal.h). */
            uint32_t first;
            size_t k = hal_ecg_read(s_ecg, sizeof s_ecg / sizeof s_ecg[0], &first);
            if (k != 0u) {
                n_ecg += k;
                ecg_next = first + (uint32_t) k;
            }
            k = hal_ppg_read(s_ppg, sizeof s_ppg / sizeof s_ppg[0], &first);
            if (k != 0u) {
                n_ppg += k;
                ppg_next = first + (uint32_t) k;
            }
            (void) hal_stream_write((const uint8_t *) s_ecg, sizeof s_ecg);
        }

        if (hal_rtc_seconds() != last_s) {
            last_s = hal_rtc_seconds();
            log_kv("t", last_s);
            log_kv("ms", hal_millis());
            log_kv("ev", ev);
            log_kv("btn1", hal_pin_read(PIN_BUTTON1) ? 1u : 0u);
            log_kv("pg", hal_pin_read(PIN_CHG_PG) ? 1u : 0u);
            log_kv("ecg", (uint32_t) n_ecg);
            log_kv("ecg_ovr", hal_ecg_overruns());
            log_kv("ppg", (uint32_t) n_ppg);
            log_kv("ppg_ovr", hal_ppg_overruns());
            /* per session: idx = delivered + ovr of this session */
            log_kv("ecg_idx", ecg_next);
            log_kv("ppg_idx", ppg_next);
            log_kv("resync", hal_acq_resyncs());
            log_kv("dma_pos_mismatch", bsp_ecg_pos_mismatches());
            log_str("\n");
            hal_pin_write(PIN_OLED_DC, false); /* exercise the GPIO path */
        }
        if (ev & HAL_EV_USB_PLUGGED) hal_delay_us(10u);
        if (hal_acq_running() == 0u) hal_delay_ms(0u);
        hal_idle();
    }
}
