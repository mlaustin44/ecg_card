/* PPG: reconstruction, continuity across DAC12 steps, bridging of LED steps,
 * foot timing (intersecting tangent) against the true waveform, pulse rate
 * and perfusion index, all through the closed acquisition loop. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp_ppg.h"
#include "dsp_pat.h"
#include "dsp_ppg_ctrl.h"
#include "synth.h"
#include "test.h"

TEST(reconstruction_formula)
{
    bool cm;
    /* hi_dark railed: fine = hi_on - 11 dac + 10 tia_dark */
    dsp_ppg_frame_t f = { .tia_dark = 310, .hi_dark = 4095, .tia_on = 2000, .hi_on = 2100 };
    int32_t v = dsp_ppg_fine(&f, 2000, &cm);
    CHECK(!cm);
    CHECK_EQ(v, 2100 - 22000 + 3100);
    /* the same light at another DAC12 code gives the same value */
    f.hi_on = (uint16_t)(2100 + 11 * 50);
    CHECK_EQ(dsp_ppg_fine(&f, 2050, &cm), v);
    /* hi_dark in range: exact hi_on - hi_dark */
    f.hi_dark = 3900;
    f.hi_on = 2100;
    CHECK_EQ(dsp_ppg_fine(&f, 2000, &cm), 2100 - 3900);
    /* hi_on railed: coarse fallback in the same units */
    f.hi_on = 0;
    CHECK_EQ(dsp_ppg_fine(&f, 2000, &cm), -10 * (2000 - 310));
    CHECK(cm);
    /* sign: more light -> lower fine (blood volume up) */
    dsp_ppg_frame_t a = { 300, 4095, 1800, 2048 }, b = { 300, 4095, 1810, 1948 };
    CHECK(dsp_ppg_fine(&b, 1800, &cm) < dsp_ppg_fine(&a, 1800, &cm));
}

typedef struct {
    double max_err_ms, mean_err_ms, sd_err_ms;
    int matched, beats, extra, hr_checked, hr_bad, pi_n;
    double pi_mean;
    int dcc_steps, agc_steps;
} pres_t;

/* Run the loop for dur s; forced steps: DAC12 +-step every dac_every s,
 * LED +25 % at led_at s. */
static void run_ppg(const synth_ppg_cfg_t *pc, double hr, double dur, double led_at, double dac_every,
                    uint16_t led0, pres_t *r, double *max_jump_dac)
{
    synth_beats_cfg_t bc = { .hr_bpm = hr, .rsa = 0.04, .rr_sd = 0.03, .duration = dur + 2, .t_first = 0.5,
                             .seed = pc->seed };
    synth_beats_t b;
    synth_beats_make(&b, &bc);
    synth_ppg_t src;
    synth_ppg_init(&src, pc, &b);
    synth_ppg_loop_t *l = malloc(sizeof *l);
    synth_ppg_loop_init(l, led0, 15);
    size_t nf = (size_t)(dur * 100);
    double *feet = malloc(nf * sizeof *feet);
    uint16_t *bpm = malloc(nf * sizeof *bpm);
    uint16_t *pis = malloc(nf * sizeof *pis);
    uint16_t *ibi = malloc(nf * sizeof *ibi);
    long *mt = malloc(nf * sizeof *mt);
    size_t nfeet = 0;
    int16_t prev_fine = 0;
    double jmax = 0;
    for (size_t k = 0; k < nf; k++) {
        double t = k * 0.01;
        if (led_at > 0 && fabs(t - led_at) < 0.005) {
            l->led = (uint16_t)(l->led * 5 / 4);
            l->pending |= DSP_PPG_IN_LED_STEP;
        }
        int forced_dac = 0;
        if (dac_every > 0 && k > 0 && k % (size_t)(dac_every * 100) == 0) {
            l->dac = (uint16_t)(l->dac + ((k / (size_t)(dac_every * 100)) % 2 ? 120 : -120));
            l->pending |= DSP_PPG_IN_DAC_STEP;
            forced_dac = 1;
        }
        dsp_ppg_frame_t f;
        dsp_ppg_out_t o;
        dsp_ppg_pulse_t p;
        if (synth_ppg_loop_frame(l, &src, &f, &o, &p)) {
            feet[nfeet] = (double)dsp_pat_foot_time_us(p.frame, p.frac_q8);
            bpm[nfeet] = p.pulse_bpm;
            pis[nfeet] = p.pi_permille;
            ibi[nfeet] = p.ibi_ms;
            mt[nfeet] = -1;
            nfeet++;
        }
        if (forced_dac && k > 0) {
            double j = fabs((double)(int16_t)(o.fine - prev_fine));
            if (j > jmax)
                jmax = j;
        }
        prev_fine = o.fine;
    }
    if (max_jump_dac)
        *max_jump_dac = jmax;
    memset(r, 0, sizeof *r);
    double se = 0, se2 = 0;
    char *used = calloc(nfeet + 1, 1);
    for (size_t i = 1; i < b.n; i++) {
        double tf = synth_ppg_true_foot(&src, i);
        if (tf < 3.0 || tf > dur - 1.0)
            continue;
        r->beats++;
        size_t best = (size_t)-1;
        double be = 1e9;
        for (size_t j = 0; j < nfeet; j++) {
            double e = fabs(feet[j] * 1e-6 - tf);
            if (e < be) {
                be = e;
                best = j;
            }
        }
        if (be < 0.05 && !used[best]) {
            used[best] = 1;
            r->matched++;
            double e = (feet[best] * 1e-6 - tf) * 1e3;
            se += e;
            se2 += e * e;
            if (fabs(e) > r->max_err_ms)
                r->max_err_ms = fabs(e);
            mt[best] = (long)i;
            if (pis[best]) {
                r->pi_mean += pis[best];
                r->pi_n++;
            }
        }
    }
    for (size_t j = 0; j < nfeet; j++)
        if (!used[j] && feet[j] * 1e-6 > 3.0 && feet[j] * 1e-6 < dur - 1.0)
            r->extra++;
    /* pulse rate: compare where the last 5 intervals came from 6 consecutive
     * true beats, all detected (the detector's own median then covers the
     * same intervals) */
    for (size_t j = 5; j < nfeet; j++) {
        if (!bpm[j] || mt[j] < 6)
            continue;
        int ok = 1;
        for (size_t q = 0; q < 5; q++)
            ok &= ibi[j - q] != 0 && mt[j - q] == mt[j] - (long)q && mt[j - q - 1] == mt[j] - (long)q - 1;
        if (!ok)
            continue;
        double iv[5];
        for (int q = 0; q < 5; q++)
            iv[q] = synth_ppg_true_foot(&src, (size_t)mt[j] - (size_t)q) -
                    synth_ppg_true_foot(&src, (size_t)mt[j] - (size_t)q - 1);
        for (int x = 1; x < 5; x++)
            for (int y = x; y > 0 && iv[y - 1] > iv[y]; y--) {
                double tmp = iv[y];
                iv[y] = iv[y - 1];
                iv[y - 1] = tmp;
            }
        int th = (int)lround(60.0 / iv[2]);
        r->hr_checked++;
        r->hr_bad += abs(th - bpm[j]) > 1;
    }
    r->mean_err_ms = r->matched ? se / r->matched : 0;
    r->sd_err_ms = r->matched > 1 ? sqrt(se2 / r->matched - r->mean_err_ms * r->mean_err_ms) : 0;
    r->pi_mean = r->pi_n ? r->pi_mean / r->pi_n : 0;
    r->dcc_steps = l->dcc_steps;
    r->agc_steps = l->agc_steps;
    free(used);
    free(ibi);
    free(mt);
    free(feet);
    free(bpm);
    free(pis);
    free(l);
    synth_beats_free(&b);
}

static const synth_ppg_cfg_t base_cfg = { .pat_s = 0.25, .refl_ua_per_ma = 0.30, .pi = 0.02, .ambient_ua = 0.10,
                                          .flicker = 0.3, .flicker_hz = 100.15, .noise_na = 0.15, .resp = 0.002,
                                          .seed = 1 };

TEST(dac_step_continuity)
{
    /* constant light: the stream fine value must not jump at forced DAC12 steps */
    synth_ppg_cfg_t c = base_cfg;
    c.pi = 0;
    c.flicker = 0;
    c.resp = 0;
    pres_t r;
    double jmax;
    run_ppg(&c, 70, 30, -1, 2.0, 1345, &r, &jmax);
    printf("  constant light, DAC12 steps of +-120 codes: max fine jump at a step %.0f codes (81 pA)\n", jmax);
    CHECK(jmax <= 20); /* vs 11 x 120 = 1320 without the formula */
}

TEST(foot_timing)
{
    /* noise_na: photocurrent-referred noise per sample. Estimate for this
     * front end: shot noise of 1.5 uA over 16 kHz 0.09 nA, TIA thermal
     * 0.02 nA, PPG_HI ADC noise 0.04 nA => ~0.1 nA (spec cases); 0.15, 0.3
     * and 0.6 nA are stress cases. The x10 tia_dark term adds ~0.25 nA
     * equivalent before the 5-frame mean. */
    const struct {
        const char *name;
        double hr, pi, noise, led_at, dac_every;
        uint16_t led0;
        int stress;
    } cs[] = {
        { "72 bpm, PI 2 %", 72, 0.02, 0.1, -1, -1, 1345, 0 },
        { "60 bpm, PI 1 %, DAC steps", 60, 0.01, 0.1, -1, 7.0, 1345, 0 },
        { "95 bpm, LED step at 20 s", 95, 0.02, 0.1, 20.0, -1, 1345, 0 },
        { "120 bpm, AGC from 0.5 mA", 120, 0.03, 0.1, -1, 9.0, 135, 0 },
        { "45 bpm, PI 1.5 %", 45, 0.015, 0.1, 33.0, 13.0, 1345, 0 },
        { "stress: PI 1 %, 0.15 nA", 60, 0.01, 0.15, -1, 7.0, 1345, 1 },
        { "stress: PI 1 %, 0.3 nA", 60, 0.01, 0.3, -1, 7.0, 1345, 1 },
        { "stress: PI 0.5 %, 0.6 nA", 50, 0.005, 0.6, 31.0, 11.0, 1345, 2 },
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        synth_ppg_cfg_t c = base_cfg;
        c.pi = cs[i].pi;
        c.noise_na = cs[i].noise;
        c.seed = 10 + i;
        pres_t r;
        run_ppg(&c, cs[i].hr, 90, cs[i].led_at, cs[i].dac_every, cs[i].led0, &r, NULL);
        printf("  %-27s feet %3d/%3d extra %d err max %5.2f mean %+.2f sd %.2f ms; rate >1 bpm off %d/%d; "
               "PI %.1f (true %.1f); DCC %d AGC %d\n",
               cs[i].name, r.matched, r.beats, r.extra, r.max_err_ms, r.mean_err_ms, r.sd_err_ms, r.hr_bad, r.hr_checked,
               r.pi_mean, c.pi * 1000, r.dcc_steps, r.agc_steps);
        /* pulses next to forced or control steps are dropped on purpose */
        int forced = (cs[i].dac_every > 0 ? (int)(90 / cs[i].dac_every) : 0) + (cs[i].led_at > 0);
        if (cs[i].stress == 0) {
            CHECK(r.matched >= r.beats - 2 * forced - r.agc_steps - 1);
            CHECK(r.extra == 0);
            CHECK(r.max_err_ms <= 5.0);
            CHECK(r.hr_checked >= 20);
            CHECK(r.hr_bad == 0);
            CHECK(fabs(r.pi_mean - c.pi * 1000) <= 0.15 * c.pi * 1000 + 0.5);
        } else if (cs[i].stress == 1) {
            CHECK(r.extra == 0);
            CHECK(r.max_err_ms <= 10.0);
        } else {
            CHECK(r.matched >= r.beats * 3 / 4);
            CHECK(r.extra <= 3);
        }
    }
}

TEST(led_step_bridged)
{
    /* the display trace must not see a light step that keeps PPG_HI in range
     * (a 25 % step rails PPG_HI: that is caught as a source switch anyway) */
    synth_ppg_cfg_t c = base_cfg;
    c.pi = 0;
    c.flicker = 0;
    c.resp = 0;
    c.noise_na = 0.1;
    synth_beats_cfg_t bc = { .hr_bpm = 60, .duration = 30, .t_first = 0.5, .seed = 3 };
    synth_beats_t b;
    synth_beats_make(&b, &bc);
    synth_ppg_t src;
    synth_ppg_init(&src, &c, &b);
    synth_ppg_loop_t *l = malloc(sizeof *l);
    synth_ppg_loop_init(l, 1345, 15);
    l->agc_every = 0;
    int mx = 0, mx_unbridged = 0;
    for (int k = 0; k < 2000; k++) {
        if (k == 1500) {
            l->led = (uint16_t)(l->led * 21 / 20);
            l->pending |= DSP_PPG_IN_LED_STEP;
        }
        dsp_ppg_frame_t f;
        dsp_ppg_out_t o;
        dsp_ppg_pulse_t p;
        synth_ppg_loop_frame(l, &src, &f, &o, &p);
        if (k > 1400 && abs(o.disp) > mx)
            mx = abs(o.disp);
    }
    /* same without the flag: the step goes through */
    synth_ppg_init(&src, &c, &b);
    synth_ppg_loop_init(l, 1345, 15);
    l->agc_every = 0;
    for (int k = 0; k < 2000; k++) {
        if (k == 1500)
            l->led = (uint16_t)(l->led * 21 / 20);
        dsp_ppg_frame_t f;
        dsp_ppg_out_t o;
        dsp_ppg_pulse_t p;
        synth_ppg_loop_frame(l, &src, &f, &o, &p);
        if (k > 1400 && abs(o.disp) > mx_unbridged)
            mx_unbridged = abs(o.disp);
    }
    printf("  LED +5 %% step: display excursion %d bridged vs %d unflagged (fine units)\n", mx, mx_unbridged);
    CHECK(mx < 40);
    CHECK(mx_unbridged > 500);
    free(l);
    synth_beats_free(&b);
}

TEST(disturbed_counts_and_avg)
{
    dsp_ppg_t p;
    dsp_ppg_init(&p);
    dsp_ppg_frame_t f = { 300, 4095, 1900, 2000 };
    dsp_ppg_out_t o;
    dsp_ppg_pulse_t pl;
    for (int i = 0; i < 10; i++)
        dsp_ppg_process(&p, &f, 1900, i == 4 ? DSP_PPG_IN_DAC_STEP : 0, &o, &pl);
    dsp_ppg_avg_t a;
    CHECK_EQ(dsp_ppg_take_avg(&p, &a), 9);
    CHECK_EQ(a.tia_on, 1900);
    CHECK_EQ(a.hi_on, 2000);
    CHECK_EQ(a.tia_dark, 300);
    CHECK_EQ(dsp_ppg_take_avg(&p, &a), 0);
    CHECK_EQ(o.coarse, 1600);
    CHECK(sizeof(dsp_ppg_t) <= 640);
}

/* ---- recovery after a gap without pulses ---------------------------------
 * Plain synthetic pleth (72 bpm, 15 % rise then exponential decay) at a fixed
 * DAC12 code; amp = pulse amplitude in hi_on codes, noise = uniform +-noise/2
 * codes on hi_on and tia_on (deterministic LCG). Returns pulses reported and
 * the time of the first one. */
typedef struct {
    dsp_ppg_t p;
    long k;          /* frames since start (pulse phase reference) */
    uint32_t rng;
} gap_t;

static int gap_run(gap_t *g, double secs, int amp, int noise, double *t_first)
{
    int pulses = 0;
    *t_first = -1;
    for (long i = 0; i < (long)(secs * 100); i++, g->k++) {
        double ph = fmod((double)g->k / 100.0 * 1.2, 1.0);
        double pu = ph < 0.15 ? ph / 0.15 : exp(-(ph - 0.15) * 4.0);
        int n = 0;
        if (noise) {
            g->rng = g->rng * 1103515245u + 12345u;
            n = (int)((g->rng >> 16) % (unsigned)(noise + 1)) - noise / 2;
        }
        int a = (int)(amp * pu);
        dsp_ppg_frame_t f = { 500, 4095, (uint16_t)(2000 + a / 10 + n), (uint16_t)(2048 - a + n) };
        dsp_ppg_out_t o;
        dsp_ppg_pulse_t pl;
        if (dsp_ppg_process(&g->p, &f, 1000, 0, &o, &pl)) {
            if (pulses == 0)
                *t_first = (double)i / 100.0;
            pulses++;
        }
    }
    return pulses;
}

/* Review finding: after ~10 s without an accepted pulse the threshold
 * decayed to 0 and "thr > 0" blocked detection forever. 30 s of pulses, a
 * gap (flat or noisy) of 5..40 s, then 60 s of pulses (also at a quarter of
 * the amplitude): detection must resume within a few beats every time. */
TEST(resumes_after_gap)
{
    static const double gaps[] = { 5, 10, 20, 40 };
    for (int noisy = 0; noisy < 2; noisy++) {
        for (size_t gi = 0; gi < sizeof gaps / sizeof gaps[0]; gi++) {
            for (int weak = 0; weak < 2; weak++) {
                gap_t *g = calloc(1, sizeof *g);
                g->rng = 12345u + (uint32_t)gi;
                dsp_ppg_init(&g->p);
                double t0, t1, t2;
                int noise = noisy ? 6 : 0;
                int a = gap_run(g, 30, 150, noise, &t0);
                int b = gap_run(g, gaps[gi], 0, noise, &t1);
                int c = gap_run(g, 60, weak ? 40 : 150, noise, &t2);
                printf("  gap %2.0f s %s %s: before %d, in gap %d, after %d (first after %.2f s)\n",
                       gaps[gi], noisy ? "noisy" : "flat ", weak ? "weak  " : "strong", a, b, c, t2);
                CHECK(a >= 30);
                /* 60 s at 72 bpm = 72 beats; allow the re-learning window */
                CHECK(c >= 66 && c <= 78);
                CHECK(t2 >= 0 && t2 < 4.0); /* within ~4 beats */
                free(g);
            }
        }
    }
}

/* A learning window that only saw a flat signal (no finger yet / no
 * perfusion) used to leave thr = 0 for good. */
TEST(flat_then_pulses)
{
    for (int noisy = 0; noisy < 2; noisy++) {
        for (int flat_s = 1; flat_s <= 13; flat_s += 3) {
            gap_t *g = calloc(1, sizeof *g);
            g->rng = 777u;
            dsp_ppg_init(&g->p);
            double t0, t1;
            gap_run(g, flat_s, 0, noisy ? 6 : 0, &t0);
            int c = gap_run(g, 30, 150, noisy ? 6 : 0, &t1);
            CHECK(c >= 30);
            CHECK(t1 >= 0 && t1 < 4.5);
            /* the pulse rate is available again after 6 feet */
            free(g);
        }
    }
}

int main(void)
{
    RUN(reconstruction_formula);
    RUN(dac_step_continuity);
    RUN(foot_timing);
    RUN(led_step_bridged);
    RUN(disturbed_counts_and_avg);
    RUN(resumes_after_gap);
    RUN(flat_then_pulses);
    return TEST_RESULT();
}
