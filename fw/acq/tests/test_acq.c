/* acq.c against the simulator (ABORT mode) with synthetic ECG / PPG. */
#include <stdlib.h>
#include <string.h>

#include "acq.h"
#include "mcp4728.h"
#include "rails.h"
#include "settings.h"
#include "sim.h"
#include "test.h"
#include "testsrc.h"

#define MAXEV 256
static struct {
    uint32_t beat_idx[MAXEV], foot_idx[MAXEV];
    uint16_t pat_ms[MAXEV];
    uint32_t nb, np;
} R; /* beats and pulses with their stream indices */

static struct {
    uint32_t ecg_blocks, ppg_blocks, beats, pulses, contacts, settles;
    uint32_t next_ecg, next_ppg;
    bool ecg_gap, ppg_gap;
    uint32_t pat_sum, pat_n;
    uint64_t fr_rise_us, fr_prev_rise_us;
    uint32_t fr_pulses, fr_bad, fr_before_electrodes, fr_close;
    uint64_t fr_min_gap_us;
    unsigned mid_after_fr_ms;  /* floating inputs at mid-scale this long after each pulse */
    uint32_t polls_in_range;   /* polls with the detector not railed */
} C;

static void back_to_rail(void *ctx)
{
    (void)ctx;
    sim_ecg_set_float_model(SIM_FLOAT_RAIL);
}

static void s_ecg(uint32_t start, uint8_t f, const int16_t *s)
{
    (void)f; (void)s;
    if (start != C.next_ecg) C.ecg_gap = true;
    C.next_ecg = start + 10;
    C.ecg_blocks++;
}
static void s_ppg(uint32_t start, uint16_t led, uint8_t f, const int16_t *cf)
{
    (void)led; (void)f; (void)cf;
    if (start != C.next_ppg) C.ppg_gap = true;
    C.next_ppg = start + 2;
    C.ppg_blocks++;
}
static void s_beat(uint32_t i, uint16_t hr)
{
    (void)hr;
    C.beats++;
    if (R.nb < MAXEV) R.beat_idx[R.nb++] = i;
}
static void s_pulse(uint32_t i, uint16_t b, uint16_t pat, uint16_t pi)
{
    (void)b; (void)pi;
    if (R.np < MAXEV) {
        R.foot_idx[R.np] = i;
        R.pat_ms[R.np++] = pat;
    }
    C.pulses++;
    if (pat) { C.pat_sum += pat; C.pat_n++; }
}
static void s_contact(bool on) { if (on) C.contacts++; }
static void s_settled(bool on) { if (on) C.settles++; }
static const acq_sink_t sink = {s_ecg, s_ppg, NULL, NULL, s_beat, s_pulse, s_contact, s_settled, NULL};

static void boot(void)
{
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    memset(&C, 0, sizeof C);
    memset(&R, 0, sizeof R);
    hal_init();
    rails_init();
    CHECK(mcp4728_init() == HAL_OK);
    acq_init(&sink);
}

/* Main-loop stand-in; also measures FAST_RESTORE pulse widths. */
static void run_until(uint64_t t_ms)
{
    bool fr = false;
    while (sim_now_ms() < t_ms) {
        acq_poll(hal_millis());
        bool now = sim_pin(PIN_FAST_RESTORE);
        if (now && !fr) {
            C.fr_prev_rise_us = C.fr_rise_us;
            C.fr_rise_us = sim_now_us();
            if (!sim_rail(SIM_RAIL_BODY_EN)) C.fr_before_electrodes++;
            if (C.fr_pulses) {
                uint64_t gap = C.fr_rise_us - C.fr_prev_rise_us;
                if (gap < ACQ_FR_REPEAT_MS * 1000u) C.fr_close++;
                if (!C.fr_min_gap_us || gap < C.fr_min_gap_us) C.fr_min_gap_us = gap;
            }
        }
        if (!now && fr) {
            uint64_t w = sim_now_us() - C.fr_rise_us;
            C.fr_pulses++;
            if (w < 20000 || w > 50000) C.fr_bad++;
            if (C.mid_after_fr_ms) {
                sim_ecg_set_float_model(SIM_FLOAT_MID);
                sim_after_ms(C.mid_after_fr_ms, back_to_rail, NULL);
            }
        }
        if (acq_active() && acq_status()->phase == ACQ_ON && !acq_status()->railed)
            C.polls_in_range++;
        fr = now;
        hal_wdt_kick();
        hal_idle();
    }
}

static acq_cfg_t cfg(bool ecg, bool ppg)
{
    acq_cfg_t c = {.ecg = ecg, .ppg = ppg, .gain_hi = false, .gain_auto = false, .rld = RLD_BOTH,
                   .notch = NOTCH_AUTO, .led_limit_ma = 15, .restore = RESTORE_FAST};
    return c;
}

TEST(vmid_fault)
{
    boot();
    sim_ecg_set_vmid_error_mv(40);
    acq_cfg_t c = cfg(true, false);
    CHECK(acq_start(&c));
    run_until(1000);
    CHECK(acq_status()->phase == ACQ_FAILED);
    CHECK(acq_status()->err == ACQ_ERR_VMID);
    CHECK(!sim_pin(PIN_ANA_EN) && !sim_pin(PIN_MEASURE_REQ));
}

TEST(ecg_session)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    acq_cfg_t c = cfg(true, false);
    CHECK(acq_start(&c));
    run_until(2000);
    CHECK(acq_status()->phase == ACQ_ON);
    CHECK(!acq_status()->contact);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    sim_set_thumbs(true);
    run_until(4000);
    CHECK(acq_status()->contact);
    CHECK(!acq_status()->settled);
    run_until(20000);
    CHECK(acq_status()->settled);
    CHECK_NEAR(acq_status()->hr_bpm, tc.hr_bpm, 2);
    /* brief start step 4: FAST_RESTORE stays low until the electrodes are
     * connected; afterwards pulses while railed (floating inputs too), at
     * most every ACQ_FR_REPEAT_MS */
    CHECK(C.fr_pulses >= 1);
    CHECK_EQ(C.fr_before_electrodes, 0);
    CHECK_EQ(C.fr_close, 0);
    CHECK_EQ(C.fr_bad, 0);
    CHECK(!C.ecg_gap);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK(sim_now_ms() == 20000);
    uint32_t idx0 = acq_status()->ecg_samples;
    /* a 150 ms stall loses samples; the timeline must stay intact */
    hal_delay_ms(150);
    run_until(21000);
    CHECK(hal_ecg_overruns() > 0);
    uint32_t idx = acq_status()->ecg_samples;
    CHECK_NEAR((double)(idx - idx0), 500.0, 1); /* 1 s of 500 Hz despite the loss */
    CHECK(!C.ecg_gap);
    /* thumbs off: a rail is a blip first (RUN goes on, the trace shows a
     * gap), "hold still" after ACQ_ECG_BLIP_MS, the end of contact after
     * ACQ_ECG_LOST_END_MS on the rail */
    sim_set_thumbs(false);
    run_until(22000);
    CHECK(acq_status()->contact);
    CHECK(acq_status()->settled);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_RUN);
    CHECK(acq_status()->blip);
    CHECK_EQ(acq_status()->blips, 1);
    run_until(23500);
    CHECK(acq_status()->contact);
    CHECK(!acq_status()->settled);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_HOLD);
    run_until(25500);
    CHECK(!acq_status()->contact);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_END);
    CHECK_EQ(acq_status()->ecg_why, ACQ_WHY_NO_STABLE);
    acq_stop();
    CHECK(acq_summary()->ecg_lost > 0);
    CHECK(acq_summary()->hrv.beats >= 15);
    CHECK_NEAR(acq_summary()->hrv.hr_avg, tc.hr_bpm, 3);
    /* bench A/B totals: a long clean contact, mostly in range */
    printf("  ecg_rail %u/%u inr_max %u ms fr %u\n", (unsigned)acq_summary()->ecg_rail_n,
           (unsigned)acq_summary()->ecg_raw_n, (unsigned)acq_summary()->ecg_run_max_ms,
           (unsigned)acq_summary()->fr_pulses);
    CHECK(acq_summary()->ecg_run_max_ms >= 5000u);
    CHECK(acq_summary()->ecg_rail_n < acq_summary()->ecg_raw_n / 2u);
    CHECK(!sim_pin(PIN_ANA_EN) && !sim_rail(SIM_RAIL_BODY_EN));
}

/* cfg.rearm (HR monitor): the end of contact after RUNNING goes back to WAIT,
 * the session goes on; the next contact settles and runs again with beats
 * (the detector re-learned), and the statistics span both contacts. */
TEST(ecg_rearm)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    acq_cfg_t c = cfg(true, false);
    c.rearm = true;
    CHECK(acq_start(&c));
    run_until(2000);
    sim_set_thumbs(true);
    run_until(15000);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_RUN);
    uint32_t settles = C.settles, beats1 = acq_summary()->hrv.beats;
    sim_set_thumbs(false);
    bool ended = false;
    while (sim_now_ms() < 25000) {
        run_until(sim_now_ms() + 10);
        ended |= acq_status()->ecg_state == ACQ_ECG_END;
    }
    CHECK(!ended);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_WAIT);
    CHECK_EQ(acq_status()->ecg_why, ACQ_WHY_NO_STABLE);
    CHECK(!acq_status()->contact && !acq_status()->settled);
    CHECK_EQ(acq_status()->hr_bpm, 0);
    CHECK(acq_active() && acq_status()->phase == ACQ_ON);
    uint32_t beats_off = C.beats;
    run_until(30000);
    CHECK_EQ(C.beats, beats_off); /* nothing reported while waiting */
    sim_set_thumbs(true);
    run_until(42000);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_RUN);
    CHECK_EQ(C.settles, settles + 1);
    CHECK(C.beats > beats_off + 5);
    CHECK_NEAR(acq_status()->hr_bpm, tc.hr_bpm, 2);
    acq_stop();
    CHECK(acq_summary()->hrv.beats > beats1 + 5);
    CHECK_NEAR(acq_summary()->hrv.hr_avg, tc.hr_bpm, 3);
    sim_set_thumbs(false);
}

/* FAST_RESTORE policy on floating inputs that sit on the rail: a pulse once
 * the output has been railed for ACQ_FR_RAIL_MS, then every ACQ_FR_REPEAT_MS;
 * the mid-scale samples the pulse forces (and DSP_LEAD_BLANK_MS after it)
 * are no evidence: the detector stays railed, nothing starts. */
static void floating_rail_run_mode(unsigned mid_after_ms, uint8_t restore)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    sim_ecg_set_float_model(SIM_FLOAT_RAIL);
    C.mid_after_fr_ms = mid_after_ms;
    acq_cfg_t c = cfg(true, false);
    c.restore = restore;
    CHECK(acq_start(&c));
    run_until(11000);
    CHECK_EQ(C.fr_before_electrodes, 0);
    acq_stop();
}

static void floating_rail_run(unsigned mid_after_ms)
{
    floating_rail_run_mode(mid_after_ms, RESTORE_FAST);
}

TEST(fast_restore_policy)
{
    floating_rail_run(0);
    printf("  floating on the rail, 10.6 s: %u pulses, closest %.1f ms apart\n", C.fr_pulses,
           C.fr_min_gap_us / 1000.0);
    CHECK(C.fr_pulses >= 40 && C.fr_pulses <= 43); /* every 250 ms */
    CHECK_EQ(C.fr_close, 0);
    CHECK(C.fr_min_gap_us < (ACQ_FR_REPEAT_MS + 20u) * 1000u);
    CHECK_EQ(C.fr_bad, 0);
    CHECK_EQ(C.polls_in_range, 0); /* the pulses' mid-scale samples: no evidence */
    CHECK(!acq_status()->contact);
    /* session totals: every pulse counted; blanked samples excluded, so a
     * railed run reads fully railed */
    const acq_summary_t *a = acq_summary();
    CHECK_EQ(a->fr_pulses, C.fr_pulses);
    CHECK(a->ecg_raw_n > 30000u && a->ecg_raw_n < 42400u);
    CHECK_EQ(a->ecg_rail_n, a->ecg_raw_n);
    CHECK_EQ(a->ecg_run_max_ms, 0u);
    /* in range for 25 ms right after each release: inside the 10 ms blanking
     * + 20 ms in-range test, still nothing */
    floating_rail_run(25);
    CHECK(C.fr_pulses >= 40);
    CHECK_EQ(C.polls_in_range, 0);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_WAIT);
    /* 45 ms in range after each release: that is evidence (in range), but
     * far from stable */
    floating_rail_run(45);
    CHECK(C.polls_in_range > 0);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_WAIT);
    sim_ecg_set_float_model(SIM_FLOAT_EXCURSIONS);
}

/* RESTORE_SLOW: pulses only after ACQ_FR_SLOW_RAIL_MS on the rail, at most
 * every ACQ_FR_SLOW_REPEAT_MS. RESTORE_OFF: never. */
TEST(restore_slow_off)
{
    floating_rail_run_mode(0, RESTORE_SLOW);
    printf("  slow, 10.6 s on the rail: %u pulses, closest %.1f ms apart\n", C.fr_pulses,
           C.fr_min_gap_us / 1000.0);
    CHECK(C.fr_pulses >= 4 && C.fr_pulses <= 6);
    CHECK(C.fr_min_gap_us >= ACQ_FR_SLOW_REPEAT_MS * 1000u);
    CHECK_EQ(C.fr_bad, 0);
    CHECK_EQ(acq_summary()->fr_pulses, C.fr_pulses);
    CHECK_EQ(acq_status()->ecg_state, ACQ_ECG_WAIT);

    floating_rail_run_mode(0, RESTORE_OFF);
    CHECK_EQ(C.fr_pulses, 0);
    CHECK_EQ(acq_summary()->fr_pulses, 0u);
    CHECK(acq_summary()->ecg_raw_n > 40000u);
    CHECK_EQ(acq_summary()->ecg_rail_n, acq_summary()->ecg_raw_n);
    sim_ecg_set_float_model(SIM_FLOAT_EXCURSIONS);
}

TEST(ppg_session)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    tc.refl_ua_per_ma = 0.6; /* too bright at 5 mA: AGC must step down */
    testsrc_install(&tc);
    sim_set_finger(true);
    acq_cfg_t c = cfg(false, true);
    CHECK(acq_start(&c));
    run_until(30000);
    const acq_status_t *s = acq_status();
    CHECK(s->phase == ACQ_ON);
    CHECK(s->finger && s->settled);
    CHECK(s->led_code < dsp_led_max_code(LED_START_MA));
    double tia = sim_ppg_tia_v();
    (void)tia;
    CHECK_NEAR(s->pulse_bpm, tc.hr_bpm, 2);
    CHECK(!C.ppg_gap);
    CHECK_EQ(sim_facts()->ppg_offset_in_sample_window, 0);
    CHECK_EQ(sim_facts()->ppg_vled_early_starts, 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
    acq_stop();
    CHECK(acq_summary()->pulses > 20);
    CHECK_NEAR(acq_summary()->pulse_avg, tc.hr_bpm, 2);
    CHECK(sim_led_now_ma() == 0.0 && !sim_pin(PIN_VLED_EN));
}

/* Review finding: pulse_bpm froze at the last value when the pulses stopped
 * (and, after ~10 s, the detector never resumed). Now: "not available"
 * after ACQ_PULSE_TIMEOUT_MS, and detection resumes when the pulse returns. */
TEST(ppg_pulse_timeout)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    sim_set_finger(true);
    acq_cfg_t c = cfg(false, true);
    CHECK(acq_start(&c));
    run_until(25000);
    const acq_status_t *s = acq_status();
    CHECK(s->settled && s->finger);
    CHECK_NEAR(s->pulse_bpm, tc.hr_bpm, 2);
    CHECK_NEAR(s->hr_bpm, tc.hr_bpm, 2);
    CHECK(s->pi_permille > 0);
    CHECK_EQ(s->quality, 4);
    testsrc_ppg_pulsatile(false);
    run_until(25000 + ACQ_PULSE_TIMEOUT_MS - 2500);
    CHECK(s->pulse_bpm != 0); /* not before the timeout */
    run_until(25000 + ACQ_PULSE_TIMEOUT_MS + 1500);
    CHECK(s->finger && s->settled);
    CHECK_EQ(s->pulse_bpm, 0);
    CHECK_EQ(s->hr_bpm, 0);
    CHECK_EQ(s->pi_permille, 0);
    CHECK_EQ(s->quality, 2);
    /* 30 s without a visible pulse, then it comes back */
    run_until(60000);
    uint32_t pulses0 = C.pulses;
    testsrc_ppg_pulsatile(true);
    run_until(75000);
    CHECK(C.pulses - pulses0 >= 14); /* 15 s at 72 bpm = 18 beats */
    CHECK_NEAR(s->pulse_bpm, tc.hr_bpm, 2);
    CHECK_NEAR(s->hr_bpm, tc.hr_bpm, 2);
    acq_stop();
}

TEST(both_pat)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    tc.pat_s = 0.230;
    testsrc_install(&tc);
    sim_set_finger(true);
    sim_set_thumbs(true);
    acq_cfg_t c = cfg(true, true);
    CHECK(acq_start(&c));
    run_until(40000);
    CHECK(acq_status()->settled);
    CHECK(C.pat_n > 10);
    double truth = testsrc_true_pat_ms(5, 40);
    double pat = (double)C.pat_sum / C.pat_n;
    printf("  PAT %.1f ms, truth %.1f ms\n", pat, truth);
    CHECK_NEAR(pat, truth, 10);
    CHECK_NEAR(acq_status()->pulse_bpm, tc.hr_bpm, 2);
    CHECK_NEAR(acq_status()->hr_bpm, tc.hr_bpm, 2);
    CHECK_NEAR(acq_status()->fast_bpm, tc.hr_bpm, 6); /* Pulse mode: pulse-to-pulse */
    acq_stop();
    CHECK(acq_summary()->pat_n > 10);
}

/* Review finding 3: the BSP discovers ring overruns at read time; the
 * filler must land where the data was lost. Same world twice: undisturbed,
 * then with the BSP ring model, 150 ms reader stalls (> the 124 ms ring) and
 * sparse missed conversions. Every R-peak and pulse foot away from a lost
 * span must have exactly the same stream index, and the same PAT. */
static uint64_t s_stall_at[] = {12000, 17300, 23100, 29900, 34700};
#define NSTALL (sizeof s_stall_at / sizeof s_stall_at[0])

static void run_both_world(bool disturbed)
{
    boot();
    if (disturbed) {
        sim_acq_set_ring_model(SIM_RING_BSP);
        sim_acq_set_missed(25013, 1709); /* ~every 6.3 s / 17 s */
    }
    testsrc_cfg_t tc = testsrc_defaults();
    tc.pat_s = 0.215;
    testsrc_install(&tc);
    sim_set_finger(true);
    sim_set_thumbs(true);
    acq_cfg_t c = cfg(true, true);
    CHECK(acq_start(&c));
    size_t k = 0;
    while (sim_now_ms() < 40000) {
        acq_poll(hal_millis());
        hal_wdt_kick();
        if (disturbed && k < NSTALL && sim_now_ms() >= s_stall_at[k]) {
            hal_delay_ms(150); /* main loop blocked */
            k++;
        } else {
            hal_idle();
        }
    }
    CHECK(acq_status()->settled);
    acq_stop();
}

TEST(timeline_exact_after_stalls)
{
    run_both_world(false);
    static uint32_t ref_b[MAXEV], ref_f[MAXEV];
    static uint16_t ref_p[MAXEV];
    uint32_t nb = R.nb, np = R.np;
    memcpy(ref_b, R.beat_idx, sizeof ref_b);
    memcpy(ref_f, R.foot_idx, sizeof ref_f);
    memcpy(ref_p, R.pat_ms, sizeof ref_p);
    CHECK_EQ(acq_summary()->ecg_lost, 0);
    CHECK(nb >= 40 && np >= 40);

    run_both_world(true);
    const acq_summary_t *s = acq_summary();
    printf("  lost: %u ECG samples, %u PPG frames; beats %u/%u, pulses %u/%u\n", s->ecg_lost,
           s->ppg_lost, R.nb, nb, R.np, np);
    /* each stall loses ~26 ms (600 - 496 samples) at least; plus misses */
    CHECK(s->ecg_lost >= NSTALL * (600u - 496u));
    CHECK(s->ppg_lost >= 2);
    CHECK_EQ(s->ecg_resyncs, 0);
    CHECK_EQ(s->ppg_resyncs, 0);
    CHECK_EQ(hal_acq_resyncs(), 0);
    CHECK_EQ(s->ecg_lost, hal_ecg_overruns());
    /* match every reference beat; those far from a disturbance exactly */
    uint32_t exact = 0, near = 0, missing = 0;
    for (uint32_t i = 0; i < nb; i++) {
        bool found = false;
        for (uint32_t j = 0; j < R.nb; j++) {
            int32_t d = (int32_t)(R.beat_idx[j] - ref_b[i]);
            if (d == 0) { exact++; found = true; break; }
            if (d >= -2 && d <= 2) { near++; found = true; break; }
        }
        if (!found) missing++;
    }
    uint32_t pmiss = 0;
    for (uint32_t i = 0; i < np; i++) {
        bool found = false;
        for (uint32_t j = 0; j < R.np && !found; j++)
            found = R.foot_idx[j] == ref_f[i];
        if (!found) pmiss++;
    }
    /* PAT values (each event carries the latest paired result, so compare
     * them as a multiset) */
    static bool used[MAXEV];
    memset(used, 0, sizeof used);
    uint32_t pat_ref = 0, pat_same = 0;
    for (uint32_t i = 0; i < np; i++) {
        if (!ref_p[i]) continue;
        pat_ref++;
        for (uint32_t j = 0; j < R.np; j++)
            if (!used[j] && R.pat_ms[j] == ref_p[i]) {
                used[j] = true;
                pat_same++;
                break;
            }
    }
    printf("  beats: %u exact, %u within 2 samples, %u missing; feet missing %u; PAT %u/%u same\n",
           exact, near, missing, pmiss, pat_same, pat_ref);
    /* no beat lost or moved by the timeline: at most the few next to a lost
     * span may differ by a sample (filter state after the filler) */
    CHECK_EQ(missing, 0);
    CHECK(near <= NSTALL + 2);
    CHECK(R.nb <= nb + 1);
    /* pulses: a foot whose window holds a filled (disturbed) frame is not
     * reported, by design; every other foot sits on the same index with the
     * same PAT */
    CHECK(pmiss <= 3);
    CHECK(pat_ref >= 30);
    CHECK(pat_same + 3 >= pat_ref);
    sim_acq_set_ring_model(SIM_RING_DEFAULT);
    sim_acq_set_missed(0, 0);
}

/* The fast 2-beat rate follows respiratory sinus arrhythmia (15 breaths/min,
 * +-8 % RR: about 11 bpm peak to peak); the 5-beat median of hr_bpm much less. */
TEST(fast_rate_follows_rsa)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    tc.hr_bpm = 70;
    tc.rr_sd = 0.005;
    tc.rsa = 0.08;
    testsrc_install(&tc);
    acq_cfg_t c = cfg(true, false);
    CHECK(acq_start(&c));
    sim_set_thumbs(true);
    run_until(14000);
    CHECK(acq_status()->settled);
    unsigned fmin = 999, fmax = 0, hmin = 999, hmax = 0, n = 0;
    double fsum = 0;
    for (uint64_t t = 14000; t < 30000; t += 100) {
        run_until(t);
        const acq_status_t *st = acq_status();
        if (!st->fast_bpm || !st->hr_bpm)
            continue;
        n++;
        fsum += st->fast_bpm;
        if (st->fast_bpm < fmin) fmin = st->fast_bpm;
        if (st->fast_bpm > fmax) fmax = st->fast_bpm;
        if (st->hr_bpm < hmin) hmin = st->hr_bpm;
        if (st->hr_bpm > hmax) hmax = st->hr_bpm;
    }
    printf("  RSA +-8 %%: fast %u..%u bpm, median-of-5 %u..%u bpm\n", fmin, fmax, hmin, hmax);
    CHECK(n > 100);
    CHECK_NEAR(fsum / n, 70, 3);
    CHECK(fmax - fmin >= 8);
    CHECK((fmax - fmin) * 2 >= 3 * (hmax - hmin));
    acq_stop();
    sim_set_thumbs(false);
}

/* Strong mains pickup (a debugger tying the card to mains ground on the
 * bench): the hum warning after ACQ_HUM_ON_MS, gone again ACQ_HUM_OFF_MS
 * after it drops; ordinary hum never raises it. */
TEST(mains_hum_warning)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    sim_ecg_set_mains(50.0, 30.0);
    acq_cfg_t c = cfg(true, false);
    CHECK(acq_start(&c));
    sim_set_thumbs(true);
    run_until(15000);
    CHECK(!acq_status()->hum);
    CHECK(acq_status()->mains_uvpp < ACQ_HUM_OFF_UVPP);
    sim_ecg_set_mains(50.0, 3000.0);
    run_until(17000);
    CHECK(!acq_status()->hum); /* not at once */
    run_until(22000);
    printf("  hum: mains %u uVpp\n", acq_status()->mains_uvpp);
    CHECK(acq_status()->hum);
    sim_ecg_set_mains(50.0, 30.0);
    run_until(23000);
    CHECK(acq_status()->hum);
    run_until(28000);
    CHECK(!acq_status()->hum);
    acq_stop();
    sim_set_thumbs(false);
}

int main(void)
{
    RUN(vmid_fault);
    RUN(ecg_session);
    RUN(ecg_rearm);
    RUN(fast_restore_policy);
    RUN(restore_slow_off);
    RUN(mains_hum_warning);
    RUN(fast_rate_follows_rsa);
    RUN(ppg_session);
    RUN(ppg_pulse_timeout);
    RUN(both_pat);
    RUN(timeline_exact_after_stalls);
    return TEST_RESULT();
}
