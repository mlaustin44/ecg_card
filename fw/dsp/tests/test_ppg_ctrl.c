/* PPG control laws: DC cancellation, LED AGC (thresholds, hysteresis, limit),
 * finger detection, and closed-loop convergence on a plant model. */
#include <math.h>
#include <stdlib.h>

#include "board.h"
#include "dsp_ppg_ctrl.h"
#include "settings.h"
#include "synth.h"
#include "test.h"

/* volts -> 12-bit code on VDDA = 3.3 V */
static uint16_t v2c(double v) { return (uint16_t)lround(v / 3.3 * 4096); }

TEST(dcc_formula)
{
    for (double v = 0.3; v <= 3.0; v += 0.1) {
        uint16_t tia = v2c(v);
        uint16_t dac = dsp_ppg_dcc_code(tia);
        /* PPG_HI(on) = 11 V_dac - 10 TIA lands at mid-scale (+-11 codes rounding of 1/11) */
        int hi = 11 * dac - 10 * tia;
        if (dac < 4095)
            CHECK(abs(hi - 2048) <= 6);
    }
    CHECK_EQ(dsp_ppg_dcc_code(4095), 3909); /* (2048 + 40950) / 11 */
    CHECK_EQ(dsp_ppg_dcc_code(0), 186);
    /* dead band */
    uint16_t dac = 1000;
    CHECK(!dsp_ppg_dcc_update(v2c(1.8), 2048 + 1000, &dac));
    CHECK_EQ(dac, 1000);
    CHECK(dsp_ppg_dcc_update(v2c(1.8), 3500, &dac));
    CHECK_EQ(dac, dsp_ppg_dcc_code(v2c(1.8)));
    /* prediction for an LED step: signal part scales, dark does not */
    uint16_t dark = v2c(0.30), on = v2c(1.30);
    uint16_t pred = dsp_ppg_dcc_predict(dark, on, 1000, 1250);
    CHECK_EQ(pred, dsp_ppg_dcc_code(v2c(0.30 + 1.0 * 1.25)));
}

TEST(led_limit)
{
    CHECK_EQ(dsp_led_max_code(5), 1344);  /* 5 mA / 3.718 uA */
    CHECK_EQ(dsp_led_max_code(10), 2689);
    CHECK_EQ(dsp_led_max_code(15), 4034);
    CHECK_EQ(dsp_led_max_code(20), 4095); /* 15.2 mA full scale on the 2.048 V reference */
    CHECK_EQ(dsp_led_max_code(led_limit_ma(LED_LIMIT_15MA)), 4034);
}

TEST(agc_rules)
{
    dsp_agc_t a;
    dsp_agc_init(&a);
    uint16_t led = 1000;
    /* inside the trigger window: nothing */
    CHECK(!dsp_agc_step(&a, v2c(1.0), &led, 4034));
    CHECK(!dsp_agc_step(&a, v2c(2.6), &led, 4034));
    /* > 2.8 V: -20 % steps until <= 2.0 V */
    CHECK(dsp_agc_step(&a, v2c(2.9), &led, 4034));
    CHECK_EQ(led, 800);
    CHECK(dsp_agc_step(&a, v2c(2.3), &led, 4034)); /* still hunting down: 2.3 > 2.0 */
    CHECK_EQ(led, 640);
    CHECK(!dsp_agc_step(&a, v2c(1.9), &led, 4034));
    CHECK_EQ(a.dir, 0);
    /* < 0.6 V: +25 % until >= 1.5 V */
    led = 400;
    CHECK(dsp_agc_step(&a, v2c(0.5), &led, 4034));
    CHECK_EQ(led, 500);
    CHECK(dsp_agc_step(&a, v2c(1.2), &led, 4034));
    CHECK_EQ(led, 625);
    CHECK(!dsp_agc_step(&a, v2c(1.6), &led, 4034));
    /* clamped at the limit */
    led = 3900;
    CHECK(dsp_agc_step(&a, v2c(0.3), &led, 4034));
    CHECK_EQ(led, 4034);
    CHECK(!dsp_agc_step(&a, v2c(0.3), &led, 4034));
    /* minimum code */
    dsp_agc_init(&a);
    led = 9;
    CHECK(dsp_agc_step(&a, v2c(3.2), &led, 4034));
    CHECK_EQ(led, DSP_AGC_MIN_CODE);
}

/* Static plant: TIA_OUT(on) = 0.245 V + ambient + k * I_led, PPG_HI from the
 * DAC12 code; AGC every call, DCC every 4th call. */
TEST(closed_loop_convergence)
{
    const double ks[] = { 0.02, 0.06, 0.15, 0.3, 0.6, 1.2 }; /* V per mA */
    for (int i = 0; i < 6; i++) {
        for (int start = 0; start < 2; start++) {
            dsp_agc_t a;
            dsp_agc_init(&a);
            uint16_t led = start ? 4000 : 50, dac = 2048, max = dsp_led_max_code(15);
            int steps = 0, last_step = -1;
            double tia = 0;
            for (int it = 0; it < 200; it++) {
                tia = 0.245 + 0.05 + ks[i] * led * 3.718e-3;
                if (tia > 3.3)
                    tia = 3.3;
                if (dsp_agc_step(&a, v2c(tia), &led, max)) {
                    steps++;
                    last_step = it;
                }
                if (it % 4 == 3) {
                    double hi = 11 * dac / 4096.0 * 3.3 - 10 * tia;
                    hi = hi < 0 ? 0 : hi > 3.3 ? 3.3 : hi;
                    dsp_ppg_dcc_update(v2c(tia), v2c(hi), &dac);
                }
            }
            double hi = 11 * dac / 4096.0 * 3.3 - 10 * tia;
            int reachable = 0.295 + ks[i] * max * 3.718e-3 >= 1.5;
            printf("  k %.2f V/mA from %4s: %2d AGC steps (last at %d), LED %4u (%.1f mA), TIA %.2f V, PPG_HI %.2f V\n",
                   ks[i], start ? "max" : "min", steps, last_step, led, led * 3.718e-3, tia, hi);
            /* inside the trigger window nothing happens; once triggered the
             * loop ends in the target window (or at the current limit) */
            CHECK(tia >= 0.6 * 0.98 || led == max);
            CHECK(tia <= 2.8);
            if (steps && reachable)
                CHECK((tia >= 1.45 && tia <= 2.05) || (led == max && tia < 1.5));
            if (!reachable && steps)
                CHECK_EQ(led, max);
            CHECK(fabs(hi - 1.65) < 0.85);
            CHECK(last_step < 40); /* converged, then no hunting */
        }
    }
}

TEST(closed_loop_with_ppg_and_drift)
{
    /* full synthetic optics through the acquisition loop, with the finger
     * pressing harder over 60 s (reflectance x2) */
    synth_beats_cfg_t bc = { .hr_bpm = 70, .rsa = 0.03, .rr_sd = 0.02, .duration = 125, .t_first = 0.5, .seed = 4 };
    synth_beats_t b;
    synth_beats_make(&b, &bc);
    synth_ppg_cfg_t pc = { .pat_s = 0.25, .refl_ua_per_ma = 0.12, .pi = 0.02, .ambient_ua = 0.05, .flicker = 0.1,
                           .flicker_hz = 100.1, .noise_na = 0.1, .seed = 5 };
    synth_ppg_t src;
    synth_ppg_init(&src, &pc, &b);
    synth_ppg_loop_t *l = malloc(sizeof *l);
    synth_ppg_loop_init(l, 135, 15);
    int agc_late = 0, agc_before = 0;
    double tia_end = 0;
    int hi_rail_late = 0;
    for (int k = 0; k < 12000; k++) {
        src.c.refl_ua_per_ma = 0.12 * (1.0 + (k > 3000 ? (k < 9000 ? (k - 3000) / 6000.0 : 1.0) : 0.0));
        int before = l->agc_steps;
        dsp_ppg_frame_t f;
        dsp_ppg_out_t o;
        dsp_ppg_pulse_t p;
        synth_ppg_loop_frame(l, &src, &f, &o, &p);
        if (l->agc_steps != before) {
            if (k > 10000)
                agc_late++;
            else
                agc_before++;
        }
        if (k > 10000) {
            tia_end = f.tia_on * 3.3 / 4096;
            hi_rail_late += (f.hi_on < 16 || f.hi_on > 4079);
        }
    }
    printf("  drift x2: %d AGC steps while settling/drifting, %d in the last 20 s; TIA(on) %.2f V; DCC %d; "
           "PPG_HI railed in %d of the last 2000 frames\n",
           agc_before, agc_late, tia_end, l->dcc_steps, hi_rail_late);
    CHECK(agc_before >= 5);
    CHECK_EQ(agc_late, 0);
    CHECK(tia_end > 0.6 && tia_end < 2.8);
    CHECK_EQ(hi_rail_late, 0);
    free(l);
    synth_beats_free(&b);
}

TEST(finger)
{
    dsp_finger_t f;
    dsp_finger_init(&f);
    int i;
    for (i = 0; i < 100 && !dsp_finger_update(&f, 400, 2000); i++) {
    }
    CHECK_EQ(i, DSP_FINGER_HOLD - 1);
    /* between thresholds: stays present */
    for (i = 0; i < 100; i++)
        CHECK(dsp_finger_update(&f, 200, 2000));
    for (i = 0; i < 100 && dsp_finger_update(&f, 100, 1000); i++) {
    }
    CHECK_EQ(i, DSP_FINGER_HOLD - 1);
    /* a short dip does not count */
    for (i = 0; i < 40; i++)
        dsp_finger_update(&f, 400, 2000);
    CHECK(dsp_finger_update(&f, 400, 2000));
    for (i = 0; i < 10; i++)
        dsp_finger_update(&f, 50, 2000);
    CHECK(dsp_finger_update(&f, 400, 2000));
    /* ambient overload: TIA on the rail */
    for (i = 0; i < 40; i++)
        dsp_finger_update(&f, 400, 4095);
    CHECK(!dsp_finger_update(&f, 400, 4095));
}

int main(void)
{
    RUN(dcc_formula);
    RUN(led_limit);
    RUN(agc_rules);
    RUN(closed_loop_convergence);
    RUN(closed_loop_with_ppg_and_drift);
    RUN(finger);
    return TEST_RESULT();
}
