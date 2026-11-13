/* Lead-off / contact, baseline, wander, quality score, auto-gain advisor. */
#include <math.h>
#include <stdlib.h>

#include "board.h"
#include "dsp_ecg_mon.h"
#include "synth.h"
#include "test.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Feed a constant / generated code for ms milliseconds, 1 sample at a time;
 * returns the sample (from the start) at which ev_mask first fired, or -1. */
static long feed(dsp_lead_t *l, long ms, uint16_t (*gen)(long), uint8_t ev_mask, long *t)
{
    long hit = -1;
    for (long i = 0; i < ms * 4; i++, (*t)++) {
        uint16_t c = gen(*t);
        uint8_t ev = dsp_lead_push(l, &c, 1);
        if ((ev & ev_mask) && hit < 0)
            hit = i;
    }
    return hit;
}

static uint16_t g_mid(long t) { (void)t; return 2047; }
static uint16_t g_rail(long t) { (void)t; return 4095; }
static uint16_t g_low_rail(long t) { (void)t; return 30; }
/* on the rail, but 50 Hz pickup pulls it off for 4 ms every cycle */
static uint16_t g_rail_mains(long t) { return (t % 80) < 16 ? 3700 : 4095; }
static uint16_t g_ecg(long t) { return (uint16_t)(2047 + 300 * sin(2 * M_PI * 10 * t / 4000.0)); }

TEST(lead_off_and_contact)
{
    dsp_lead_t l;
    long t = 0;
    dsp_lead_init(&l, 2047);
    CHECK(dsp_lead_is_off(&l));
    long c = feed(&l, 100, g_ecg, DSP_LEAD_EV_CONTACT, &t);
    printf("  contact reported after %.1f ms off the rail\n", c / 4.0);
    CHECK_NEAR(c / 4.0, 20, 0.5);
    CHECK(!dsp_lead_is_off(&l));
    /* 80 ms on the rail: not lead-off; counts as clipping */
    CHECK_EQ(feed(&l, 80, g_rail, DSP_LEAD_EV_OFF, &t), -1);
    CHECK(!dsp_lead_is_off(&l));
    CHECK_EQ(dsp_lead_take_clip(&l), 320);
    CHECK_EQ(dsp_lead_take_clip(&l), 0);
    feed(&l, 100, g_mid, 0, &t);
    long off = feed(&l, 300, g_rail, DSP_LEAD_EV_OFF, &t);
    printf("  lead-off reported %.2f ms after reaching the rail\n", off / 4.0);
    CHECK_NEAR(off / 4.0, 100.25, 0.3);
    CHECK(dsp_lead_is_off(&l));
    long c2 = feed(&l, 100, g_mid, DSP_LEAD_EV_CONTACT, &t);
    CHECK_NEAR(c2 / 4.0, 20, 0.5);
    /* low rail too, with mains riding on it */
    off = feed(&l, 300, g_low_rail, DSP_LEAD_EV_OFF, &t);
    CHECK(off > 0);
    feed(&l, 100, g_mid, 0, &t);
    off = feed(&l, 500, g_rail_mains, DSP_LEAD_EV_OFF, &t);
    printf("  rail with 50 Hz excursions: lead-off after %.1f ms\n", off / 4.0);
    CHECK(off > 0 && off / 4.0 < 160);
    /* stays off while mains keeps kicking it 4 ms off the rail */
    CHECK_EQ(feed(&l, 500, g_rail_mains, DSP_LEAD_EV_CONTACT, &t), -1);
}

TEST(baseline_and_rail_rate)
{
    dsp_lead_t l;
    dsp_lead_init(&l, 2000);
    uint16_t buf[4000];
    for (int i = 0; i < 4000; i++)
        buf[i] = 3300;
    dsp_lead_push(&l, buf, 4000);
    CHECK_NEAR(dsp_lead_baseline(&l), 1300, 2);
    CHECK(dsp_lead_far_from_mid(&l));
    for (int i = 0; i < 4000; i++)
        buf[i] = (uint16_t)(i < 100 ? 4095 : 2100);
    dsp_lead_push(&l, buf, 4000);
    CHECK_NEAR(dsp_lead_baseline(&l), 100, 2);
    CHECK(!dsp_lead_far_from_mid(&l));
    CHECK_EQ(dsp_lead_rail_last_s(&l), 100);
}

TEST(wander)
{
    dsp_wander_t w;
    dsp_wander_init(&w, ECG_GAIN_LO);
    /* 0.5 Hz, 1 mV peak: 0.5 s block means are +-(2/pi) mV -> range 1273 uV */
    const double uv = 805.7 / (8.0 * 558.0);
    for (int i = 0; i < 2000; i++) {
        int16_t x = (int16_t)lround(1000.0 / uv * sin(2 * M_PI * 0.5 * i / 500.0));
        dsp_wander_push(&w, &x, 1);
    }
    unsigned pp = dsp_wander_uvpp(&w);
    printf("  0.5 Hz 2 mVpp wander -> %u uV block-mean range (1273 expected)\n", pp);
    CHECK_NEAR(pp, 1273, 10);
    dsp_wander_init(&w, ECG_GAIN_LO);
    int16_t z[2000] = { 0 };
    dsp_wander_push(&w, z, 2000);
    CHECK_EQ(dsp_wander_uvpp(&w), 0);
}

TEST(quality)
{
    dsp_quality_in_t q = { .lead_off = false, .rail_per_s = 0, .mains_uvpp = 20, .wander_uvpp = 100,
                           .rr_spread_pct = 8, .ms_since_beat = 500, .ms_since_contact = 20000 };
    CHECK_EQ(dsp_ecg_quality(&q), 4);
    q.mains_uvpp = 150;
    CHECK_EQ(dsp_ecg_quality(&q), 3);
    q.mains_uvpp = 500;
    CHECK_EQ(dsp_ecg_quality(&q), 2);
    q.wander_uvpp = 2000;
    CHECK_EQ(dsp_ecg_quality(&q), 1); /* clamped while in contact */
    q.mains_uvpp = 20;
    q.wander_uvpp = 700;
    q.rr_spread_pct = 45;
    CHECK_EQ(dsp_ecg_quality(&q), 2);
    q.rr_spread_pct = 8;
    q.rail_per_s = 10;
    CHECK_EQ(dsp_ecg_quality(&q), 2);
    q.rail_per_s = 300;
    CHECK_EQ(dsp_ecg_quality(&q), 0);
    q.rail_per_s = 0;
    q.wander_uvpp = 0;
    q.lead_off = true;
    CHECK_EQ(dsp_ecg_quality(&q), 0);
    q.lead_off = false;
    /* settling: no RR yet, 1 s after contact -> not penalised */
    q.rr_spread_pct = 255;
    q.ms_since_beat = 1000;
    q.ms_since_contact = 1000;
    CHECK_EQ(dsp_ecg_quality(&q), 4);
    q.ms_since_contact = 5000;
    q.ms_since_beat = 5000;
    CHECK_EQ(dsp_ecg_quality(&q), 3);
}

TEST(gain_advisor)
{
    dsp_gain_t g;
    dsp_gain_init(&g, false);
    /* small QRS (0.6 mV ~ 3300 units) for 5 s -> UP */
    dsp_gain_advice_t a = DSP_GAIN_KEEP;
    for (int ms = 0; ms < 4900; ms += 100) {
        if (ms % 800 == 0)
            dsp_gain_beat(&g, 3300);
        a = dsp_gain_tick(&g, 100, 0);
    }
    CHECK_EQ(a, DSP_GAIN_KEEP);
    a = dsp_gain_tick(&g, 100, 0);
    CHECK_EQ(a, DSP_GAIN_UP);
    /* large QRS: stays */
    dsp_gain_init(&g, false);
    for (int ms = 0; ms < 12000; ms += 100) {
        if (ms % 800 == 0)
            dsp_gain_beat(&g, 9000);
        a = dsp_gain_tick(&g, 100, 0);
    }
    CHECK_EQ(a, DSP_GAIN_KEEP);
    /* small QRS but one clip in each window: stays */
    dsp_gain_init(&g, false);
    for (int ms = 0; ms < 12000; ms += 100) {
        if (ms % 800 == 0)
            dsp_gain_beat(&g, 3000);
        a = dsp_gain_tick(&g, 100, ms % 4000 == 0 ? 5u : 0u);
    }
    CHECK_EQ(a, DSP_GAIN_KEEP);
    /* at 1098x: repeated clipping -> DOWN; isolated clip -> stays */
    dsp_gain_init(&g, true);
    for (int ms = 0; ms < 10000; ms += 100)
        a = dsp_gain_tick(&g, 100, ms == 3000 ? 3u : 0u);
    CHECK_EQ(a, DSP_GAIN_KEEP);
    for (int ms = 0; ms < 1000; ms += 100)
        a = dsp_gain_tick(&g, 100, ms % 300 == 0 ? 2u : 0u);
    CHECK_EQ(a, DSP_GAIN_DOWN);
    dsp_gain_init(&g, true);
    dsp_gain_beat(&g, 31000);
    CHECK_EQ(dsp_gain_tick(&g, 100, 0), DSP_GAIN_DOWN);
}

int main(void)
{
    RUN(lead_off_and_contact);
    RUN(baseline_and_rail_rate);
    RUN(wander);
    RUN(quality);
    RUN(gain_advisor);
    return TEST_RESULT();
}
