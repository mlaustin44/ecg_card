/* Cross-check against the MSPM0 side: fw/app/src/stream.c is compiled as-is
 * into this test, with hal_stream_write() capturing its wire bytes.
 *
 *  - every MSPM0 emitter's frames are accepted by the radio parser, with the
 *    stream_fmt.h payload layouts;
 *  - frames built by the radio are byte-identical to the MSPM0's
 *    stream_build() and decode with the MSPM0's cobs_decode / CRC. */
#include <string.h>

#include "hal.h"
#include "link_fmt.h"
#include "rl_codec.h"
#include "rl_msg.h"
#include "rl_parser.h"
#include "stream.h"
#include "test.h"

static uint8_t cap[8192];
static size_t ncap;

size_t hal_stream_write(const uint8_t *buf, size_t n)
{
    if (ncap + n > sizeof cap)
        return 0;
    memcpy(cap + ncap, buf, n);
    ncap += n;
    return n;
}

static rl_parser_t P;
static rl_frame_t F[32];
static uint8_t FP[32][RL_MAX_PAYLOAD];
static int nf;

static void parse_capture(void)
{
    rl_frame_t f;
    rl_parser_init(&P);
    nf = 0;
    for (size_t i = 0; i < ncap; i++) {
        if (rl_parser_feed(&P, cap[i], &f) && nf < 32) {
            memcpy(FP[nf], f.payload, f.len);
            F[nf] = f;
            F[nf].payload = FP[nf];
            nf++;
        }
    }
    ncap = 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }

TEST(mspm0_frames_into_radio_parser)
{
    const int16_t ecg[10] = {-1, 2, -300, 400, 0, 32767, -32768, 7, 8, 9};
    const int16_t ppg[4] = {-100, 200, 0, -1};

    stream_init();
    ncap = 0;
    stream_ecg_block(0x01020304u, 0x05, ecg);
    stream_ppg_block(0x0A0B0C0Du, 0x0123, 0x06, ppg);
    stream_ev_r_peak(1000u, 72);
    stream_ev_pulse(2000u, 65, 180, 12);
    stream_ev_lead(true);
    stream_ev_state(3, 50, 2048, 88, 0x01);
    stream_ev_button(2, 1);
    parse_capture();

    CHECK_EQ(nf, 7);
    CHECK_EQ(P.st.crc_err + P.st.cobs_err + P.st.len_err + P.st.oversize, 0);
    CHECK_EQ(P.st.seq_gaps, 0);
    for (int i = 0; i < nf; i++)
        CHECK_EQ(F[i].seq, i);

    /* STREAM_ECG_BLOCK: u32 start, u8 flags, i16 s[10] = 25 bytes */
    CHECK_EQ(F[0].type, STREAM_ECG_BLOCK);
    CHECK_EQ(F[0].len, 25);
    CHECK_EQ(le32(F[0].payload), 0x01020304u);
    CHECK_EQ(F[0].payload[4], 0x05);
    CHECK_EQ((int16_t)le16(&F[0].payload[5 + 2 * 2]), -300);
    CHECK_EQ((int16_t)le16(&F[0].payload[5 + 2 * 6]), -32768);

    /* STREAM_PPG_BLOCK: u32 start, u16 led, u8 flags, 2 x {i16, i16} = 15 */
    CHECK_EQ(F[1].type, STREAM_PPG_BLOCK);
    CHECK_EQ(F[1].len, 15);
    CHECK_EQ(le32(F[1].payload), 0x0A0B0C0Du);
    CHECK_EQ(le16(&F[1].payload[4]), 0x0123);
    CHECK_EQ(F[1].payload[6], 0x06);
    CHECK_EQ((int16_t)le16(&F[1].payload[7]), -100);
    CHECK_EQ((int16_t)le16(&F[1].payload[13]), -1);

    /* events, and the radio's Heart Rate extraction from them */
    uint8_t kind;
    uint16_t bpm;
    CHECK_EQ(F[2].type, STREAM_EVENT);
    CHECK_EQ(F[2].len, 7);
    CHECK(rl_msg_event_bpm(F[2].payload, F[2].len, &kind, &bpm));
    CHECK_EQ(kind, STREAM_EV_R_PEAK);
    CHECK_EQ(bpm, 72);
    CHECK_EQ(F[3].len, 11);
    CHECK(rl_msg_event_bpm(F[3].payload, F[3].len, &kind, &bpm));
    CHECK_EQ(kind, STREAM_EV_PULSE);
    CHECK_EQ(bpm, 65);
    CHECK_EQ(F[4].len, 2);
    CHECK(!rl_msg_event_bpm(F[4].payload, F[4].len, &kind, &bpm));
    CHECK_EQ(F[5].len, 11); /* EV_STATE with the front-end gains */
    CHECK_EQ(F[6].len, 3);
}

TEST(mspm0_link_frames_into_radio_parser)
{
    /* LINK_STATUS, LINK_RADIO_CTRL and LINK_ACK through the MSPM0 framer */
    uint8_t st[LINK_STATUS_LEN];
    rl_status_t s;
    rl_radio_ctrl_t rc;
    for (unsigned i = 0; i < sizeof st; i++)
        st[i] = (uint8_t)(i + 1);
    stream_init();
    ncap = 0;
    CHECK(stream_send(LINK_STATUS, st, sizeof st));
    const uint8_t ctrl[] = {LINK_RC_SET_INFO, LINK_INFO_MCU_FW, '0', '.', '9'};
    CHECK(stream_send(LINK_RADIO_CTRL, ctrl, sizeof ctrl));
    const uint8_t ack[] = {0x33, LINK_RES_OK};
    CHECK(stream_send(LINK_ACK, ack, sizeof ack));
    parse_capture();
    CHECK_EQ(nf, 3);
    CHECK(rl_msg_parse_status(F[0].payload, F[0].len, &s));
    CHECK_EQ(s.batt_pct, 4);
    CHECK_EQ(s.uptime_min, 0x1413);
    CHECK_EQ(rl_msg_parse_radio_ctrl(F[1].payload, F[1].len, &rc), LINK_RES_OK);
    CHECK_EQ(rc.text_len, 3);
    uint8_t seq, res;
    CHECK(rl_msg_parse_ack(F[2].payload, F[2].len, &seq, &res));
    CHECK_EQ(seq, 0x33);
}

TEST(radio_frames_into_mspm0_decoder)
{
    static uint8_t a[COBS_MAX(STREAM_FRAME_MAX) + 1u], b[RL_WIRE_MAX], dec[STREAM_FRAME_MAX];
    uint8_t pl[STREAM_APP_MAX_PAYLOAD];
    uint32_t x = 7;
    int same = 1, decodes = 1;

    for (size_t len = 0; len <= STREAM_APP_MAX_PAYLOAD; len++) {
        for (size_t i = 0; i < len; i++) {
            x = x * 1103515245u + 12345u;
            pl[i] = (x >> 16) % 4 == 0 ? 0 : (uint8_t)(x >> 8);
        }
        uint8_t type = (uint8_t)(len & 1 ? LINK_CONN_STATE : LINK_CMD), seq = (uint8_t)(len * 3);
        size_t na = stream_build(type, seq, pl, len, a);
        size_t nb = rl_frame_encode(type, seq, pl, len, b, sizeof b);
        same &= na == nb && memcmp(a, b, na) == 0;
        /* MSPM0 decoder + CRC check on the radio's bytes */
        size_t d = cobs_decode(b, nb - 1, dec, sizeof dec);
        decodes &= d == len + 6 && dec[0] == type && dec[1] == seq &&
                   (size_t)(dec[2] | (dec[3] << 8)) == len &&
                   crc16_ccitt_false(dec, len + 4, 0xFFFF) == (uint16_t)(dec[len + 4] | (dec[len + 5] << 8)) &&
                   memcmp(&dec[4], pl, len) == 0;
    }
    CHECK(same);
    CHECK(decodes);

    /* the radio's real messages */
    rl_hello_t h = {0, 1, 0, {1, 2, 3, 4, 5, 6}, 1};
    uint8_t hb[LINK_HELLO_LEN];
    rl_msg_hello(&h, hb);
    size_t nb = rl_frame_encode(LINK_HELLO, 0, hb, sizeof hb, b, sizeof b);
    size_t d = cobs_decode(b, nb - 1, dec, sizeof dec);
    CHECK_EQ(d, 4 + LINK_HELLO_LEN + 2);
    CHECK_EQ(dec[0], LINK_HELLO);
    CHECK_EQ(dec[2], LINK_HELLO_LEN);
    CHECK(memcmp(&dec[4], hb, sizeof hb) == 0);
}

int main(void)
{
    RUN(mspm0_frames_into_radio_parser);
    RUN(mspm0_link_frames_into_radio_parser);
    RUN(radio_frames_into_mspm0_decoder);
    return TEST_RESULT();
}
