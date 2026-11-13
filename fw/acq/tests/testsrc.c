#include "testsrc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

static struct {
    testsrc_cfg_t c;
    synth_beats_t beats;
    bool have;
    synth_ecg_t ecg;
    synth_ppg_t ppg;
} T;

testsrc_cfg_t testsrc_defaults(void)
{
    testsrc_cfg_t c;
    memset(&c, 0, sizeof c);
    c.hr_bpm = 72.0;
    c.rr_sd = 0.03;
    c.rsa = 0.0;
    c.t_first = 0.3;
    c.duration = 600.0;
    c.amp_mv = 1.0;
    c.pat_s = 0.200;
    c.refl_ua_per_ma = 0.15;
    c.pi = 0.02;
    c.seed = 1234;
    return c;
}

static int32_t ecg_fn(uint64_t t_us, void *ctx)
{
    (void)ctx;
    double t = (double)t_us * 1e-6;
    /* advance the first-contributing-beat cursor (time is monotonic) */
    while (T.ecg.bi + 1 < T.beats.n && t - T.beats.t[T.ecg.bi] > 0.8)
        T.ecg.bi++;
    return (int32_t)lrint(synth_ecg_clean_mv(&T.ecg, t) * 1000.0);
}

static uint32_t ppg_fn(uint64_t t_us, void *ctx)
{
    (void)ctx;
    double ua = synth_ppg_signal_ua(&T.ppg, (double)t_us * 1e-6, 1.0);
    return ua > 0 ? (uint32_t)lrint(ua * 1e6) : 0u;
}

void testsrc_install(const testsrc_cfg_t *c)
{
    if (T.have)
        synth_beats_free(&T.beats);
    T.c = *c;
    synth_beats_cfg_t bc;
    memset(&bc, 0, sizeof bc);
    bc.hr_bpm = c->hr_bpm;
    bc.rsa = c->rsa;
    bc.rr_sd = c->rr_sd;
    bc.t_first = c->t_first;
    bc.duration = c->duration;
    bc.seed = c->seed;
    synth_beats_make(&T.beats, &bc);
    T.have = true;

    synth_ecg_cfg_t ec;
    memset(&ec, 0, sizeof ec);
    ec.amp_mv = c->amp_mv;
    ec.t_ratio = 0.3;
    ec.gain = 558;
    ec.mid = 2048;
    ec.seed = c->seed + 1;
    synth_ecg_init(&T.ecg, &ec, &T.beats);

    synth_ppg_cfg_t pc;
    memset(&pc, 0, sizeof pc);
    pc.pat_s = c->pat_s;
    pc.refl_ua_per_ma = c->refl_ua_per_ma;
    pc.pi = c->pi;
    pc.seed = c->seed + 2;
    synth_ppg_init(&T.ppg, &pc, &T.beats);

    sim_set_ecg_source(ecg_fn, NULL);
    sim_set_ppg_source(ppg_fn, NULL);
}

const synth_beats_t *testsrc_beats(void) { return &T.beats; }
const synth_ppg_t *testsrc_ppg(void) { return &T.ppg; }

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

testsrc_truth_t testsrc_truth(double t0, double t1)
{
    testsrc_truth_t r;
    memset(&r, 0, sizeof r);
    double sum = 0, sum2 = 0, sd2 = 0, prev = 0;
    int n = 0, nsd = 0;
    double rr5[5];
    int n5 = 0;
    r.hr_min5 = 1e9;
    r.hr_max5 = 0;
    for (size_t i = 1; i < T.beats.n; i++) {
        if (T.beats.t[i - 1] < t0 || T.beats.t[i] > t1)
            continue;
        double rr = (T.beats.t[i] - T.beats.t[i - 1]) * 1000.0;
        sum += rr;
        sum2 += rr * rr;
        if (n > 0) {
            sd2 += (rr - prev) * (rr - prev);
            nsd++;
        }
        prev = rr;
        n++;
        rr5[n5 % 5] = rr;
        n5++;
        if (n5 >= 5) {
            double s[5];
            memcpy(s, rr5, sizeof s);
            qsort(s, 5, sizeof s[0], cmp_d);
            double hr = 60000.0 / s[2];
            if (hr < r.hr_min5) r.hr_min5 = hr;
            if (hr > r.hr_max5) r.hr_max5 = hr;
        }
    }
    r.beats = n + 1;
    if (n > 0) {
        double mean = sum / n;
        r.hr_avg = 60000.0 / mean;
        r.sdnn_ms = n > 1 ? sqrt((sum2 - n * mean * mean) / (n - 1)) : 0;
        r.rmssd_ms = nsd ? sqrt(sd2 / nsd) : 0;
    }
    return r;
}

double testsrc_true_pat_ms(double t0, double t1)
{
    double s = 0;
    int n = 0;
    for (size_t i = 1; i + 1 < T.beats.n; i++) {
        if (T.beats.t[i] < t0 || T.beats.t[i] > t1)
            continue;
        double f = synth_ppg_true_foot(&T.ppg, i);
        if (f < 0)
            continue;
        s += (f - T.beats.t[i]) * 1000.0;
        n++;
    }
    return n ? s / n : 0.0;
}
