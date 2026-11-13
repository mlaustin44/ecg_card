/* Session accumulators (acq_stats.h) over sessions far longer than can be
 * simulated: the old uint32 PAT sum in us overflowed after ~21500 beats
 * (~5 h at 72 bpm) and its uint16 count wrapped at 65536. */
#include "acq_stats.h"
#include "test.h"

TEST(pat_mean_long_session)
{
    acq_mean_t m = {0, 0};
    CHECK_EQ(acq_pat_mean_ms(&m), 0);
    /* 24 h at 72 bpm = 103680 beats of 230 ms: saturates at 65535, mean exact */
    for (uint32_t i = 0; i < 103680u; i++)
        acq_pat_add(&m, 230000u);
    CHECK_EQ(m.n, UINT16_MAX);
    CHECK_EQ(acq_pat_mean_ms(&m), 230);
    /* the worst case the DSP can deliver (DSP_PAT_MAX_US) does not overflow */
    acq_mean_t w = {0, 0};
    for (uint32_t i = 0; i < 70000u; i++)
        acq_pat_add(&w, 500000u);
    CHECK_EQ(w.n, UINT16_MAX);
    CHECK_EQ(acq_pat_mean_ms(&w), 500);
    /* resolution: 16 us units, the ms mean is unaffected */
    acq_mean_t r = {0, 0};
    for (uint32_t i = 0; i < 1000u; i++)
        acq_pat_add(&r, 199000u + (i % 2u) * 2000u); /* mean 200.000 ms */
    CHECK_EQ(acq_pat_mean_ms(&r), 200);
    acq_mean_t r2 = {0, 0};
    acq_pat_add(&r2, 200499u);
    CHECK_EQ(acq_pat_mean_ms(&r2), 200);
    acq_pat_add(&r2, 200499u);
    acq_pat_add(&r2, 201600u);
    CHECK_EQ(acq_pat_mean_ms(&r2), 201); /* 200.866 ms */
}

TEST(interval_and_pi_means)
{
    acq_mean_t ibi = {0, 0}, pi = {0, 0};
    CHECK_EQ(acq_mean_bpm(&ibi), 0);
    for (uint32_t i = 0; i < 200000u; i++) {
        acq_mean_add(&ibi, 2500u); /* the largest accepted interval */
        acq_mean_add(&pi, 65535u); /* PI is capped at 65535 permille by the DSP */
    }
    CHECK_EQ(ibi.n, UINT16_MAX);
    CHECK_EQ(acq_mean_bpm(&ibi), 24);
    CHECK(pi.n > 0 && pi.sum <= UINT32_MAX);
    CHECK_EQ(acq_mean_get(&pi), 65535);
    acq_mean_t a = {0, 0};
    acq_mean_add(&a, 833u);
    acq_mean_add(&a, 834u);
    CHECK_EQ(acq_mean_bpm(&a), 72);
    CHECK_EQ(acq_mean_get(&a), 834); /* 833.5 rounds up */
}

int main(void)
{
    RUN(pat_mean_long_session);
    RUN(interval_and_pi_means);
    return TEST_RESULT();
}
