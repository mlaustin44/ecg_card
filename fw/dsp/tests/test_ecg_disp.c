/* Display path: corners, notch depth, settling, limit cycles, stability. */
#include <math.h>
#include <stdlib.h>

#include "board.h"
#include "dsp_ecg_disp.h"
#include "synth.h"
#include "test.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double UV558 = 805.7 / (8.0 * 558.0); /* uV per stream unit */

/* Gain (output uV / input uV) at f, measured after settle_s. */
static double gain_at(double f, uint8_t notch, double settle_s)
{
    dsp_disp_t s;
    dsp_disp_init(&s, ECG_GAIN_LO, notch);
    const double a = 8000; /* stream units, ~1.44 mV */
    double cycles = f < 2 ? 6 : 40;
    size_t nset = (size_t)(settle_s * 500), nm = (size_t)(cycles / f * 500);
    double *y = malloc(nm * sizeof *y);
    for (size_t i = 0; i < nset + nm; i++) {
        int16_t x = (int16_t)lround(a * sin(2 * M_PI * f * (double)i / 500.0));
        int16_t o;
        dsp_disp_process(&s, &x, &o, 1);
        if (i >= nset)
            y[i - nset] = o;
    }
    double amp = synth_tone_amp(y, nm, 500.0, f, NULL);
    free(y);
    return amp / (a * UV558);
}

TEST(corners)
{
    double g10 = gain_at(10, 0, 2);
    double g067 = gain_at(0.67, 0, 8) / g10;
    double g03 = gain_at(0.3, 0, 8) / g10;
    double g40 = gain_at(40, 0, 1) / g10;
    printf("  gain 10 Hz %.4f; -3 dB checks: 0.67 Hz %.2f dB, 40 Hz %.2f dB; 0.3 Hz %.1f dB\n",
           g10, 20 * log10(g067), 20 * log10(g40), 20 * log10(g03));
    CHECK_NEAR(g10, 1.0, 0.02);
    CHECK_NEAR(20 * log10(g067), -3.01, 0.3);
    CHECK(20 * log10(g03) < -12); /* breathing-rate baseline wander */
    CHECK_NEAR(20 * log10(g40), -3.01, 0.3);
    double g150 = gain_at(150, 0, 1);
    printf("  150 Hz %.1f dB\n", 20 * log10(g150));
    CHECK(20 * log10(g150) < -20);
}

TEST(notch_depth)
{
    for (int hz = 50; hz <= 60; hz += 10) {
        double g = gain_at(hz, (uint8_t)hz, 2);
        double gref = gain_at(hz, 0, 2);
        double depth = -20 * log10(g / gref + 1e-12);
        double off = gain_at(hz == 50 ? 60 : 50, (uint8_t)hz, 2) / gain_at(hz == 50 ? 60 : 50, 0, 2);
        double edge = gain_at(hz + 3, (uint8_t)hz, 2) / gain_at(hz + 3, 0, 2);
        printf("  notch %d Hz: depth %.1f dB; other mains %.2f dB; +3 Hz %.2f dB\n", hz, depth,
               20 * log10(off), 20 * log10(edge));
        CHECK(depth >= 30);
        CHECK(fabs(20 * log10(off)) < 0.5);
        CHECK(fabs(20 * log10(edge)) < 1.0);
        /* mains drift: Q 30 trades off-centre depth for settling speed */
        double d1 = gain_at(hz + 0.1, (uint8_t)hz, 2) / gain_at(hz + 0.1, 0, 2);
        double d2 = gain_at(hz + 0.2, (uint8_t)hz, 2) / gain_at(hz + 0.2, 0, 2);
        printf("  notch %d Hz off-centre: +0.1 Hz %.1f dB, +0.2 Hz %.1f dB\n", hz, -20 * log10(d1),
               -20 * log10(d2));
        CHECK(-20 * log10(d1) >= 15);
        CHECK(-20 * log10(d2) >= 10);
    }
}

TEST(step_settling)
{
    /* 1 mV step: HP decay to < 1 % of the step */
    dsp_disp_t s;
    dsp_disp_init(&s, ECG_GAIN_LO, 50);
    int16_t z = 0, o;
    for (int i = 0; i < 500; i++)
        dsp_disp_process(&s, &z, &o, 1);
    int16_t x = (int16_t)lround(1000.0 / UV558);
    int last_big = 0, peak = 0;
    for (int i = 0; i < 2500; i++) {
        dsp_disp_process(&s, &x, &o, 1);
        if (abs(o) > peak)
            peak = abs(o);
        if (abs(o) > 10)
            last_big = i;
    }
    printf("  1 mV step: peak %d uV, settled to < 1 %% after %.0f ms\n", peak, last_big * 2.0);
    CHECK(peak > 900 && peak < 1100);
    CHECK(last_big * 2 < 1600);
    /* notch envelope: a 50 Hz burst is removed within 1 s */
    dsp_disp_init(&s, ECG_GAIN_LO, 50);
    last_big = 0;
    for (int i = 0; i < 1500; i++) {
        int16_t v = (int16_t)lround(2000.0 / UV558 * sin(2 * M_PI * 50 * i / 500.0));
        dsp_disp_process(&s, &v, &o, 1);
        if (abs(o) > 20)
            last_big = i;
    }
    printf("  2 mV 50 Hz onset: residual < 1 %% after %.0f ms\n", last_big * 2.0);
    CHECK(last_big * 2 < 1000);
}

TEST(no_limit_cycles)
{
    /* after arbitrary activity, zero input must give exactly zero output, and
     * a constant input must decay to exactly zero (HP has no dead band) */
    synth_rng_t r;
    synth_rng_seed(&r, 3);
    for (int notch = 0; notch <= 60; notch += 10) {
        if (notch && notch != 50 && notch != 60)
            continue;
        for (int c = 0; c < 3; c++) {
            dsp_disp_t s;
            dsp_disp_init(&s, c ? ECG_GAIN_HI : ECG_GAIN_LO, (uint8_t)notch);
            int16_t o;
            for (int i = 0; i < 5000; i++) {
                int16_t v = (int16_t)lround(12000 * (synth_uniform(&r) - 0.5));
                dsp_disp_process(&s, &v, &o, 1);
            }
            const int16_t level[3] = { 0, 1234, -32768 };
            int16_t v = level[c];
            int nonzero_late = 0;
            int32_t internal_late = 0;
            for (int i = 0; i < 30000; i++) {
                dsp_disp_process(&s, &v, &o, 1);
                if (i > 15000) {
                    nonzero_late += o != 0;
                    if (abs(s.lp.y1) > internal_late)
                        internal_late = abs(s.lp.y1);
                }
            }
            CHECK_EQ(nonzero_late, 0);
            CHECK(internal_late <= 1); /* within 1/256 stream unit inside */
        }
    }
}

TEST(long_run_stability)
{
    /* 1 hour of full-scale noise and square waves, then silence */
    dsp_disp_t s;
    dsp_disp_init(&s, ECG_GAIN_HI, 60);
    synth_rng_t r;
    synth_rng_seed(&r, 11);
    int16_t o;
    int sat = 0;
    for (long i = 0; i < 3600L * 500; i++) {
        int16_t v;
        if ((i / 50000) % 2)
            v = (i / 7) % 2 ? 32767 : -32768;
        else
            v = (int16_t)lround(65535 * (synth_uniform(&r) - 0.5));
        dsp_disp_process(&s, &v, &o, 1);
        sat += (o == 32767 || o == -32768);
    }
    int16_t z = 0;
    int nonzero = 0;
    for (int i = 0; i < 30000; i++) {
        dsp_disp_process(&s, &z, &o, 1);
        if (i > 10000)
            nonzero += o != 0;
    }
    printf("  1 h full-scale abuse: %d saturated outputs, recovered to 0\n", sat);
    CHECK_EQ(nonzero, 0);
}

TEST(reset_no_transient)
{
    dsp_disp_t s;
    dsp_disp_init(&s, ECG_GAIN_LO, 50);
    int16_t x = 9000, o;
    dsp_disp_process(&s, &x, &o, 1);
    CHECK_EQ(o, 0); /* primed on the first sample */
    for (int i = 0; i < 100; i++)
        dsp_disp_process(&s, &x, &o, 1);
    dsp_disp_reset(&s);
    x = -7000;
    int mx = 0;
    for (int i = 0; i < 100; i++) {
        dsp_disp_process(&s, &x, &o, 1);
        if (abs(o) > mx)
            mx = abs(o);
    }
    CHECK_EQ(mx, 0);
}

int main(void)
{
    RUN(corners);
    RUN(notch_depth);
    RUN(step_settling);
    RUN(no_limit_cycles);
    RUN(long_run_stability);
    RUN(reset_no_transient);
    return TEST_RESULT();
}
