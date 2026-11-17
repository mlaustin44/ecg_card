#include "dsp_qrs.h"

#include "dsp_coefs.h"
#include "dsp_delays.h"
#include "dsp_util.h"

static const dsp_bq_coef_t c_clean[2] = DSP_QRS_CLEAN_INIT;
static const dsp_bq_coef_t c_hp = DSP_QRS_HP_INIT;
static const dsp_bq_coef_t c_lp = DSP_QRS_LP_INIT;

#define RM (DSP_QRS_RING - 1u)
#define DM (DSP_QRS_DRING - 1u)

#define F_PRIMED   0x01u
#define F_LEARNING 0x02u
#define F_HAVE     0x04u /* a previous beat exists */
#define F_SB       0x08u /* search-back candidate stored */
#define F_FALLING  0x10u /* MWI past a confirmed peak, tracking the valley */

#define SETTLE_N     100u  /* samples ignored after (re)start */
#define LEARN_N      1024u /* learning window (2.05 s) */
#define CONFIRM_N    40u   /* 80 ms with no larger MWI confirms a peak */
#define REFRACT_N    100u  /* 200 ms */
#define TWAVE_N      180u  /* 360 ms */
#define REFINE_W     25    /* +-50 ms */
#define DERIV_MAX    7000  /* 75 * 7000^2 < 2^32 */
#define THR_I_FLOOR  20000u
#define THR_F_FLOOR  40u
#define DECAY_START  1000u /* 2 s without a beat */
#define DECAY_EVERY  500u
#define SB_MARGIN    150u  /* time-based search-back: 1.66 RR + 300 ms */

void dsp_qrs_init(dsp_qrs_t *q, uint32_t start_index)
{
    q->t = start_index;
    dsp_qrs_reset(q);
}

void dsp_qrs_reset(dsp_qrs_t *q)
{
    q->flags = F_LEARNING | F_FALLING;
    q->learn_n = 0;
    q->learn_sum_i = 0;
    q->learn_max_i = q->learn_sum_f = q->learn_max_f = 0;
    q->spki = q->npki = q->spkf = q->npkf = 0;
    q->thr_i1 = THR_I_FLOOR;
    q->thr_f1 = THR_F_FLOOR;
    q->mwi = 0;
    q->cand_v = 0;
    q->cand_t = q->t;
    q->last_r = 0;
    q->last_frac = 0;
    q->last_slope = 0;
    q->last_beat_t = q->t;
    q->decay_t = q->t;
    q->rr5_n = q->rr5_i = q->rr8_n = q->rr8_i = 0;
    q->sb_lim = 0;
    q->hr = 0;
    q->pol = 0;
    for (unsigned i = 0; i < DSP_QRS_DRING; i++)
        q->br[i] = q->dr[i] = 0;
}

void dsp_qrs_skip(dsp_qrs_t *q, uint32_t n)
{
    q->t += n;
    /* thresholds, polarity and the RR history stay; the beat before the gap
     * is forgotten (no interval across the gap, no refractory / T-wave test
     * against it, no stale search-back candidate) */
    q->flags &= (uint8_t)~(F_PRIMED | F_HAVE | F_SB);
}

/* (a - b) in 1/256 samples, saturated to +-2^30. */
static int32_t rdiff(uint32_t ai, uint8_t af, uint32_t bi, uint8_t bf)
{
    int32_t d = (int32_t)(ai - bi);
    if (d > (1 << 22))
        return 1 << 30;
    if (d < -(1 << 22))
        return -(1 << 30);
    return d * 256 + (int32_t)af - (int32_t)bf;
}

static void update_thresholds(dsp_qrs_t *q)
{
    uint32_t ti = q->npki + (q->spki > q->npki ? (q->spki - q->npki) >> 2 : 0u);
    uint32_t tf = q->npkf + (q->spkf > q->npkf ? (q->spkf - q->npkf) >> 2 : 0u);
    q->thr_i1 = ti > THR_I_FLOOR ? ti : THR_I_FLOOR;
    q->thr_f1 = tf > THR_F_FLOOR ? tf : THR_F_FLOOR;
}

static uint32_t rr_mean(const dsp_qrs_t *q)
{
    uint32_t s = 0;
    for (unsigned i = 0; i < q->rr8_n; i++)
        s += q->rr8[i];
    return q->rr8_n ? s / q->rr8_n : 0u; /* per beat, not per sample */
}

typedef struct {
    size_t n, max;
    dsp_qrs_beat_t *out;
} sink_t;

static void accept(dsp_qrs_t *q, sink_t *k, uint32_t r, uint8_t frac, uint32_t peaki,
                   uint32_t peakf, uint32_t slope, uint16_t amp, uint8_t flags)
{
    dsp_qrs_beat_t b;
    b.index = r;
    b.frac_q8 = frac;
    b.flags = flags;
    b.rr_ms = 0;
    b.amp = amp;
    if (q->flags & F_HAVE) {
        int32_t rr = rdiff(r, frac, q->last_r, q->last_frac);
        if (rr > 0 && rr < (65535 << 7)) {
            q->rr_q8[q->rr5_i] = (uint32_t)rr;
            q->rr5_i = (uint8_t)((q->rr5_i + 1u) % 5u);
            if (q->rr5_n < 5u)
                q->rr5_n++;
            uint32_t rs = (uint32_t)rr >> 8;
            q->rr8[q->rr8_i] = (uint16_t)(rs > 65535u ? 65535u : rs);
            q->rr8_i = (uint8_t)((q->rr8_i + 1u) & 7u);
            if (q->rr8_n < 8u)
                q->rr8_n++;
            b.rr_ms = (uint16_t)(((uint32_t)rr + 64u) >> 7); /* 2 ms per sample */
            q->sb_lim = (uint16_t)((rr_mean(q) * 425u) >> 8); /* 1.66 RR */
            if (q->rr5_n == 5u) {
                uint32_t med = dsp_median5_u32(q->rr_q8);
                q->hr = (uint16_t)((30000u * 256u + med / 2u) / med);
            }
        }
    }
    b.hr_bpm = q->hr;
    q->last_r = r;
    q->last_frac = frac;
    q->last_slope = slope;
    q->last_beat_t = q->t;
    q->decay_t = q->t + DECAY_START;
    q->flags = (uint8_t)((q->flags | F_HAVE) & ~F_SB);
    if (flags & DSP_QRS_F_SEARCHBACK) {
        q->spki = q->spki - (q->spki >> 2) + (peaki >> 2);
        q->spkf = q->spkf - (q->spkf >> 2) + (peakf >> 2);
    } else {
        q->spki = q->spki - (q->spki >> 3) + (peaki >> 3);
        q->spkf = q->spkf - (q->spkf >> 3) + (peakf >> 3);
    }
    update_thresholds(q);
    if (k->n < k->max)
        k->out[k->n++] = b;
}

static void accept_searchback(dsp_qrs_t *q, sink_t *k)
{
    accept(q, k, q->sb_r, q->sb_frac, q->sb_peaki, q->sb_peakf, q->sb_slope, q->sb_amp,
           DSP_QRS_F_SEARCHBACK);
}

/* Search-back limit, 1.66 x mean RR (samples), cached per beat. */
static uint32_t missed_limit(const dsp_qrs_t *q)
{
    return q->sb_lim;
}

typedef struct {
    uint32_t r, peaki, peakf, slope;
    uint16_t amp;
    uint8_t frac;
} cand_t;

/* Measure the confirmed MWI peak at q->cand_t (now = q->t): band-pass peak,
 * maximum slope and the refined R-peak. */
static void analyse(dsp_qrs_t *q, cand_t *o)
{
    uint32_t ct = q->cand_t;

    /* QRS location: largest |band-pass| in the integration window */
    uint32_t tb = ct, peakf = 0, slope = 0;
    for (uint32_t i = 0; i <= DSP_QRS_MWI + 3u; i++) {
        uint32_t kk = ct - i;
        uint32_t a = (uint32_t)dsp_abs32(q->br[kk & DM]);
        if (a > peakf) {
            peakf = a;
            tb = kk;
        }
        if (i < DSP_QRS_MWI) {
            uint32_t s = (uint32_t)dsp_abs32(q->dr[kk & DM]);
            if (s > slope)
                slope = s;
        }
    }

    /* R-peak: extremum of the clean signal around the QRS */
    uint32_t c = tb - (uint32_t)DSP_QRS_BP_DELAY;
    int32_t mx = -32768, mn = 32767;
    uint32_t kmx = c, kmn = c;
    for (int i = -REFINE_W; i <= REFINE_W; i++) {
        uint32_t kk = c + (uint32_t)i;
        int32_t v = q->cr[kk & RM];
        if (v > mx) {
            mx = v;
            kmx = kk;
        }
        if (v < mn) {
            mn = v;
            kmn = kk;
        }
    }
    int32_t base = ((int32_t)q->cr[(c - REFINE_W) & RM] + q->cr[(c + REFINE_W) & RM]) / 2;
    int32_t pos = mx - base, neg = base - mn;
    if (pos > neg && q->pol < 8)
        q->pol++;
    else if (neg > pos && q->pol > -8)
        q->pol--;
    uint32_t kp = q->pol >= 0 ? kmx : kmn;
    int32_t ym = q->cr[(kp - 1u) & RM], y0 = q->cr[kp & RM], yp = q->cr[(kp + 1u) & RM];
    int32_t den = ym - 2 * y0 + yp;
    int32_t fr = 0;
    if (den != 0) {
        fr = ((ym - yp) * 128) / den; /* per candidate, not per sample */
        if (fr > 127)
            fr = 127;
        if (fr < -127)
            fr = -127;
    }
    int32_t tot = fr - DSP_QRS_CLEAN_DELAY_Q8;
    o->r = kp + (uint32_t)(tot >> 8);
    o->frac = (uint8_t)(tot & 255);
    o->amp = (uint16_t)(mx - mn);
    o->peaki = q->cand_v;
    o->peakf = peakf;
    o->slope = slope;
}

/* Pan-Tompkins decision for a confirmed candidate. */
static void classify(dsp_qrs_t *q, sink_t *k)
{
    cand_t cd;
    analyse(q, &cd);
    uint32_t r = cd.r, peaki = cd.peaki, peakf = cd.peakf, slope = cd.slope;
    uint8_t rf = cd.frac;
    uint16_t amp = cd.amp;
    int have = (q->flags & F_HAVE) != 0;
    int32_t rd = have ? rdiff(r, rf, q->last_r, q->last_frac) : (1 << 30);

    /* search-back: a beat was missed between the last one and this candidate */
    if (have && (q->flags & F_SB)) {
        uint32_t lim = missed_limit(q);
        if (lim && rd > (int32_t)(lim * 256u)) {
            accept_searchback(q, k);
            rd = rdiff(r, rf, q->last_r, q->last_frac);
        }
    }
    if (rd < (int32_t)(REFRACT_N * 256u))
        return; /* refractory: same QRS seen again, or earlier */

    int twave = rd < (int32_t)(TWAVE_N * 256u) && slope < q->last_slope / 2u;
    if (!twave && peaki > q->thr_i1 && peakf > q->thr_f1) {
        accept(q, k, r, rf, peaki, peakf, slope, amp, 0);
        return;
    }
    if (!twave && peaki > q->thr_i1 / 2u && peakf > q->thr_f1 / 2u &&
        (!(q->flags & F_SB) || peaki > q->sb_peaki)) {
        q->sb_peaki = peaki;
        q->sb_peakf = peakf;
        q->sb_r = r;
        q->sb_frac = rf;
        q->sb_slope = slope;
        q->sb_amp = amp;
        q->flags |= F_SB;
    }
    q->npki = q->npki - (q->npki >> 3) + (peaki >> 3);
    q->npkf = q->npkf - (q->npkf >> 3) + (peakf >> 3);
    update_thresholds(q);
}

static void learn(dsp_qrs_t *q, int16_t bs)
{
    if (q->learn_n < SETTLE_N) {
        q->learn_n++;
        return;
    }
    uint32_t a = (uint32_t)dsp_abs32(bs);
    q->learn_sum_i += q->mwi;
    q->learn_sum_f += a;
    if (q->mwi > q->learn_max_i)
        q->learn_max_i = q->mwi;
    if (a > q->learn_max_f)
        q->learn_max_f = a;
    if (++q->learn_n == SETTLE_N + LEARN_N) {
        q->spki = q->learn_max_i / 3u;
        q->npki = (uint32_t)(q->learn_sum_i >> 11); /* mean / 2 */
        q->spkf = q->learn_max_f / 3u;
        q->npkf = q->learn_sum_f >> 11;
        update_thresholds(q);
        q->flags &= (uint8_t)~F_LEARNING;
        q->last_beat_t = q->t;
        q->decay_t = q->t + DECAY_START;
    }
}

/* Learning-phase candidate: remember the latest one that is at least half
 * the largest MWI so far (a QRS, not a T wave). */
static void learn_candidate(dsp_qrs_t *q)
{
    if (q->learn_n < SETTLE_N || q->cand_v < q->learn_max_i / 2u)
        return;
    cand_t cd;
    analyse(q, &cd);
    q->sb_r = cd.r;
    q->sb_frac = cd.frac;
    q->sb_slope = cd.slope;
    q->sb_peaki = cd.peaki;
    q->sb_peakf = cd.peakf;
    q->sb_amp = cd.amp;
    q->flags |= F_SB;
}

/* End of learning: the last large learning candidate becomes the previous
 * beat, so refractory and T-wave tests apply at once; it is reported if it
 * is recent (< 1.5 s), otherwise only used as the reference. */
static void end_learning(dsp_qrs_t *q, sink_t *k)
{
    if (!(q->flags & F_SB))
        return;
    q->flags &= (uint8_t)~F_SB;
    if (q->t - q->sb_r < 750u) {
        accept(q, k, q->sb_r, q->sb_frac, q->sb_peaki, q->sb_peakf, q->sb_slope, q->sb_amp, 0);
    } else {
        q->last_r = q->sb_r;
        q->last_frac = q->sb_frac;
        q->last_slope = q->sb_slope;
        q->flags |= F_HAVE;
    }
}

static void step(dsp_qrs_t *q, int16_t xin, sink_t *k)
{
    int32_t x = (int32_t)xin * 256;
    uint32_t t = q->t;
    if (!(q->flags & F_PRIMED)) {
        dsp_bq_reset(&q->clean[0], x, x);
        dsp_bq_reset(&q->clean[1], x, x);
        dsp_bq_reset(&q->hp, x, 0);
        dsp_bq_reset(&q->lp, 0, 0);
        for (unsigned i = 0; i < DSP_QRS_RING; i++)
            q->cr[i] = xin;
        /* band-pass, derivative and MWI start from rest (after a gap they
         * hold samples from before it) */
        for (unsigned i = 0; i < DSP_QRS_DRING; i++)
            q->br[i] = q->dr[i] = 0;
        q->mwi = 0;
        q->cand_v = 0;
        q->cand_t = t;
        q->flags |= F_PRIMED | F_FALLING;
        q->decay_t = t + DECAY_START; /* the gap is no reason to lower thresholds */
    }
    int32_t c = dsp_bq_step(&q->clean[0], &c_clean[0], x);
    c = dsp_bq_step(&q->clean[1], &c_clean[1], c);
    q->cr[t & RM] = dsp_sat16(dsp_rshr32(c, 8));
    int32_t b = dsp_bq_step(&q->hp, &c_hp, c);
    b = dsp_bq_step(&q->lp, &c_lp, b);
    int16_t bs = dsp_sat16(dsp_rshr32(b, 8));
    q->br[t & DM] = bs;
    int32_t d = (2 * (int32_t)bs + q->br[(t - 2u) & DM] - q->br[(t - 6u) & DM]
                 - 2 * (int32_t)q->br[(t - 8u) & DM]) >> 2;
    if (d > DERIV_MAX)
        d = DERIV_MAX;
    if (d < -DERIV_MAX)
        d = -DERIV_MAX;
    int32_t dold = q->dr[(t - DSP_QRS_MWI) & DM];
    q->dr[t & DM] = (int16_t)d;
    q->mwi = q->mwi + (uint32_t)(d * d) - (uint32_t)(dold * dold);

    if (q->flags & F_LEARNING) {
        learn(q, bs);
        if (!(q->flags & F_LEARNING))
            end_learning(q, k);
    }

    /* MWI peak tracking */
    uint32_t v = q->mwi;
    if (q->flags & F_FALLING) {
        if (v < q->cand_v) {
            q->cand_v = v;
        } else if (v > q->cand_v) {
            q->flags &= (uint8_t)~F_FALLING;
            q->cand_v = v;
            q->cand_t = t;
        }
    } else if (v > q->cand_v) {
        q->cand_v = v;
        q->cand_t = t;
    } else if (t - q->cand_t >= CONFIRM_N) {
        if (q->flags & F_LEARNING)
            learn_candidate(q);
        else
            classify(q, k);
        q->flags |= F_FALLING;
        q->cand_v = v;
    }

    if (!(q->flags & F_LEARNING)) {
        /* time-based search-back */
        if ((q->flags & (F_HAVE | F_SB)) == (F_HAVE | F_SB)) {
            uint32_t lim = missed_limit(q);
            if (lim && t - q->last_r > lim + SB_MARGIN)
                accept_searchback(q, k);
        }
        /* threshold decay after 2 s without a beat */
        if ((int32_t)(t - q->decay_t) >= 0) {
            q->spki >>= 1;
            q->spkf >>= 1;
            q->npki >>= 1;
            q->npkf >>= 1;
            update_thresholds(q);
            q->decay_t = t + DECAY_EVERY;
        }
    }
    q->t = t + 1u;
}

size_t dsp_qrs_process(dsp_qrs_t *q, const int16_t *x, size_t n, dsp_qrs_beat_t *beats,
                       size_t max_beats)
{
    sink_t k = { 0, max_beats, beats };
    for (size_t i = 0; i < n; i++)
        step(q, x[i], &k);
    return k.n;
}

uint16_t dsp_qrs_hr(const dsp_qrs_t *q)
{
    return q->hr;
}

uint8_t dsp_qrs_rr_spread_pct(const dsp_qrs_t *q)
{
    if (q->rr5_n < 5u)
        return 255u;
    uint32_t mn = q->rr_q8[0], mx = q->rr_q8[0];
    for (unsigned i = 1; i < 5u; i++) {
        if (q->rr_q8[i] < mn)
            mn = q->rr_q8[i];
        if (q->rr_q8[i] > mx)
            mx = q->rr_q8[i];
    }
    uint32_t med = dsp_median5_u32(q->rr_q8);
    uint32_t p = (mx - mn) * 100u / med; /* rr_q8 < 2^24: no overflow */
    return (uint8_t)(p > 254u ? 254u : p);
}

uint32_t dsp_qrs_since_beat(const dsp_qrs_t *q)
{
    return q->t - q->last_beat_t;
}
