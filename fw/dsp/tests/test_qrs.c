/* QRS detection through the real chain: synthetic ECG -> 4 kHz ADC codes
 * (with the analog front end simulated) -> decimator -> Pan-Tompkins.
 * Scores sensitivity / positive predictivity, R-peak timing and heart rate
 * against the generator's true beats; HRV against the true RR series;
 * lead-off / re-contact behaviour. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "dsp_decim.h"
#include "dsp_ecg_mon.h"
#include "dsp_hrv.h"
#include "dsp_pat.h"
#include "dsp_qrs.h"
#include "synth.h"
#include "test.h"

typedef struct {
    const char *name;
    double hr, rsa, rr_sd, amp, t_ratio;
    double mains_uv, mains_hz, wander_uv, emg_uv, white_uv;
    int gain;
    double dur;
    int noisy;
    double ectopic_every;
} qcase_t;

typedef struct {
    uint32_t idx;
    uint8_t frac;
    uint16_t hr, rr_ms, amp;
    uint8_t flags;
    double t;     /* s, on the shared timeline */
    uint32_t at;  /* stream index when reported */
} det_t;

typedef struct {
    det_t *d;
    size_t n, cap;
} dets_t;

static void push_det(dets_t *v, const dsp_qrs_beat_t *b, uint32_t at)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? 2 * v->cap : 256;
        v->d = realloc(v->d, v->cap * sizeof *v->d);
    }
    det_t *x = &v->d[v->n++];
    x->idx = b->index;
    x->frac = b->frac_q8;
    x->hr = b->hr_bpm;
    x->rr_ms = b->rr_ms;
    x->amp = b->amp;
    x->flags = b->flags;
    x->t = dsp_pat_r_time_us(b->index, b->frac_q8) * 1e-6;
    x->at = at;
}

/* Run synthetic ECG [t0, t1) through decimator + detector. lead_off_from/to:
 * optional interval with the input on the rail (app resets on re-contact). */
static void run_chain(synth_ecg_t *e, double dur, int gain, dets_t *out, double off_from,
                      double off_to, int *events)
{
    dsp_decim_t d;
    dsp_qrs_t *q = malloc(sizeof *q);
    dsp_lead_t l;
    dsp_decim_init(&d, (uint16_t)e->c.mid);
    dsp_qrs_init(q, 0);
    dsp_lead_init(&l, (uint16_t)e->c.mid);
    (void)gain;
    long n = (long)(dur * 4000);
    uint32_t sidx = 0;
    int reset_pending = 0;
    for (long i = 0; i < n; i += 80) {
        uint16_t raw[80];
        int16_t s[DSP_DECIM_MAX_OUT(80)];
        for (int j = 0; j < 80; j++) {
            raw[j] = synth_ecg_next(e);
            double t = (i + j) / 4000.0;
            if (t >= off_from && t < off_to)
                raw[j] = 4095;
        }
        uint8_t ev = dsp_lead_push(&l, raw, 80);
        if (events)
            *events |= ev;
        if (ev & DSP_LEAD_EV_CONTACT)
            reset_pending = 1;
        size_t k = dsp_decim_push(&d, raw, 80, s);
        if (reset_pending && i > 4000) {
            dsp_qrs_reset(q); /* the app does this on re-contact */
            reset_pending = 0;
        }
        if (dsp_lead_is_off(&l) && i > 4000) {
            sidx += (uint32_t)k;
            dsp_qrs_skip(q, (uint32_t)k); /* keep the stream timeline */
            continue;
        }
        dsp_qrs_beat_t b[4];
        size_t nb = dsp_qrs_process(q, s, k, b, 4);
        sidx += (uint32_t)k;
        for (size_t j = 0; j < nb; j++)
            push_det(out, &b[j], sidx);
    }
    free(q);
}

typedef struct {
    int tp, fn, fp;
    double max_err_ms, mean_err_ms, max_lat_ms;
    int hr_checked, hr_bad;
    int max_hr_err;
} score_t;

static int true_hr(const synth_beats_t *b, size_t i)
{
    if (i < 5)
        return 0;
    double rr[5];
    for (int k = 0; k < 5; k++)
        rr[k] = b->t[i - k] - b->t[i - k - 1];
    for (int a = 1; a < 5; a++)
        for (int c = a; c > 0 && rr[c - 1] > rr[c]; c--) {
            double tmp = rr[c];
            rr[c] = rr[c - 1];
            rr[c - 1] = tmp;
        }
    return (int)lround(60.0 / rr[2]);
}

static void score(const synth_beats_t *b, const dets_t *v, double t_lo, double t_hi, score_t *s)
{
    memset(s, 0, sizeof *s);
    char *used = calloc(v->n ? v->n : 1, 1);
    double sum_err = 0;
    for (size_t i = 0; i < b->n; i++) {
        double tr = b->t[i];
        if (tr < t_lo || tr > t_hi)
            continue;
        size_t best = (size_t)-1;
        double be = 1e9;
        for (size_t j = 0; j < v->n; j++) {
            double e = fabs(v->d[j].t - tr);
            if (e < be && !used[j]) {
                be = e;
                best = j;
            }
        }
        if (best != (size_t)-1 && be <= 0.075) {
            used[best] = 1;
            s->tp++;
            double err = (v->d[best].t - tr) * 1e3;
            sum_err += err;
            if (fabs(err) > s->max_err_ms)
                s->max_err_ms = fabs(err);
            double lat = (v->d[best].at * 2e-3 - 0.010) - tr;
            if (!(v->d[best].flags & DSP_QRS_F_SEARCHBACK) && lat * 1e3 > s->max_lat_ms)
                s->max_lat_ms = lat * 1e3;
            /* HR: compare when the last 6 true beats were all detected */
            int th = true_hr(b, i);
            if (th && v->d[best].hr && best >= 5) {
                int ok = 1;
                for (int k = 1; k <= 5; k++)
                    ok &= fabs(v->d[best - k].t - b->t[i - k]) < 0.075;
                if (ok) {
                    int err_hr = abs((int)v->d[best].hr - th);
                    s->hr_checked++;
                    s->hr_bad += err_hr > 1;
                    if (err_hr > s->max_hr_err)
                        s->max_hr_err = err_hr;
                }
            }
        } else {
            s->fn++;
        }
    }
    for (size_t j = 0; j < v->n; j++)
        if (!used[j] && v->d[j].t >= t_lo && v->d[j].t <= t_hi)
            s->fp++;
    s->mean_err_ms = s->tp ? sum_err / s->tp : 0;
    free(used);
}

static const qcase_t cases[] = {
    /* name             hr   rsa   rrsd  amp  T     mains  hz  wander emg  white gain  dur noisy */
    { "clean 40 bpm",    40, 0.03, 0.02, 1.0, 0.3,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "clean 60 bpm",    60, 0.04, 0.02, 1.0, 0.3,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "clean 100 bpm",  100, 0.03, 0.02, 1.0, 0.3,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "clean 150 bpm",  150, 0.02, 0.01, 1.0, 0.3,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "clean 200 bpm",  200, 0.01, 0.01, 1.0, 0.25,  0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "clean 0.2 mV",    70, 0.03, 0.02, 0.2, 0.3,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "clean 2 mV",      70, 0.03, 0.02, 2.0, 0.3,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "clean 1098x",     80, 0.03, 0.02, 0.6, 0.3,   0,   50,   0,    0,   2, 1098, 120, 0, 0 },
    { "tall T 70 bpm",   70, 0.03, 0.02, 0.8, 1.0,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "tall T 120 bpm", 120, 0.02, 0.02, 0.8, 0.9,   0,   50,   0,    0,   3, 558, 120, 0, 0 },
    { "noisy 50 Hz",     70, 0.04, 0.03, 1.0, 0.3, 500,   50, 400,   30,   5, 558, 120, 1, 0 },
    { "noisy 60 Hz",    110, 0.03, 0.03, 0.8, 0.3, 500,   60, 400,   30,   5, 558, 120, 1, 0 },
    { "noisy 0.3 mV",    65, 0.04, 0.03, 0.3, 0.3, 200,   50, 300,   20,   5, 558, 120, 1, 0 },
    { "noisy 1098x 180", 180, 0.01, 0.01, 0.5, 0.25, 300,  60, 200,   25,   3, 1098, 120, 1, 0 },
    { "noisy tall T",    75, 0.03, 0.02, 0.7, 1.0, 400,   50, 300,   30,   5, 558, 120, 1, 0 },
    { "ectopics",        70, 0.03, 0.02, 1.0, 0.3,   0,   50,   0,    0,   3, 558, 120, 0, 9 },
    { "heavy EMG",       80, 0.03, 0.03, 0.6, 0.3, 300,   50, 300,   60,   5, 558, 120, 1, 0 },
    { "everything",      95, 0.04, 0.04, 0.5, 0.6, 900,   60, 800,   45,   8, 558, 120, 1, 0 },
};

TEST(detection)
{
    int all_tp = 0, all_fn = 0, all_fp = 0;
    double worst_err = 0, worst_lat = 0;
    printf("  %-16s %5s %4s %4s %7s %7s %7s %7s %6s %5s\n", "case", "beats", "FN", "FP", "Se%",
           "PPV%", "max|e|", "mean e", "lat", "HRerr");
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const qcase_t *k = &cases[c];
        synth_beats_cfg_t bc = { .hr_bpm = k->hr, .rsa = k->rsa, .rr_sd = k->rr_sd, .duration = k->dur,
                                 .t_first = 0.4, .seed = 100 + c, .ectopic_every = k->ectopic_every };
        synth_beats_t b;
        synth_beats_make(&b, &bc);
        synth_ecg_cfg_t ec = { .amp_mv = k->amp, .t_ratio = k->t_ratio, .mains_uv = k->mains_uv,
                               .mains_hz = k->mains_hz, .wander_uv = k->wander_uv, .emg_uv = k->emg_uv,
                               .white_uv = k->white_uv, .gain = k->gain, .mid = 2043, .analog = 1,
                               .seed = 200 + c };
        synth_ecg_t e;
        synth_ecg_init(&e, &ec, &b);
        dets_t v = { 0 };
        run_chain(&e, k->dur, k->gain, &v, -1, -1, NULL);
        score_t s;
        score(&b, &v, 3.0, k->dur - 1.0, &s);
        double se = 100.0 * s.tp / (s.tp + s.fn), ppv = 100.0 * s.tp / (s.tp + s.fp ? s.tp + s.fp : 1);
        printf("  %-16s %5d %4d %4d %7.2f %7.2f %7.2f %7.2f %6.0f %2d/%d\n", k->name, s.tp + s.fn, s.fn,
               s.fp, se, ppv, s.max_err_ms, s.mean_err_ms, s.max_lat_ms, s.max_hr_err, s.hr_bad);
        double lim = k->noisy ? 95.0 : 99.0;
        CHECK(se >= lim);
        CHECK(ppv >= lim);
        CHECK(s.max_err_ms <= 6.0);
        CHECK(s.max_lat_ms <= DSP_QRS_LATENCY_MAX * 2.0);
        if (!k->ectopic_every) {
            CHECK(s.hr_checked > 20);
            CHECK(s.max_hr_err <= 1);
        }
        all_tp += s.tp;
        all_fn += s.fn;
        all_fp += s.fp;
        if (s.max_err_ms > worst_err)
            worst_err = s.max_err_ms;
        if (s.max_lat_ms > worst_lat)
            worst_lat = s.max_lat_ms;
        free(v.d);
        synth_beats_free(&b);
    }
    printf("  total: %d beats, %d FN, %d FP; worst |timing error| %.2f ms; worst report latency %.0f ms\n",
           all_tp + all_fn, all_fn, all_fp, worst_err, worst_lat);
}

TEST(hrv_against_truth)
{
    synth_beats_cfg_t bc = { .hr_bpm = 68, .rsa = 0.06, .rr_sd = 0.03, .duration = 300, .t_first = 0.4, .seed = 77 };
    synth_beats_t b;
    synth_beats_make(&b, &bc);
    synth_ecg_cfg_t ec = { .amp_mv = 0.8, .t_ratio = 0.3, .mains_uv = 100, .mains_hz = 50, .wander_uv = 150,
                           .emg_uv = 10, .white_uv = 3, .gain = 558, .mid = 2047, .analog = 1, .seed = 78 };
    synth_ecg_t e;
    synth_ecg_init(&e, &ec, &b);
    dets_t v = { 0 };
    run_chain(&e, bc.duration, 558, &v, -1, -1, NULL);
    dsp_hrv_t h;
    dsp_hrv_init(&h);
    size_t first = 0;
    while (first < v.n && v.d[first].t < 3.0)
        first++;
    dsp_hrv_add_beat(&h, 0);
    for (size_t i = first + 1; i < v.n; i++)
        dsp_hrv_add_beat(&h, v.d[i].rr_ms);
    dsp_hrv_summary_t sm;
    dsp_hrv_summary(&h, &sm);
    /* truth over the same span */
    double t0 = v.d[first].t - 0.05, t1 = v.d[v.n - 1].t + 0.05;
    double s1 = 0, s2 = 0, sd2 = 0, prev = 0;
    int n = 0, nsd = 0;
    for (size_t i = 1; i < b.n; i++) {
        if (b.t[i - 1] < t0 || b.t[i] > t1)
            continue;
        double rr = (b.t[i] - b.t[i - 1]) * 1e3;
        s1 += rr;
        s2 += rr * rr;
        if (prev > 0) {
            sd2 += (rr - prev) * (rr - prev);
            nsd++;
        }
        prev = rr;
        n++;
    }
    double mean = s1 / n, sdnn = sqrt((s2 - n * mean * mean) / (n - 1)), rmssd = sqrt(sd2 / nsd);
    printf("  true: n %d SDNN %.2f RMSSD %.2f HR %.1f | measured: beats %u NN %u SDNN %u RMSSD %u HR %u (%u..%u)\n",
           n, sdnn, rmssd, 60000 / mean, sm.beats, sm.nn_count, sm.sdnn_ms, sm.rmssd_ms, sm.hr_avg,
           sm.hr_min, sm.hr_max);
    CHECK(abs((int)sm.nn_count - n) <= 2);
    CHECK_NEAR(sm.sdnn_ms, sdnn, 1.0 + 0.03 * sdnn);
    CHECK_NEAR(sm.rmssd_ms, rmssd, 1.0 + 0.05 * rmssd);
    CHECK_NEAR(sm.hr_avg, 60000 / mean, 1.0);
    free(v.d);
    synth_beats_free(&b);
}

TEST(lead_off_recontact)
{
    synth_beats_cfg_t bc = { .hr_bpm = 75, .rsa = 0.02, .rr_sd = 0.02, .duration = 60, .t_first = 0.4, .seed = 5 };
    synth_beats_t b;
    synth_beats_make(&b, &bc);
    synth_ecg_cfg_t ec = { .amp_mv = 1.0, .t_ratio = 0.3, .mains_uv = 50, .mains_hz = 50, .wander_uv = 100,
                           .white_uv = 3, .gain = 558, .mid = 2047, .analog = 1, .seed = 6 };
    synth_ecg_t e;
    synth_ecg_init(&e, &ec, &b);
    dets_t v = { 0 };
    int ev = 0;
    run_chain(&e, bc.duration, 558, &v, 25.0, 28.0, &ev);
    CHECK(ev & DSP_LEAD_EV_OFF);
    CHECK(ev & DSP_LEAD_EV_CONTACT);
    /* no detections while off; detection resumes within ~3 s of contact */
    int during = 0;
    double first_after = 1e9;
    for (size_t i = 0; i < v.n; i++) {
        if (v.d[i].t > 25.05 && v.d[i].t < 28.0)
            during++;
        if (v.d[i].t >= 28.0 && v.d[i].t < first_after)
            first_after = v.d[i].t;
    }
    score_t s;
    score(&b, &v, 32.0, 59.0, &s);
    printf("  lead-off 25-28 s: %d detections while off, first beat %.2f s after contact; "
           "after: Se %d/%d FP %d\n", during, first_after - 28.0, s.tp, s.tp + s.fn, s.fp);
    CHECK_EQ(during, 0);
    CHECK(first_after - 28.0 < 3.5);
    CHECK_EQ(s.fn, 0);
    CHECK_EQ(s.fp, 0);
    free(v.d);
    synth_beats_free(&b);
}

TEST(searchback_low_beat)
{
    /* every 7th beat at 40 % amplitude: recovered by THR2 / search-back */
    synth_beats_cfg_t bc = { .hr_bpm = 72, .rsa = 0.0, .rr_sd = 0.01, .duration = 90, .t_first = 0.4, .seed = 9 };
    synth_beats_t b;
    synth_beats_make(&b, &bc);
    synth_ecg_cfg_t ec = { .amp_mv = 1.0, .t_ratio = 0.3, .white_uv = 3, .gain = 558, .mid = 2047, .analog = 1,
                           .seed = 10 };
    /* emulate weak beats by scaling: build two generators and mix per beat */
    synth_ecg_t e;
    synth_ecg_init(&e, &ec, &b);
    dsp_decim_t d;
    dsp_qrs_t *q = malloc(sizeof *q);
    dsp_decim_init(&d, 2047);
    dsp_qrs_init(q, 0);
    dets_t v = { 0 };
    uint32_t sidx = 0;
    int sb = 0;
    for (long i = 0; i < 90L * 4000; i += 80) {
        uint16_t raw[80];
        int16_t s[DSP_DECIM_MAX_OUT(80)];
        for (int j = 0; j < 80; j++) {
            double t = (i + j) / 4000.0;
            /* nearest beat index */
            size_t bi = 0;
            while (bi + 1 < b.n && b.t[bi + 1] < t + 0.4)
                bi++;
            double scale = (bi % 7 == 6) ? 0.4 : 1.0;
            uint16_t c = synth_ecg_next(&e);
            raw[j] = (uint16_t)lround(2047 + (c - 2047.0) * scale);
        }
        size_t k = dsp_decim_push(&d, raw, 80, s);
        dsp_qrs_beat_t bb[4];
        size_t nb = dsp_qrs_process(q, s, k, bb, 4);
        sidx += (uint32_t)k;
        for (size_t j = 0; j < nb; j++) {
            push_det(&v, &bb[j], sidx);
            sb += (bb[j].flags & DSP_QRS_F_SEARCHBACK) != 0;
        }
    }
    score_t sc;
    score(&b, &v, 3.0, 89.0, &sc);
    printf("  weak beats: Se %d/%d, FP %d, %d via search-back, max err %.2f ms\n", sc.tp, sc.tp + sc.fn,
           sc.fp, sb, sc.max_err_ms);
    CHECK(sc.fn <= 1);
    CHECK_EQ(sc.fp, 0);
    free(q);
    free(v.d);
    synth_beats_free(&b);
}

int main(void)
{
    RUN(detection);
    RUN(hrv_against_truth);
    RUN(lead_off_recontact);
    RUN(searchback_low_beat);
    return TEST_RESULT();
}
