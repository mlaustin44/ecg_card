/* ECG DMA ring index arithmetic of hal_acq.c, as pure functions (no TI
 * headers, no globals) so the host can test every wrap / overrun / resync
 * case: fw/bsp/tests/test_bsp_ring.c (built by the host preset).
 *
 * Counts. The DMA writes the ring circularly; "DMA count" d = number of
 * samples the DMA has moved since hal_acq_start() (mod 2^32), so sample d
 * sits in ring[d % n]. The timeline index (hal.h) of that sample is d plus
 * the number of conversions lost at ISR time before it (a result that landed
 * while the ADC's DMAEN was off at a lap end, or MEMRES0 overwritten); those
 * losses are only possible at a lap start, so the ISR publishes the
 * cumulative loss per lap ("skew", bsp_ring_skew_*), and a read never
 * crosses a lap boundary. */
#ifndef BSP_RING_H
#define BSP_RING_H

#include <stdbool.h>
#include <stdint.h>

#define BSP_RING_SKEW_LAPS 4u /* laps whose skew is remembered (power of 2) */

typedef struct {
    uint32_t rd;        /* DMA count of the next sample to hand out */
    uint32_t last_w;    /* last write count derived (monotonic check) */
    uint32_t delivered; /* samples handed out since start */
    uint32_t seen_end;  /* timeline index after the last sample handed out */
    uint32_t resyncs;   /* write position found behind the reader */
    uint32_t pos_mismatch; /* DMASZ and DMADA disagreed (diagnostics) */
    bool use_da;        /* DMASZ seen stuck at its programmed value: use DMADA */
    uint32_t lap_skew[BSP_RING_SKEW_LAPS]; /* written by the ISR */
    uint32_t skew;      /* conversions lost at ISR time so far (ISR) */
} bsp_ring_t;

typedef struct {
    uint32_t start;   /* DMA count of the first sample to copy */
    uint32_t count;   /* samples to copy: ring[(start + i) % n] */
    uint32_t dropped; /* overwritten samples skipped by this call */
    bool resync;      /* write position was behind the reader */
} bsp_ring_take_t;

static inline void bsp_ring_reset(bsp_ring_t *r)
{
    *r = (bsp_ring_t){0};
}

/* Position of the DMA inside the ring (0..n-1), or n when a lap has just
 * completed and the size counter has not reloaded yet.
 *   sz     : DMASZ, transfers remaining in this lap. TRM SLAU846 5.2.3.3:
 *            "The DMASZ register is decremented after each transfer"; when it
 *            reaches zero "it is reloaded from its temporary register". This
 *            is the source used.
 *   da_idx : (DMADA - ring base) / 2. For the repeated modes the TRM only says
 *            the addresses are copied to temporary registers which then
 *            advance (for repeated block mode explicitly "the temporary
 *            values ... are incremented"), so DMADA may not read back live.
 *            Cross-check only: a disagreement of more than one transfer (one
 *            may land between the two reads) is counted in pos_mismatch for
 *            bring-up. The one exception is DMASZ stuck at its programmed
 *            value while DMADA moves (DMASZ = n with DMADA more than one
 *            transfer from the base): DMASZ is then evidently not live, and
 *            DMADA is used from then on (latched until bsp_ring_reset()). */
static inline uint32_t bsp_ring_dma_pos(bsp_ring_t *r, uint32_t n, uint32_t sz, uint32_t da_idx)
{
    uint32_t pos = sz <= n ? n - sz : 0u; /* sz > n: garbage, treat as lap start */
    if (da_idx >= n) {
        r->pos_mismatch++;
        return pos;
    }
    uint32_t p = pos % n;
    uint32_t d = da_idx >= p ? da_idx - p : p - da_idx;
    if (d > 1u && d < n - 1u) {
        r->pos_mismatch++;
        if (sz == n)
            r->use_da = true; /* DMASZ stuck at its programmed value */
    }
    return r->use_da ? da_idx : pos;
}

/* Total samples written by the DMA (DMA count), from the ISR's completed-lap
 * counter, the raw "lap done" flag (set, ISR not yet run) and the position.
 * While the done flag is pending the ADC has cleared its DMAEN, so the DMA
 * cannot have advanced into the new lap. pos == n is the same instant. A
 * result below the previous one by less than a lap means a lap end the flag
 * did not show yet (read race): one lap is added. Anything else is left to
 * bsp_ring_take() to resync. */
static inline uint32_t bsp_ring_written(bsp_ring_t *r, uint32_t laps, bool done_pending,
                                        uint32_t pos, uint32_t n)
{
    if (pos >= n) {
        pos = 0u;
        done_pending = true;
    }
    uint32_t w = (laps + (done_pending ? 1u : 0u)) * n + pos;
    int32_t back = (int32_t)(w - r->last_w);
    if (back < 0 && back > -(int32_t)n)
        w += n;
    r->last_w = w;
    return w;
}

/* What the reader may copy now, at most max samples, never across a lap
 * boundary (so one skew applies), keeping `safety` samples away from the
 * write position. Advances r->rd.
 *   - w behind rd ((int32_t)(w - rd) < 0, an unreliable position source):
 *     resync, rd = w, nothing copied (no unsigned wrap to ~4e9);
 *   - more than n - safety samples pending: the oldest are dropped. */
static inline bsp_ring_take_t bsp_ring_take(bsp_ring_t *r, uint32_t w, uint32_t n,
                                            uint32_t safety, uint32_t max)
{
    bsp_ring_take_t t = {r->rd, 0u, 0u, false};
    int32_t avail = (int32_t)(w - r->rd);
    if (avail < 0) {
        r->rd = w;
        r->resyncs++;
        t.start = w;
        t.resync = true;
        return t;
    }
    uint32_t a = (uint32_t)avail;
    if (a > n - safety) {
        t.dropped = a - (n - safety);
        r->rd += t.dropped;
        a -= t.dropped;
        t.start = r->rd;
    }
    uint32_t to_lap_end = n - (r->rd % n);
    uint32_t k = a < max ? a : max;
    if (k > to_lap_end)
        k = to_lap_end;
    t.count = k;
    r->rd += k;
    return t;
}

/* ISR side. Lap end (DMA done) of lap `laps_before` (the completed-lap count
 * before incrementing): `lost` conversions were lost before the new lap's
 * first sample; publish the skew for the new lap. */
static inline void bsp_ring_skew_lap_end(bsp_ring_t *r, uint32_t laps_before, uint32_t lost)
{
    r->skew += lost;
    r->lap_skew[(laps_before + 1u) & (BSP_RING_SKEW_LAPS - 1u)] = r->skew;
}

/* ISR side. A conversion lost inside the current lap `laps` (overflow while
 * no lap end is pending): later samples of this lap move up by one. */
static inline void bsp_ring_skew_now(bsp_ring_t *r, uint32_t laps, uint32_t lost)
{
    r->skew += lost;
    r->lap_skew[laps & (BSP_RING_SKEW_LAPS - 1u)] = r->skew;
}

/* Timeline index of DMA count d (valid for the lap being read and the one
 * after it). */
static inline uint32_t bsp_ring_index(const bsp_ring_t *r, uint32_t d, uint32_t n)
{
    return d + r->lap_skew[(d / n) & (BSP_RING_SKEW_LAPS - 1u)];
}

/* Bookkeeping after a take of `count` samples starting at timeline index
 * first: hal_ecg_overruns() = seen_end - delivered. */
static inline void bsp_ring_delivered(bsp_ring_t *r, uint32_t first, uint32_t count)
{
    if (count == 0u)
        return;
    r->delivered += count;
    if ((int32_t)(first + count - r->seen_end) > 0)
        r->seen_end = first + count;
}

static inline uint32_t bsp_ring_overruns(const bsp_ring_t *r)
{
    return r->seen_end - r->delivered;
}

#endif
