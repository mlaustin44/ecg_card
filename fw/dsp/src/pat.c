#include "dsp_pat.h"

#include "board.h"
#include "dsp_delays.h"

void dsp_pat_init(dsp_pat_t *p)
{
    p->r_n = p->r_i = p->f_n = p->f_i = p->o_n = p->o_i = 0;
    p->rejected = 0;
}

uint32_t dsp_pat_r_time_us(uint32_t index, uint8_t frac_q8)
{
    return index * 2000u + (((uint32_t)frac_q8 * 125u + 8u) >> 4)
         + (uint32_t)(int32_t)(DSP_ECG_STREAM_T0_US - DSP_ECG_ANALOG_DELAY_US);
}

uint32_t dsp_pat_foot_time_us(uint32_t frame, uint8_t frac_q8)
{
    return frame * 10000u + (((uint32_t)frac_q8 * 625u + 8u) >> 4)
         + (uint32_t)(PPG_T_ON_SAMPLE_US - DSP_PPG_ANALOG_DELAY_US);
}

uint32_t dsp_pat_foot_ecg_index(uint32_t frame, uint8_t frac_q8)
{
    /* t = m * 2000 + DSP_ECG_STREAM_T0_US  =>  m = (t - T0) / 2000 */
    uint32_t t = dsp_pat_foot_time_us(frame, frac_q8) - (uint32_t)(int32_t)DSP_ECG_STREAM_T0_US;
    return (t + 1000u) / 2000u; /* per pulse */
}

/* R-peak k back from the newest (0 = newest). */
static uint32_t r_at(const dsp_pat_t *p, unsigned k)
{
    return p->r_us[(p->r_i + 3u - k) & 3u];
}

static void push_out(dsp_pat_t *p, uint32_t v, uint32_t foot)
{
    if (p->o_n == 4u) { /* drop the oldest */
        p->o_i = (uint8_t)((p->o_i + 1u) & 3u);
        p->o_n--;
    }
    p->out_us[(p->o_i + p->o_n) & 3u] = v;
    p->out_foot[(p->o_i + p->o_n) & 3u] = foot;
    p->o_n++;
}

/* Try to resolve the oldest pending foot. Returns false if it must wait. */
static bool resolve(dsp_pat_t *p)
{
    uint32_t tf = p->foot_us[p->f_i];
    if (p->r_n == 0 || (int32_t)(r_at(p, 0) - tf) <= 0)
        return false; /* no R-peak after the foot yet */
    /* latest R before the foot: k such that r[k] < tf <= r[k-1] */
    unsigned k = 1;
    while (k < p->r_n && (int32_t)(r_at(p, k) - tf) >= 0)
        k++;
    bool ok = false;
    if (k < p->r_n) {
        uint32_t tr = r_at(p, k);
        int32_t pat = (int32_t)(tf - tr);
        uint32_t rr = r_at(p, k - 1u) - tr;
        ok = pat >= DSP_PAT_MIN_US && pat <= DSP_PAT_MAX_US;
        if (ok && k + 1u < p->r_n) {
            uint32_t rr_prev = tr - r_at(p, k + 1u);
            ok = rr <= rr_prev + rr_prev / 2u;
        }
        if (ok)
            push_out(p, (uint32_t)pat, tf);
    }
    if (!ok && p->rejected < 65535u)
        p->rejected++;
    p->f_i = (uint8_t)((p->f_i + 1u) & 3u);
    p->f_n--;
    return true;
}

void dsp_pat_add_r(dsp_pat_t *p, uint32_t index, uint8_t frac_q8)
{
    p->r_i = (uint8_t)((p->r_i + 1u) & 3u);
    p->r_us[(p->r_i + 3u) & 3u] = dsp_pat_r_time_us(index, frac_q8);
    if (p->r_n < 4u)
        p->r_n++;
    while (p->f_n && resolve(p)) {
    }
}

void dsp_pat_add_foot(dsp_pat_t *p, uint32_t frame, uint8_t frac_q8)
{
    if (p->f_n == 4u) { /* stale: ECG not delivering, drop the oldest */
        p->f_i = (uint8_t)((p->f_i + 1u) & 3u);
        p->f_n--;
    }
    p->foot_us[(p->f_i + p->f_n) & 3u] = dsp_pat_foot_time_us(frame, frac_q8);
    p->f_n++;
    while (p->f_n && resolve(p)) {
    }
}

bool dsp_pat_pop(dsp_pat_t *p, uint32_t *pat_us, uint32_t *foot_us)
{
    if (p->o_n == 0)
        return false;
    *pat_us = p->out_us[p->o_i];
    if (foot_us)
        *foot_us = p->out_foot[p->o_i];
    p->o_i = (uint8_t)((p->o_i + 1u) & 3u);
    p->o_n--;
    return true;
}
