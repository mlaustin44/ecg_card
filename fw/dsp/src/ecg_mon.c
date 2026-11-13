#include "dsp_ecg_mon.h"

#include "dsp_util.h"

int32_t dsp_uv_q20(int gain); /* mains.c */

/* ---- Front-end state ------------------------------------------------------ */

static void stats_clear(dsp_lead_stats_t *s)
{
    s->n = 0;
    s->rail = 0;
    s->sum = 0;
    s->min = UINT16_MAX;
    s->max = 0;
}

void dsp_lead_init(dsp_lead_t *l, uint16_t mid_code)
{
    l->base_q9 = 0;
    l->clip = 0;
    l->rail_run = 0;
    l->in_run = 0;
    l->ok_run = 0;
    l->sec_n = 0;
    l->sec_rail = 0;
    l->last_s = 0;
    l->mid = (int16_t)mid_code;
    l->railed = 1;
    l->stable = 0;
    stats_clear(&l->st);
}

uint8_t dsp_lead_push(dsp_lead_t *l, const uint16_t *raw, size_t n, size_t n_blank)
{
    uint8_t ev = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = raw[i];
        bool rail = c < DSP_LEAD_RAIL_LO || c >= DSP_LEAD_RAIL_HI;
        if (l->st.n < (1u << 20)) { /* 262 s: the sum cannot overflow */
            l->st.n++;
            l->st.sum += c;
            l->st.rail += rail;
        }
        if (c < l->st.min)
            l->st.min = c;
        if (c > l->st.max)
            l->st.max = c;
        l->sec_rail += rail;
        if (++l->sec_n == 4000u) {
            l->last_s = l->sec_rail;
            l->sec_rail = 0;
            l->sec_n = 0;
        }
        if (i < n_blank)
            continue; /* FAST_RESTORE: no evidence either way */

        int32_t x = (int32_t)c - l->mid;
        l->base_q9 += (x * 512 - l->base_q9) >> 9;
        if (rail) {
            l->ok_run = 0;
            if (l->rail_run < UINT32_MAX)
                l->rail_run++;
            if (!l->railed) {
                if (l->stable)
                    l->clip++;
                if (l->rail_run > DSP_LEAD_RAIL_RUN) {
                    l->railed = 1;
                    l->in_run = 0;
                    ev |= l->stable ? DSP_LEAD_EV_RAIL | DSP_LEAD_EV_UNSTABLE : DSP_LEAD_EV_RAIL;
                    l->stable = 0;
                } else {
                    l->in_run++; /* a short clip */
                }
            }
        } else {
            if (l->ok_run < UINT16_MAX)
                l->ok_run++;
            if (l->ok_run >= DSP_LEAD_OK_RUN)
                l->rail_run = 0;
            if (l->railed) {
                if (l->ok_run >= DSP_LEAD_OK_RUN) {
                    l->railed = 0;
                    l->in_run = l->ok_run;
                    ev |= DSP_LEAD_EV_INRANGE;
                }
            } else if (l->in_run < UINT32_MAX) {
                l->in_run++;
            }
        }
        if (!l->railed && !l->stable && l->in_run >= DSP_LEAD_STABLE_RUN) {
            l->stable = 1;
            ev |= DSP_LEAD_EV_STABLE;
        }
    }
    return ev;
}

bool dsp_lead_railed(const dsp_lead_t *l) { return l->railed != 0; }
bool dsp_lead_stable(const dsp_lead_t *l) { return l->stable != 0; }
uint32_t dsp_lead_rail_samples(const dsp_lead_t *l) { return l->railed ? l->rail_run : 0u; }
uint32_t dsp_lead_in_samples(const dsp_lead_t *l) { return l->railed ? 0u : l->in_run; }

int16_t dsp_lead_baseline(const dsp_lead_t *l)
{
    return (int16_t)dsp_rshr32(l->base_q9, 9);
}

bool dsp_lead_far_from_mid(const dsp_lead_t *l)
{
    int32_t b = dsp_lead_baseline(l);
    return b > 1024 || b < -1024;
}

uint32_t dsp_lead_take_clip(dsp_lead_t *l)
{
    uint32_t c = l->clip;
    l->clip = 0;
    return c;
}

uint16_t dsp_lead_rail_last_s(const dsp_lead_t *l)
{
    return l->last_s;
}

void dsp_lead_take_stats(dsp_lead_t *l, dsp_lead_stats_t *out)
{
    *out = l->st;
    stats_clear(&l->st);
}

/* ---- Baseline wander ------------------------------------------------------ */

#define WANDER_BLOCK 250u

void dsp_wander_init(dsp_wander_t *w, int gain)
{
    w->acc = 0;
    w->n = 0;
    w->idx = 0;
    w->cnt = 0;
    w->uv_q20 = dsp_uv_q20(gain);
    for (unsigned i = 0; i < 4; i++)
        w->mean[i] = 0;
}

void dsp_wander_push(dsp_wander_t *w, const int16_t *x, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        w->acc += x[i];
        if (++w->n == WANDER_BLOCK) {
            w->mean[w->idx] = (int16_t)(w->acc / (int32_t)WANDER_BLOCK); /* 2 per second */
            w->idx = (uint8_t)((w->idx + 1u) & 3u);
            if (w->cnt < 4u)
                w->cnt++;
            w->acc = 0;
            w->n = 0;
        }
    }
}

uint16_t dsp_wander_uvpp(const dsp_wander_t *w)
{
    if (w->cnt < 4u)
        return 0;
    int32_t mn = w->mean[0], mx = w->mean[0];
    for (unsigned i = 1; i < 4; i++) {
        if (w->mean[i] < mn)
            mn = w->mean[i];
        if (w->mean[i] > mx)
            mx = w->mean[i];
    }
    int64_t uv = dsp_rshr64((int64_t)(mx - mn) * w->uv_q20, 20);
    return uv > 65535 ? 65535u : (uint16_t)uv;
}

/* ---- Quality ---------------------------------------------------------------- */

uint8_t dsp_ecg_quality(const dsp_quality_in_t *in)
{
    if (in->lead_off || in->rail_per_s > 200u)
        return 0;
    int q = 4;
    if (in->rail_per_s > 0u)
        q -= 1;
    if (in->mains_uvpp > 400u)
        q -= 2;
    else if (in->mains_uvpp > 100u)
        q -= 1;
    if (in->wander_uvpp > 1500u)
        q -= 2;
    else if (in->wander_uvpp > 500u)
        q -= 1;
    int settling = in->rr_spread_pct == 255u && in->ms_since_contact < 3000u;
    if (!settling && (in->ms_since_beat > 3000u || in->rr_spread_pct > 30u))
        q -= 1;
    return (uint8_t)(q < 1 ? 1 : q);
}

/* ---- Auto-gain advisor -------------------------------------------------------- */

void dsp_gain_init(dsp_gain_t *g, bool gain_hi)
{
    g->win_ms = 0;
    g->max_amp = 0;
    g->beats = 0;
    g->clip_events = 0;
    g->clipped = 0;
    g->gain_hi = gain_hi ? 1u : 0u;
    g->advice = DSP_GAIN_KEEP;
}

void dsp_gain_beat(dsp_gain_t *g, uint16_t amp)
{
    if (amp > g->max_amp)
        g->max_amp = amp;
    if (g->beats < 255u)
        g->beats++;
    if (g->gain_hi && amp > DSP_GAIN_DOWN_AMP)
        g->advice = DSP_GAIN_DOWN;
}

dsp_gain_advice_t dsp_gain_tick(dsp_gain_t *g, uint16_t elapsed_ms, uint32_t clip_samples)
{
    if (clip_samples) {
        g->clipped = 1;
        if (g->clip_events < 255u)
            g->clip_events++;
        if (g->gain_hi && g->clip_events >= 3u)
            g->advice = DSP_GAIN_DOWN;
    }
    uint32_t w = (uint32_t)g->win_ms + elapsed_ms;
    if (w >= DSP_GAIN_WINDOW_MS) {
        if (!g->gain_hi && g->beats >= 3u && g->max_amp < DSP_GAIN_UP_AMP && !g->clipped)
            g->advice = DSP_GAIN_UP;
        w = 0;
        g->max_amp = 0;
        g->beats = 0;
        g->clip_events = 0;
        g->clipped = 0;
    }
    g->win_ms = (uint16_t)w;
    return (dsp_gain_advice_t)g->advice;
}
