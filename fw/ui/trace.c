#include "trace.h"

#include <string.h>

#include "gfx.h"

const uint16_t trace_ecg_steps_uv[ECG_STEPS] = {500, 1000, 1500, 2000, 3000, 5000, 7500, 10000};
/* the largest 1-2-5 value not above half of each step */
static const uint16_t ecg_bar_uv[ECG_STEPS] = {200, 500, 500, 1000, 1000, 2000, 2000, 5000};

static uint16_t ref_uv;  /* median QRS, uV (0: unknown) */
static bool ref_down;    /* R waves point down */

void ui_trace_ecg_ref(uint16_t qrs_uv, bool r_down)
{
    ref_uv = qrs_uv;
    ref_down = r_down;
}

#define PPG_RANGE_DEFAULT 4096
#define PPG_RANGE_MIN     16

static trace_state_t tr_[UI_TRACE_COUNT];

const trace_state_t *trace_state(ui_trace_t t)
{
    return &tr_[t < UI_TRACE_COUNT ? t : 0];
}

static void set_range(trace_state_t *s, int32_t range)
{
    if (range < PPG_RANGE_MIN)
        range = PPG_RANGE_MIN;
    if (range > 0x7FFF * 2)
        range = 0x7FFF * 2;
    s->range = range;
    s->k = (int32_t)((256u << 16) / (uint32_t)range);
}

void ui_trace_reset(uint16_t sweep_time_ms)
{
    if (sweep_time_ms < 100u)
        sweep_time_ms = 3000u;
    for (int t = 0; t < UI_TRACE_COUNT; t++) {
        trace_state_t *s = &tr_[t];
        memset(s, 0, sizeof *s);
        memset(s->lo, 0xFF, sizeof s->lo);
        uint32_t fs = t == UI_TRACE_ECG ? ECG_HZ : PPG_HZ;
        s->n_sweep = (fs * sweep_time_ms + 500u) / 1000u;
        if (s->n_sweep == 0)
            s->n_sweep = 1;
        if (t == UI_TRACE_ECG) {
            s->step = ECG_STEP_DEFAULT;
            set_range(s, trace_ecg_steps_uv[s->step]);
            s->center = (int32_t)((uint32_t)s->range / 6u); /* baseline in the lower third */
        } else {
            set_range(s, PPG_RANGE_DEFAULT);
        }
    }
}

static inline uint8_t norm(const trace_state_t *s, int32_t x, bool *clip)
{
    int32_t d = x - s->center;
    if (d > s->range)
        d = s->range;
    else if (d < -s->range)
        d = -s->range;
    int32_t v = 128 + ((d * s->k) >> 16);
    if (v < 0) {
        *clip = true;
        return 0;
    }
    if (v > 255) {
        *clip = true;
        return 255;
    }
    return (uint8_t)v;
}

static void rescale_ecg(trace_state_t *s)
{
    int32_t need, comfy_need;
    if (ref_uv) {
        /* QRS at most 75 % of the height; a smaller step only with the QRS
         * at most 70 % of it (the median of 5 beats barely jitters) */
        need = ((int32_t)ref_uv * 4 + 2) / 3;
        comfy_need = ((int32_t)ref_uv * 10 + 6) / 7;
    } else {
        int32_t p2p = (int32_t)s->smax - s->smin;
        need = p2p + p2p / 4 + 1; /* 25 % headroom */
        comfy_need = need + need / 4;
    }
    uint8_t fit = 0, comfy = 0;
    while (fit < ECG_STEPS - 1 && trace_ecg_steps_uv[fit] < need)
        fit++;
    /* a smaller step is taken only with room to spare, two sweeps running */
    while (comfy < ECG_STEPS - 1 && trace_ecg_steps_uv[comfy] < comfy_need)
        comfy++;
    uint8_t old = s->step;
    if (s->wraps == 1 && !ref_uv) {
        s->step = fit;
        s->down = 0;
    } else if (fit > s->step) {
        s->step = fit;
        s->down = 0;
    } else if (comfy < s->step) {
        if (++s->down >= 2) {
            s->step = comfy;
            s->down = 0;
        }
    } else {
        s->down = 0;
    }
    int32_t range = trace_ecg_steps_uv[s->step];
    int32_t mid;
    if (ref_uv && s->cnt) {
        /* baseline (the sweep mean) at 30 % of the height from the side the
         * R waves start from */
        int32_t mean = s->sum / (int32_t)s->cnt;
        mid = ref_down ? mean - range / 5 : mean + range / 5;
    } else {
        mid = ((int32_t)s->smax + s->smin) / 2;
    }
    int32_t dc = mid - s->center;
    int32_t tol = ref_uv ? range / 16 : range / 8;
    if (s->step != old || s->wraps == 1 || dc > tol || dc < -tol)
        s->center = mid;
    set_range(s, range);
}

static void rescale_ppg(trace_state_t *s)
{
    int32_t p2p = (int32_t)s->smax - s->smin;
    if (p2p < PPG_RANGE_MIN / 2)
        p2p = PPG_RANGE_MIN / 2;
    int32_t mid = ((int32_t)s->smax + s->smin) / 2;
    if (s->wraps == 1 || p2p * 10 > s->range * 9 || p2p * 5 < s->range * 2) {
        set_range(s, p2p + p2p / 2); /* about two thirds of the height */
        s->center = mid;
    } else {
        int32_t dc = mid - s->center;
        if (dc > s->range / 8 || dc < -s->range / 8)
            s->center = mid;
    }
}

static void advance(trace_state_t *s, ui_trace_t t)
{
    if (!s->col_has) {
        /* a column without samples (sweep faster than the sample rate):
         * hold the previous level so the line stays continuous */
        uint16_t prev = s->pen ? (uint16_t)(s->pen - 1u) : (uint16_t)(TRACE_W - 1);
        if (s->lo[prev] <= s->hi[prev]) {
            uint8_t v = s->hi[prev];
            s->lo[s->pen] = s->hi[s->pen] = v;
        }
    }
    if (++s->pen >= TRACE_W) {
        s->pen = 0;
        if (s->wraps < 0xFFFF)
            s->wraps++;
        if (s->any) {
            if (t == UI_TRACE_ECG)
                rescale_ecg(s);
            else
                rescale_ppg(s);
        }
        s->any = false;
        s->clipped = 0;
        s->sum = 0;
        s->cnt = 0;
    } else if (s->clipped && s->wraps == 0 && t == UI_TRACE_PPG) {
        /* first PPG sweep: open the range as soon as the signal clips */
        int32_t p2p = (int32_t)s->smax - s->smin;
        set_range(s, p2p + p2p / 2);
        s->center = ((int32_t)s->smax + s->smin) / 2;
        s->clipped = 0;
    }
    uint16_t ahead = (uint16_t)(s->pen + TRACE_GAP);
    if (ahead >= TRACE_W)
        ahead = (uint16_t)(ahead - TRACE_W);
    s->lo[ahead] = 0xFF;
    s->hi[ahead] = 0;
    s->lo[s->pen] = 0xFF; /* fresh column (already clear except after reset) */
    s->hi[s->pen] = 0;
    s->col_has = false;
}

void ui_trace_gap(ui_trace_t t, uint16_t n)
{
    if (t >= UI_TRACE_COUNT)
        return;
    trace_state_t *s = &tr_[t];
    if (!s->n_sweep)
        ui_trace_reset(3000u);
    for (uint16_t i = 0; i < n; i++) {
        s->acc += TRACE_W;
        while (s->acc >= s->n_sweep) {
            s->acc -= s->n_sweep;
            s->col_has = true; /* leave the column empty: no hold-previous fill */
            advance(s, t);
        }
    }
}

void ui_trace_push(ui_trace_t t, const int16_t *samples, uint16_t n)
{
    if (t >= UI_TRACE_COUNT || !samples)
        return;
    trace_state_t *s = &tr_[t];
    if (!s->n_sweep)
        ui_trace_reset(3000u);
    for (uint16_t i = 0; i < n; i++) {
        int16_t x = samples[i];
        if (!s->any) {
            s->smin = s->smax = x;
            s->any = true;
        } else if (x < s->smin) {
            s->smin = x;
        } else if (x > s->smax) {
            s->smax = x;
        }
        if (s->cnt < UINT16_MAX) {
            s->sum += x;
            s->cnt++;
        }
        bool clip = false;
        uint8_t v = norm(s, x, &clip);
        if (clip)
            s->clipped = 1;
        uint16_t c = s->pen;
        if (!s->col_has) {
            uint8_t lo = v, hi = v;
            if (c > 0) {
                /* join the previous column halfway, so steep edges are a
                 * continuous line and not a staircase of dots */
                uint8_t plo = s->lo[c - 1], phi = s->hi[c - 1];
                if (plo <= phi) {
                    if (v > phi + 1) {
                        uint8_t mid = (uint8_t)((phi + v) >> 1);
                        s->hi[c - 1] = mid;
                        lo = (uint8_t)(mid + 1);
                    } else if (v + 1 < plo) {
                        uint8_t mid = (uint8_t)((plo + v + 1) >> 1);
                        s->lo[c - 1] = mid;
                        hi = (uint8_t)(mid - 1);
                    }
                }
            }
            s->lo[c] = lo;
            s->hi[c] = hi;
            s->col_has = true;
        } else {
            if (v < s->lo[c])
                s->lo[c] = v;
            if (v > s->hi[c])
                s->hi[c] = v;
        }
        s->acc += TRACE_W;
        while (s->acc >= s->n_sweep) {
            s->acc -= s->n_sweep;
            advance(s, t);
        }
    }
}

uint16_t trace_bar_uv(void)
{
    return ecg_bar_uv[tr_[UI_TRACE_ECG].step < ECG_STEPS ? tr_[UI_TRACE_ECG].step : 0];
}

int trace_bar_px(int h)
{
    const trace_state_t *s = &tr_[UI_TRACE_ECG];
    if (s->range <= 0 || h <= 0)
        return 0;
    return (int)(((uint32_t)h * trace_bar_uv() + (uint32_t)s->range / 2u) / (uint32_t)s->range);
}

void trace_draw(ui_trace_t t, int y0, int h)
{
    if (t >= UI_TRACE_COUNT || h < 3)
        return;
    const trace_state_t *s = &tr_[t];
    /* values map to rows y0 .. y0+h-2; the pen adds one row below */
    uint32_t mult = (uint32_t)(h - 2) * 257u;
    int ybase = y0 + h - 2;
    for (int x = 0; x < TRACE_W; x++) {
        uint8_t lo = s->lo[x], hi = s->hi[x];
        if (lo > hi)
            continue;
        int yt = ybase - (int)((hi * mult + 32768u) >> 16);
        int yb = ybase - (int)((lo * mult + 32768u) >> 16);
        /* 2 x 2 pen: a 1-px line is a 0.17 mm hairline on this panel */
        gfx_vline(x, yt, yb + 1, GFX_SET);
        gfx_vline(x + 1, yt, yb + 1, GFX_SET);
    }
}
