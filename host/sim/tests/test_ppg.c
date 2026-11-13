#include <math.h>
#include <string.h>

#include "common.h"

static uint32_t src_const(uint64_t t, void *ctx)
{
    (void)t;
    return *(uint32_t *)ctx;
}
static uint32_t src_pulse(uint64_t t, void *ctx)
{
    (void)ctx;
    return (uint32_t)(100000.0 + 10000.0 * sin(6.283185307179586 * 1.0 * (double)t * 1e-6));
}

static void mcp_a_code(unsigned code, bool vdd_ref)
{
    uint8_t w[3] = {0x40, (uint8_t)((vdd_ref ? 0x00 : 0x80) | (code >> 8)), (uint8_t)code};
    hal_i2c_xfer(0x60, w, 3, NULL, 0);
}
static unsigned code_for_ma(double ma) { return (unsigned)lrint(ma * 1000.0 / 3.718); }

static void ppg_up(double ma)
{
    high(PIN_ANA_EN);
    high(PIN_BUCK_PS_SYNC);
    hal_ppg_analog(true);
    high(PIN_VLED_EN);
    hal_delay_ms(PPG_VLED_START_MS);
    mcp_a_code(code_for_ma(ma), false);
    hal_clock_fast(true);
}

static hal_ppg_frame_t fr[2000];
static size_t nfr;
static void run_frames(unsigned ms)
{
    nfr = 0;
    for (unsigned i = 0; i < ms; i++) {
        hal_wdt_kick();
        hal_delay_ms(1);
        nfr += ppg_read(fr + nfr, 2000 - nfr);
    }
}

TEST(frame_timing_and_led_window)
{
    fresh();
    ppg_up(5.0);
    hal_acq_start(HAL_ACQ_PPG);
    CHECK(sim_led_blank());
    hal_delay_us(9799);
    CHECK(sim_led_blank());
    hal_delay_us(1);
    CHECK(!sim_led_blank());
    hal_delay_us(149);
    hal_ppg_frame_t f[4];
    CHECK_EQ(ppg_read(f, 4), 0); /* on sample at 9950 not taken yet */
    hal_delay_us(1);
    CHECK_EQ(ppg_read(f, 4), 1);
    hal_delay_us(48);
    CHECK(!sim_led_blank());
    hal_delay_us(1);
    CHECK(!sim_led_blank()); /* 9999 */
    hal_delay_us(1);
    CHECK(sim_led_blank());  /* 10000 */
    run_frames(990);
    CHECK_EQ(nfr, 99 - 0); /* frames 1..99 */
    CHECK_EQ(sim_facts()->ppg_frames, 100);
    CHECK_EQ(hal_ppg_overruns(), 0);
    hal_acq_stop();
    CHECK(sim_led_blank());
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(on_minus_dark_is_proportional_to_led_current)
{
    uint32_t pa_per_ma = 100000; /* 0.1 uA per mA */
    double mas[3] = {2.5, 5.0, 10.0};
    double delta[3];
    for (int i = 0; i < 3; i++) {
        fresh();
        sim_set_ppg_source(src_const, &pa_per_ma);
        sim_set_finger(true);
        ppg_up(mas[i]);
        hal_acq_start(HAL_ACQ_PPG);
        run_frames(100);
        CHECK(nfr >= 8);
        hal_ppg_frame_t f = fr[nfr - 1];
        CHECK_NEAR(f.tia_dark, 0.245 / 3.3 * 4096.0, 1.5); /* bias level only */
        delta[i] = (double)f.tia_on - f.tia_dark;
        double actual_ma = code_for_ma(mas[i]) * 3.718e-3;
        double expect = actual_ma * 100000.0 * 1e-6 / 3.3 * 4096.0; /* pA -> V via 1 Mohm */
        CHECK_NEAR(delta[i], expect, 1.5);
        CHECK_NEAR(sim_led_set_ma(), actual_ma, 0.01);
        CHECK_EQ(sim_violation_count(), 0);
    }
    CHECK_NEAR(delta[1] / delta[0], 2.0, 0.01);
    CHECK_NEAR(delta[2] / delta[1], 2.0, 0.01);

    /* no finger: no light returns */
    fresh();
    sim_set_ppg_source(src_const, &pa_per_ma);
    ppg_up(5.0);
    hal_acq_start(HAL_ACQ_PPG);
    run_frames(50);
    CHECK_EQ(fr[nfr - 1].tia_on, fr[nfr - 1].tia_dark);
    /* LED current zero => nothing, even with a finger */
    sim_set_finger(true);
    mcp_a_code(0, false);
    run_frames(50);
    CHECK_EQ(fr[nfr - 1].tia_on, fr[nfr - 1].tia_dark);
    /* ... or with VLED off (the boost may be switched while LED_BLANK is high) */
    mcp_a_code(code_for_ma(5.0), false);
    low(PIN_VLED_EN);
    run_frames(50);
    CHECK_EQ(fr[nfr - 1].tia_on, fr[nfr - 1].tia_dark);
}

TEST(dac12_offset_moves_hi_on)
{
    uint32_t pa_per_ma = 100000;
    fresh();
    sim_set_ppg_source(src_const, &pa_per_ma);
    sim_set_finger(true);
    ppg_up(5.0);
    hal_acq_start(HAL_ACQ_PPG);
    run_frames(30);
    hal_ppg_frame_t f = fr[nfr - 1];
    CHECK_EQ(f.hi_on, 0);   /* DAC12 = 0: 11*0 - 10*TIA clips low */
    CHECK_EQ(f.hi_dark, 0);
    double tia_on_v = f.tia_on * 3.3 / 4096.0;
    /* control loop: V_DAC12 = (1.65 + 10 * TIA_on) / 11 */
    unsigned code = (unsigned)lrint((1.65 + 10.0 * tia_on_v) / 11.0 / 3.3 * 4096.0);
    hal_ppg_set_offset((uint16_t)code);
    run_frames(30);
    f = fr[nfr - 1];
    CHECK_NEAR(f.hi_on, 2048, 12);
    CHECK_EQ(f.hi_dark, 4095); /* dark: TIA is only 0.245 V, so PPG_HI rails high */
    hal_ppg_set_offset((uint16_t)(code + 10));
    run_frames(30);
    CHECK_NEAR(fr[nfr - 1].hi_on - f.hi_on, 11.0 * 10.0, 2.0);
    /* fine resolution: 1 DAC code = 11 hi codes; 1 TIA code = 10 hi codes */
    CHECK_EQ(sim_facts()->ppg_offset_changes, 2);
    CHECK_EQ(sim_facts()->ppg_offset_in_sample_window, 0);
    /* a step inside the dark..on window is recorded */
    while (sim_led_blank()) hal_delay_us(10); /* LED just came on: dark taken, on sample pending */
    hal_ppg_set_offset((uint16_t)(code + 20));
    CHECK_EQ(sim_facts()->ppg_offset_in_sample_window, 1);
    /* re-enabling the analog blocks resets DAC12 */
    hal_acq_stop();
    hal_ppg_analog(false);
    hal_ppg_analog(true);
    CHECK_EQ(hal_adc_read_once(HAL_ADC_PPG_HI), 0);
}

TEST(ambient_cancels_in_on_minus_dark)
{
    uint32_t pa_per_ma = 100000;
    double d[3];
    for (int i = 0; i < 3; i++) {
        fresh();
        sim_set_ppg_source(src_const, &pa_per_ma);
        sim_set_finger(true);
        if (i == 1) sim_ppg_set_ambient(200000.0, 0.0, 0.0);
        if (i == 2) sim_ppg_set_ambient(200000.0, 50000.0, 100.0);
        ppg_up(5.0);
        hal_acq_start(HAL_ACQ_PPG);
        run_frames(200);
        double acc = 0, dark = 0;
        for (size_t k = 5; k < nfr; k++) {
            acc += (double)fr[k].tia_on - fr[k].tia_dark;
            dark += fr[k].tia_dark;
        }
        d[i] = acc / (double)(nfr - 5);
        dark /= (double)(nfr - 5);
        if (i == 1) CHECK_NEAR(dark, (0.245 + 0.2) / 3.3 * 4096.0, 2.0);
    }
    CHECK_NEAR(d[1], d[0], 0.5);
    CHECK_NEAR(d[2], d[0], 0.5 + 12.0); /* flicker: only the 250 us gap's slope remains */
}

TEST(pulsatile_source_and_clipping)
{
    fresh();
    sim_set_ppg_source(src_pulse, NULL);
    sim_set_finger(true);
    ppg_up(5.0);
    hal_acq_start(HAL_ACQ_PPG);
    run_frames(1000);
    int mn = 9999, mx = 0;
    for (size_t k = 0; k < nfr; k++) {
        int d = (int)fr[k].tia_on - (int)fr[k].tia_dark;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
    }
    double dc = 5.0 * 0.99996 * 100000.0 * 1e-6 / 3.3 * 4096.0;
    CHECK_NEAR((mx - mn) / 2.0, dc * 0.1, 3.0);  /* 10 % modulation depth */
    CHECK_NEAR((mx + mn) / 2.0, dc, 4.0);
    /* huge light: TIA clips at the rail */
    uint32_t big = 10000000;
    sim_set_ppg_source(src_const, &big);
    run_frames(50);
    CHECK_EQ(fr[nfr - 1].tia_on, 4095);
}

TEST(ring_overrun_and_shared_timeline)
{
    fresh();
    ppg_up(5.0);
    hal_acq_start(HAL_ACQ_PPG | HAL_ACQ_ECG);
    CHECK_EQ(hal_acq_running(), HAL_ACQ_PPG | HAL_ACQ_ECG);
    uint16_t e[64];
    CHECK_EQ(ecg_read(e, 64), 1); /* ECG sample 0 at the same instant as frame 0 start */
    hal_delay_us(9949);
    hal_ppg_frame_t f[4];
    CHECK_EQ(ppg_read(f, 4), 0);
    CHECK_EQ(ecg_read(e, 64), 39); /* samples 1..39 at 250..9750 us */
    hal_delay_us(1);
    CHECK_EQ(ppg_read(f, 4), 1);
    CHECK_EQ(ecg_read(e, 64), 0);
    hal_delay_us(50);
    CHECK_EQ(ecg_read(e, 64), 1); /* sample 40 at 10000 us, next frame starts there */
    hal_delay_ms(500);
    CHECK_EQ(hal_ppg_overruns(), 0); /* counted once a read passes the loss */
    uint32_t first;
    CHECK_EQ(hal_ppg_read(f, 4, &first), 4);
    CHECK_EQ(first, 51 - SIM_PPG_RING_FRAMES); /* frames 0..50 done, 1 read */
    CHECK_EQ(hal_ppg_overruns(), 50 - SIM_PPG_RING_FRAMES);
    /* a missed frame: the index skips it, a read stops before the gap */
    sim_acq_set_missed(0, 3);
    while (hal_ppg_read(f, 4, &first) != 0) {
    }
    hal_delay_ms(60);
    uint32_t idx[8];
    size_t got = 0, k;
    while ((k = hal_ppg_read(f, 4, &first)) != 0) {
        for (size_t i = 0; i < k && got < 8; i++) idx[got++] = first + (uint32_t)i;
    }
    CHECK(got >= 3);
    for (size_t i = 0; i < got; i++) CHECK((idx[i] + 1) % 3 != 0);
    sim_acq_set_missed(0, 0);
    CHECK_EQ(sim_violation_count_of(SIM_V_PPG_START), 0);
}

TEST(adc_one_shot_values)
{
    fresh();
    sim_ppg_set_ambient(100000.0, 0.0, 0.0);
    high(PIN_ANA_EN);
    hal_ppg_analog(true);
    hal_ppg_set_offset(500);
    CHECK_NEAR(hal_adc_read_once(HAL_ADC_TIA), (0.245 + 0.1) / 3.3 * 4096.0, 1.5);
    double hi = 11.0 * 500.0 / 4096.0 * 3.3 - 10.0 * (0.245 + 0.1);
    CHECK_NEAR(hal_adc_read_once(HAL_ADC_PPG_HI), hi / 3.3 * 4096.0, 2.0);
    hal_ppg_analog(false);
    CHECK_EQ(hal_adc_read_once(HAL_ADC_TIA), 0);
    CHECK_EQ(sim_violation_count(), 0);
}

int main(void)
{
    RUN(frame_timing_and_led_window);
    RUN(on_minus_dark_is_proportional_to_led_current);
    RUN(dac12_offset_moves_hi_on);
    RUN(ambient_cancels_in_on_minus_dark);
    RUN(pulsatile_source_and_clipping);
    RUN(ring_overrun_and_shared_timeline);
    RUN(adc_one_shot_values);
    return TEST_RESULT();
}
