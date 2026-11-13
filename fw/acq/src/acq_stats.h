/* Session mean accumulators of acq.c (internal; header-only so the host can
 * test multi-hour sessions without simulating them).
 *
 * Every session statistic is a saturating (sum, count) pair: once the count
 * reaches UINT16_MAX (or the sum would overflow) further values are ignored,
 * so the mean stays the mean of the first 65535 values instead of wrapping.
 * At one value per beat that is >= 9 h at 120 bpm. */
#ifndef ACQ_STATS_H
#define ACQ_STATS_H

#include <stdint.h>

typedef struct {
    uint32_t sum;
    uint16_t n;
} acq_mean_t;

static inline void acq_mean_add(acq_mean_t *m, uint32_t v)
{
    if (m->n < UINT16_MAX && m->sum <= UINT32_MAX - v) {
        m->sum += v;
        m->n++;
    }
}

/* Rounded mean, 0 if empty. */
static inline uint32_t acq_mean_get(const acq_mean_t *m)
{
    return m->n ? (m->sum + m->n / 2u) / m->n : 0u;
}

/* Rate in bpm from a mean interval in ms (60000 * n fits: n <= 65535). */
static inline uint16_t acq_mean_bpm(const acq_mean_t *ibi_ms)
{
    return ibi_ms->n && ibi_ms->sum
               ? (uint16_t)((60000u * ibi_ms->n + ibi_ms->sum / 2u) / ibi_ms->sum)
               : 0u;
}

/* PAT: accumulated in 16 us units (<= 31250 per value: the sum of 65535
 * values stays < 2^31), mean returned in ms. */
#define ACQ_PAT_UNIT_US 16u
static inline void acq_pat_add(acq_mean_t *m, uint32_t pat_us)
{
    acq_mean_add(m, (pat_us + ACQ_PAT_UNIT_US / 2u) / ACQ_PAT_UNIT_US);
}
static inline uint16_t acq_pat_mean_ms(const acq_mean_t *m)
{
    return m->n ? (uint16_t)((acq_mean_get(m) * ACQ_PAT_UNIT_US + 500u) / 1000u) : 0u;
}

#endif
