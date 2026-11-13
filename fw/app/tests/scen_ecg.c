/* ECG sessions end to end: arm, contact, settling, running, summary. */
#include <string.h>

#include "acq.h"
#include "scen.h"

static void to_ecg_card(void)
{
    scen_run_ms(300);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
}

static bool is_running(void) { return scen_live_state() == UI_MEAS_RUNNING && scen_screen() == UI_SCREEN_ECG; }
static bool is_settling(void) { return scen_live_state() == UI_MEAS_SETTLING; }
static bool is_waiting(void) { return scen_live_state() == UI_MEAS_WAIT_CONTACT; }
static bool is_summary(void) { return scen_screen() == UI_SCREEN_SUMMARY; }
static bool is_ecg_card(void) { return scen_screen() == UI_SCREEN_ECG_CARD; }

/* Start on the card, thumbs on, wait for RUNNING; returns the RUNNING time. */
static uint64_t arm_and_run(void)
{
    scen_tap(1);
    scen_run_ms(500);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    return sim_now_ms();
}

TEST(full_session)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    tc.hr_bpm = 66;
    tc.rr_sd = 0.04;
    testsrc_install(&tc);
    /* realistic 60 Hz pickup (with no mains at all the detector may pick
     * 50 Hz from QRS harmonics: a DSP property, see the report) */
    sim_ecg_set_mains(60.0, 30.0);
    to_ecg_card();
    scen_shot("ecg_card");
    CHECK(!sim_rail(SIM_RAIL_3V3A));
    scen_tap(1);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK_EQ(scen_live_state(), UI_MEAS_WAIT_CONTACT);
    CHECK(sim_rail(SIM_RAIL_3V3A) && sim_pin(PIN_BUCK_PS_SYNC));
    CHECK(!sim_rail(SIM_RAIL_BODY_EN)); /* VMID still settling */
    scen_run_ms(600);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    CHECK(sim_pin(PIN_RLD_L_EN) && sim_pin(PIN_RLD_R_EN) && !sim_pin(PIN_GAIN_HI));
    CHECK(sim_clock_fast());
    scen_shot("ecg_wait_contact");
    sim_set_thumbs(true);
    uint64_t t_on = sim_now_ms();
    CHECK(scen_run_cond(is_settling, 2000));
    scen_run_ms(800);
    scen_shot("ecg_settling");
    CHECK(scen_m()->live.settle_pct > 0 && scen_m()->live.settle_pct < 100);
    CHECK(scen_run_cond(is_running, 5000));
    uint64_t t_run = sim_now_ms();
    printf("  contact -> running %llu ms\n", (unsigned long long)(t_run - t_on));
    CHECK(t_run - t_on >= 2500 && t_run - t_on < 4500);
    uint32_t fl0 = sim_facts()->oled_async_writes;
    scen_run_ms(12000);
    double fps = (sim_facts()->oled_async_writes - fl0) / 12.0;
    printf("  display %.1f frames/s while running\n", fps);
    CHECK(fps > 20.0 && fps <= UI_MAX_FPS + 0.1);
    CHECK_NEAR(scen_m()->live.hr_bpm, tc.hr_bpm, 2);
    CHECK(scen_m()->live.quality >= 3);
    CHECK_EQ(scen_m()->live.notch_hz, 60);
    CHECK(scen_lit_pixels() > 150);
    scen_shot("ecg_running");
    scen_run_ms(30000);
    CHECK_NEAR(scen_m()->live.hr_bpm, tc.hr_bpm, 2);
    CHECK(scen_m()->live.elapsed_s >= 40);
    /* thumbs off: summary a bit over 2 s after the lead-off */
    sim_set_thumbs(false);
    uint64_t t_off = sim_now_ms();
    scen_run_ms(1500);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK_EQ(scen_live_state(), UI_MEAS_WAIT_CONTACT);
    CHECK(scen_run_cond(is_summary, 2000));
    uint64_t dt = sim_now_ms() - t_off;
    CHECK(dt > 2000 && dt < 2500);
    scen_run_ms(200);
    scen_shot("ecg_summary");
    const ui_summary_t *s = &scen_m()->summary;
    testsrc_truth_t tr = testsrc_truth((double)t_run / 1000.0, (double)t_off / 1000.0);
    printf("  summary: %u beats avg %u min %u max %u SDNN %u RMSSD %u (truth %d beats, avg %.1f, "
           "%.1f..%.1f, SDNN %.1f RMSSD %.1f) %u s\n",
           s->beats, s->hr_avg, s->hr_min, s->hr_max, s->sdnn_ms, s->rmssd_ms, tr.beats, tr.hr_avg,
           tr.hr_min5, tr.hr_max5, tr.sdnn_ms, tr.rmssd_ms, s->duration_s);
    CHECK(s->valid);
    CHECK(!s->stopped_by_usb);
    CHECK_EQ(s->mode, UI_MODE_ECG);
    CHECK_NEAR(s->beats, tr.beats, 3);
    CHECK_NEAR(s->hr_avg, tr.hr_avg, 1.5);
    CHECK_NEAR(s->hr_min, tr.hr_min5, 2);
    CHECK_NEAR(s->hr_max, tr.hr_max5, 2);
    CHECK_NEAR(s->sdnn_ms, tr.sdnn_ms, 0.15 * tr.sdnn_ms + 2);
    CHECK_NEAR(s->rmssd_ms, tr.rmssd_ms, 0.15 * tr.rmssd_ms + 2);
    CHECK_NEAR(s->duration_s, (t_off - t_run) / 1000.0, 1.5);
    /* rails down, electrodes disconnected */
    CHECK(scen_analog_all_off());
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
    CHECK_EQ(scen.body_en_outside_session, 0);
    CHECK_EQ(scen.analog_outside, 0);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK_EQ(app_debug()->loop_acq_max_ms <= 20, 1);
    printf("  max step during acquisition %llu us (fw: %u ms)\n",
           (unsigned long long)scen.max_step_acq_us, (unsigned)app_debug()->loop_acq_max_ms);
    CHECK(scen.max_step_acq_us <= 20000);
    /* summary -> card */
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    CHECK_EQ(sim_facts()->wdt_resets, 0);
}

TEST(brief_loss_and_too_short)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    to_ecg_card();
    arm_and_run();
    scen_run_ms(4000);
    /* lift for 1 s: stays in the session, settles again */
    sim_set_thumbs(false);
    scen_run_ms(1000);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_settling, 1500));
    CHECK(scen_run_cond(is_running, 5000));
    scen_run_ms(1000);
    /* end now with BUTTON1: RUNNING happened but too short */
    scen_tap(1);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_SUMMARY);
    CHECK(!scen_m()->summary.valid);
    scen_shot("ecg_summary_too_short");
    CHECK(scen_analog_all_off());
    sim_set_thumbs(false);
}

TEST(no_contact_timeout_and_abort)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    to_ecg_card();
    scen_tap(1);
    uint64_t t0 = sim_now_ms();
    CHECK(scen_run_cond(is_ecg_card, 35000));
    uint64_t dt = sim_now_ms() - t0;
    printf("  disarmed after %llu ms\n", (unsigned long long)dt);
    CHECK(dt >= 29700 && dt < 30500);
    CHECK(scen_analog_all_off());
    /* contact lost during settling only: back to waiting */
    scen_tap(1);
    scen_run_ms(500);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_settling, 2000));
    sim_set_thumbs(false);
    scen_run_ms(3000);
    CHECK(is_waiting());
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    /* BUTTON1 before RUNNING: back to the card, no summary */
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    /* BUTTON2 long mid-record: Home, no summary */
    arm_and_run();
    scen_press(2, 2100);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK(scen_analog_all_off());
    sim_set_thumbs(false);
    CHECK_EQ(scen.body_en_outside_session, 0);
}

TEST(auto_gain_and_notch)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    tc.amp_mv = 0.5; /* small QRS: the advisor asks for 1098x */
    testsrc_install(&tc);
    sim_ecg_set_mains(50.0, 60.0);
    to_ecg_card();
    arm_and_run();
    CHECK_EQ(acq_status()->notch_hz, 50); /* decided while settling */
    CHECK(!sim_pin(PIN_GAIN_HI));
    scen_run_ms(15000);
    CHECK_NEAR(scen_m()->live.hr_bpm, tc.hr_bpm, 2);
    sim_set_thumbs(false);
    CHECK(scen_run_cond(is_summary, 4000));
    CHECK(scen_m()->summary.valid);
    CHECK_EQ(acq_summary()->gain_advice, 1);
    scen_tap(1);
    /* next record starts at 1098x (between records only) */
    scen_tap(1);
    scen_run_ms(600);
    CHECK(sim_pin(PIN_GAIN_HI));
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    CHECK(scen_m()->live.gain_hi);
    scen_run_ms(12000);
    CHECK_NEAR(scen_m()->live.hr_bpm, tc.hr_bpm, 2);
    scen_shot("ecg_running_1098");
    scen_tap(1);
    CHECK(scen_analog_all_off());
    sim_set_thumbs(false);
}

int main(void)
{
    RUN(full_session);
    RUN(brief_loss_and_too_short);
    RUN(no_contact_timeout_and_abort);
    RUN(auto_gain_and_notch);
    printf("watchdog resets %u, violations %zu\n", (unsigned)sim_facts()->wdt_resets,
           sim_violation_count());
    return TEST_RESULT();
}
