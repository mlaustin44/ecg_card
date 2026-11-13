/* Session statistics: exact values, outlier rejection, bounded state. */
#include <math.h>

#include "dsp_hrv.h"
#include "synth.h"
#include "test.h"

static void truth(const uint16_t *rr, int n, double *sdnn, double *rmssd, double *hr)
{
    double s = 0, s2 = 0, d2 = 0;
    for (int i = 0; i < n; i++) {
        s += rr[i];
        s2 += (double)rr[i] * rr[i];
        if (i)
            d2 += ((double)rr[i] - rr[i - 1]) * ((double)rr[i] - rr[i - 1]);
    }
    double m = s / n;
    *sdnn = sqrt((s2 - n * m * m) / (n - 1));
    *rmssd = sqrt(d2 / (n - 1));
    *hr = 60000.0 / m;
}

TEST(exact_values)
{
    synth_rng_t r;
    synth_rng_seed(&r, 1);
    static uint16_t rr[3000];
    for (int i = 0; i < 3000; i++)
        rr[i] = (uint16_t)lround(850 + 40 * sin(i * 0.4) + 15 * synth_gauss(&r));
    dsp_hrv_t h;
    dsp_hrv_init(&h);
    dsp_hrv_add_beat(&h, 0);
    int acc = 0;
    for (int i = 0; i < 3000; i++)
        acc += dsp_hrv_add_beat(&h, rr[i]);
    dsp_hrv_summary_t s;
    dsp_hrv_summary(&h, &s);
    double sdnn, rmssd, hr;
    truth(rr, 3000, &sdnn, &rmssd, &hr);
    printf("  3000 NN: SDNN %u (%.2f) RMSSD %u (%.2f) HR %u (%.2f) min %u max %u\n", s.sdnn_ms, sdnn,
           s.rmssd_ms, rmssd, s.hr_avg, hr, s.hr_min, s.hr_max);
    CHECK_EQ(acc, 3000);
    CHECK_EQ(s.beats, 3001);
    CHECK_EQ(s.nn_count, 3000);
    CHECK_NEAR(s.sdnn_ms, sdnn, 0.5);
    CHECK_NEAR(s.rmssd_ms, rmssd, 0.5);
    CHECK_NEAR(s.hr_avg, hr, 0.5);
    CHECK(s.hr_min >= 60000 / 1000 && s.hr_max <= 60000 / 700 + 1 && s.hr_min < s.hr_max);
    CHECK(sizeof(dsp_hrv_t) <= 64);
}

TEST(outliers)
{
    /* steady 800 ms with an ectopic (560 + 1040) and a missed beat (1600) */
    const uint16_t seq[] = { 800, 810, 790, 805, 795, 560, 1040, 800, 810, 1600, 795, 805, 800, 790, 810 };
    const int n = sizeof seq / sizeof seq[0];
    dsp_hrv_t h;
    dsp_hrv_init(&h);
    dsp_hrv_add_beat(&h, 0);
    int acc = 0;
    for (int i = 0; i < n; i++)
        acc += dsp_hrv_add_beat(&h, seq[i]);
    dsp_hrv_summary_t s;
    dsp_hrv_summary(&h, &s);
    const uint16_t good[] = { 800, 810, 790, 805, 795, 800, 810, 795, 805, 800, 790, 810 };
    double sdnn, rmssd, hr;
    truth(good, 12, &sdnn, &rmssd, &hr);
    /* RMSSD only over pairs not broken by a rejection */
    const int pairs[][2] = { { 800, 810 }, { 810, 790 }, { 790, 805 }, { 805, 795 }, { 800, 810 },
                             { 795, 805 }, { 805, 800 }, { 800, 790 }, { 790, 810 } };
    double d2 = 0;
    for (int i = 0; i < 9; i++)
        d2 += (double)(pairs[i][0] - pairs[i][1]) * (pairs[i][0] - pairs[i][1]);
    double rm = sqrt(d2 / 9);
    printf("  accepted %d/%d; SDNN %u (%.2f), RMSSD %u (%.2f)\n", acc, n, s.sdnn_ms, sdnn, s.rmssd_ms, rm);
    CHECK_EQ(acc, 12);
    CHECK_NEAR(s.sdnn_ms, sdnn, 0.5);
    CHECK_NEAR(s.rmssd_ms, rm, 0.5);
    CHECK_EQ(s.beats, n + 1);
}

TEST(rate_change_followed)
{
    /* a genuine step 1000 -> 600 ms is followed after the reference catches up */
    dsp_hrv_t h;
    dsp_hrv_init(&h);
    dsp_hrv_add_beat(&h, 0);
    for (int i = 0; i < 10; i++)
        dsp_hrv_add_beat(&h, 1000);
    int acc = 0;
    for (int i = 0; i < 20; i++)
        acc += dsp_hrv_add_beat(&h, 600);
    CHECK(acc >= 17);
    dsp_hrv_summary_t s;
    dsp_hrv_summary(&h, &s);
    CHECK_EQ(s.hr_max, 100);
    CHECK_EQ(s.hr_min, 60);
}

TEST(not_available)
{
    dsp_hrv_t h;
    dsp_hrv_init(&h);
    dsp_hrv_summary_t s;
    dsp_hrv_summary(&h, &s);
    CHECK_EQ(s.hr_avg, 0);
    CHECK_EQ(s.sdnn_ms, 0);
    CHECK_EQ(s.rmssd_ms, 0);
    dsp_hrv_add_beat(&h, 0);
    dsp_hrv_add_beat(&h, 900);
    dsp_hrv_summary(&h, &s);
    CHECK_EQ(s.hr_avg, 67);
    CHECK_EQ(s.sdnn_ms, 0);
    CHECK_EQ(s.hr_min, 0);
}

int main(void)
{
    RUN(exact_values);
    RUN(outliers);
    RUN(rate_change_followed);
    RUN(not_available);
    return TEST_RESULT();
}
