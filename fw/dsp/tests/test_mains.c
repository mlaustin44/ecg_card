/* Mains analysis: amplitude accuracy, auto notch choice and hysteresis. */
#include <math.h>
#include <stdlib.h>

#include "board.h"
#include "dsp_decim.h"
#include "dsp_mains.h"
#include "synth.h"
#include "test.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Run an ECG with mains through decimator + analyser for sec seconds. */
static void run(dsp_mains_t *m, double mains_hz, double mains_uv, int gain, double sec,
                double ecg_mv, uint64_t seed)
{
    synth_beats_t b;
    synth_beats_cfg_t bc = { .hr_bpm = 70, .rr_sd = 0.03, .duration = sec + 1, .t_first = 0.3, .seed = seed };
    synth_beats_make(&b, &bc);
    synth_ecg_cfg_t ec = { .amp_mv = ecg_mv, .t_ratio = 0.3, .mains_uv = mains_uv, .mains_hz = mains_hz,
                           .wander_uv = 150, .white_uv = 3, .gain = gain, .mid = 2040, .seed = seed };
    synth_ecg_t e;
    synth_ecg_init(&e, &ec, &b);
    dsp_decim_t d;
    dsp_decim_init(&d, 2040);
    long n = (long)(sec * 4000);
    for (long i = 0; i < n; i += 80) {
        uint16_t raw[80];
        int16_t out[DSP_DECIM_MAX_OUT(80)];
        for (int j = 0; j < 80; j++)
            raw[j] = synth_ecg_next(&e);
        size_t k = dsp_decim_push(&d, raw, 80, out);
        dsp_mains_push(m, out, k);
    }
    synth_beats_free(&b);
}

TEST(amplitude)
{
    const double amps[] = { 20, 100, 500 };
    for (int g = 0; g < 2; g++) {
        int gain = g ? ECG_GAIN_HI : ECG_GAIN_LO;
        for (int i = 0; i < 3; i++) {
            for (int hz = 50; hz <= 60; hz += 10) {
                dsp_mains_t m;
                dsp_mains_init(&m, gain);
                run(&m, hz, amps[i], gain, 3.0, 1.0, 5 + (uint64_t)i);
                double expect = 2 * amps[i];
                double got = dsp_mains_uvpp(&m, (uint8_t)hz);
                printf("  gain %d, %d Hz %4.0f uVpp: measured %5u uVpp, other bin %u\n", gain, hz,
                       expect, (unsigned)got, (unsigned)dsp_mains_uvpp(&m, hz == 50 ? 60 : 50));
                CHECK_NEAR(got, expect, 0.05 * expect + 6);
                CHECK_EQ(dsp_mains_auto_hz(&m), hz);
                CHECK(dsp_mains_residual_uvpp(&m) == got);
            }
        }
    }
}

TEST(clean_floor_and_hysteresis)
{
    dsp_mains_t m;
    dsp_mains_init(&m, ECG_GAIN_LO);
    run(&m, 50, 0, ECG_GAIN_LO, 4.0, 1.0, 9);
    printf("  no mains: 50 Hz bin %u uVpp, 60 Hz bin %u uVpp (ECG leakage floor)\n",
           (unsigned)dsp_mains_uvpp(&m, 50), (unsigned)dsp_mains_uvpp(&m, 60));
    CHECK(dsp_mains_residual_uvpp(&m) < 20);

    /* decided 50 -> needs 2 windows of 60 to switch */
    dsp_mains_init(&m, ECG_GAIN_LO);
    int16_t x[250];
    for (int w = 0; w < 3; w++) {
        for (int i = 0; i < 250; i++)
            x[i] = (int16_t)lround(500 * sin(2 * M_PI * 50 * i / 500.0));
        dsp_mains_push(&m, x, 250);
    }
    CHECK_EQ(dsp_mains_auto_hz(&m), 50);
    for (int i = 0; i < 250; i++)
        x[i] = (int16_t)lround(500 * sin(2 * M_PI * 60 * i / 500.0));
    dsp_mains_push(&m, x, 250);
    CHECK_EQ(dsp_mains_auto_hz(&m), 50);
    dsp_mains_push(&m, x, 250);
    CHECK_EQ(dsp_mains_auto_hz(&m), 60);
    /* both similar: no change */
    for (int i = 0; i < 250; i++)
        x[i] = (int16_t)lround(300 * sin(2 * M_PI * 60 * i / 500.0) + 300 * sin(2 * M_PI * 50 * i / 500.0));
    for (int w = 0; w < 4; w++)
        dsp_mains_push(&m, x, 250);
    CHECK_EQ(dsp_mains_auto_hz(&m), 60);
}

int main(void)
{
    RUN(amplitude);
    RUN(clean_floor_and_hysteresis);
    return TEST_RESULT();
}
