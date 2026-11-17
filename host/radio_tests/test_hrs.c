/* Heart Rate Measurement (0x2A37) state: flags, RR intervals, cadence
 * (rl_hrs.c, link_fmt.h "Standard services"). */
#include <string.h>

#include "link_fmt.h"
#include "rl_hrs.h"
#include "stream_fmt.h"
#include "test.h"

#define MEAS_WAIT    0u
#define MEAS_SETTLE  1u
#define MEAS_RUNNING 2u

static uint8_t ev[11];

static const uint8_t *rpeak(uint32_t idx, uint16_t bpm)
{
    ev[0] = STREAM_EV_R_PEAK;
    ev[1] = (uint8_t)idx;
    ev[2] = (uint8_t)(idx >> 8);
    ev[3] = (uint8_t)(idx >> 16);
    ev[4] = (uint8_t)(idx >> 24);
    ev[5] = (uint8_t)bpm;
    ev[6] = (uint8_t)(bpm >> 8);
    return ev;
}

static const uint8_t *pulse(uint32_t idx, uint16_t bpm)
{
    rpeak(idx, bpm);
    ev[0] = STREAM_EV_PULSE;
    ev[7] = ev[8] = ev[9] = ev[10] = 0;
    return ev;
}

static bool beat(rl_hrs_t *h, uint32_t idx, uint16_t bpm, uint32_t t)
{
    return rl_hrs_event(h, rpeak(idx, bpm), 7, t);
}

static uint16_t rr_at(const uint8_t *v, unsigned i)
{
    return (uint16_t)(v[2 + 2 * i] | (v[3 + 2 * i] << 8));
}

/* active (ECG), subscribed at t = 0, in contact */
static void setup(rl_hrs_t *h)
{
    rl_hrs_init(h);
    rl_hrs_status(h, LINK_MODE_ECG, MEAS_RUNNING, 0);
    rl_hrs_subscribed(h, true, 0);
}

TEST(constants)
{
    CHECK_EQ(RL_HRS_MAX_LEN, 20);     /* fits a 23-byte default ATT MTU */
    CHECK_EQ(RL_HRS_MEAS_RUNNING, 2); /* UI_MEAS_RUNNING (link_fmt.h) */
}

TEST(rr_conversion)
{
    uint16_t rr = 0xEEEE;
    CHECK(rl_hrs_rr_1024(400, &rr)); /* 800 ms = 819.2 */
    CHECK_EQ(rr, 819);
    CHECK(rl_hrs_rr_1024(500, &rr)); /* 1000 ms = 1024 exactly */
    CHECK_EQ(rr, 1024);
    CHECK(rl_hrs_rr_1024(427, &rr)); /* 854 ms = 874.496: down */
    CHECK_EQ(rr, 874);
    CHECK(rl_hrs_rr_1024(428, &rr)); /* 856 ms = 876.544: up */
    CHECK_EQ(rr, 877);
    CHECK(rl_hrs_rr_1024(125, &rr)); /* 250 ms: lowest accepted */
    CHECK_EQ(rr, 256);
    CHECK(rl_hrs_rr_1024(1250, &rr)); /* 2500 ms: highest accepted */
    CHECK_EQ(rr, 2560);
    rr = 0xEEEE;
    CHECK(!rl_hrs_rr_1024(124, &rr)); /* 248 ms */
    CHECK(!rl_hrs_rr_1024(1251, &rr)); /* 2502 ms */
    CHECK(!rl_hrs_rr_1024(0, &rr));
    CHECK(!rl_hrs_rr_1024(0xFFFFFF00u, &rr)); /* index went backwards */
    CHECK(!rl_hrs_rr_1024(4295093u, &rr));    /* * 1000 wraps to 125704: in range */
    CHECK_EQ(rr, 0xEEEE);
    /* every accepted value: within half a unit of the exact value */
    for (uint32_t s = 125; s <= 1250; s++) {
        CHECK(rl_hrs_rr_1024(s, &rr));
        CHECK_NEAR(rr, s * 2.048, 0.5);
    }
}

TEST(flags_and_contact)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    rl_hrs_init(&h);
    rl_hrs_status(&h, LINK_MODE_ECG, MEAS_SETTLE, 0);
    rl_hrs_subscribed(&h, true, 0);
    CHECK(rl_hrs_enabled(&h));
    CHECK_EQ(rl_hrs_take(&h, 1000, v), 2);
    CHECK_EQ(v[0], 0x04); /* contact supported, not detected, u8 bpm */
    CHECK_EQ(v[1], 0);    /* no beat yet */
    rl_hrs_status(&h, LINK_MODE_ECG, MEAS_RUNNING, 1500);
    CHECK_EQ(rl_hrs_take(&h, 2000, v), 2);
    CHECK_EQ(v[0], 0x06);
    rl_hrs_status(&h, LINK_MODE_ECG, MEAS_WAIT, 2500);
    rl_hrs_take(&h, 3000, v);
    CHECK_EQ(v[0], 0x04);
    /* other meas states are not contact either */
    rl_hrs_status(&h, LINK_MODE_HRM, 3u, 3500);
    rl_hrs_take(&h, 4000, v);
    CHECK_EQ(v[0], 0x04);
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_RUNNING, 4500);
    rl_hrs_take(&h, 5000, v);
    CHECK_EQ(v[0], 0x06);
}

TEST(bpm_value)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 1000, 72, 100);
    rl_hrs_take(&h, 100, v);
    CHECK_EQ(v[1], 72);
    beat(&h, 1400, 75, 900);
    rl_hrs_take(&h, 1100, v);
    CHECK_EQ(v[1], 75);
    rl_hrs_take(&h, 2100, v); /* no new beat: latest value again */
    CHECK_EQ(v[1], 75);
    beat(&h, 1600, 300, 2200); /* saturates */
    rl_hrs_take(&h, 3100, v);
    CHECK_EQ(v[1], 255);
}

TEST(first_beat_no_rr_then_rr)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 1000, 70, 100);
    CHECK_EQ(rl_hrs_take(&h, 100, v), 2); /* one beat: no interval */
    CHECK_EQ(v[0], 0x06);
    beat(&h, 1400, 70, 900);  /* 800 ms */
    beat(&h, 1900, 70, 1050); /* 1000 ms */
    CHECK_EQ(rl_hrs_take(&h, 1100, v), 6);
    CHECK_EQ(v[0], 0x16);
    CHECK_EQ(v[1], 70);
    CHECK_EQ(rr_at(v, 0), 819); /* oldest first */
    CHECK_EQ(rr_at(v, 1), 1024);
    CHECK_EQ(v[2], 0x33); /* little-endian */
    CHECK_EQ(v[3], 0x03);
    /* each interval once */
    CHECK_EQ(rl_hrs_take(&h, 2100, v), 2);
    CHECK_EQ(v[0], 0x06);
}

TEST(out_of_range_rr_dropped)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 0, 70, 10);
    beat(&h, 100, 70, 20);  /* 200 ms: dropped, but restarts the interval */
    beat(&h, 500, 70, 30);  /* 800 ms from the previous beat */
    beat(&h, 2500, 70, 40); /* 4000 ms: dropped */
    beat(&h, 2750, 70, 50); /* 500 ms */
    beat(&h, 2750, 70, 60); /* same index: dropped */
    CHECK_EQ(rl_hrs_take(&h, 1000, v), 6);
    CHECK_EQ(rr_at(v, 0), 819);
    CHECK_EQ(rr_at(v, 1), 512);
    CHECK_EQ(h.rr_rejected, 3);
}

TEST(rr_index_wrap)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 0xFFFFFF00u, 60, 10);
    beat(&h, 0x00000090u, 60, 20); /* 0x190 = 400 samples */
    CHECK_EQ(rl_hrs_take(&h, 1000, v), 4);
    CHECK_EQ(rr_at(v, 0), 819);
}

TEST(nine_pending_keeps_newest)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    uint32_t idx = 0;
    setup(&h);
    beat(&h, idx, 70, 1);
    /* 11 intervals of 300, 301, ... 310 samples */
    for (uint32_t i = 0; i < 11; i++) {
        idx += 300 + i;
        beat(&h, idx, 70, 2 + i);
    }
    CHECK_EQ(h.rr_overflow, 2);
    CHECK_EQ(rl_hrs_take(&h, 1000, v), RL_HRS_MAX_LEN);
    CHECK_EQ(v[0], 0x16);
    for (unsigned i = 0; i < 9; i++) {
        uint16_t want = 0;
        rl_hrs_rr_1024(302 + i, &want);
        CHECK_EQ(rr_at(v, i), want);
    }
}

TEST(cadence_1hz)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    rl_hrs_init(&h);
    rl_hrs_status(&h, LINK_MODE_ECG, MEAS_RUNNING, 50);
    rl_hrs_subscribed(&h, true, 200);
    CHECK_EQ(rl_hrs_wait_ms(&h, 200), 1000); /* first one a second after enabling */
    CHECK_EQ(rl_hrs_wait_ms(&h, 700), 500);
    CHECK_EQ(rl_hrs_wait_ms(&h, 1200), 0);
    rl_hrs_take(&h, 1200, v);
    CHECK_EQ(rl_hrs_wait_ms(&h, 1200), 1000);
    CHECK_EQ(rl_hrs_wait_ms(&h, 2199), 1);
    CHECK_EQ(rl_hrs_wait_ms(&h, 2200), 0);
    CHECK_EQ(rl_hrs_wait_ms(&h, 5000), 0); /* late: due, not negative */
    /* an unchanged LINK_STATUS (every 5 s) does not move the period */
    rl_hrs_take(&h, 5000, v);
    rl_hrs_status(&h, LINK_MODE_ECG, MEAS_RUNNING, 5300);
    CHECK_EQ(rl_hrs_wait_ms(&h, 5300), 700);
    /* millisecond counter wrap */
    rl_hrs_take(&h, 0xFFFFFE00u, v);
    CHECK_EQ(rl_hrs_wait_ms(&h, 0x00000100u), 1000 - 0x300);
}

TEST(beat_after_quiet_second)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    CHECK(beat(&h, 1000, 70, 300)); /* nothing sent since enabling: at once */
    rl_hrs_take(&h, 300, v);
    CHECK_EQ(v[1], 70);
    CHECK(!beat(&h, 1400, 71, 900)); /* sent 600 ms ago: wait for the period */
    CHECK_EQ(rl_hrs_wait_ms(&h, 900), 400);
    rl_hrs_take(&h, 1300, v);
    CHECK(!beat(&h, 1800, 72, 1700));
    CHECK(!beat(&h, 2200, 72, 2299)); /* 999 ms */
    CHECK(beat(&h, 2600, 72, 2300));  /* 1000 ms with nothing sent */
    /* LEAD / STATE / short payloads are not beats */
    rl_hrs_take(&h, 2300, v);
    const uint8_t lead[2] = {STREAM_EV_LEAD, 1};
    CHECK(!rl_hrs_event(&h, lead, 2, 5000));
    CHECK(!rl_hrs_event(&h, rpeak(3000, 70), 6, 5000));
    CHECK(rl_hrs_event(&h, rpeak(3000, 70), 7, 5000));
}

TEST(idle_or_unsubscribed_sends_nothing)
{
    rl_hrs_t h;
    rl_hrs_init(&h);
    rl_hrs_subscribed(&h, true, 0);
    rl_hrs_status(&h, LINK_MODE_IDLE, 0xFFu, 0);
    CHECK(!rl_hrs_enabled(&h));
    CHECK_EQ(rl_hrs_wait_ms(&h, 5000), RL_HRS_NEVER);
    CHECK(!beat(&h, 1000, 70, 100));
    CHECK_EQ(h.bpm, 0); /* stale event after the mode ended: ignored */
    /* active but not subscribed */
    rl_hrs_init(&h);
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_RUNNING, 0);
    CHECK(!rl_hrs_enabled(&h));
    CHECK_EQ(rl_hrs_wait_ms(&h, 5000), RL_HRS_NEVER);
    CHECK(!beat(&h, 1000, 70, 100));
    CHECK_EQ(h.bpm, 70); /* tracked, ready for a subscription */
    /* the mode ends: disabled, reset */
    rl_hrs_subscribed(&h, true, 200);
    CHECK(rl_hrs_enabled(&h));
    rl_hrs_status(&h, LINK_MODE_IDLE, 0xFFu, 300);
    CHECK(!rl_hrs_enabled(&h));
    CHECK_EQ(rl_hrs_wait_ms(&h, 5000), RL_HRS_NEVER);
}

TEST(subscription_restarts_clean)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    rl_hrs_init(&h);
    rl_hrs_status(&h, LINK_MODE_ECG, MEAS_RUNNING, 0);
    beat(&h, 0, 70, 100);
    beat(&h, 400, 70, 900); /* not subscribed: interval not kept */
    CHECK_EQ(h.n_rr, 0);
    rl_hrs_subscribed(&h, true, 1000);
    CHECK(beat(&h, 800, 71, 1700)); /* first since enabling: at once, with RR */
    CHECK_EQ(rl_hrs_take(&h, 1700, v), 4);
    CHECK_EQ(v[1], 71);
    CHECK_EQ(rr_at(v, 0), 819);
    /* unsubscribe with an interval pending; it is gone on resubscribing */
    beat(&h, 1200, 71, 2000);
    CHECK_EQ(h.n_rr, 1);
    rl_hrs_subscribed(&h, false, 2100);
    CHECK_EQ(rl_hrs_wait_ms(&h, 2100), RL_HRS_NEVER);
    rl_hrs_subscribed(&h, true, 2200);
    CHECK_EQ(rl_hrs_wait_ms(&h, 2200), 1000);
    CHECK_EQ(rl_hrs_take(&h, 3200, v), 2);
    /* subscribing again while subscribed changes nothing */
    beat(&h, 1600, 71, 3300);
    rl_hrs_subscribed(&h, true, 3400);
    CHECK_EQ(h.n_rr, 1);
    CHECK_EQ(rl_hrs_wait_ms(&h, 3400), 800);
}

TEST(contact_lost_and_regained)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 0, 70, 100);
    beat(&h, 400, 70, 900); /* pending 819 */
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_WAIT, 950); /* contact lost */
    CHECK_EQ(rl_hrs_take(&h, 1000, v), 2);
    CHECK_EQ(v[0], 0x04);
    CHECK_EQ(v[1], 70);
    beat(&h, 800, 80, 1200);  /* not in contact: bpm only */
    beat(&h, 1200, 80, 1500);
    CHECK_EQ(rl_hrs_take(&h, 2000, v), 2);
    CHECK_EQ(v[1], 80);
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_SETTLE, 2100);
    beat(&h, 1600, 80, 2200);
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_RUNNING, 2300); /* regained */
    beat(&h, 2000, 75, 2400); /* first beat after regaining: no RR */
    CHECK_EQ(rl_hrs_take(&h, 3000, v), 2);
    CHECK_EQ(v[0], 0x06);
    beat(&h, 2500, 75, 3400); /* 1000 ms */
    CHECK_EQ(rl_hrs_take(&h, 4000, v), 4);
    CHECK_EQ(v[0], 0x16);
    CHECK_EQ(rr_at(v, 0), 1024);
}

TEST(pulse_fallback_no_rr)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    rl_hrs_init(&h);
    rl_hrs_status(&h, LINK_MODE_PPG, MEAS_RUNNING, 0);
    rl_hrs_subscribed(&h, true, 0);
    CHECK(rl_hrs_event(&h, pulse(0, 66), 11, 100));
    rl_hrs_event(&h, pulse(400, 66), 11, 900);
    rl_hrs_event(&h, pulse(800, 67), 11, 1000);
    CHECK_EQ(rl_hrs_take(&h, 1000, v), 2); /* PULSE: bpm, never RR */
    CHECK_EQ(v[0], 0x06);
    CHECK_EQ(v[1], 67);
    /* both: R_PEAK wins, PULSE ignored while ECG is fresh */
    rl_hrs_status(&h, LINK_MODE_BOTH, MEAS_RUNNING, 1100);
    beat(&h, 10000, 72, 1200);
    rl_hrs_event(&h, pulse(10010, 90), 11, 1300);
    rl_hrs_take(&h, 2000, v);
    CHECK_EQ(v[1], 72);
    /* no R_PEAK for RL_HR_ECG_HOLD_MS: PULSE takes over, still no RR */
    rl_hrs_event(&h, pulse(12000, 91), 11, 1200 + RL_HR_ECG_HOLD_MS + 1);
    CHECK_EQ(rl_hrs_take(&h, 1200 + RL_HR_ECG_HOLD_MS + 1, v), 2);
    CHECK_EQ(v[1], 91);
    /* R_PEAK returns: RR from consecutive R_PEAKs only */
    beat(&h, 12100, 73, 7000); /* 2100 samples after the last R_PEAK: 4.2 s, dropped */
    beat(&h, 12500, 73, 7800);
    CHECK_EQ(rl_hrs_take(&h, 8000, v), 4);
    CHECK_EQ(v[1], 73);
    CHECK_EQ(rr_at(v, 0), 819);
}

TEST(mode_restart_resets)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 0, 70, 100);
    beat(&h, 400, 70, 900);
    rl_hrs_status(&h, LINK_MODE_IDLE, 0xFFu, 1000);
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_RUNNING, 2000);
    CHECK(rl_hrs_enabled(&h));
    CHECK_EQ(rl_hrs_wait_ms(&h, 2000), 1000);
    CHECK_EQ(rl_hrs_take(&h, 3000, v), 2);
    CHECK_EQ(v[1], 0); /* bpm from the previous session not reused */
    beat(&h, 50, 65, 3100); /* new acquisition, index from 0: no RR yet */
    CHECK_EQ(rl_hrs_take(&h, 4000, v), 2);
    CHECK_EQ(v[1], 65);
    /* a mode change that keeps a mode active is not a restart */
    rl_hrs_status(&h, LINK_MODE_ECG, MEAS_RUNNING, 4100);
    beat(&h, 450, 66, 4200);
    CHECK_EQ(rl_hrs_take(&h, 5000, v), 4);
    CHECK_EQ(v[1], 66);
}

TEST(bpm_stale_after_5s)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    CHECK_EQ(RL_HRS_BPM_STALE_MS, 5000);
    setup(&h);
    beat(&h, 1000, 70, 100);
    rl_hrs_take(&h, 100, v);
    CHECK_EQ(v[1], 70);
    CHECK_EQ(rl_hrs_take(&h, 100 + RL_HRS_BPM_STALE_MS - 1, v), 2);
    CHECK_EQ(v[1], 70); /* 4999 ms: still the last beat */
    rl_hrs_take(&h, 100 + RL_HRS_BPM_STALE_MS, v);
    CHECK_EQ(v[0], 0x06); /* contact bit unchanged */
    CHECK_EQ(v[1], 0);
    rl_hrs_take(&h, 6100, v);
    CHECK_EQ(v[1], 0);
    /* recovers on the next beat */
    CHECK(!beat(&h, 4000, 72, 6500)); /* sent 400 ms ago */
    rl_hrs_take(&h, 7100, v);
    CHECK_EQ(v[1], 72);
    CHECK_EQ(v[0], 0x06); /* 6 s since the previous R_PEAK: no RR */
    beat(&h, 4400, 73, 7400);
    CHECK_EQ(rl_hrs_take(&h, 8100, v), 4);
    CHECK_EQ(v[1], 73);
    CHECK_EQ(rr_at(v, 0), 819);
    /* a beat within the window keeps it fresh */
    beat(&h, 6400, 74, 12000);
    rl_hrs_take(&h, 16999, v);
    CHECK_EQ(v[1], 74);
    rl_hrs_take(&h, 17000, v);
    CHECK_EQ(v[1], 0);
}

TEST(bpm_stale_regardless_of_contact)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 1000, 70, 100);
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_WAIT, 500); /* contact lost */
    rl_hrs_take(&h, 1000, v);
    CHECK_EQ(v[0], 0x04);
    CHECK_EQ(v[1], 70);
    rl_hrs_take(&h, 5100, v);
    CHECK_EQ(v[0], 0x04);
    CHECK_EQ(v[1], 0);
    /* beats without contact still count as beats */
    beat(&h, 2000, 68, 5500);
    rl_hrs_take(&h, 6100, v);
    CHECK_EQ(v[0], 0x04);
    CHECK_EQ(v[1], 68);
    /* stale while unsubscribed: 0 on resubscribing */
    rl_hrs_subscribed(&h, false, 6200);
    rl_hrs_subscribed(&h, true, 20000);
    rl_hrs_take(&h, 21000, v);
    CHECK_EQ(v[1], 0);
}

TEST(bpm_stale_pulse_fallback)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    rl_hrs_init(&h);
    rl_hrs_status(&h, LINK_MODE_PPG, MEAS_RUNNING, 0);
    rl_hrs_subscribed(&h, true, 0);
    rl_hrs_event(&h, pulse(0, 66), 11, 100);
    rl_hrs_take(&h, 5099, v);
    CHECK_EQ(v[1], 66);
    rl_hrs_take(&h, 5100, v);
    CHECK_EQ(v[1], 0);
    rl_hrs_event(&h, pulse(3000, 64), 11, 5500);
    rl_hrs_take(&h, 6100, v);
    CHECK_EQ(v[1], 64);
    /* both: a PULSE ignored while ECG is fresh is not a beat */
    rl_hrs_status(&h, LINK_MODE_BOTH, MEAS_RUNNING, 6200);
    beat(&h, 9000, 72, 6300);
    rl_hrs_event(&h, pulse(9010, 90), 11, 8000);
    rl_hrs_event(&h, pulse(9500, 90), 11, 11000);
    rl_hrs_take(&h, 11300, v);
    CHECK_EQ(v[1], 0); /* last accepted beat at 6300 */
    rl_hrs_event(&h, pulse(9600, 91), 11, 11301); /* fallback now accepted */
    rl_hrs_take(&h, 12300, v);
    CHECK_EQ(v[1], 91);
}

TEST(bpm_stale_mode_restart)
{
    rl_hrs_t h;
    uint8_t v[RL_HRS_MAX_LEN];
    setup(&h);
    beat(&h, 1000, 70, 100);
    rl_hrs_status(&h, LINK_MODE_IDLE, 0xFFu, 1000);
    CHECK_EQ(rl_hrs_wait_ms(&h, 9000), RL_HRS_NEVER); /* idle: nothing due */
    rl_hrs_status(&h, LINK_MODE_HRM, MEAS_RUNNING, 9000);
    rl_hrs_take(&h, 10000, v);
    CHECK_EQ(v[1], 0); /* restart: none yet */
    beat(&h, 50, 65, 10500);
    rl_hrs_take(&h, 15499, v); /* stale timing from the new beat */
    CHECK_EQ(v[1], 65);
    rl_hrs_take(&h, 15500, v);
    CHECK_EQ(v[1], 0);
}

int main(void)
{
    RUN(constants);
    RUN(rr_conversion);
    RUN(flags_and_contact);
    RUN(bpm_value);
    RUN(first_beat_no_rr_then_rr);
    RUN(out_of_range_rr_dropped);
    RUN(rr_index_wrap);
    RUN(nine_pending_keeps_newest);
    RUN(cadence_1hz);
    RUN(beat_after_quiet_second);
    RUN(idle_or_unsubscribed_sends_nothing);
    RUN(subscription_restarts_clean);
    RUN(contact_lost_and_regained);
    RUN(pulse_fallback_no_rr);
    RUN(mode_restart_resets);
    RUN(bpm_stale_after_5s);
    RUN(bpm_stale_regardless_of_contact);
    RUN(bpm_stale_pulse_fallback);
    RUN(bpm_stale_mode_restart);
    return TEST_RESULT();
}
