/* Decimator: response by sine injection, DC scaling, saturation, block-size
 * independence, timing (group delay). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dsp_decim.h"
#include "dsp_delays.h"
#include "stream_fmt.h"
#include "synth.h"
#include "test.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MID 2051

/* Run a sine (phase ph) of amplitude a codes at f Hz for 2 s; returns the
 * output in codes (stream / 8) in y (caller frees), count in *n. */
static double *run_sine(double f, double a, double ph, size_t *n)
{
    dsp_decim_t d;
    dsp_decim_init(&d, MID);
    const size_t nraw = 4000 * 2;
    uint16_t *raw = malloc(nraw * sizeof *raw);
    int16_t *out = malloc(DSP_DECIM_MAX_OUT(nraw) * sizeof *out);
    for (size_t i = 0; i < nraw; i++)
        raw[i] = (uint16_t)lround(MID + a * sin(2 * M_PI * f * (double)i / 4000.0 + ph));
    *n = dsp_decim_push(&d, raw, nraw, out);
    double *y = malloc(*n * sizeof *y);
    for (size_t i = 0; i < *n; i++)
        y[i] = out[i] / (double)STREAM_ECG_SCALE;
    free(raw);
    free(out);
    return y;
}

/* |H(f)| measured by two quadrature injections: mean(y_sin^2) + mean(y_cos^2)
 * = a^2 |H|^2 whatever the output alias frequency (also DC / Nyquist). */
static double gain_at(double f, double a)
{
    double ms = 0;
    const size_t skip = 100; /* 0.2 s */
    for (int q = 0; q < 2; q++) {
        size_t n;
        double *y = run_sine(f, a, q * M_PI / 2, &n);
        double s = 0;
        for (size_t i = skip; i < n; i++)
            s += y[i] * y[i];
        ms += s / (double)(n - skip);
        free(y);
    }
    return sqrt(ms) / a;
}

/* Phase lag of output m against the raw sine at raw index 8m + 7, radians. */
static double lag_at(double f)
{
    size_t n;
    double *y = run_sine(f, 1000, 0, &n);
    const size_t skip = 100;
    double *ref = malloc(n * sizeof *ref);
    for (size_t i = 0; i < n; i++)
        ref[i] = sin(2 * M_PI * f * (8.0 * (double)i + 7.0) / 4000.0);
    double ph, phr;
    synth_tone_amp(y + skip, n - skip, 500.0, f, &ph);
    synth_tone_amp(ref + skip, n - skip, 500.0, f, &phr);
    free(y);
    free(ref);
    double lag = phr - ph;
    while (lag > M_PI)
        lag -= 2 * M_PI;
    while (lag < -M_PI)
        lag += 2 * M_PI;
    return lag;
}

TEST(passband_and_stopband)
{
    double gmin = 1e9, gmax = -1e9;
    for (double f = 0.5; f <= 150.0; f += 2.5) {
        double g = 20 * log10(gain_at(f, 1500));
        if (g < gmin)
            gmin = g;
        if (g > gmax)
            gmax = g;
    }
    printf("  passband 0.5-150 Hz: %.3f .. %.3f dB (ripple %.3f dB p-p)\n", gmin, gmax, gmax - gmin);
    CHECK(gmax - gmin <= 0.12); /* 0.087 dB by design + 12-bit input rounding */
    CHECK(fabs(gmax) < 0.1);
    double worst = -1e9;
    for (double f = 250; f <= 2000; f += 5) {
        double g = 20 * log10(gain_at(f, 1500) + 1e-9);
        if (g > worst)
            worst = g;
    }
    double a250 = 20 * log10(gain_at(250, 1500));
    double a350 = 20 * log10(gain_at(350, 1500));
    double a1k = 20 * log10(gain_at(1000, 1500));
    printf("  attenuation: 250 Hz %.1f dB, 350 Hz %.1f dB, 1000 Hz %.1f dB, worst 250-2000 Hz %.1f dB\n",
           -a250, -a350, -a1k, -worst);
    CHECK(a250 <= -40);
    CHECK(a350 <= -40);
    CHECK(a1k <= -40);
    CHECK(worst <= -40);
}

TEST(group_delay)
{
    /* phase lag at low frequency = 47 raw samples (linear phase) */
    for (double f = 2; f <= 40; f += 7) { /* lag < pi: no unwrapping needed */
        double lag = lag_at(f);
        double samples = lag / (2 * M_PI * f) * 4000.0;
        CHECK_NEAR(samples, DSP_DECIM_DELAY_RAW, 0.05);
    }
}

TEST(dc_scaling)
{
    for (int v = -2000; v <= 2000; v += 333) {
        dsp_decim_t d;
        dsp_decim_init(&d, MID);
        uint16_t raw[800];
        int16_t out[DSP_DECIM_MAX_OUT(800)];
        for (int i = 0; i < 800; i++)
            raw[i] = (uint16_t)(MID + v);
        size_t n = dsp_decim_push(&d, raw, 800, out);
        CHECK_EQ(n, 100);
        int bad = 0;
        for (size_t i = 0; i < n; i++)
            bad += out[i] != v * STREAM_ECG_SCALE; /* exact, from the first sample (primed) */
        CHECK_EQ(bad, 0);
    }
}

TEST(saturation)
{
    /* mid 0, full-scale step: overshoot must saturate, not wrap */
    dsp_decim_t d;
    dsp_decim_init(&d, 0);
    uint16_t raw[1600];
    int16_t out[DSP_DECIM_MAX_OUT(1600)];
    for (int i = 0; i < 1600; i++)
        raw[i] = i < 800 ? 0 : 4095;
    size_t n = dsp_decim_push(&d, raw, 1600, out);
    int16_t mx = -32768, mn = 32767;
    for (size_t i = 0; i < n; i++) {
        if (out[i] > mx)
            mx = out[i];
        if (out[i] < mn)
            mn = out[i];
    }
    CHECK_EQ(mx, 32767);  /* overshoot clipped */
    CHECK(mn > -4000);    /* pre-ringing only, no wrap-around */
    CHECK_EQ(out[n - 1], 32760);
    /* negative side */
    dsp_decim_init(&d, 4095);
    for (int i = 0; i < 1600; i++)
        raw[i] = i < 800 ? 4095 : 0;
    n = dsp_decim_push(&d, raw, 1600, out);
    mn = 32767;
    for (size_t i = 0; i < n; i++)
        if (out[i] < mn)
            mn = out[i];
    CHECK_EQ(mn, -32768);
    mx = -32768;
    for (size_t i = 0; i < n; i++)
        if (out[i] > mx)
            mx = out[i];
    CHECK(mx < 4000);
    CHECK_EQ(out[n - 1], -32760);
}

TEST(block_size_independence)
{
    enum { N = 40000 };
    uint16_t *raw = malloc(N * sizeof *raw);
    int16_t *ref = malloc(N * sizeof *ref);
    int16_t *got = malloc(N * sizeof *got);
    synth_rng_t r;
    synth_rng_seed(&r, 7);
    for (int i = 0; i < N; i++)
        raw[i] = (uint16_t)(synth_uniform(&r) * 4096);
    dsp_decim_t d;
    dsp_decim_init(&d, MID);
    size_t nref = dsp_decim_push(&d, raw, N, ref);
    CHECK_EQ(nref, N / 8);
    const size_t sizes[] = { 1, 3, 7, 8, 13, 64, 0 };
    for (int s = 0; s < 7; s++) {
        dsp_decim_init(&d, MID);
        size_t pos = 0, nout = 0;
        while (pos < N) {
            size_t b = sizes[s] ? sizes[s] : 1 + (size_t)(synth_uniform(&r) * 200);
            if (b > N - pos)
                b = N - pos;
            int16_t tmp[DSP_DECIM_MAX_OUT(256)];
            size_t k = dsp_decim_push(&d, raw + pos, b, tmp);
            CHECK(k <= DSP_DECIM_MAX_OUT(b));
            /* output m appears with raw sample 8m+7 */
            CHECK_EQ(nout + k, (pos + b) / 8);
            memcpy(got + nout, tmp, k * sizeof *tmp);
            nout += k;
            pos += b;
        }
        CHECK_EQ(nout, nref);
        CHECK(memcmp(got, ref, nref * sizeof *ref) == 0);
    }
    free(raw);
    free(ref);
    free(got);
}

int main(void)
{
    RUN(passband_and_stopband);
    RUN(group_delay);
    RUN(dc_scaling);
    RUN(saturation);
    RUN(block_size_independence);
    return TEST_RESULT();
}
