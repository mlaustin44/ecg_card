/* Every link_fmt.h payload the radio builds or parses, at exact offsets. */
#include <string.h>

#include "link_fmt.h"
#include "rl_msg.h"
#include "test.h"

TEST(hello_layout)
{
    rl_hello_t h = {0, 1, 2, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, 2};
    uint8_t b[LINK_HELLO_LEN + 4];
    memset(b, 0xEE, sizeof b);
    h.fw_major = 3;
    CHECK_EQ(rl_msg_hello(&h, b), 12);
    CHECK_EQ(LINK_HELLO_LEN, 12);
    CHECK_EQ(b[0], LINK_PROTO_VERSION);
    CHECK_EQ(b[1], 3);
    CHECK_EQ(b[2], 1);
    CHECK_EQ(b[3], 2);
    CHECK_EQ(b[4], 0x11); /* least significant address byte first */
    CHECK_EQ(b[9], 0x66);
    CHECK_EQ(b[10], 2);
    CHECK_EQ(b[11], 0);
    CHECK_EQ(b[12], 0xEE); /* nothing written past the end */
}

TEST(conn_state_layout)
{
    rl_conn_state_t s = {LINK_CS_CONNECTED, 247, 16, LINK_SUB_ECG | LINK_SUB_HR, 0x1234, 2};
    uint8_t b[LINK_CONN_STATE_LEN + 2];
    memset(b, 0xEE, sizeof b);
    CHECK_EQ(rl_msg_conn_state(&s, b), 10);
    CHECK_EQ(LINK_CONN_STATE_LEN, 10);
    CHECK_EQ(b[0], 3);
    CHECK_EQ(b[1], 247);
    CHECK_EQ(b[2], 0);
    CHECK_EQ(b[3], 16);
    CHECK_EQ(b[4], 0);
    CHECK_EQ(b[5], 0x11);
    CHECK_EQ(b[6], 0x34);
    CHECK_EQ(b[7], 0x12);
    CHECK_EQ(b[8], 2);
    CHECK_EQ(b[9], 0);
    CHECK_EQ(b[10], 0xEE);
}

TEST(ack_both_ways)
{
    uint8_t b[2], seq = 0, res = 0;
    CHECK_EQ(rl_msg_ack(0x7F, LINK_RES_BUSY, b), 2);
    CHECK_EQ(b[0], 0x7F);
    CHECK_EQ(b[1], 5);
    CHECK(rl_msg_parse_ack(b, 2, &seq, &res));
    CHECK_EQ(seq, 0x7F);
    CHECK_EQ(res, LINK_RES_BUSY);
    CHECK(!rl_msg_parse_ack(b, 1, &seq, &res));
    CHECK(!rl_msg_parse_ack(b, 3, &seq, &res));
}

TEST(cmd_length)
{
    CHECK(!rl_msg_cmd_len_ok(0));
    CHECK(rl_msg_cmd_len_ok(1));
    CHECK(rl_msg_cmd_len_ok(16));
    CHECK(!rl_msg_cmd_len_ok(17));
}

TEST(status_layout)
{
    uint8_t p[LINK_STATUS_LEN + 1];
    rl_status_t s;
    for (unsigned i = 0; i < sizeof p; i++)
        p[i] = (uint8_t)(0xA0 + i);
    CHECK_EQ(LINK_STATUS_LEN, 20);
    CHECK(!rl_msg_parse_status(p, 19, &s));
    CHECK(rl_msg_parse_status(p, 20, &s));
    CHECK_EQ(s.proto, 0xA0);
    CHECK_EQ(s.mode, 0xA1);
    CHECK_EQ(s.meas, 0xA2);
    CHECK_EQ(s.batt_pct, 0xA3);
    CHECK_EQ(s.flags, 0xA4);
    CHECK_EQ(s.gain, 0xA5);
    CHECK_EQ(s.notch, 0xA6);
    CHECK_EQ(s.rld, 0xA7);
    CHECK_EQ(s.led_limit, 0xA8);
    CHECK_EQ(s.sweep, 0xA9);
    CHECK_EQ(s.notch_hz, 0xAA);
    CHECK_EQ(s.hr_bpm, 0xAB);
    CHECK_EQ(s.batt_mv, 0xADAC);
    CHECK_EQ(s.fw_major, 0xAE);
    CHECK_EQ(s.fw_minor, 0xAF);
    CHECK_EQ(s.fw_patch, 0xB0);
    CHECK_EQ(s.uptime_min, 0xB3B2); /* offset 17 reserved */
    CHECK(memcmp(s.raw, p, 20) == 0);
    /* a longer (future) payload: first 20 bytes kept */
    CHECK(rl_msg_parse_status(p, 21, &s));
    CHECK(memcmp(s.raw, p, 20) == 0);
}

TEST(status_default_and_battery)
{
    uint8_t d[LINK_STATUS_LEN];
    rl_msg_status_default(d);
    CHECK_EQ(d[0], LINK_PROTO_VERSION);
    CHECK_EQ(d[1], 0);
    CHECK_EQ(d[2], 0xFF);
    CHECK_EQ(d[3], 0xFF);
    for (unsigned i = 4; i < LINK_STATUS_LEN; i++)
        CHECK_EQ(d[i], 0);
    CHECK_EQ(rl_msg_battery_level(255), 0);
    CHECK_EQ(rl_msg_battery_level(0), 0);
    CHECK_EQ(rl_msg_battery_level(57), 57);
    CHECK_EQ(rl_msg_battery_level(100), 100);
    CHECK_EQ(rl_msg_battery_level(130), 100);
}

TEST(radio_ctrl)
{
    rl_radio_ctrl_t rc;
    const uint8_t start[] = {LINK_RC_ADV_START}, stop[] = {LINK_RC_ADV_STOP},
                  disc[] = {LINK_RC_DISCONNECT}, shut[] = {LINK_RC_SHUTDOWN},
                  extra[] = {LINK_RC_ADV_START, 0}, unk[] = {4}, unk2[] = {0x7F};
    CHECK_EQ(rl_msg_parse_radio_ctrl(start, 1, &rc), LINK_RES_OK);
    CHECK_EQ(rc.op, LINK_RC_ADV_START);
    CHECK_EQ(rl_msg_parse_radio_ctrl(stop, 1, &rc), LINK_RES_OK);
    CHECK_EQ(rc.op, LINK_RC_ADV_STOP);
    CHECK_EQ(rl_msg_parse_radio_ctrl(disc, 1, &rc), LINK_RES_OK);
    CHECK_EQ(rl_msg_parse_radio_ctrl(shut, 1, &rc), LINK_RES_OK);
    CHECK_EQ(rc.op, LINK_RC_SHUTDOWN);
    CHECK_EQ(rl_msg_parse_radio_ctrl(extra, 2, &rc), LINK_RES_BAD_ARG);
    CHECK_EQ(rl_msg_parse_radio_ctrl(start, 0, &rc), LINK_RES_BAD_ARG);
    CHECK_EQ(rl_msg_parse_radio_ctrl(unk, 1, &rc), LINK_RES_UNKNOWN);
    CHECK_EQ(rl_msg_parse_radio_ctrl(unk2, 1, &rc), LINK_RES_UNKNOWN);

    const uint8_t info[] = {LINK_RC_SET_INFO, LINK_INFO_MCU_FW, '1', '.', '2', '.', '3'};
    CHECK_EQ(rl_msg_parse_radio_ctrl(info, sizeof info, &rc), LINK_RES_OK);
    CHECK_EQ(rc.info_kind, 1);
    CHECK_EQ(rc.text_len, 5);
    CHECK(memcmp(rc.text, "1.2.3", 5) == 0);
    CHECK(rc.text == &info[2]);
    const uint8_t info_empty[] = {LINK_RC_SET_INFO, LINK_INFO_MCU_FW};
    CHECK_EQ(rl_msg_parse_radio_ctrl(info_empty, 2, &rc), LINK_RES_OK);
    CHECK_EQ(rc.text_len, 0);
    CHECK_EQ(rl_msg_parse_radio_ctrl(info, 1, &rc), LINK_RES_BAD_ARG);
    const uint8_t info_kind[] = {LINK_RC_SET_INFO, 2, 'x'};
    CHECK_EQ(rl_msg_parse_radio_ctrl(info_kind, 3, &rc), LINK_RES_UNKNOWN);
    uint8_t longinfo[40] = {LINK_RC_SET_INFO, LINK_INFO_MCU_FW};
    memset(&longinfo[2], 'v', 38);
    CHECK_EQ(rl_msg_parse_radio_ctrl(longinfo, sizeof longinfo, &rc), LINK_RES_OK);
    CHECK_EQ(rc.text_len, RL_INFO_MAX);
}

TEST(event_bpm)
{
    /* R_PEAK: kind, u32 index, u16 hr */
    const uint8_t rp[7] = {STREAM_EV_R_PEAK, 1, 2, 3, 4, 72, 0};
    /* PULSE: kind, u32 foot, u16 bpm, u16 pat, u16 pi */
    const uint8_t pu[11] = {STREAM_EV_PULSE, 1, 2, 3, 4, 0x2C, 0x01, 9, 9, 9, 9};
    const uint8_t lead[2] = {STREAM_EV_LEAD, 1};
    uint8_t kind = 0;
    uint16_t bpm = 0;
    CHECK(rl_msg_event_bpm(rp, 7, &kind, &bpm));
    CHECK_EQ(kind, STREAM_EV_R_PEAK);
    CHECK_EQ(bpm, 72);
    CHECK(!rl_msg_event_bpm(rp, 6, &kind, &bpm));
    CHECK(rl_msg_event_bpm(pu, 11, &kind, &bpm));
    CHECK_EQ(kind, STREAM_EV_PULSE);
    CHECK_EQ(bpm, 300);
    CHECK(!rl_msg_event_bpm(pu, 10, &kind, &bpm));
    CHECK(!rl_msg_event_bpm(lead, 2, &kind, &bpm));
    CHECK(!rl_msg_event_bpm(lead, 0, &kind, &bpm));
}

TEST(hr_select)
{
    rl_hr_sel_t s = {0};
    CHECK(rl_hr_select(&s, STREAM_EV_PULSE, 1000)); /* no ECG yet: PPG used */
    CHECK(rl_hr_select(&s, STREAM_EV_R_PEAK, 2000));
    CHECK(!rl_hr_select(&s, STREAM_EV_PULSE, 2500)); /* ECG wins */
    CHECK(!rl_hr_select(&s, STREAM_EV_PULSE, 2000 + RL_HR_ECG_HOLD_MS));
    CHECK(rl_hr_select(&s, STREAM_EV_PULSE, 2001 + RL_HR_ECG_HOLD_MS)); /* ECG gone */
    CHECK(!rl_hr_select(&s, STREAM_EV_LEAD, 9000));
    /* wrap of the millisecond counter */
    rl_hr_sel_t w = {0};
    CHECK(rl_hr_select(&w, STREAM_EV_R_PEAK, 0xFFFFFF00u));
    CHECK(!rl_hr_select(&w, STREAM_EV_PULSE, 0x00000100u));
}

TEST(ack_tracking)
{
    rl_acktrack_t a;
    rl_ack_init(&a);
    rl_ack_sent(&a, 10, 0);
    rl_ack_sent(&a, 11, 100);
    CHECK(rl_ack_received(&a, 10));
    CHECK(!rl_ack_received(&a, 10)); /* already acknowledged */
    CHECK_EQ(a.acked, 1);
    CHECK_EQ(a.unmatched, 1);
    CHECK_EQ(rl_ack_poll(&a, 100 + RL_ACK_TIMEOUT_MS), 0);
    CHECK_EQ(rl_ack_poll(&a, 101 + RL_ACK_TIMEOUT_MS), 1);
    CHECK_EQ(a.timeouts, 1);
    CHECK(!rl_ack_received(&a, 11)); /* late ACK is unmatched */
    /* more outstanding commands than slots: oldest is given up */
    rl_ack_init(&a);
    for (unsigned i = 0; i < RL_ACK_SLOTS + 2; i++)
        rl_ack_sent(&a, (uint8_t)i, 10u * i);
    CHECK_EQ(a.overwritten, 2);
    CHECK_EQ(a.timeouts, 2);
    CHECK(!rl_ack_received(&a, 0));
    CHECK(!rl_ack_received(&a, 1));
    CHECK(rl_ack_received(&a, 2));
    CHECK(rl_ack_received(&a, RL_ACK_SLOTS + 1));
}

int main(void)
{
    RUN(hello_layout);
    RUN(conn_state_layout);
    RUN(ack_both_ways);
    RUN(cmd_length);
    RUN(status_layout);
    RUN(status_default_and_battery);
    RUN(radio_ctrl);
    RUN(event_bpm);
    RUN(hr_select);
    RUN(ack_tracking);
    return TEST_RESULT();
}
