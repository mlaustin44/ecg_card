#include "dsp_ppg.h"

#include "dsp_coefs.h"
#include "dsp_delays.h"
#include "dsp_util.h"

#if DSP_PPG_DARK_N != 15 || DSP_PPG_DARK_DELAY != 7 || DSP_PPG_HIST < DSP_PPG_DARK_DELAY + 1
#error "dark mean scaling below assumes a 15-frame centred mean"
#endif
#if DSP_PPG_FIR_N != DSP_PPG_FIR_TAPS
#error "dsp_ppg.h DSP_PPG_FIR_TAPS does not match the generated coefficients"
#endif

static const int16_t h_fir[DSP_PPG_FIR_N] = DSP_PPG_FIR_INIT;
static const dsp_bq_coef_t c_hp = DSP_PPG_HP_INIT;
static const dsp_bq_coef_t c_lp = DSP_PPG_LP_INIT;

#define SM (DSP_PPG_RING - 1u)

#define F_PRIMED   0x01u
#define F_LEARNING 0x02u
#define F_HAVE_UP  0x04u
#define F_IN_UP    0x08u
#define F_REOFFSET 0x10u
#define F_HAVE_FT  0x20u
#define F_COARSE   0x40u
#define F_HAVE_AC  0x80u

#define HI_MIN       16u
#define HI_MAX       4079u
#define LEARN_FRAMES 200u
#define REFRACT      28u   /* frames between upslopes (214 bpm) */
#define UP_MAX_LEN   10u
#define SLOPE_BACK   6u    /* LP max-slope search before the display maximum */
#define MIN_SEARCH   15u   /* frames searched back for the foot minimum (150 ms) */
#define NO_PULSE_DECAY 300u
#define HP_GUARD     16    /* HP input scaling (4 fractional bits) */

int32_t dsp_ppg_fine(const dsp_ppg_frame_t *f, uint16_t dac12, bool *coarse_mode)
{
    int32_t d11 = 11 * (int32_t)dac12;
    if (f->hi_on < HI_MIN || f->hi_on > HI_MAX) {
        *coarse_mode = true;
        return -10 * ((int32_t)f->tia_on - (int32_t)f->tia_dark);
    }
    *coarse_mode = false;
    int32_t dark10 = (f->hi_dark >= HI_MIN && f->hi_dark <= HI_MAX)
                         ? d11 - (int32_t)f->hi_dark
                         : 10 * (int32_t)f->tia_dark;
    return (int32_t)f->hi_on - d11 + dark10;
}

void dsp_ppg_init(dsp_ppg_t *p)
{
    p->t = 0;
    p->sum_dark = p->sum_tia = p->sum_hi = 0;
    p->n_avg = 0;
    dsp_ppg_reset(p);
}

void dsp_ppg_reset(dsp_ppg_t *p)
{
    p->flags = 0;
    p->ibi_n = p->ibi_i = 0;
    p->n_bridge = 0;
    p->offset = 0;
    p->dist_age = 0;
    p->no_pulse = 0;
    p->thr = p->slope_avg = p->learn_max = p->up_max = 0;
    p->amp_avg = 0;
    p->y1 = p->y2 = 0;
    p->smax = p->prev_min = 0;
}

static int32_t s_at(const dsp_ppg_t *p, uint32_t k)
{
    return p->s[k & SM];
}

static int32_t d_at(const dsp_ppg_t *p, uint32_t k)
{
    return s_at(p, k + 1u) - s_at(p, k - 1u);
}

/* Upslope with maximum central-difference slope near s-index u0 (current
 * s-index t >= u0 + 3). Returns true and fills *pl if a valid foot was found. */
static bool foot(dsp_ppg_t *p, uint32_t u0, uint32_t prev_up, bool have_prev, uint32_t t,
                 dsp_ppg_pulse_t *pl)
{
    uint32_t u = u0;
    int32_t d0 = d_at(p, u0);
    for (uint32_t k = u0 - 2u; k != u0 + 3u; k++) {
        if (t - k < 3u)
            break;
        int32_t v = d_at(p, k);
        if (v > d0) {
            d0 = v;
            u = k;
        }
    }
    if (t - u < 3u)
        return false;
    int32_t dm = d_at(p, u - 1u), dp = d_at(p, u + 1u);
    int32_t den = dm - 2 * d0 + dp;
    int32_t dl = 0; /* sub-frame position of the maximum slope, Q8 */
    if (den < 0) {
        dl = ((dm - dp) * 128) / den;
        if (dl > 128)
            dl = 128;
        if (dl < -128)
            dl = -128;
    }
    /* maximum slope (per 2 frames), Q8 */
    int32_t pk = d0 * 256 + ((dp - dm) * dl) / 4;
    if (pk <= 0)
        return false;
    int32_t su = s_at(p, u);
    int32_t ys = su * 256 + (dl >= 0 ? (s_at(p, u + 1u) - su) * dl : (su - s_at(p, u - 1u)) * dl);

    /* preceding minimum: lowest 3-frame mean (less noise-selection bias
     * than the lowest single frame) */
    uint32_t w = MIN_SEARCH;
    if (have_prev && u - prev_up < w + 5u)
        w = u - prev_up > 8u ? u - prev_up - 5u : 3u;
    uint32_t kmin = u - 2u;
    int32_t vmin3 = s_at(p, u - 1u) + s_at(p, u - 2u) + s_at(p, u - 3u);
    for (uint32_t i = 3; i <= w; i++) {
        uint32_t k = u - i;
        int32_t v3 = s_at(p, k - 1u) + s_at(p, k) + s_at(p, k + 1u);
        if (v3 < vmin3) {
            vmin3 = v3;
            kmin = k;
        }
    }
    int64_t ymin = dsp_sdiv64_round((int64_t)vmin3 * 256, 3);
    int64_t diff = (int64_t)ys - ymin;
    if (diff < 0)
        return false;
    /* tangent: s = ys + (pk / 2 / 256) (x - xs) reaches ymin at xs - 512 diff / pk */
    int32_t rel = dl - dsp_sdiv64_round(diff * 512, pk); /* Q8 frames relative to u */
    if (rel < -(int32_t)((u - kmin + 3u) * 256u))
        return false;
    /* reject if any input frame feeding s[kmin-1 .. u+2] was disturbed */
    if ((uint32_t)p->dist_age <= t - kmin + DSP_PPG_FIR_TAPS)
        return false;
    /* reject small bumps (noise, dicrotic notch): the rise from the minimum to
     * the tangent point must be >= 1/4 of the running mean rise */
    int32_t rise = (int32_t)dsp_rshr64(diff, 8);
    if (p->amp_avg > 0 && rise * 4 < p->amp_avg)
        return false;
    p->amp_avg = p->amp_avg > 0 ? p->amp_avg - p->amp_avg / 4 + rise / 4 : rise;

    int32_t tot = rel - DSP_PPG_FOOT_DELAY * 256;
    pl->frame = u + (uint32_t)(tot >> 8);
    pl->frac_q8 = (uint8_t)(tot & 255);
    pl->ibi_ms = 0;
    pl->pi_permille = 0;

    /* PI of the previous beat: its maximum (smax since its upslope) minus
     * its own foot minimum, so slow baseline drift does not enter */
    int32_t ymin_i = (int32_t)dsp_rshr64(ymin, 8);
    int32_t dc = p->dc_q5 >> 5;
    if ((p->flags & F_HAVE_AC) && dc > 0 && p->smax > p->prev_min) {
        uint32_t pi = (uint32_t)(p->smax - p->prev_min) * 100u / (uint32_t)dc;
        pl->pi_permille = (uint16_t)(pi > 65535u ? 65535u : pi);
    }
    p->prev_min = ymin_i;
    p->flags |= F_HAVE_AC;
    int32_t df = (int32_t)(pl->frame - p->last_foot_f);
    if ((p->flags & F_HAVE_FT) && df > 0 && df <= 300) {
        int32_t ibi = df * 256 + pl->frac_q8 - p->last_foot_frac;
        uint32_t med = p->ibi_n == 5u ? dsp_median5_u32(p->ibi_q8) : 0u;
        /* 0.3..2.5 s, and < 1.6 x the current median (a missed pulse) */
        if (ibi >= 30 * 256 && ibi <= 250 * 256 && (!med || (uint32_t)ibi * 5u < med * 8u)) {
            p->ibi_q8[p->ibi_i] = (uint32_t)ibi;
            p->ibi_i = (uint8_t)((p->ibi_i + 1u) % 5u);
            if (p->ibi_n < 5u)
                p->ibi_n++;
            pl->ibi_ms = (uint16_t)((ibi * 10 + 128) >> 8);
        }
    }
    pl->pulse_bpm = 0;
    if (p->ibi_n == 5u) {
        uint32_t med = dsp_median5_u32(p->ibi_q8);
        pl->pulse_bpm = (uint16_t)((6000u * 256u + med / 2u) / med);
    }
    p->last_foot_f = pl->frame;
    p->last_foot_frac = pl->frac_q8;
    p->flags |= F_HAVE_FT;
    return true;
}

/* Detection runs on the display band (0.5-8 Hz: smoother, no wander); the
 * foot is then timed on the linear-phase LP signal s. yd = display filter
 * output (4 fractional bits) at frame t. */
static bool detect(dsp_ppg_t *p, uint32_t t, int32_t yd, dsp_ppg_pulse_t *pl)
{
    bool found = false;
    int32_t st = s_at(p, t);
    if (st > p->smax)
        p->smax = st;
    if (p->no_pulse < 65535u)
        p->no_pulse++;
    int32_t d = yd - p->y2; /* display slope at t - 1 */
    p->y2 = p->y1;
    p->y1 = yd;
    uint32_t tc = t - 1u;
    if (p->flags & F_LEARNING) {
        if (d > p->learn_max && p->no_pulse > 50u) /* skip the filter start */
            p->learn_max = d;
        if (p->no_pulse >= LEARN_FRAMES) {
            p->slope_avg = p->learn_max;
            p->thr = (p->slope_avg * 2) / 5;
            p->flags &= (uint8_t)~F_LEARNING;
            p->no_pulse = 0;
        }
        return false;
    }
    if (!(p->flags & F_IN_UP)) {
        if (p->thr > 0 && d > p->thr &&
            (!(p->flags & F_HAVE_UP) || tc - p->last_up_t >= REFRACT)) {
            p->flags |= F_IN_UP;
            p->up_max = d;
            p->up_t = tc;
        }
    } else if (d > p->up_max) {
        p->up_max = d;
        p->up_t = tc;
    } else if (2 * d < p->up_max || tc - p->up_t >= UP_MAX_LEN) {
        uint32_t prev = p->last_up_t;
        bool have_prev = (p->flags & F_HAVE_UP) != 0;
        p->flags = (uint8_t)((p->flags & ~F_IN_UP) | F_HAVE_UP);
        p->last_up_t = p->up_t;
        /* maximum LP slope near the display-band maximum (the display IIR
         * lags s by about 2-3 frames) */
        uint32_t u = p->up_t;
        int32_t best = d_at(p, u);
        for (uint32_t k = p->up_t - SLOPE_BACK; k != p->up_t + 2u; k++) {
            if (t - k < 2u)
                break;
            int32_t v = d_at(p, k);
            if (v > best) {
                best = v;
                u = k;
            }
        }
        found = foot(p, u, prev, have_prev, t, pl);
        if (found) {
            p->slope_avg = p->slope_avg - p->slope_avg / 4 + p->up_max / 4;
            p->thr = (p->slope_avg * 2) / 5;
            p->no_pulse = 0;
        } else {
            /* no AC, and no interval across a rejected pulse */
            p->flags &= (uint8_t)~(F_HAVE_AC | F_HAVE_FT);
        }
        p->smax = st;
    }
    if (p->no_pulse >= NO_PULSE_DECAY) {
        p->slope_avg /= 2;
        p->thr = (p->slope_avg * 2) / 5;
        p->no_pulse = NO_PULSE_DECAY - 100u;
    }
    return found;
}

/* Delayed-frame history flags (dflags) */
#define H_COARSE 0x80u
#define H_EXACT  0x40u
#define H_IN     0x07u

bool dsp_ppg_process(dsp_ppg_t *p, const dsp_ppg_frame_t *f, uint16_t dac12, uint8_t in_flags,
                     dsp_ppg_out_t *out, dsp_ppg_pulse_t *pulse)
{
    bool coarse_mode;
    int32_t raw = dsp_ppg_fine(f, dac12, &coarse_mode);
    int32_t coarse = (int32_t)f->tia_on - (int32_t)f->tia_dark;
    out->coarse = dsp_sat16(coarse);
    out->fine = (int16_t)(uint16_t)((uint32_t)raw & 0xFFFFu);
    out->flags = coarse_mode ? DSP_PPG_OUT_COARSE : 0u;
    if (!in_flags) {
        p->sum_dark += f->tia_dark;
        p->sum_tia += f->tia_on;
        p->sum_hi += f->hi_on;
        if (p->n_avg < 65535u)
            p->n_avg++;
    }

    /* Processing value with a low-noise dark term: the x10 dark term uses the
     * centred DSP_PPG_DARK_N-frame mean of tia_dark, so each frame is
     * processed DARK_DELAY frames late. Dark samples do not depend on the
     * DAC12 or LED codes, so the mean may span disturbed frames. */
    int32_t part = coarse_mode ? -10 * (int32_t)f->tia_on : (int32_t)f->hi_on - 11 * (int32_t)dac12;
    uint8_t hf = (uint8_t)((in_flags & H_IN) | (coarse_mode ? H_COARSE : 0u));
    int32_t exact = 0;
    if (!coarse_mode && f->hi_dark >= HI_MIN && f->hi_dark <= HI_MAX) {
        exact = (int32_t)f->hi_on - (int32_t)f->hi_dark;
        hf |= H_EXACT;
    }
    if (!(p->flags & F_PRIMED)) {
        for (unsigned i = 0; i < DSP_PPG_DARK_N; i++)
            p->dark[i] = f->tia_dark;
        p->dark_sum = (uint32_t)f->tia_dark * DSP_PPG_DARK_N;
        for (unsigned i = 0; i < DSP_PPG_HIST; i++) {
            p->dpart[i] = part;
            p->dexact[i] = exact;
            p->dflags[i] = (uint8_t)(hf & ~H_IN);
        }
        p->dk_i = 0;
        p->dh_i = 0;
    }
    p->dark_sum += (uint32_t)f->tia_dark - p->dark[p->dk_i];
    p->dark[p->dk_i] = f->tia_dark;
    if (++p->dk_i == DSP_PPG_DARK_N)
        p->dk_i = 0;
    p->dpart[p->dh_i] = part;
    p->dexact[p->dh_i] = exact;
    p->dflags[p->dh_i] = hf;
    p->dh_i = (uint8_t)((p->dh_i + 1u) & (DSP_PPG_HIST - 1u));
    unsigned di = (p->dh_i + DSP_PPG_HIST - 1u - DSP_PPG_DARK_DELAY) & (DSP_PPG_HIST - 1u);
    uint8_t dfl = p->dflags[di];
    /* 10 x mean = 10 x sum / 15 = 2 x sum / 3 (Q16 multiply, sum < 2^16) */
    int32_t dark10 = (int32_t)((p->dark_sum * 43691u + 32768u) >> 16);
    int32_t draw = (dfl & H_EXACT) ? p->dexact[di] : p->dpart[di] + dark10;
    bool dcoarse = (dfl & H_COARSE) != 0;
    uint8_t din = dfl & H_IN;

    int32_t b;
    uint32_t t = p->t;
    if (!(p->flags & F_PRIMED)) {
        b = draw;
        for (unsigned i = 0; i < DSP_PPG_FIR_TAPS; i++)
            p->fir[i] = b;
        for (unsigned i = 0; i < DSP_PPG_RING; i++)
            p->s[i] = b;
        p->fir_i = 0;
        dsp_bq_reset(&p->hp, b * HP_GUARD, 0);
        dsp_bq_reset(&p->lp, 0, 0);
        p->y1 = p->y2 = 0;
        p->b1 = p->b2 = b;
        p->offset = 0;
        p->flags = F_PRIMED | F_LEARNING | (dcoarse ? F_COARSE : 0u);
        p->dc_q5 = coarse * 32;
        p->smax = b;
        p->dist_age = 0;
        p->no_pulse = 0;
    } else {
        bool switched = dcoarse != ((p->flags & F_COARSE) != 0);
        if (din || switched) {
            b = p->b1 + (p->n_bridge < 3u ? p->b1 - p->b2 : 0);
            if (p->n_bridge < 255u)
                p->n_bridge++;
            if ((din & DSP_PPG_IN_LED_STEP) || switched)
                p->flags |= F_REOFFSET;
            p->dist_age = 0;
        } else {
            if (p->flags & F_REOFFSET) {
                int32_t pred = p->b1 + (p->n_bridge < 3u ? p->b1 - p->b2 : 0);
                p->offset = pred - draw;
                p->flags &= (uint8_t)~F_REOFFSET;
            }
            b = draw + p->offset;
            p->n_bridge = 0;
            if (p->dist_age < 65535u)
                p->dist_age++;
        }
        p->flags = (uint8_t)((p->flags & ~F_COARSE) | (dcoarse ? F_COARSE : 0u));
    }
    if (in_flags)
        out->flags |= DSP_PPG_OUT_BRIDGED;
    p->b2 = p->b1;
    p->b1 = b;

    /* linear-phase FIR LP; symmetric, so the ring can be walked oldest first */
    p->fir[p->fir_i] = b;
    if (++p->fir_i == DSP_PPG_FIR_TAPS)
        p->fir_i = 0;
    int64_t acc = 0;
    unsigned j = 0;
    for (unsigned k = p->fir_i; k < DSP_PPG_FIR_TAPS; k++, j++)
        acc += (int64_t)h_fir[j] * p->fir[k];
    for (unsigned k = 0; j < DSP_PPG_FIR_TAPS; k++, j++)
        acc += (int64_t)h_fir[j] * p->fir[k];
    int32_t s = (int32_t)dsp_rshr64(acc, 15);
    p->s[t & SM] = s;

    int32_t y = dsp_bq_step(&p->hp, &c_hp, s * HP_GUARD);
    y = dsp_bq_step(&p->lp, &c_lp, y);
    out->disp = dsp_sat16(dsp_rshr32(y, 4));

    p->dc_q5 += (coarse * 32 - p->dc_q5) >> 5;

    bool found = detect(p, t, y, pulse);
    if (found)
        out->flags |= DSP_PPG_OUT_PULSE;
    p->t = t + 1u;
    return found;
}

uint16_t dsp_ppg_take_avg(dsp_ppg_t *p, dsp_ppg_avg_t *avg)
{
    uint16_t n = p->n_avg;
    if (n) {
        avg->tia_dark = (uint16_t)((p->sum_dark + n / 2u) / n);
        avg->tia_on = (uint16_t)((p->sum_tia + n / 2u) / n);
        avg->hi_on = (uint16_t)((p->sum_hi + n / 2u) / n);
    }
    p->sum_dark = p->sum_tia = p->sum_hi = 0;
    p->n_avg = 0;
    return n;
}
