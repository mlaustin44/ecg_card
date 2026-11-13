/* Pulse arrival time: pairing rules, timeline constants, and recovery of a
 * known injected delay through BOTH full chains (4 kHz ECG codes -> decimator
 * -> QRS; PPG frames -> acquisition loop -> foot). */
#include <math.h>
#include <stdlib.h>

#include "board.h"
#include "dsp_decim.h"
#include "dsp_delays.h"
#include "dsp_pat.h"
#include "dsp_qrs.h"
#include "synth.h"
#include "test.h"

TEST(timeline)
{
    /* stream sample m represents raw sample 8m + 7 - 47 = 8m - 40 */
    CHECK_EQ(dsp_pat_r_time_us(100, 0), 100u * 2000u - 10000u - DSP_ECG_ANALOG_DELAY_US);
    CHECK_EQ(dsp_pat_r_time_us(100, 128) - dsp_pat_r_time_us(100, 0), 1000u);
    CHECK_EQ(dsp_pat_foot_time_us(7, 0), 7u * 10000u + PPG_T_ON_SAMPLE_US - DSP_PPG_ANALOG_DELAY_US);
    CHECK_EQ(dsp_pat_foot_time_us(7, 128) - dsp_pat_foot_time_us(7, 0), 5000u);
    /* foot at frame 100 (1.00995 s) -> stream index (1.00995 + 0.010) / 2 ms = 510 */
    CHECK_EQ(dsp_pat_foot_ecg_index(100, 0), 510u);
    /* wrap-around safety: differences across 2^32 us */
    dsp_pat_t p;
    dsp_pat_init(&p);
    uint32_t r0 = 2147483000u; /* about 35.8 min into a record */
    uint32_t idx = (r0 + 10000u + DSP_ECG_ANALOG_DELAY_US) / 2000u;
    dsp_pat_add_r(&p, idx, 0);
    uint32_t tr = dsp_pat_r_time_us(idx, 0);
    uint32_t frame = (tr + 250000u - PPG_T_ON_SAMPLE_US) / 10000u;
    dsp_pat_add_foot(&p, frame, 0);
    dsp_pat_add_r(&p, idx + 400, 0);
    uint32_t pat;
    uint32_t ft;
    CHECK(dsp_pat_pop(&p, &pat, &ft));
    CHECK_NEAR((double)pat, (double)(dsp_pat_foot_time_us(frame, 0) - tr), 0.5);
    CHECK_EQ(ft, dsp_pat_foot_time_us(frame, 0));
}

static uint32_t idx_for(double t_s) /* R time -> stream index */
{
    return (uint32_t)lround((t_s * 1e6 + 10000 + DSP_ECG_ANALOG_DELAY_US) / 2000.0);
}

static uint32_t frame_for(double t_s)
{
    return (uint32_t)lround((t_s * 1e6 - PPG_T_ON_SAMPLE_US + DSP_PPG_ANALOG_DELAY_US) / 10000.0);
}

TEST(pairing_rules)
{
    dsp_pat_t p;
    uint32_t v;
    dsp_pat_init(&p);
    /* R at 1.0, 1.8, 2.6 s; feet at 1.25 and 2.05 s, foot reported before its R */
    dsp_pat_add_r(&p, idx_for(1.0), 0);
    dsp_pat_add_foot(&p, frame_for(1.25), 0);
    CHECK(!dsp_pat_pop(&p, &v, NULL)); /* waits until an R-peak after the foot is known */
    dsp_pat_add_r(&p, idx_for(1.8), 0);
    CHECK(dsp_pat_pop(&p, &v, NULL));
    CHECK_NEAR(v, 250000, 6000);
    dsp_pat_add_foot(&p, frame_for(2.05), 0);
    CHECK(!dsp_pat_pop(&p, &v, NULL));
    dsp_pat_add_r(&p, idx_for(2.6), 0);
    CHECK(dsp_pat_pop(&p, &v, NULL));
    CHECK_NEAR(v, 250000, 6000);
    /* implausible: 60 ms and 650 ms */
    dsp_pat_add_foot(&p, frame_for(2.66), 0);
    dsp_pat_add_r(&p, idx_for(3.4), 0);
    CHECK(!dsp_pat_pop(&p, &v, NULL));
    dsp_pat_add_foot(&p, frame_for(4.05), 0);
    dsp_pat_add_r(&p, idx_for(4.2), 0);
    CHECK(!dsp_pat_pop(&p, &v, NULL));
    /* missed R: foot at 5.25 belongs to the (missed) R at 5.0; previous R at
     * 4.2 gives 1.05 s -> out of range; at a fast rate the in-range case is
     * caught by the RR rule */
    dsp_pat_init(&p);
    dsp_pat_add_r(&p, idx_for(10.0), 0);
    dsp_pat_add_r(&p, idx_for(10.3), 0);
    /* R at 10.6 missed; foot at 10.75 would pair with 10.3 (450 ms) */
    dsp_pat_add_foot(&p, frame_for(10.75), 0);
    dsp_pat_add_r(&p, idx_for(10.9), 0); /* interval 10.3 -> 10.9 = 2 x RR */
    CHECK(!dsp_pat_pop(&p, &v, NULL));
    CHECK_EQ(p.rejected, 1); /* counter restarted by dsp_pat_init() */
}

/* Both chains with shared beats and a known PAT. */
static void run_both(double hr, double pat_s, double pi, double dur, uint64_t seed, double *mean_err,
                     double *max_err, int *n_pat, int *n_beats, double *overlap)
{
    synth_beats_cfg_t bc = { .hr_bpm = hr, .rsa = 0.04, .rr_sd = 0.03, .duration = dur + 1, .t_first = 0.5,
                             .seed = seed };
    synth_beats_t b;
    synth_beats_make(&b, &bc);
    synth_ecg_cfg_t ec = { .amp_mv = 0.9, .t_ratio = 0.3, .mains_uv = 150, .mains_hz = 50, .wander_uv = 200,
                           .emg_uv = 10, .white_uv = 3, .gain = 558, .mid = 2045, .analog = 1, .seed = seed + 1 };
    synth_ecg_t e;
    synth_ecg_init(&e, &ec, &b);
    synth_ppg_cfg_t pc = { .pat_s = pat_s, .refl_ua_per_ma = 0.3, .pi = pi, .ambient_ua = 0.1, .flicker = 0.3,
                           .flicker_hz = 100.15, .noise_na = 0.1, .resp = 0.002, .seed = seed + 2 };
    synth_ppg_t src;
    synth_ppg_init(&src, &pc, &b);
    synth_ppg_loop_t *l = malloc(sizeof *l);
    synth_ppg_loop_init(l, 1345, 15);
    dsp_decim_t d;
    dsp_decim_init(&d, 2045);
    dsp_qrs_t *q = malloc(sizeof *q);
    dsp_qrs_init(q, 0);
    dsp_pat_t pat;
    dsp_pat_init(&pat);
    /* true PAT of each beat: tangent foot of the composite (overlapping)
     * pulse waveform minus the true R-peak; equals pat_s for isolated pulses */
    double *tf = malloc(b.n * sizeof *tf);
    for (size_t i = 0; i < b.n; i++)
        tf[i] = i ? synth_ppg_true_foot(&src, i) : -1;
    double se = 0, mx = 0, sinj = 0;
    int n = 0;
    /* interleave in 10 ms steps: 40 raw ECG samples, then one PPG frame */
    for (long k = 0; k < (long)(dur * 100); k++) {
        uint16_t raw[40];
        int16_t s[DSP_DECIM_MAX_OUT(40)];
        for (int j = 0; j < 40; j++)
            raw[j] = synth_ecg_next(&e);
        size_t ns = dsp_decim_push(&d, raw, 40, s);
        dsp_qrs_beat_t bt[2];
        size_t nb = dsp_qrs_process(q, s, ns, bt, 2);
        for (size_t j = 0; j < nb; j++)
            dsp_pat_add_r(&pat, bt[j].index, bt[j].frac_q8);
        dsp_ppg_frame_t f;
        dsp_ppg_out_t o;
        dsp_ppg_pulse_t p;
        if (synth_ppg_loop_frame(l, &src, &f, &o, &p))
            dsp_pat_add_foot(&pat, p.frame, p.frac_q8);
        uint32_t v, ft;
        while (dsp_pat_pop(&pat, &v, &ft)) {
            if (k < 400)
                continue; /* learning */
            size_t bi = 1;
            for (size_t i = 1; i < b.n; i++)
                if (fabs(tf[i] - ft * 1e-6) < fabs(tf[bi] - ft * 1e-6))
                    bi = i;
            double err = (v * 1e-6 - (tf[bi] - b.t[bi])) * 1e3;
            se += err;
            sinj += (tf[bi] - b.t[bi] - pat_s) * 1e3;
            if (fabs(err) > mx)
                mx = fabs(err);
            n++;
        }
    }
    *mean_err = n ? se / n : 0;
    *overlap = n ? sinj / n : 0;
    free(tf);
    *max_err = mx;
    *n_pat = n;
    *n_beats = 0;
    for (size_t i = 0; i < b.n; i++)
        *n_beats += b.t[i] > 4.5 && b.t[i] < dur - 1.0;
    free(q);
    free(l);
    synth_beats_free(&b);
}

TEST(end_to_end)
{
    const struct {
        double hr, pat, pi;
    } cs[] = { { 72, 0.250, 0.02 }, { 100, 0.180, 0.02 }, { 55, 0.320, 0.015 }, { 130, 0.150, 0.03 } };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        double mean, mx, ov;
        int n, nb;
        run_both(cs[i].hr, cs[i].pat, cs[i].pi, 90, 40 + i, &mean, &mx, &n, &nb, &ov);
        printf("  HR %3.0f, injected PAT %3.0f ms (+%.2f ms pulse overlap): %d PATs for %d beats, "
               "error mean %+.2f ms, max %.2f ms\n", cs[i].hr, cs[i].pat * 1e3, ov, n, nb, mean, mx);
        CHECK(n >= nb * 8 / 10);
        CHECK(mx <= 5.0);
    }
}

int main(void)
{
    RUN(timeline);
    RUN(pairing_rules);
    RUN(end_to_end);
    return TEST_RESULT();
}
