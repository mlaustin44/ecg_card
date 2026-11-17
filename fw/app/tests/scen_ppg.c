/* PPG sessions on battery and on USB: AGC / DC cancel, LED limit, summary. */
#include "acq.h"
#include "scen.h"

static bool is_running(void) { return scen_live_state() == UI_MEAS_RUNNING; }
static bool is_summary(void) { return scen_screen() == UI_SCREEN_SUMMARY; }
static bool is_ppg_card(void) { return scen_screen() == UI_SCREEN_PPG_CARD; }

static void to_ppg_card(void)
{
    scen_tap(2);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_PPG_CARD);
}

static bool leds_dark(void)
{
    return sim_led_now_ma() == 0.0 && !sim_pin(PIN_VLED_EN) && sim_mcp_pd(0) != 0 &&
           sim_led_blank();
}

TEST(battery_bright_finger)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    tc.hr_bpm = 78;
    tc.refl_ua_per_ma = 0.6; /* 5 mA start rails TIA_OUT: AGC must step down */
    testsrc_install(&tc);
    scen_run_ms(300);
    to_ppg_card();
    scen_shot("ppg_card");
    scen_tap(1);
    scen_run_ms(300);
    CHECK_EQ(scen_screen(), UI_SCREEN_PPG);
    CHECK_EQ(scen_live_state(), UI_MEAS_WAIT_CONTACT);
    CHECK(sim_pin(PIN_VLED_EN) && !sim_pin(PIN_MEASURE_REQ) && !sim_rail(SIM_RAIL_BODY_EN));
    scen_shot("ppg_wait");
    sim_set_finger(true);
    CHECK(scen_run_cond(is_running, 5000));
    scen_run_ms(15000);
    const acq_status_t *st = acq_status();
    printf("  LED code %u (%.2f mA), DAC12 %u, TIA %.2f V, PPG_HI %.2f V, pulse %u, PI %u\n",
           st->led_code, sim_led_set_ma(), st->dac12, sim_ppg_tia_v(), sim_ppg_hi_v(),
           st->pulse_bpm, st->pi_permille);
    CHECK(st->led_code < 1344);            /* stepped down from 5 mA */
    CHECK_NEAR(scen_m()->live.pulse_bpm, tc.hr_bpm, 2);
    CHECK_EQ(scen_m()->live.hr_bpm, scen_m()->live.pulse_bpm); /* big digits */
    CHECK_NEAR(scen_m()->live.pi_permille, 20, 6);
    scen_shot("ppg_running");
    CHECK_EQ(sim_facts()->ppg_offset_in_sample_window, 0);
    CHECK_EQ(sim_facts()->ppg_vled_early_starts, 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
    CHECK(scen.max_led_ma <= 15.05);
    sim_set_finger(false);
    CHECK(scen_run_cond(is_summary, 4000));
    scen_run_ms(100);
    const ui_summary_t *s = &scen_m()->summary;
    printf("  summary: pulse %u PI %u beats %u %u s\n", s->pulse_avg, s->pi_permille, s->beats,
           s->duration_s);
    CHECK(s->valid);
    CHECK_EQ(s->mode, UI_MODE_PPG);
    CHECK_NEAR(s->pulse_avg, tc.hr_bpm, 2);
    scen_shot("ppg_summary");
    CHECK(leds_dark());
    CHECK(scen_analog_all_off());
    CHECK(scen.max_step_acq_us <= 20000);
    scen_tap(1);
    CHECK(is_ppg_card());
}

TEST(usb_dim_finger_limit)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    tc.hr_bpm = 58;
    tc.refl_ua_per_ma = 0.06; /* dim: AGC steps up into the 10 mA limit */
    testsrc_install(&tc);
    sim_set_usb(true);
    scen_run_ms(300);
    /* Settings: LED limit 15 -> 5 -> 10 mA */
    for (int i = 0; i < 4; i++)
        scen_tap(2);
    scen_tap(1); /* open */
    for (int i = 0; i < SET_ITEM_LED_LIMIT; i++)
        scen_tap(2);
    scen_tap(1);
    scen_tap(1);
    CHECK_EQ(app_settings()->led_limit, LED_LIMIT_10MA);
    scen_press(2, 2100);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    to_ppg_card();
    sim_set_finger(true);
    scen_tap(1);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(20000);
    const acq_status_t *st = acq_status();
    printf("  LED code %u (%.2f mA max seen %.2f), TIA %.2f V, pulse %u\n", st->led_code,
           sim_led_set_ma(), scen.max_led_ma, sim_ppg_tia_v(), st->pulse_bpm);
    CHECK(st->led_code > 1344);
    CHECK(st->led_code <= 2689);
    CHECK(scen.max_led_ma <= 10.05);
    CHECK_NEAR(scen_m()->live.pulse_bpm, tc.hr_bpm, 2);
    CHECK(!sim_rail(SIM_RAIL_BODY_EN) && !sim_pin(PIN_MEASURE_REQ));
    /* BUTTON1 ends it: summary */
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_SUMMARY);
    CHECK(scen_m()->summary.valid);
    CHECK(leds_dark());
    CHECK(scen_analog_all_off());
    CHECK_EQ(scen.measure_req_on_usb, 0);
}

TEST(no_finger_timeout)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_ms(300);
    to_ppg_card();
    scen_tap(1);
    CHECK(scen_run_cond(is_ppg_card, 32000));
    CHECK(leds_dark());
    CHECK(scen_analog_all_off());
    /* finger lifted briefly: re-settles, stays in the session */
    sim_set_finger(true);
    scen_tap(1);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(3000);
    sim_set_finger(false);
    scen_run_ms(1200);
    sim_set_finger(true);
    scen_run_ms(500);
    CHECK_EQ(scen_screen(), UI_SCREEN_PPG);
    CHECK(scen_run_cond(is_running, 6000));
    scen_press(2, 2100);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK(leds_dark());
}

int main(void)
{
    RUN(battery_bright_finger);
    RUN(usb_dim_finger_limit);
    RUN(no_finger_timeout);
    return TEST_RESULT();
}
