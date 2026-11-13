/* Host test of the BSP ECG ring index arithmetic (fw/bsp/bsp_ring.h): the
 * individual wrap / overrun / resync cases, then a model of the hardware
 * (ADC0 -> DMA repeat-single into ring[512], the ADC clearing DMAEN at each
 * lap end, the ISR re-arming it with some latency, a reader that stalls for
 * 150 ms) driving exactly the calls hal_acq.c makes. Every delivered sample
 * carries its true index as its value, so the timeline is checked exactly. */
#include <stdlib.h>
#include <string.h>

#include "bsp_ring.h"
#include "test.h"

#define N    512u
#define SAFE 16u

TEST(written_wrap_and_pending_flag)
{
    bsp_ring_t r;
    bsp_ring_reset(&r);
    CHECK_EQ(bsp_ring_written(&r, 0, false, 0, N), 0);
    CHECK_EQ(bsp_ring_written(&r, 0, false, 511, N), 511);
    /* lap end, ISR not yet run: flag pending, position back at 0 */
    CHECK_EQ(bsp_ring_written(&r, 0, true, 0, N), 512);
    /* DMASZ read as 0 (before its reload): same instant, counted once */
    CHECK_EQ(bsp_ring_written(&r, 0, false, N, N), 512);
    CHECK_EQ(bsp_ring_written(&r, 0, true, N, N), 512);
    /* ISR ran */
    CHECK_EQ(bsp_ring_written(&r, 1, false, 3, N), 515);
    /* position wrapped but neither flag nor lap count show it yet (read
     * race): one lap added, never a backward step */
    bsp_ring_reset(&r);
    CHECK_EQ(bsp_ring_written(&r, 0, false, 510, N), 510);
    CHECK_EQ(bsp_ring_written(&r, 0, false, 2, N), 514);
    /* a far backward value is left for take() to resync */
    bsp_ring_reset(&r);
    CHECK_EQ(bsp_ring_written(&r, 10, false, 0, N), 5120);
    CHECK_EQ(bsp_ring_written(&r, 2, false, 0, N), 1024);
}

TEST(dma_pos_source_and_cross_check)
{
    bsp_ring_t r;
    bsp_ring_reset(&r);
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N, 0), 0);       /* lap start */
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N - 7, 7), 7);
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N - 7, 8), 7);   /* one transfer between the reads */
    CHECK_EQ(bsp_ring_dma_pos(&r, N, 1, 0), 511);     /* DMADA wrapped first */
    CHECK_EQ(bsp_ring_dma_pos(&r, N, 0, 0), N);       /* before the reload */
    CHECK_EQ(r.pos_mismatch, 0);
    /* DMADA not live (stays at the base): DMASZ still used, counted */
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N - 100, 0), 100);
    CHECK_EQ(r.pos_mismatch, 1);
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N - 100, 9999), 100); /* outside the ring */
    CHECK_EQ(r.pos_mismatch, 2);
    CHECK_EQ(bsp_ring_dma_pos(&r, N, 0xFFFFu, 5), 0);  /* garbage size */
    /* DMASZ stuck at its programmed value while DMADA moves: DMADA used,
     * from then on */
    bsp_ring_reset(&r);
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N, 1), 0); /* not yet distinguishable */
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N, 200), 200);
    CHECK_EQ(r.pos_mismatch, 1);
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N, 1), 1);
    CHECK_EQ(bsp_ring_dma_pos(&r, N, N, 0), 0);
}

TEST(take_overrun_lap_boundary_resync)
{
    bsp_ring_t r;
    bsp_ring_reset(&r);
    bsp_ring_take_t t = bsp_ring_take(&r, 100, N, SAFE, 64);
    CHECK(t.start == 0 && t.count == 64 && t.dropped == 0 && !t.resync);
    t = bsp_ring_take(&r, 100, N, SAFE, 64);
    CHECK(t.start == 64 && t.count == 36);
    t = bsp_ring_take(&r, 100, N, SAFE, 64);
    CHECK(t.count == 0 && !t.resync);
    /* never across a lap boundary */
    t = bsp_ring_take(&r, 590, N, SAFE, 1000);
    CHECK(t.start == 100 && t.count == 412 && t.dropped == 0);
    t = bsp_ring_take(&r, 590, N, SAFE, 1000);
    CHECK(t.start == 512 && t.count == 78);
    /* reader 2010 behind: the oldest are dropped, N - SAFE kept */
    t = bsp_ring_take(&r, 2600, N, SAFE, 1000);
    CHECK_EQ(t.dropped, 2600 - 590 - (N - SAFE));
    CHECK_EQ(t.start, 2600 - (N - SAFE));
    CHECK_EQ(t.count, N - (t.start % N));
    /* write position behind the reader: resync, no unsigned wrap */
    bsp_ring_reset(&r);
    r.rd = 5000;
    t = bsp_ring_take(&r, 4990, N, SAFE, 64);
    CHECK(t.resync && t.count == 0 && t.dropped == 0);
    CHECK_EQ(r.rd, 4990);
    CHECK_EQ(r.resyncs, 1);
    t = bsp_ring_take(&r, 5000, N, SAFE, 64);
    CHECK(t.start == 4990 && t.count == 10);
}

TEST(index_skew_and_overruns)
{
    bsp_ring_t r;
    bsp_ring_reset(&r);
    /* 2 conversions lost at the end of lap 0, 1 inside lap 1 later */
    bsp_ring_skew_lap_end(&r, 0, 2);
    CHECK_EQ(bsp_ring_index(&r, 511, N), 511);
    CHECK_EQ(bsp_ring_index(&r, 512, N), 514);
    bsp_ring_skew_now(&r, 1, 1);
    CHECK_EQ(bsp_ring_index(&r, 700, N), 703);
    bsp_ring_skew_lap_end(&r, 1, 0);
    CHECK_EQ(bsp_ring_index(&r, 1024, N), 1027);
    /* overruns = timeline passed - delivered */
    bsp_ring_delivered(&r, 0, 512);
    bsp_ring_delivered(&r, 514, 100);
    CHECK_EQ(bsp_ring_overruns(&r), 2);
    bsp_ring_delivered(&r, 614, 0);
    CHECK_EQ(bsp_ring_overruns(&r), 2);
}

TEST(counts_wrap_modulo_2_32)
{
    bsp_ring_t r;
    bsp_ring_reset(&r);
    uint32_t laps = 0xFFFFFFFFu / N; /* last lap before the 32-bit wrap */
    r.last_w = laps * N;
    r.rd = laps * N + 400;
    uint32_t w = bsp_ring_written(&r, laps, false, 500, N);
    CHECK_EQ(w, laps * N + 500);
    w = bsp_ring_written(&r, laps, true, 0, N); /* wraps to 0 */
    CHECK_EQ(w, 0);
    bsp_ring_take_t t = bsp_ring_take(&r, w, N, SAFE, 1000);
    CHECK(!t.resync && t.count == 112 && t.dropped == 0);
    w = bsp_ring_written(&r, laps + 1u, false, 30, N);
    CHECK_EQ(w, 30);
    t = bsp_ring_take(&r, w, N, SAFE, 1000);
    CHECK(!t.resync && t.start == 0 && t.count == 30);
}

/* ---- hardware model ------------------------------------------------------ */
static struct {
    uint16_t ring[N];
    uint32_t pos;        /* DMA position in the ring */
    bool done_ris;       /* ADC DMADONE raw flag */
    bool ovf_ris;        /* ADC OVIFG raw flag (a flag, not a counter) */
    bool adc_dmaen;
    bool memres;         /* MEMRES0 loaded, not moved */
    bool da_live;        /* DMADA reads back live (else stuck at the base) */
    bool sz_stuck;       /* DMASZ reads its programmed value (not live) */
    uint32_t laps;       /* ISR's completed-lap count */
    uint32_t truth;      /* next true sample index */
    unsigned isr_wait, isr_delay; /* conversions the pending ISR still waits */
    bsp_ring_t r;
} H;

static void conversion(void)
{
    uint16_t v = (uint16_t)H.truth++;
    if (H.adc_dmaen) {
        H.ring[H.pos++] = v;
        if (H.pos == N) {
            H.pos = 0;
            H.done_ris = true;
            H.adc_dmaen = false; /* the ADC clears its own DMAEN */
        }
    } else {
        if (H.memres)
            H.ovf_ris = true;
        H.memres = true;
    }
}

/* hal_acq.c ADC0_IRQHandler: OVIFG has the higher IIDX priority. */
static void isr(void)
{
    for (;;) {
        if (H.ovf_ris) {
            H.ovf_ris = false;
            if (H.done_ris)
                H.r.skew++;
            else
                bsp_ring_skew_now(&H.r, H.laps, 1);
        } else if (H.done_ris) {
            H.done_ris = false;
            uint32_t lost = H.memres ? 1u : 0u;
            H.memres = false;
            bsp_ring_skew_lap_end(&H.r, H.laps, lost);
            H.adc_dmaen = true;
            H.laps++;
        } else {
            break;
        }
    }
}

/* hal_acq.c hal_ecg_read */
static size_t reader(uint16_t *dst, size_t max, uint32_t *first)
{
    uint32_t sz = H.sz_stuck ? N : N - H.pos;
    uint32_t da = H.da_live ? H.pos : 0;
    uint32_t pos = bsp_ring_dma_pos(&H.r, N, sz, da);
    uint32_t w = bsp_ring_written(&H.r, H.laps, H.done_ris, pos, N);
    bsp_ring_take_t t = bsp_ring_take(&H.r, w, N, SAFE, (uint32_t)max);
    uint32_t f = bsp_ring_index(&H.r, t.start, N);
    bsp_ring_delivered(&H.r, f, t.count);
    for (uint32_t i = 0; i < t.count; i++)
        dst[i] = H.ring[(t.start + i) & (N - 1u)];
    if (t.count)
        *first = f;
    return t.count;
}

/* isr_late: ISR latency after a lap end is 0..2 conversions at random (the
 * deadline is one conversion; one miss is detected via MEMRES0, a second via
 * OVIFG; more than two cannot be counted by a flag: a hardware limit of this
 * design, documented in hal_acq.c). */
static void model_run(unsigned seed, bool da_live, bool sz_stuck, bool isr_late,
                      uint32_t *misplaced, uint32_t *samples, uint32_t *lost, uint32_t *stalls)
{
    memset(&H, 0, sizeof H);
    H.adc_dmaen = true;
    H.da_live = da_live;
    H.sz_stuck = sz_stuck;
    srand(seed);
    uint32_t next = 0;
    uint16_t buf[64];
    *misplaced = *samples = *lost = *stalls = 0;
    for (long step = 0; step < 2000000; step++) {
        conversion();
        if (H.done_ris || H.ovf_ris) {
            if (H.isr_wait >= H.isr_delay) {
                isr();
                H.isr_wait = 0;
                H.isr_delay = isr_late ? (unsigned)(rand() % 3) : 0u;
            } else {
                H.isr_wait++;
            }
        }
        if (rand() % 1000 < 40) { /* main loop polls: drain like acq.c */
            for (int c = 0; c < 32; c++) {
                uint32_t first;
                size_t n = reader(buf, 64, &first);
                if (n == 0)
                    break;
                if ((int32_t)(first - next) < 0)
                    (*misplaced)++; /* indices never go back without a resync */
                *lost += first - next;
                for (size_t i = 0; i < n; i++)
                    if (buf[i] != (uint16_t)(first + i))
                        (*misplaced)++;
                *samples += (uint32_t)n;
                next = first + (uint32_t)n;
                if (next != H.r.delivered + bsp_ring_overruns(&H.r))
                    (*misplaced)++;
            }
        }
        if (rand() % 100000 == 0) { /* reader stalls 150 ms = 600 samples */
            (*stalls)++;
            for (int s = 0; s < 600; s++) {
                conversion();
                if (H.done_ris || H.ovf_ris)
                    isr();
            }
        }
    }
}

TEST(model_timeline_exact)
{
    for (int variant = 0; variant < 6; variant++) {
        uint32_t bad, n, lost, stalls;
        /* 0..3: DMASZ live, DMADA live or not; 4..5: DMASZ stuck, DMADA live */
        bool da_live = (variant & 1) || variant >= 4, late = (variant & 2) || variant == 5;
        bool sz_stuck = variant >= 4;
        model_run(1u + (unsigned)variant, da_live, sz_stuck, late, &bad, &n, &lost, &stalls);
        printf("  DMASZ %s, DMADA %s, ISR %s: %u samples, %u lost (%u stalls), %u misplaced, "
               "%u resyncs, %u position mismatches\n",
               sz_stuck ? "stuck" : "live ", da_live ? "live " : "stuck", late ? "late  " : "prompt", n,
               lost, stalls, bad, H.r.resyncs, H.r.pos_mismatch);
        CHECK_EQ(bad, 0);
        CHECK(stalls >= 10);
        CHECK(lost >= stalls * (600u - (N - SAFE)));
        CHECK_EQ(lost, bsp_ring_overruns(&H.r));
        CHECK_EQ(H.r.resyncs, 0);
        CHECK(n + lost == H.r.seen_end);
        if (da_live && !sz_stuck)
            CHECK_EQ(H.r.pos_mismatch, 0);
        else
            CHECK(H.r.pos_mismatch > 0);
        if (!late)
            CHECK(H.r.skew == 0);
        else
            CHECK(H.r.skew > 0); /* late ISRs did lose conversions, all placed */
    }
}

int main(void)
{
    RUN(written_wrap_and_pending_flag);
    RUN(dma_pos_source_and_cross_check);
    RUN(take_overrun_lap_boundary_resync);
    RUN(index_skew_and_overruns);
    RUN(counts_wrap_modulo_2_32);
    RUN(model_timeline_exact);
    return TEST_RESULT();
}
