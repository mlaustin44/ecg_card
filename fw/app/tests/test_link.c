/* Link layer: RX parser (byte-wise, any split, corruption, garbage, fuzz
 * under ASan) and LINK_CMD validation. */
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "settings_store.h"
#include "sim.h"
#include "test.h"

typedef struct {
    int n;
    uint8_t type[64], seq[64];
    size_t len[64];
    uint8_t p[64][LINK_RX_MAX_PAYLOAD];
} got_t;

static void on(void *ctx, uint8_t type, uint8_t seq, const uint8_t *p, size_t n)
{
    got_t *g = ctx;
    if (g->n >= 64)
        return;
    g->type[g->n] = type;
    g->seq[g->n] = seq;
    g->len[g->n] = n;
    memcpy(g->p[g->n], p, n);
    g->n++;
}

static size_t wire(uint8_t type, uint8_t seq, const uint8_t *p, size_t n, uint8_t *out)
{
    return stream_build(type, seq, p, n, out);
}

TEST(frames_any_split)
{
    uint8_t w[3][COBS_MAX(STREAM_FRAME_MAX) + 1];
    uint8_t hello[LINK_HELLO_LEN] = {1, 1, 2, 3, 0, 0, 0, 0xAA, 0, 0xBB, 0, 0};
    uint8_t ack[2] = {7, 0};
    const char *txt = "boot ok";
    size_t n0 = wire(LINK_HELLO, 0, hello, sizeof hello, w[0]);
    size_t n1 = wire(LINK_ACK, 1, ack, 2, w[1]);
    size_t n2 = wire(STREAM_LOG, 2, (const uint8_t *)txt, strlen(txt), w[2]);
    uint8_t all[256];
    size_t n = 0;
    all[n++] = 0; /* leading zeros (idle-low line) are not frames */
    all[n++] = 0;
    memcpy(all + n, w[0], n0); n += n0;
    memcpy(all + n, w[1], n1); n += n1;
    memcpy(all + n, w[2], n2); n += n2;
    for (size_t chunk = 1; chunk <= n; chunk++) {
        link_rx_t r;
        got_t g;
        memset(&g, 0, sizeof g);
        link_rx_init(&r);
        for (size_t i = 0; i < n; i += chunk)
            link_rx_feed(&r, all + i, i + chunk <= n ? chunk : n - i, on, &g);
        CHECK_EQ(g.n, 3);
        CHECK_EQ(g.type[0], LINK_HELLO);
        CHECK_EQ(g.len[0], LINK_HELLO_LEN);
        CHECK(memcmp(g.p[0], hello, sizeof hello) == 0);
        CHECK_EQ(g.type[1], LINK_ACK);
        CHECK_EQ(g.p[1][0], 7);
        CHECK_EQ(g.type[2], STREAM_LOG);
        CHECK(memcmp(g.p[2], txt, strlen(txt)) == 0);
        CHECK_EQ(r.st.frames, 3);
        CHECK_EQ(r.st.crc_errors + r.st.bad_frames + r.st.seq_gaps, 0);
    }
}

TEST(corruption_gaps_oversize)
{
    uint8_t w[COBS_MAX(STREAM_FRAME_MAX) + 1];
    uint8_t ack[2] = {1, 0};
    link_rx_t r;
    got_t g;
    memset(&g, 0, sizeof g);
    link_rx_init(&r);
    /* every single-bit flip of a frame is rejected (CRC or COBS/length) */
    size_t n = wire(LINK_ACK, 0, ack, 2, w);
    for (size_t i = 0; i + 1 < n; i++)
        for (int b = 0; b < 8; b++) {
            uint8_t c[sizeof w];
            memcpy(c, w, n);
            c[i] ^= (uint8_t)(1u << b);
            link_rx_feed(&r, c, n, on, &g);
            if (c[i] == 0) /* the flip made a delimiter: two fragments */
                link_rx_feed(&r, (const uint8_t *)"", 1, on, &g);
        }
    CHECK_EQ(g.n, 0);
    CHECK(r.st.crc_errors > 0 && r.st.bad_frames > 0);
    /* sequence gaps */
    link_rx_init(&r);
    n = wire(LINK_ACK, 5, ack, 2, w);
    link_rx_feed(&r, w, n, on, &g);
    n = wire(LINK_ACK, 6, ack, 2, w);
    link_rx_feed(&r, w, n, on, &g);
    n = wire(LINK_ACK, 9, ack, 2, w);
    link_rx_feed(&r, w, n, on, &g);
    CHECK_EQ(r.st.frames, 3);
    CHECK_EQ(r.st.seq_gaps, 1);
    link_rx_restart(&r); /* radio rebooted: seq starts over without a gap */
    n = wire(LINK_ACK, 0, ack, 2, w);
    link_rx_feed(&r, w, n, on, &g);
    CHECK_EQ(r.st.seq_gaps, 1);
    /* a run of non-zero bytes longer than any frame: dropped, then resync */
    uint8_t junk[400];
    memset(junk, 0x41, sizeof junk);
    g.n = 0;
    link_rx_feed(&r, junk, sizeof junk, on, &g);
    link_rx_feed(&r, (const uint8_t *)"", 1, on, &g);
    CHECK_EQ(r.st.bad_frames, 1);
    n = wire(LINK_ACK, 1, ack, 2, w);
    link_rx_feed(&r, w, n, on, &g);
    CHECK_EQ(g.n, 1);
}

/* Random bytes, random frames with random damage: never crashes (ASan /
 * UBSan), never delivers a payload longer than the limit, and good frames
 * after the noise are still found. */
TEST(fuzz)
{
    srand(12345);
    link_rx_t r;
    got_t g;
    link_rx_init(&r);
    uint8_t buf[2048];
    for (int round = 0; round < 3000; round++) {
        memset(&g, 0, sizeof g);
        size_t n = (size_t)(rand() % (int)sizeof buf);
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)(rand() % 4 == 0 ? 0 : rand());
        if (round % 3 == 0) {
            uint8_t p[64];
            for (int i = 0; i < 64; i++) p[i] = (uint8_t)rand();
            n = wire((uint8_t)rand(), (uint8_t)rand(), p, (size_t)(rand() % 65), buf);
            for (int k = rand() % 3; k > 0; k--)
                buf[rand() % (int)n] ^= (uint8_t)rand();
        }
        link_rx_feed(&r, buf, n, on, &g);
        for (int k = 0; k < g.n; k++)
            CHECK(g.len[k] <= LINK_RX_MAX_PAYLOAD);
    }
    uint8_t w[COBS_MAX(STREAM_FRAME_MAX) + 1], ack[2] = {3, 0};
    memset(&g, 0, sizeof g);
    uint8_t z = 0;
    link_rx_feed(&r, &z, 1, on, &g);
    size_t n = wire(LINK_ACK, 0, ack, 2, w);
    link_rx_feed(&r, w, n, on, &g);
    CHECK_EQ(g.n, 1);
    CHECK_EQ(g.type[0], LINK_ACK);
    printf("  fuzz: %u good, %u crc, %u bad\n", r.st.frames, r.st.crc_errors, r.st.bad_frames);
}

static uint8_t parse(const uint8_t *p, size_t n, link_cmd_t *c) { return link_cmd_parse(p, n, c); }

TEST(command_validation)
{
    link_cmd_t c;
    const uint8_t start_ecg[] = {LINK_CMD_START, 1}, start_both[] = {LINK_CMD_START, 3};
    CHECK_EQ(parse(start_ecg, 2, &c), LINK_RES_OK);
    CHECK_EQ(c.cmd, LINK_CMD_START);
    CHECK_EQ(c.mode, 1);
    CHECK_EQ(parse(start_both, 2, &c), LINK_RES_OK);
    const uint8_t start0[] = {LINK_CMD_START, 0}, start4[] = {LINK_CMD_START, 4},
                  start_long[] = {LINK_CMD_START, 1, 0};
    CHECK_EQ(parse(start0, 2, &c), LINK_RES_BAD_ARG);
    CHECK_EQ(parse(start4, 2, &c), LINK_RES_BAD_ARG);
    CHECK_EQ(parse(start_ecg, 1, &c), LINK_RES_BAD_ARG);
    CHECK_EQ(parse(start_long, 3, &c), LINK_RES_BAD_ARG);
    const uint8_t stop[] = {LINK_CMD_STOP, 0}, status[] = {LINK_CMD_GET_STATUS};
    CHECK_EQ(parse(stop, 1, &c), LINK_RES_OK);
    CHECK_EQ(parse(stop, 2, &c), LINK_RES_BAD_ARG);
    CHECK_EQ(parse(status, 1, &c), LINK_RES_OK);
    /* SET: every item / value against the Settings rules */
    static const uint8_t counts[SET_ITEM_COUNT] = {GAIN_MODE_COUNT, NOTCH_MODE_COUNT, RLD_MODE_COUNT,
                                                   LED_LIMIT_COUNT, SWEEP_COUNT, BLE_MODE_COUNT,
                                                   RESTORE_MODE_COUNT, VIEW_MODE_COUNT};
    for (unsigned item = 0; item <= SET_ITEM_COUNT; item++)
        for (unsigned v = 0; v < 6; v++) {
            uint8_t set[] = {LINK_CMD_SET, (uint8_t)item, (uint8_t)v};
            bool ok = item < SET_ITEM_COUNT && v < counts[item];
            CHECK_EQ(parse(set, 3, &c), ok ? LINK_RES_OK : LINK_RES_BAD_ARG);
            if (ok)
                CHECK(c.item == item && c.value == v);
        }
    const uint8_t set_short[] = {LINK_CMD_SET, 0};
    CHECK_EQ(parse(set_short, 2, &c), LINK_RES_BAD_ARG);
    /* SET_TIME: 2026-11-12 12:00 UTC, +60 min */
    uint32_t t = 1794484800u;
    uint8_t st[7] = {LINK_CMD_SET_TIME, (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16),
                     (uint8_t)(t >> 24), 60, 0};
    CHECK_EQ(parse(st, 7, &c), LINK_RES_OK);
    CHECK_EQ(c.unix_s, t);
    CHECK_EQ(c.utc_offset_min, 60);
    st[5] = (uint8_t)(-300 & 0xFF);
    st[6] = (uint8_t)((-300 >> 8) & 0xFF);
    CHECK_EQ(parse(st, 7, &c), LINK_RES_OK);
    CHECK_EQ(c.utc_offset_min, -300);
    st[5] = (uint8_t)(900 & 0xFF);
    st[6] = (uint8_t)(900 >> 8);
    CHECK_EQ(parse(st, 7, &c), LINK_RES_BAD_ARG); /* +15 h */
    st[5] = 0;
    st[6] = 0;
    st[4] = 0x10; /* year 1978 */
    CHECK_EQ(parse(st, 7, &c), LINK_RES_BAD_ARG);
    CHECK_EQ(parse(st, 6, &c), LINK_RES_BAD_ARG);
    /* unknown, empty, too long */
    const uint8_t unk[] = {9}, zero[] = {0};
    CHECK_EQ(parse(unk, 1, &c), LINK_RES_UNKNOWN);
    CHECK_EQ(c.cmd, 9);
    CHECK_EQ(parse(zero, 1, &c), LINK_RES_UNKNOWN);
    CHECK_EQ(parse(zero, 0, &c), LINK_RES_BAD_ARG);
    uint8_t big[LINK_CMD_MAX_LEN + 1] = {LINK_CMD_STOP};
    CHECK_EQ(parse(big, sizeof big, &c), LINK_RES_BAD_ARG);
}

/* link_send(): all or nothing, the seq advances for drops too, and
 * nothing is written while the radio is off. */
TEST(send_drops)
{
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_COLLECT);
    hal_init();
    link_tx_reset();
    uint8_t p[60] = {1};
    CHECK(!link_send(STREAM_LOG, p, 10)); /* radio off: dropped, no write */
    CHECK_EQ(sim_violation_count(), 0);
    hal_radio_power(true);
    unsigned ok = 0, dropped = 0;
    for (int i = 0; i < 20; i++)
        link_send(STREAM_LOG, p, sizeof p) ? ok++ : dropped++;
    CHECK_EQ(ok, 7); /* 7 x 67 bytes fit in 512 */
    CHECK_EQ(dropped, 13);
    CHECK_EQ(link_tx_last_seq(), 20);
    hal_delay_ms(20);
    CHECK(link_send(STREAM_LOG, p, 4)); /* drained: fits again */
    hal_delay_ms(5);
    CHECK_EQ(sim_radio_info()->frames, 8);
    CHECK_EQ(sim_radio_info()->seq_gaps, 1); /* the radio sees one jump (8 -> 21) */
    hal_radio_power(false);
}

int main(void)
{
    RUN(frames_any_split);
    RUN(corruption_gaps_oversize);
    RUN(fuzz);
    RUN(command_validation);
    RUN(send_drops);
    return TEST_RESULT();
}
