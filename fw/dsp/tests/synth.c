#include "synth.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- random ----------------------------------------------------------- */
void synth_rng_seed(synth_rng_t *r, uint64_t seed)
{
    r->s = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    r->have = 0;
}

static uint64_t next_u64(synth_rng_t *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

double synth_uniform(synth_rng_t *r)
{
    return (double)(next_u64(r) >> 11) * (1.0 / 9007199254740992.0);
}

double synth_gauss(synth_rng_t *r)
{
    if (r->have) {
        r->have = 0;
        return r->spare;
    }
    double u, v, s;
    do {
        u = 2.0 * synth_uniform(r) - 1.0;
        v = 2.0 * synth_uniform(r) - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    double m = sqrt(-2.0 * log(s) / s);
    r->spare = v * m;
    r->have = 1;
    return u * m;
}

/* ---- beats -------------------------------------------------------------- */
void synth_beats_make(synth_beats_t *b, const synth_beats_cfg_t *c)
{
    synth_rng_t r;
    synth_rng_seed(&r, c->seed);
    double rr0 = 60.0 / c->hr_bpm;
    size_t cap = (size_t)(c->duration / (rr0 * 0.5)) + 16;
    b->t = malloc(cap * sizeof(double));
    b->n = 0;
    double t = c->t_first;
    size_t k = 0;
    while (t < c->duration && b->n < cap) {
        b->t[b->n++] = t;
        double rr = rr0 * (1.0 + c->rsa * sin(2.0 * M_PI * 0.25 * t) + c->rr_sd * synth_gauss(&r));
        k++;
        if (c->ectopic_every > 0 && (k % (size_t)c->ectopic_every) == 0) {
            /* premature beat followed by a compensatory pause (sum = 2 RR) */
            double pre = rr * 0.7;
            if (t + pre < c->duration && b->n < cap)
                b->t[b->n++] = t + pre;
            rr = 2.0 * rr;
        }
        t += rr;
    }
}

void synth_beats_free(synth_beats_t *b)
{
    free(b->t);
    b->t = NULL;
    b->n = 0;
}

/* ---- ECG ---------------------------------------------------------------- */
static double gss(double x, double mu, double sd)
{
    double z = (x - mu) / sd;
    return exp(-0.5 * z * z);
}

static double beat_mv(double dt, double rr, double a, double tr)
{
    double s = sqrt(rr);
    return 0.12 * a * gss(dt, -0.15 * s, 0.022)
         - 0.10 * a * gss(dt, -0.025, 0.008)
         + a * gss(dt, 0.0, 0.0095)
         - 0.18 * a * gss(dt, 0.027, 0.009)
         + tr * a * gss(dt, 0.26 * s, 0.045 * s);
}

void synth_ecg_init(synth_ecg_t *e, const synth_ecg_cfg_t *c, const synth_beats_t *b)
{
    e->c = *c;
    e->beats = b;
    e->bi = 0;
    e->n = 0;
    synth_rng_seed(&e->rng, c->seed);
    e->lp1 = e->lp2 = e->hp1_x = e->hp2_x = 0;
    e->x_prev = e->h1_prev = e->h2_prev = e->l1_prev = 0;
    e->emg_hp_x = e->emg_hp_y = e->emg_lp = 0;
    e->mains_phase = synth_uniform(&e->rng) * 2.0 * M_PI;
    e->analog_primed = 0;
}

double synth_ecg_clean_mv(const synth_ecg_t *e, double t)
{
    const synth_beats_t *b = e->beats;
    double v = 0;
    for (size_t i = e->bi; i < b->n; i++) {
        double dt = t - b->t[i];
        if (dt < -0.45)
            break;
        if (dt > 0.75)
            continue;
        double rr = i > 0 ? b->t[i] - b->t[i - 1] : (b->n > 1 ? b->t[1] - b->t[0] : 1.0);
        v += beat_mv(dt, rr, e->c.amp_mv, e->c.t_ratio);
    }
    return v;
}

static double input_uv(synth_ecg_t *e, double t, double emg)
{
    double v = 1000.0 * synth_ecg_clean_mv(e, t);
    v += e->c.mains_uv * sin(2.0 * M_PI * e->c.mains_hz * t + e->mains_phase);
    v += e->c.wander_uv * (0.7 * sin(2.0 * M_PI * 0.3 * t + 0.4) + 0.5 * sin(2.0 * M_PI * 0.07 * t));
    v += emg;
    return v;
}

/* First-order LP (corner fc) state y over one step dt with the input going
 * linearly from x0 to x1; returns the new output. */
static double foh_lp(double *y, double x0, double x1, double fc, double dt)
{
    double tau = 1.0 / (2.0 * M_PI * fc), e = exp(-dt / tau), sl = (x1 - x0) / dt;
    *y = x1 - sl * tau + (*y - x0 + sl * tau) * e;
    return *y;
}

uint16_t synth_ecg_next(synth_ecg_t *e)
{
    const double dt = 1.0 / 4000.0;
    double t = (double)e->n * dt;
    while (e->bi < e->beats->n && e->beats->t[e->bi] < t - 0.8)
        e->bi++;
    /* EMG-like noise: white -> 20 Hz HP -> 150 Hz LP (1st order each) */
    double w = e->c.emg_uv > 0 ? synth_gauss(&e->rng) * e->c.emg_uv / 0.32 : 0.0;
    double ah = 1.0 - exp(-2.0 * M_PI * 20.0 * dt), al = 1.0 - exp(-2.0 * M_PI * 150.0 * dt);
    e->emg_hp_x += ah * (w - e->emg_hp_x);
    double hp = w - e->emg_hp_x;
    e->emg_lp += al * (hp - e->emg_lp);
    double emg = e->emg_lp + (e->c.white_uv > 0 ? e->c.white_uv * synth_gauss(&e->rng) : 0.0);

    double v = input_uv(e, t, emg);
    if (e->c.analog) {
        /* 0.32 Hz and 0.48 Hz HP, 159 Hz and 339 Hz LP: exact first-order
         * responses for an input that is linear between samples */
        if (!e->analog_primed) {
            e->x_prev = v;
            e->hp1_x = v; /* HP1 internal LP at the input: HP outputs start at 0 */
            e->hp2_x = 0;
            e->lp1 = e->lp2 = 0;
            e->h1_prev = e->h2_prev = e->l1_prev = 0;
            e->analog_primed = 1;
        }
        double h1 = v - foh_lp(&e->hp1_x, e->x_prev, v, 0.32, dt);
        double h2 = h1 - foh_lp(&e->hp2_x, e->h1_prev, h1, 0.48, dt);
        double l1 = foh_lp(&e->lp1, e->h2_prev, h2, 159.0, dt);
        double l2 = foh_lp(&e->lp2, e->l1_prev, l1, 339.0, dt);
        e->x_prev = v;
        e->h1_prev = h1;
        e->h2_prev = h2;
        e->l1_prev = l1;
        v = l2;
    }
    double code = e->c.mid + v * e->c.gain / 805.7 + 0.3 * synth_gauss(&e->rng);
    e->n++;
    code = floor(code + 0.5);
    if (code < 0)
        code = 0;
    if (code > 4095)
        code = 4095;
    return (uint16_t)code;
}

/* ---- PPG ---------------------------------------------------------------- */
static double pulse_shape(double tau)
{
    if (tau <= 0)
        return 0;
    double r = 1.0 - exp(-(tau / 0.09) * (tau / 0.09));
    return r * exp(-tau / 0.30) + 0.12 * gss(tau, 0.32, 0.04) * (1.0 - exp(-tau / 0.05));
}

static double shape_max(void)
{
    double m = 0;
    for (double tau = 0; tau < 0.6; tau += 1e-4) {
        double v = pulse_shape(tau);
        if (v > m)
            m = v;
    }
    return m;
}

void synth_ppg_init(synth_ppg_t *p, const synth_ppg_cfg_t *c, const synth_beats_t *b)
{
    p->c = *c;
    p->beats = b;
    p->k = 0;
    synth_rng_seed(&p->rng, c->seed);
    /* isolated pulse: tangent at max slope intersected with v = 0 */
    double best = 0, ts = 0;
    const double h = 1e-5;
    for (double tau = h; tau < 0.3; tau += h) {
        double s = (pulse_shape(tau + h) - pulse_shape(tau - h)) / (2 * h);
        if (s > best) {
            best = s;
            ts = tau;
        }
    }
    p->foot_off = ts - pulse_shape(ts) / best;
}

double synth_ppg_volume(const synth_ppg_t *p, double t)
{
    static double vmax = 0;
    if (vmax == 0)
        vmax = shape_max();
    double v = 0;
    const synth_beats_t *b = p->beats;
    size_t lo = 0, hi = b->n;
    while (lo < hi) { /* first beat with t >= t - 3 s */
        size_t mid = (lo + hi) / 2;
        if (b->t[mid] < t - 3.0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < b->n; i++) {
        double on = b->t[i] + p->c.pat_s - p->foot_off;
        if (on > t)
            break;
        if (t - on < 2.5)
            v += pulse_shape(t - on);
    }
    return v / vmax;
}

double synth_ppg_signal_ua(const synth_ppg_t *p, double t, double led_ma)
{
    double dc = p->c.refl_ua_per_ma * led_ma * (1.0 + p->c.resp * sin(2.0 * M_PI * 0.2 * t));
    return dc * (1.0 - p->c.pi * synth_ppg_volume(p, t));
}

static uint16_t adc(synth_rng_t *r, double v)
{
    double c = floor(v / 3.3 * 4096.0 + 0.3 * synth_gauss(r) + 0.5);
    return (uint16_t)(c < 0 ? 0 : c > 4095 ? 4095 : c);
}

void synth_ppg_next(synth_ppg_t *p, uint16_t led_code, uint16_t dac12, dsp_ppg_frame_t *f)
{
    double t0 = p->k * 0.01;
    double td = t0 + 0.0097, ton = t0 + 0.00995;
    double led_ma = led_code * 3.718e-3;
    double amb_d = p->c.ambient_ua * (1.0 + p->c.flicker * sin(2.0 * M_PI * p->c.flicker_hz * td));
    double amb_o = p->c.ambient_ua * (1.0 + p->c.flicker * sin(2.0 * M_PI * p->c.flicker_hz * ton));
    double i_d = amb_d + 1e-3 * p->c.noise_na * synth_gauss(&p->rng);
    double i_o = synth_ppg_signal_ua(p, ton, led_ma) + amb_o + 1e-3 * p->c.noise_na * synth_gauss(&p->rng);
    double tia_d = 0.245 + i_d, tia_o = 0.245 + i_o; /* 1 V per uA */
    if (tia_d > 3.3) tia_d = 3.3;
    if (tia_o > 3.3) tia_o = 3.3;
    double vdac = dac12 / 4096.0 * 3.3;
    double hi_d = 11 * vdac - 10 * tia_d, hi_o = 11 * vdac - 10 * tia_o;
    hi_d = hi_d < 0 ? 0 : hi_d > 3.3 ? 3.3 : hi_d;
    hi_o = hi_o < 0 ? 0 : hi_o > 3.3 ? 3.3 : hi_o;
    f->tia_dark = adc(&p->rng, tia_d);
    f->hi_dark = adc(&p->rng, hi_d);
    f->tia_on = adc(&p->rng, tia_o);
    f->hi_on = adc(&p->rng, hi_o);
    p->k++;
}

double synth_ppg_true_foot(const synth_ppg_t *p, size_t i)
{
    const synth_beats_t *b = p->beats;
    double on = b->t[i] + p->c.pat_s - p->foot_off;
    double prev_on = i > 0 ? b->t[i - 1] + p->c.pat_s - p->foot_off : on - 1.0;
    const double h = 1e-4;
    /* y = -light: pulse up */
    double best = 0, ts = 0, ys = 0;
    for (double t = on; t < on + 0.25; t += h) {
        double s = (synth_ppg_signal_ua(p, t - h, 1.0) - synth_ppg_signal_ua(p, t + h, 1.0)) / (2 * h);
        if (s > best) {
            best = s;
            ts = t;
            ys = -synth_ppg_signal_ua(p, t, 1.0);
        }
    }
    if (best <= 0)
        return -1;
    double t_lo = prev_on + 0.25 > on - 0.45 ? prev_on + 0.25 : on - 0.45;
    double ymin = 1e9;
    for (double t = t_lo; t <= ts; t += h) {
        double y = -synth_ppg_signal_ua(p, t, 1.0);
        if (y < ymin)
            ymin = y;
    }
    return ts - (ys - ymin) / best;
}

/* ---- closed-loop PPG ------------------------------------------------------ */
void synth_ppg_loop_init(synth_ppg_loop_t *l, uint16_t led, uint8_t limit_ma)
{
    dsp_ppg_init(&l->ppg);
    dsp_agc_init(&l->agc);
    l->led = led;
    l->dac = 0xFFFF; /* set from the first frame */
    l->max_code = dsp_led_max_code(limit_ma);
    l->pending = 0;
    l->k = 0;
    l->last_step = 0;
    l->agc_every = 25;
    l->dcc_every = 100;
    l->agc_steps = l->dcc_steps = 0;
    l->agc_tia_sum = l->agc_dark_sum = l->agc_n = 0;
    l->predict_dcc = 1;
}

bool synth_ppg_loop_frame(synth_ppg_loop_t *l, synth_ppg_t *src, dsp_ppg_frame_t *f,
                          dsp_ppg_out_t *out, dsp_ppg_pulse_t *pulse)
{
    if (l->dac == 0xFFFF) {
        /* first frame: measure with mid DAC, then cancel */
        synth_ppg_t probe = *src;
        synth_ppg_next(&probe, l->led, 2048, f);
        l->dac = dsp_ppg_dcc_code(f->tia_on);
    }
    synth_ppg_next(src, l->led, l->dac, f);
    uint8_t flags = l->pending;
    l->pending = 0;
    bool got = dsp_ppg_process(&l->ppg, f, l->dac, flags, out, pulse);
    if (!flags) {
        l->agc_tia_sum += f->tia_on;
        l->agc_dark_sum += f->tia_dark;
        l->agc_n++;
    }
    l->k++;
    if (l->dcc_every && l->k % (uint32_t)l->dcc_every == 0) {
        if (dsp_ppg_take_avg(&l->ppg, &l->avg) && dsp_ppg_dcc_update(l->avg.tia_on, l->avg.hi_on, &l->dac)) {
            l->pending |= DSP_PPG_IN_DAC_STEP;
            l->dcc_steps++;
            l->last_step = l->k;
        }
    }
    if (l->agc_every && l->k % (uint32_t)l->agc_every == 0) {
        if (l->agc_n && l->k - l->last_step > 5u) {
            uint16_t tia = (uint16_t)(l->agc_tia_sum / l->agc_n);
            uint16_t old = l->led;
            if (dsp_agc_step(&l->agc, tia, &l->led, l->max_code)) {
                l->pending |= DSP_PPG_IN_LED_STEP;
                if (l->predict_dcc) {
                    uint16_t dark = (uint16_t)(l->agc_dark_sum / l->agc_n);
                    uint16_t c = dsp_ppg_dcc_predict(dark, tia, old, l->led);
                    if (c != l->dac) {
                        l->dac = c;
                        l->pending |= DSP_PPG_IN_DAC_STEP;
                    }
                }
                l->agc_steps++;
                l->last_step = l->k;
            }
        }
        l->agc_tia_sum = l->agc_dark_sum = l->agc_n = 0;
    }
    return got;
}

/* ---- tone fit ------------------------------------------------------------ */
double synth_tone_amp(const double *x, size_t n, double fs, double f, double *phase)
{
    /* least squares x = a cos + b sin + c */
    double m[3][4] = { { 0 } };
    for (size_t i = 0; i < n; i++) {
        double w = 2.0 * M_PI * f * (double)i / fs;
        double v[3] = { cos(w), sin(w), 1.0 };
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++)
                m[r][c] += v[r] * v[c];
            m[r][3] += v[r] * x[i];
        }
    }
    for (int c = 0; c < 3; c++) {
        int piv = c;
        for (int r = c + 1; r < 3; r++)
            if (fabs(m[r][c]) > fabs(m[piv][c]))
                piv = r;
        for (int k = 0; k < 4; k++) {
            double tmp = m[c][k];
            m[c][k] = m[piv][k];
            m[piv][k] = tmp;
        }
        if (fabs(m[c][c]) < 1e-300)
            continue;
        for (int r = 0; r < 3; r++) {
            if (r == c)
                continue;
            double q = m[r][c] / m[c][c];
            for (int k = 0; k < 4; k++)
                m[r][k] -= q * m[c][k];
        }
    }
    double a = fabs(m[0][0]) > 1e-300 ? m[0][3] / m[0][0] : 0;
    double b = fabs(m[1][1]) > 1e-300 ? m[1][3] / m[1][1] : 0;
    if (phase)
        *phase = atan2(-b, a); /* x = A cos(w + phase) */
    return sqrt(a * a + b * b);
}
