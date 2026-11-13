/* ECG contact / session logic in the world of the first hardware run:
 * dry thumbs (150 mV electrode offset steps through U5's 0.5 s coupling),
 * floating inputs that rail with brief in-range excursions, thumb
 * adjustments, the BSP ring losing one sample per 512-sample lap. The
 * firmware is the real application; the state is followed at every main-loop
 * pass. */
#include <stdlib.h>
#include <string.h>

#include "acq.h"
#include "scen.h"

/* ---- world ---------------------------------------------------------------- */

static void hw_world(void)
{
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    sim_ecg_set_mains(50.0, 30.0);
    sim_ecg_set_contact_transient_uv(150000); /* 150 mV at every touch */
    sim_acq_set_ring_model(SIM_RING_BSP);
    sim_acq_set_missed(SIM_BSP_ECG_RING_SAMPLES, 0); /* lost=354 in 45 s on the board */
}

static void to_ecg_card(void)
{
    scen_run_ms(300);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
}

/* ---- state watcher (every main-loop pass) --------------------------------- */

static struct {
    acq_ecg_state_t stop_at; /* scen_run_cond() stops when the state is this */
    bool stop_on_summary;
    uint8_t last;
    bool seen_settle;
    uint32_t wait_after_settle; /* WAIT entered after the first SETTLE */
    uint32_t to_wait, to_settle, to_hold;
    uint64_t t_settle, t_run, t_end, t_card;
    bool ran;
} W;

static void watch_reset(void)
{
    memset(&W, 0, sizeof W);
    W.stop_at = (acq_ecg_state_t)255;
}

static bool watch(void)
{
    const acq_status_t *st = acq_status();
    if (acq_active() && st->phase == ACQ_ON) {
        uint8_t s = st->ecg_state;
        if (s != W.last) {
            uint64_t t = sim_now_ms();
            if (s == ACQ_ECG_SETTLE) {
                W.to_settle++;
                if (!W.seen_settle) W.t_settle = t;
                W.seen_settle = true;
            }
            if (s == ACQ_ECG_WAIT) {
                W.to_wait++;
                if (W.seen_settle) W.wait_after_settle++;
            }
            if (s == ACQ_ECG_HOLD) W.to_hold++;
            if (s == ACQ_ECG_RUN && !W.ran) {
                W.t_run = t;
                W.ran = true;
            }
            if (s == ACQ_ECG_END) W.t_end = t;
            W.last = s;
        }
        if (s == W.stop_at) return true;
    }
    if (W.stop_on_summary && scen_screen() == UI_SCREEN_SUMMARY) return true;
    return false;
}

static bool run_watch(uint64_t ms) { return scen_run_cond(watch, ms); }

/* Arm on the card (thumbs off: floating); returns the arm time. */
static uint64_t arm(void)
{
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    return sim_now_ms();
}

/* ---- tests ------------------------------------------------------------------ */

/* One touch with a 150 mV offset step: RUNNING within ~4 s, and once the
 * signal was stable the screen never goes back to "place thumbs". The old
 * logic flapped between the two for as long as the step decayed. */
TEST(touch_150mv)
{
    scen_boot(false);
    hw_world();
    to_ecg_card();
    watch_reset();
    arm();
    run_watch(2000); /* floating: excursions, FAST_RESTORE pulses */
    CHECK_EQ(W.to_settle, 0);
    sim_set_thumbs(true);
    uint64_t t_on = sim_now_ms();
    W.stop_at = ACQ_ECG_RUN;
    CHECK(run_watch(8000));
    printf("  touch -> settling %llu ms, -> running %llu ms\n",
           (unsigned long long)(W.t_settle - t_on), (unsigned long long)(W.t_run - t_on));
    CHECK(W.t_run - t_on <= 4500);
    CHECK_EQ(W.wait_after_settle, 0);
    CHECK_EQ(W.to_settle, 1);
    W.stop_at = (acq_ecg_state_t)255;
    run_watch(15000);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_RUN);
    CHECK_EQ(W.to_hold, 0);
    CHECK_NEAR(scen_m()->live.hr_bpm, 72, 2);
    CHECK(acq_summary()->ecg_lost > 100); /* the BSP lap loss, re-filled */
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_SUMMARY);
    CHECK(scen_m()->summary.valid);
    sim_set_thumbs(false);
}

/* Three thumb adjustments (new offset steps) while settling: RUNNING comes
 * later, the screen stays on "Hold still". */
TEST(adjust_while_settling)
{
    scen_boot(false);
    hw_world();
    to_ecg_card();
    watch_reset();
    arm();
    run_watch(1000);
    sim_set_thumbs(true);
    uint64_t t_on = sim_now_ms();
    W.stop_at = ACQ_ECG_SETTLE;
    CHECK(run_watch(5000));
    W.stop_at = (acq_ecg_state_t)255;
    static const int32_t step[3] = {120000, -100000, 150000};
    for (int k = 0; k < 3; k++) {
        run_watch(1000); /* settling on a stable signal ... */
        CHECK(!acq_status()->railed);
        CHECK(acq_status()->settle_pct > 0);
        uint8_t pct = acq_status()->settle_pct;
        sim_ecg_thumb_adjust(step[k], 0); /* ... the thumb moves */
        run_watch(300);
        CHECK(acq_status()->settle_pct < pct); /* settling restarted */
        CHECK_EQ(scen_live_state(), UI_MEAS_SETTLING);
        /* back to stable: the settling restarts */
        while (acq_status()->railed || acq_status()->settle_pct == 0)
            if (!run_watch(10) && sim_now_ms() - t_on > 30000) break;
    }
    W.stop_at = ACQ_ECG_RUN;
    CHECK(run_watch(8000));
    printf("  touch -> running %llu ms with 3 adjustments while settling\n",
           (unsigned long long)(W.t_run - t_on));
    CHECK(W.t_run - t_on > 8000);
    CHECK_EQ(W.wait_after_settle, 0);
    CHECK_EQ(W.to_settle, 1);
    scen_tap(1);
    sim_set_thumbs(false);
}

/* A thumb movement while RUNNING: "Hold still" (HOLD), the session goes on. */
TEST(adjust_while_running)
{
    scen_boot(false);
    hw_world();
    to_ecg_card();
    watch_reset();
    arm();
    sim_set_thumbs(true);
    W.stop_at = ACQ_ECG_RUN;
    CHECK(run_watch(8000));
    W.stop_at = (acq_ecg_state_t)255;
    run_watch(5000);
    uint32_t beats0 = acq_status()->ecg_beats;
    sim_ecg_thumb_adjust(150000, 0);
    run_watch(500);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_HOLD);
    CHECK_EQ(scen_live_state(), UI_MEAS_SETTLING);
    uint64_t t_adj = sim_now_ms() - 500;
    W.stop_at = ACQ_ECG_RUN;
    CHECK(run_watch(8000));
    printf("  thumb moved while running: back to running after %llu ms\n",
           (unsigned long long)(sim_now_ms() - t_adj));
    CHECK(sim_now_ms() - t_adj < 6000);
    CHECK_EQ(W.wait_after_settle, 0);
    CHECK(W.t_end == 0);
    W.stop_at = (acq_ecg_state_t)255;
    run_watch(5000);
    CHECK(acq_status()->ecg_beats > beats0 + 5);
    scen_tap(1);
    CHECK(scen_m()->summary.valid);
    sim_set_thumbs(false);
}

/* Thumbs lifted after RUNNING: the summary in 3-4 s at most, despite the
 * floating-input excursions (which kept the old app from ever ending). */
static uint64_t lift_to_summary(sim_float_model_t model, double ex_rate)
{
    scen_boot(false);
    hw_world();
    sim_ecg_set_float_model(model);
    to_ecg_card();
    watch_reset();
    arm();
    sim_set_thumbs(true);
    W.stop_at = ACQ_ECG_RUN;
    CHECK(run_watch(8000));
    W.stop_at = (acq_ecg_state_t)255;
    run_watch(12000);
    sim_ecg_set_float_excursions(ex_rate, 20, 300, 5);
    sim_set_thumbs(false);
    uint64_t t_off = sim_now_ms();
    W.stop_on_summary = true;
    CHECK(run_watch(10000));
    uint64_t dt = sim_now_ms() - t_off;
    CHECK_EQ(scen_screen(), UI_SCREEN_SUMMARY);
    CHECK(scen_m()->summary.valid);
    CHECK(scen_analog_all_off());
    CHECK_EQ(W.wait_after_settle, 0);
    return dt;
}

TEST(lift_ends_session)
{
    uint64_t d1 = lift_to_summary(SIM_FLOAT_EXCURSIONS, 1.0);
    uint64_t d3 = lift_to_summary(SIM_FLOAT_EXCURSIONS, 4.0);
    uint64_t dr = lift_to_summary(SIM_FLOAT_RAIL, 1.0);
    uint64_t dm = lift_to_summary(SIM_FLOAT_MID, 1.0);
    printf("  lift -> summary: excursions 1/s %llu ms, 4/s %llu ms, rail %llu ms, quiet "
           "mid-scale %llu ms\n", (unsigned long long)d1, (unsigned long long)d3,
           (unsigned long long)dr, (unsigned long long)dm);
    CHECK(d1 <= 3000 && d3 <= 3000 && dr <= 3000);
    /* open inputs at mid-scale: only the missing beats tell (no beat for
     * ACQ_ECG_NO_BEAT_MS after the last one, up to one RR before the lift) */
    CHECK(dm >= ACQ_ECG_NO_BEAT_MS - 1000 && dm <= ACQ_ECG_NO_BEAT_MS + 400);
}

/* Floating inputs alone: never RUNNING; disarmed after 30 s. */
TEST(floating_only)
{
    static const struct { sim_float_model_t m; double rate; } w[] = {
        {SIM_FLOAT_EXCURSIONS, 1.0}, {SIM_FLOAT_EXCURSIONS, 4.0}, {SIM_FLOAT_RAIL, 1.0},
    };
    for (size_t i = 0; i < sizeof w / sizeof w[0]; i++) {
        scen_boot(false);
        hw_world();
        sim_ecg_set_float_model(w[i].m);
        sim_ecg_set_float_excursions(w[i].rate, 20, 300, 11 + i);
        to_ecg_card();
        watch_reset();
        uint64_t t0 = arm();
        while (scen_screen() == UI_SCREEN_ECG && sim_now_ms() - t0 < 35000)
            run_watch(50);
        uint64_t dt = sim_now_ms() - t0;
        printf("  floating (model %d, %.0f/s): disarmed after %llu ms, %u settling entries\n",
               (int)w[i].m, w[i].rate, (unsigned long long)dt, W.to_settle);
        CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
        CHECK(dt >= 29900 && dt < 30700);
        /* at 4/s two excursions sometimes merge into >= 0.5 s in range: a
         * short "settling" may show, never RUNNING */
        CHECK(W.to_settle <= (w[i].rate > 2.0 ? 2u : 0u));
        CHECK(!W.ran);
        CHECK(scen_analog_all_off());
    }
    /* open inputs quietly at mid-scale look stable: settling, but without a
     * beat it falls back to waiting and never runs; the disarm then follows
     * 30 s after that fallback */
    scen_boot(false);
    hw_world();
    sim_ecg_set_float_model(SIM_FLOAT_MID);
    to_ecg_card();
    watch_reset();
    uint64_t t0 = arm();
    while (scen_screen() == UI_SCREEN_ECG && sim_now_ms() - t0 < 45000)
        run_watch(50);
    uint64_t dt = sim_now_ms() - t0;
    printf("  floating at mid-scale: %u settling entries, %u fallbacks, disarmed after %llu ms\n",
           W.to_settle, W.to_wait, (unsigned long long)dt);
    CHECK(!W.ran);
    CHECK_EQ(W.to_settle, 1);
    CHECK_EQ(W.to_wait, 1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    CHECK(dt >= 29900 && dt < 30700); /* the beat-less contact does not restart it */
}

/* The struggle of the first session: light, intermittent touches and thumb
 * adjustments for 16 s, then holding still. The old logic logged 33
 * contact / lost pairs; now the state may go WAIT <-> SETTLE only on the
 * coarse time scale (3 s without a stable signal), and RUNNING follows the
 * still hold within ~4 s. Prints the RTT log of the session. */
static uint32_t lcg(uint32_t *x)
{
    *x = *x * 1664525u + 1013904223u;
    return *x >> 8;
}

TEST(struggle_then_hold)
{
    scen_boot(false);
    hw_world();
    sim_ecg_set_float_excursions(2.0, 20, 300, 2);
    to_ecg_card();
    watch_reset();
    sim_log_clear();
    uint64_t t0 = arm();
    uint32_t r = 12345;
    uint64_t t = t0 + 2000;
    bool on = false;
    while (t < t0 + 18000) {
        on = !on;
        sim_at_thumbs(t, on);
        uint64_t len = on ? 150 + lcg(&r) % 900 : 40 + lcg(&r) % 400;
        if (on)
            for (uint64_t ta = t + 100 + lcg(&r) % 300; ta < t + len; ta += 200 + lcg(&r) % 400) {
                int32_t uv = (int32_t)(30000 + lcg(&r) % 170000);
                sim_at_thumb_adjust(ta, lcg(&r) & 1 ? uv : -uv, lcg(&r) % 3 ? 0 : 20);
            }
        t += len;
    }
    uint64_t t_hold = t;
    if (!on) sim_at_thumbs(t, true);
    run_watch(t_hold - sim_now_ms());
    uint32_t flips = W.to_wait + W.to_settle;
    W.stop_at = ACQ_ECG_RUN;
    bool ran = W.ran || run_watch(10000);
    uint64_t t_run = W.t_run;
    W.stop_at = (acq_ecg_state_t)255;
    run_watch(8000);
    sim_set_thumbs(false);
    uint64_t t_lift = sim_now_ms();
    W.stop_on_summary = true;
    run_watch(6000);
    printf("  struggle %llu s: %u WAIT/SETTLE changes; held still at %llu ms, running %llu ms "
           "later; summary %llu ms after the lift\n", (unsigned long long)((t_hold - t0) / 1000),
           flips, (unsigned long long)(t_hold - t0),
           (unsigned long long)(ran ? t_run - t_hold : 0),
           (unsigned long long)(sim_now_ms() - t_lift));
    CHECK(ran);
    CHECK(flips <= 8);
    CHECK(t_run <= t_hold || t_run - t_hold <= 4500);
    CHECK_EQ(scen_screen(), UI_SCREEN_SUMMARY);
    /* the RTT log: transitions only, one evidence line per second, well
     * under the budget of the 1 KB buffer */
    const char *log = sim_log_text();
    size_t secs = (size_t)((sim_now_ms() - t0) / 1000u);
    printf("  RTT log: %zu bytes in %zu s (%.0f B/s)\n", sim_log_len(), secs,
           (double)sim_log_len() / (double)secs);
    CHECK(sim_log_len() < 300u * secs);
    CHECK(strstr(log, "acq: contact") == NULL);
    size_t longest = 0, len = 0;
    for (const char *p = log; *p; p++) {
        if (*p == '\n') {
            if (len > longest) longest = len;
            len = 0;
        } else {
            len++;
        }
    }
    CHECK(longest <= 110);
    if (getenv("SCEN_PRINT_LOG")) fputs(log, stdout);
    sim_set_thumbs(false);
}

/* BOTH mode: the ECG and PPG evidence lines together stay inside the RTT
 * budget, and look like the documented format. */
TEST(both_mode_log)
{
    scen_boot(false);
    hw_world();
    sim_set_finger(true);
    scen_run_ms(300);
    for (int i = 0; i < 3; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH_CARD);
    sim_log_clear();
    uint64_t t0 = sim_now_ms();
    scen_tap(1);
    sim_set_thumbs(true);
    watch_reset();
    run_watch(20000);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_RUN);
    const char *log = sim_log_text();
    double bps = (double)sim_log_len() * 1000.0 / (double)(sim_now_ms() - t0);
    const char *e = NULL, *p = NULL;
    for (const char *q = log; (q = strstr(q, "] ecg: run rail=")) != NULL; q++) e = q;
    for (const char *q = log; (q = strstr(q, "] ppg: on led=")) != NULL; q++) p = q;
    while (e && e > log && e[-1] != '\n') e--;
    while (p && p > log && p[-1] != '\n') p--;
    printf("  BOTH: %.0f B/s of log\n", bps);
    if (e) printf("  %.*s\n", (int)(strchr(e, '\n') - e), e);
    if (p) printf("  %.*s\n", (int)(strchr(p, '\n') - p), p);
    CHECK(bps < 300.0);
    CHECK(e != NULL && p != NULL);
    if (getenv("SCEN_PRINT_LOG")) fputs(log, stdout);
    scen_tap(1);
    sim_set_thumbs(false);
    sim_set_finger(false);
}

int main(void)
{
    RUN(touch_150mv);
    RUN(adjust_while_settling);
    RUN(adjust_while_running);
    RUN(lift_ends_session);
    RUN(floating_only);
    RUN(struggle_then_hold);
    RUN(both_mode_log);
    printf("watchdog resets %u, violations %zu\n", (unsigned)sim_facts()->wdt_resets,
           sim_violation_count());
    return TEST_RESULT();
}
