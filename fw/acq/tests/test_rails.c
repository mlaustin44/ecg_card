/* rails.c against the simulator in ABORT mode: every sequence must satisfy
 * the board's rail rules (a violation aborts the test). */
#include "mcp4728.h"
#include "rails.h"
#include "settings.h"
#include "sim.h"
#include "test.h"

static void boot(void)
{
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    hal_init();
    rails_init();
    CHECK(mcp4728_init() == HAL_OK);
}

static bool all_off(void)
{
    static const board_pin_t p[] = {PIN_ANA_EN, PIN_VLED_EN, PIN_BUCK_PS_SYNC, PIN_MEASURE_REQ,
                                    PIN_FAST_RESTORE, PIN_GAIN_HI, PIN_RLD_L_EN, PIN_RLD_R_EN};
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++)
        if (sim_pin(p[i]))
            return false;
    return sim_led_blank() && !hal_acq_running() && !sim_clock_fast() && sim_mcp_pd(0) != 0;
}

TEST(init_all_off)
{
    boot();
    CHECK(all_off());
    CHECK(!rails_analog_is_on());
    /* nothing may be raised without the analog rail */
    CHECK(!rails_ecg_frontend(true, RLD_BOTH));
    CHECK(!rails_fast_restore(true));
    CHECK(!rails_measure_req(true));
    CHECK(!rails_ppg_analog_on(100));
    CHECK(!rails_vled(true));
    CHECK(!rails_led_set(100));
    CHECK(!rails_acq_start(HAL_ACQ_ECG));
    CHECK(all_off());
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(ecg_sequence)
{
    boot();
    rails_analog_on();
    CHECK(sim_pin(PIN_ANA_EN) && sim_pin(PIN_BUCK_PS_SYNC));
    hal_delay_ms(ECG_VMID_SETTLE_MS);
    CHECK(rails_ecg_frontend(true, RLD_LEFT));
    CHECK(sim_pin(PIN_GAIN_HI) && sim_pin(PIN_RLD_L_EN) && !sim_pin(PIN_RLD_R_EN));
    CHECK(rails_ecg_frontend(false, RLD_RIGHT));
    CHECK(!sim_pin(PIN_GAIN_HI) && !sim_pin(PIN_RLD_L_EN) && sim_pin(PIN_RLD_R_EN));
    CHECK(rails_ecg_frontend(false, RLD_OFF));
    CHECK(!sim_pin(PIN_RLD_L_EN) && !sim_pin(PIN_RLD_R_EN));
    CHECK(rails_ecg_frontend(false, RLD_BOTH));
    CHECK(rails_measure_req(true));
    sim_set_thumbs(true);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    CHECK(rails_acq_start(HAL_ACQ_ECG));
    CHECK(sim_clock_fast());
    CHECK(hal_acq_running() == HAL_ACQ_ECG);
    /* VLED may not change while a stream runs */
    CHECK(!rails_vled(true));
    hal_delay_ms(50);
    rails_off();
    CHECK(all_off());
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
    rails_off(); /* idempotent */
    CHECK(all_off());
    /* standby preconditions hold after the stop sequence */
    sim_stop_at(sim_now_ms() + 2000);
    hal_standby(1);
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(measure_req_refused_on_usb)
{
    boot();
    sim_set_usb(true);
    rails_analog_on();
    hal_delay_ms(5);
    CHECK(!rails_measure_req(true));
    CHECK(!sim_pin(PIN_MEASURE_REQ));
    sim_set_usb(false);
    CHECK(rails_measure_req(true));
    CHECK(sim_pin(PIN_MEASURE_REQ));
    /* USB plugged: the HAL ISR drops MEASURE_REQ itself */
    sim_set_usb(true);
    CHECK(!sim_pin(PIN_MEASURE_REQ));
    rails_off();
    CHECK(all_off());
}

TEST(ppg_sequence_and_limit)
{
    boot();
    rails_analog_on();
    CHECK(rails_ppg_analog_on(1000));
    CHECK_EQ(rails_ppg_offset_code(), 1000);
    CHECK(rails_vled(true));
    CHECK(sim_pin(PIN_VLED_EN));
    /* not without VLED settle, but rails itself does not wait: the caller
     * does (acq.c). Here: wait, then the LED current. */
    hal_delay_ms(10);
    CHECK(rails_led_set(4095)); /* clamped to the board ceiling */
    CHECK(rails_led_code() <= 4034);
    CHECK(sim_led_set_ma() <= (double)LED_MAX_MA + 0.01);
    CHECK(rails_acq_start(HAL_ACQ_PPG));
    hal_delay_ms(100);
    CHECK(sim_facts()->ppg_frames >= 9);
    CHECK_EQ(sim_facts()->ppg_vled_early_starts, 0);
    rails_off();
    CHECK(all_off());
    CHECK(sim_led_now_ma() == 0.0);
    CHECK(!sim_pin(PIN_VLED_EN));
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(stop_with_i2c_trouble)
{
    /* LED DAC write fails once during the stop: retried, still clean */
    boot();
    rails_analog_on();
    CHECK(rails_ppg_analog_on(500));
    CHECK(rails_vled(true));
    hal_delay_ms(10);
    CHECK(rails_led_set(1000));
    CHECK(rails_acq_start(HAL_ACQ_PPG));
    hal_delay_ms(30);
    sim_i2c_fail_next(1, HAL_ERR_NACK);
    rails_off();
    CHECK(all_off());
    CHECK(!rails_led_dac_stuck());
    CHECK(rails_i2c_errors() >= 1);
}

int main(void)
{
    RUN(init_all_off);
    RUN(ecg_sequence);
    RUN(measure_req_refused_on_usb);
    RUN(ppg_sequence_and_limit);
    RUN(stop_with_i2c_trouble);
    return TEST_RESULT();
}
