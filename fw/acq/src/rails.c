#include "rails.h"

#include "dsp_ppg_ctrl.h"
#include "hal.h"
#include "mcp4728.h"
#include "settings.h"

#define LED_OFF_TRIES 3u

static struct {
    bool analog;
    bool ppg_analog;
    bool vled;
    uint16_t led_code;   /* 0 = channel A powered down (as far as we know) */
    uint16_t dac12;
    uint32_t analog_ms, vled_ms;
    uint32_t i2c_errors;
    bool dac_stuck;
} R;

static void pin(board_pin_t p, bool high) { hal_pin_write(p, high); }

void rails_init(void)
{
    R.analog = true; /* force every step of rails_off() to run */
    R.ppg_analog = true;
    R.vled = true;
    R.led_code = 0;
    R.i2c_errors = 0;
    R.dac_stuck = false;
    rails_off();
}

void rails_analog_on(void)
{
    if (R.analog)
        return;
    pin(PIN_ANA_EN, true);
    pin(PIN_BUCK_PS_SYNC, true);
    R.analog = true;
    R.analog_ms = hal_millis();
}

bool rails_analog_is_on(void) { return R.analog; }
uint32_t rails_analog_on_ms(void) { return R.analog_ms; }

bool rails_ecg_frontend(bool gain_hi, uint8_t rld)
{
    if (!R.analog)
        return false;
    pin(PIN_GAIN_HI, gain_hi);
    pin(PIN_RLD_L_EN, rld == RLD_BOTH || rld == RLD_LEFT);
    pin(PIN_RLD_R_EN, rld == RLD_BOTH || rld == RLD_RIGHT);
    return true;
}

bool rails_fast_restore(bool on)
{
    if (on && !R.analog)
        return false;
    pin(PIN_FAST_RESTORE, on);
    return true;
}

bool rails_measure_req(bool on)
{
    if (!on) {
        pin(PIN_MEASURE_REQ, false);
        return true;
    }
    /* CHG_PG low = USB power present: never request the electrodes. The
     * interlock would block it anyway; firmware reports, not works around. */
    if (!R.analog || !hal_pin_read(PIN_CHG_PG))
        return false;
    pin(PIN_MEASURE_REQ, true);
    /* An edge between the read and the write is caught by the CHG_PG ISR,
     * which drives MEASURE_REQ low itself; re-check for the window before
     * the ISR is entered. */
    if (!hal_pin_read(PIN_CHG_PG)) {
        pin(PIN_MEASURE_REQ, false);
        return false;
    }
    return true;
}

bool rails_ppg_analog_on(uint16_t dac12)
{
    if (!R.analog)
        return false;
    if (!R.ppg_analog) {
        hal_ppg_analog(true);
        R.ppg_analog = true;
    }
    hal_ppg_set_offset(dac12);
    R.dac12 = dac12;
    return true;
}

void rails_ppg_offset(uint16_t dac12)
{
    if (!R.ppg_analog)
        return;
    if (dac12 > 4095u)
        dac12 = 4095u;
    hal_ppg_set_offset(dac12);
    R.dac12 = dac12;
}

uint16_t rails_ppg_offset_code(void) { return R.dac12; }

bool rails_vled(bool on)
{
    if (on == R.vled)
        return true;
    /* LED_BLANK is high whenever no stream runs (hal.h); VLED_EN may only
     * change then. */
    if (hal_acq_running() != 0u)
        return false;
    if (on && !R.analog)
        return false;
    pin(PIN_VLED_EN, on);
    R.vled = on;
    if (on)
        R.vled_ms = hal_millis();
    return true;
}

uint32_t rails_vled_on_ms(void) { return R.vled_ms; }

bool rails_led_set(uint16_t code)
{
    if (!R.analog)
        return false;
    uint16_t cap = dsp_led_max_code(LED_MAX_MA);
    if (code > cap)
        code = cap;
    if (mcp4728_set_led_code(code) != HAL_OK) {
        R.i2c_errors++;
        return false;
    }
    R.led_code = code;
    return true;
}

uint16_t rails_led_code(void) { return R.led_code; }

bool rails_acq_start(unsigned streams)
{
    if (!R.analog || streams == 0u || hal_acq_running() != 0u)
        return false;
    if ((streams & HAL_ACQ_PPG) && (!R.ppg_analog || !R.vled))
        return false;
    hal_clock_fast(true);
    hal_acq_start(streams);
    return true;
}

static void led_dac_off(void)
{
    if (!mcp4728_led_is_on() && R.led_code == 0u)
        return;
    for (unsigned i = 0; i < LED_OFF_TRIES; i++) {
        if (mcp4728_led_off() == HAL_OK) {
            R.led_code = 0;
            R.dac_stuck = false;
            return;
        }
        R.i2c_errors++;
    }
    if (mcp4728_power_down_all() == HAL_OK) {
        R.led_code = 0;
        R.dac_stuck = false;
        return;
    }
    /* Out of options. LED_BLANK is high (Q1 holds GREEN_SET at ground), so
     * no LED current flows; the rail still goes down rather than leaving
     * the analog section powered indefinitely. Reported via diagnostics. */
    R.i2c_errors++;
    R.dac_stuck = true;
}

void rails_off(void)
{
    /* 1-2: everything that switches the body or the front end */
    pin(PIN_MEASURE_REQ, false);
    pin(PIN_FAST_RESTORE, false);
    pin(PIN_GAIN_HI, false);
    pin(PIN_RLD_L_EN, false);
    pin(PIN_RLD_R_EN, false);
    /* 3: timers and ADCs; leaves LED_BLANK high */
    if (hal_acq_running() != 0u)
        hal_acq_stop();
    /* 4: LED DAC channel A powered down */
    led_dac_off();
    /* 5: LED boost (LED_BLANK is high now) */
    if (R.vled) {
        pin(PIN_VLED_EN, false);
        R.vled = false;
    }
    /* 6: on-chip PPG analog */
    if (R.ppg_analog) {
        hal_ppg_analog(false);
        R.ppg_analog = false;
    }
    /* 7: rails */
    if (R.analog) {
        pin(PIN_BUCK_PS_SYNC, false);
        pin(PIN_ANA_EN, false);
        R.analog = false;
    }
    /* 8 */
    hal_clock_fast(false);
}

uint32_t rails_i2c_errors(void) { return R.i2c_errors; }
bool rails_led_dac_stuck(void) { return R.dac_stuck; }
