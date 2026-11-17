/* ECG + PPG together: HR, pulse, PAT against the simulated delay, and the
 * stream decoded on the C side; the capture is saved for the Python tools
 * (build/app_capture.bin, checked by the app_capture_python test). */
#include "acq.h"
#include "scen.h"
#include "stream.h"

static bool is_running(void) { return scen_live_state() == UI_MEAS_RUNNING; }
static bool is_summary(void) { return scen_screen() == UI_SCREEN_SUMMARY; }

TEST(both_session)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    tc.hr_bpm = 70;
    tc.pat_s = 0.215;
    tc.rr_sd = 0.03;
    testsrc_install(&tc);
    sim_ecg_set_mains(50.0, 20.0);
    scen_run_ms(300);
    for (int i = 0; i < 3; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH_CARD);
    scen_shot("both_card");
    sim_stream_clear();
    scen_tap(1);
    scen_run_ms(600);
    sim_set_thumbs(true);
    sim_set_finger(true);
    CHECK(scen_run_cond(is_running, 6000));
    uint64_t t_run = sim_now_ms();
    scen_run_ms(30000);
    const ui_live_t *l = &scen_m()->live;
    double pat_truth = testsrc_true_pat_ms((double)t_run / 1000.0, (double)sim_now_ms() / 1000.0);
    printf("  HR %u pulse %u PAT %u ms (truth %.1f) PI %u notch %u\n", l->hr_bpm, l->pulse_bpm,
           l->pat_ms, pat_truth, l->pi_permille, l->notch_hz);
    CHECK_NEAR(l->hr_bpm, tc.hr_bpm, 2);
    CHECK_NEAR(l->pulse_bpm, tc.hr_bpm, 2);
    CHECK_NEAR(l->pat_ms, pat_truth, 10);
    CHECK_EQ(l->notch_hz, 50);
    scen_shot("both_running");
    /* finger off for a while: the session goes on, PPG values clear */
    sim_set_finger(false);
    scen_run_ms(3000);
    CHECK(is_running());
    CHECK_EQ(scen_m()->live.pulse_bpm, 0);
    sim_set_finger(true);
    scen_run_ms(12000);
    CHECK_NEAR(scen_m()->live.pulse_bpm, tc.hr_bpm, 2);
    uint64_t t_off = sim_now_ms();
    sim_set_thumbs(false);
    sim_set_finger(false);
    CHECK(scen_run_cond(is_summary, ACQ_ECG_LOST_END_MS + 1000));
    scen_run_ms(100);
    const ui_summary_t *s = &scen_m()->summary;
    testsrc_truth_t tr = testsrc_truth((double)t_run / 1000.0, (double)t_off / 1000.0);
    printf("  summary: HR %u (%.1f) pulse %u PAT %u (%.1f) PI %u beats %u SDNN %u (%.1f) RMSSD %u (%.1f)\n",
           s->hr_avg, tr.hr_avg, s->pulse_avg, s->pat_ms, pat_truth, s->pi_permille, s->beats,
           s->sdnn_ms, tr.sdnn_ms, s->rmssd_ms, tr.rmssd_ms);
    CHECK(s->valid);
    CHECK_EQ(s->mode, UI_MODE_BOTH);
    CHECK_NEAR(s->hr_avg, tr.hr_avg, 1.5);
    CHECK_NEAR(s->pulse_avg, tc.hr_bpm, 2);
    CHECK_NEAR(s->pat_ms, pat_truth, 10);
    CHECK_NEAR(s->sdnn_ms, tr.sdnn_ms, 0.15 * tr.sdnn_ms + 2);
    scen_shot("both_summary");
    CHECK(scen_analog_all_off());

    /* ---- the stream ---- */
    scen_decoded_t d = scen_decode_stream();
    printf("  stream: %u frames, %u ECG blocks, %u PPG blocks, %u R, %u pulses (%u PAT, mean %.1f), "
           "%u lead, %u state, %u buttons; errors cobs %u crc %u len %u seq %u gaps %u/%u\n",
           d.frames, d.ecg_blocks, d.ppg_blocks, d.r_peaks, d.pulses, d.pat_n,
           d.pat_n ? d.pat_sum_ms / d.pat_n : 0.0, d.leads, d.states, d.buttons, d.bad_cobs,
           d.bad_crc, d.bad_len, d.seq_gaps, d.ecg_index_gaps, d.ppg_index_gaps);
    CHECK_EQ(d.bad_cobs + d.bad_crc + d.bad_len, 0);
    CHECK_EQ(d.seq_gaps, 0);
    CHECK_EQ(d.ecg_index_gaps, 0);
    CHECK_EQ(d.ppg_index_gaps, 0);
    CHECK_EQ(d.runs, 1);
    /* 500 Hz / 10 and 100 Hz / 2 over the whole acquisition */
    CHECK_NEAR((double)d.ecg_blocks, (double)d.ppg_blocks, 2);
    CHECK(d.ecg_blocks > 2500);
    CHECK(d.r_peaks > 50 && d.pulses > 40 && d.pat_n > 30);
    CHECK_NEAR(d.pat_sum_ms / d.pat_n, pat_truth, 10);
    CHECK_NEAR(d.hr_sum / d.hr_n, tc.hr_bpm, 3);
    CHECK(d.leads >= 2 && d.states >= 5 && d.buttons >= 1);
    CHECK(d.ecg_settling_blocks > 100 && d.ecg_leadoff_blocks > 10);
    CHECK(d.ppg_nofinger_blocks > 10);
    CHECK_EQ(stream_drops(), 0);
    CHECK(sim_stream_save("build/app_capture.bin"));
    CHECK(scen.max_step_acq_us <= 20000);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
}

int main(void)
{
    RUN(both_session);
    return TEST_RESULT();
}
