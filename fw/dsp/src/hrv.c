#include "dsp_hrv.h"

#include "dsp_util.h"

#define RR_MIN_MS 273u
#define RR_MAX_MS 2000u

void dsp_hrv_init(dsp_hrv_t *h)
{
    h->sum_d2 = h->sum_sd2 = 0;
    h->sum_nn = 0;
    h->sum_d = 0;
    h->beats = h->n_nn = h->n_sd = 0;
    h->ref = h->prev_nn = 0;
    h->hr_min = h->hr_max = 0;
    h->raw_n = h->raw_i = h->nn5_n = h->nn5_i = 0;
}

static uint16_t bpm(uint32_t ms)
{
    return ms ? (uint16_t)((60000u + ms / 2u) / ms) : 0u;
}

int dsp_hrv_add_beat(dsp_hrv_t *h, uint16_t rr)
{
    if (h->beats < 65535u)
        h->beats++;
    if (rr == 0) {
        /* no interval: restart the chain and the outlier reference */
        h->prev_nn = 0;
        h->raw_n = h->raw_i = 0;
        return 0;
    }
    int ok = rr >= RR_MIN_MS && rr <= RR_MAX_MS;
    if (ok && h->raw_n >= 3u) {
        uint32_t ref = dsp_median_u16(h->raw, h->raw_n);
        uint32_t tol = ref / 5u;
        ok = rr + tol >= ref && rr <= ref + tol;
    }
    h->raw[h->raw_i] = rr;
    h->raw_i = (uint8_t)((h->raw_i + 1u) % 5u);
    if (h->raw_n < 5u)
        h->raw_n++;
    if (!ok) {
        h->prev_nn = 0;
        return 0;
    }
    if (h->n_nn == 0)
        h->ref = rr;
    if (h->n_nn < 65535u) {
        int32_t d = (int32_t)rr - h->ref;
        h->n_nn++;
        h->sum_nn += rr;
        h->sum_d += d;
        h->sum_d2 += (uint64_t)((int64_t)d * d);
        if (h->prev_nn) {
            int32_t sd = (int32_t)rr - h->prev_nn;
            h->sum_sd2 += (uint64_t)((int64_t)sd * sd);
            h->n_sd++;
        }
    }
    h->prev_nn = rr;
    h->nn5[h->nn5_i] = rr;
    h->nn5_i = (uint8_t)((h->nn5_i + 1u) % 5u);
    if (h->nn5_n < 5u)
        h->nn5_n++;
    if (h->nn5_n == 5u) {
        uint16_t hr = bpm(dsp_median_u16(h->nn5, 5));
        if (h->hr_min == 0 || hr < h->hr_min)
            h->hr_min = hr;
        if (hr > h->hr_max)
            h->hr_max = hr;
    }
    return 1;
}

void dsp_hrv_summary(const dsp_hrv_t *h, dsp_hrv_summary_t *o)
{
    o->beats = h->beats;
    o->nn_count = h->n_nn;
    o->hr_avg = h->n_nn ? bpm((h->sum_nn + h->n_nn / 2u) / h->n_nn) : 0u;
    o->hr_min = h->hr_min;
    o->hr_max = h->hr_max;
    o->sdnn_ms = 0;
    o->rmssd_ms = 0;
    if (h->n_nn >= 2u) {
        /* var = (n S2 - S1^2) / (n (n - 1)) */
        uint64_t n = h->n_nn;
        int64_t s1 = h->sum_d;
        uint64_t num = n * h->sum_d2 - (uint64_t)(s1 * s1);
        uint32_t den = (uint32_t)(n * (n - 1u));
        uint32_t var_q8 = dsp_udiv64(num * 256u + den / 2u, den); /* ms^2 * 256 */
        o->sdnn_ms = (uint16_t)((dsp_isqrt64(var_q8) + 8u) >> 4);
    }
    if (h->n_sd >= 1u) {
        uint32_t ms_q8 = dsp_udiv64(h->sum_sd2 * 256u + h->n_sd / 2u, h->n_sd);
        o->rmssd_ms = (uint16_t)((dsp_isqrt64(ms_q8) + 8u) >> 4);
    }
}
