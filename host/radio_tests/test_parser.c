/* The radio's byte-at-a-time frame parser: clean streams, corruption,
 * truncation, oversize blocks, resynchronisation and sequence gaps. */
#include <string.h>

#include "rl_parser.h"
#include "test.h"

static rl_parser_t P;
static rl_frame_t got[64];
static uint8_t got_payload[64][RL_MAX_PAYLOAD];
static int ngot;

static void feed(const uint8_t *b, size_t n)
{
    rl_frame_t f;
    for (size_t i = 0; i < n; i++) {
        if (rl_parser_feed(&P, b[i], &f) && ngot < 64) {
            memcpy(got_payload[ngot], f.payload, f.len);
            got[ngot] = f;
            got[ngot].payload = got_payload[ngot];
            ngot++;
        }
    }
}

static size_t frame(uint8_t type, uint8_t seq, const uint8_t *p, size_t n, uint8_t *wire)
{
    return rl_frame_encode(type, seq, p, n, wire, RL_WIRE_MAX);
}

static void reset(void)
{
    rl_parser_init(&P);
    ngot = 0;
}

TEST(byte_by_byte)
{
    static uint8_t wire[RL_WIRE_MAX];
    const uint8_t a[] = {1, 0, 2, 0, 0, 3};
    reset();
    for (int i = 0; i < 10; i++) {
        size_t n = frame(0x10, (uint8_t)i, a, sizeof a, wire);
        feed(wire, n);
    }
    CHECK_EQ(ngot, 10);
    CHECK_EQ(P.st.frames, 10);
    CHECK_EQ(got[9].seq, 9);
    CHECK_EQ(got[9].len, sizeof a);
    CHECK(memcmp(got[9].payload, a, sizeof a) == 0);
    CHECK_EQ(P.st.crc_err + P.st.cobs_err + P.st.len_err + P.st.oversize + P.st.seq_gaps, 0);
}

TEST(leading_and_repeated_delimiters)
{
    static uint8_t wire[RL_WIRE_MAX];
    const uint8_t z[3] = {0, 0, 0};
    reset();
    feed(z, 3); /* wake bytes / idle zeros */
    size_t n = frame(0x02, 0, NULL, 0, wire);
    feed(wire, n);
    feed(z, 2);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].type, 0x02);
    CHECK_EQ(got[0].len, 0);
    CHECK_EQ(P.st.empty, 5);
    CHECK_EQ(P.st.crc_err + P.st.cobs_err + P.st.len_err, 0);
}

TEST(corruption)
{
    static uint8_t wire[RL_WIRE_MAX];
    const uint8_t a[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    reset();
    size_t n = frame(0x12, 0, a, sizeof a, wire);
    /* flip one payload bit: still a valid COBS block, CRC catches it */
    CHECK_EQ(wire[6], 0x22); /* 02 12 02 05 08 11 22 ...: a payload byte */
    wire[6] ^= 0x01;
    feed(wire, n);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(P.st.crc_err, 1);
    /* the next frame is received normally */
    n = frame(0x12, 1, a, sizeof a, wire);
    feed(wire, n);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 1);
    /* a COBS code pointing past the end */
    n = frame(0x12, 2, a, sizeof a, wire);
    wire[0] = 0xFE;
    feed(wire, n);
    CHECK_EQ(P.st.cobs_err, 1);
    CHECK_EQ(ngot, 1);
}

TEST(truncation)
{
    static uint8_t wire[RL_WIRE_MAX];
    const uint8_t a[25] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const uint8_t z = 0;
    reset();
    size_t n = frame(0x10, 7, a, sizeof a, wire);
    /* first half of a frame, then the link drops a byte run and a
     * delimiter arrives */
    feed(wire, n / 2);
    feed(&z, 1);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(P.st.cobs_err + P.st.len_err + P.st.crc_err, 1);
    /* recovery */
    n = frame(0x10, 8, a, sizeof a, wire);
    feed(wire, n);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].seq, 8);
    /* one byte missing in the middle (no delimiter lost) */
    n = frame(0x10, 9, a, sizeof a, wire);
    memmove(&wire[10], &wire[11], n - 11);
    feed(wire, n - 1);
    CHECK_EQ(ngot, 1);
    n = frame(0x10, 10, a, sizeof a, wire);
    feed(wire, n);
    CHECK_EQ(ngot, 2);
}

TEST(oversize_and_resync)
{
    static uint8_t wire[RL_WIRE_MAX], junk[700];
    const uint8_t a[] = {9, 8, 7};
    const uint8_t z = 0;
    reset();
    memset(junk, 0x5A, sizeof junk);
    feed(junk, sizeof junk); /* 700 non-zero bytes: longer than any frame */
    feed(&z, 1);
    CHECK_EQ(P.st.oversize, 1);
    CHECK_EQ(ngot, 0);
    size_t n = frame(0x41, 3, a, sizeof a, wire);
    feed(wire, n);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].type, 0x41);
    /* random garbage containing zeros, then a good frame */
    uint32_t x = 99;
    for (size_t i = 0; i < sizeof junk; i++) {
        x = x * 1664525u + 1013904223u;
        junk[i] = (uint8_t)(x >> 24);
    }
    feed(junk, sizeof junk);
    feed(&z, 1);
    int before = ngot;
    n = frame(0x41, 4, a, sizeof a, wire);
    feed(wire, n);
    CHECK(ngot >= before + 1);
    CHECK_EQ(got[ngot - 1].seq, 4);
}

TEST(max_payload)
{
    static uint8_t wire[RL_WIRE_MAX], pl[RL_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof pl; i++)
        pl[i] = (uint8_t)(i * 7);
    reset();
    size_t n = frame(0x30, 0, pl, sizeof pl, wire);
    feed(wire, n);
    CHECK_EQ(ngot, 1);
    CHECK_EQ(got[0].len, RL_MAX_PAYLOAD);
    CHECK(memcmp(got[0].payload, pl, sizeof pl) == 0);
    CHECK_EQ(P.st.oversize, 0);
}

TEST(len_field_over_512)
{
    /* a well-formed COBS block with CRC whose len says 513 */
    static uint8_t f[RL_FRAME_MAX + 1], wire[RL_COBS_MAX(RL_FRAME_MAX + 1) + 1];
    const uint8_t z = 0;
    reset();
    f[0] = 0x30;
    f[1] = 0;
    f[2] = 0x01;
    f[3] = 0x02;
    uint16_t crc = rl_crc16(f, 4 + 513, RL_CRC_INIT);
    f[4 + 513] = (uint8_t)crc;
    f[5 + 513] = (uint8_t)(crc >> 8);
    size_t n = rl_cobs_encode(f, 519, wire);
    feed(wire, n);
    feed(&z, 1);
    CHECK_EQ(ngot, 0);
    CHECK_EQ(P.st.oversize + P.st.len_err + P.st.cobs_err, 1);
}

TEST(sequence_gaps)
{
    static uint8_t wire[RL_WIRE_MAX];
    const uint8_t seqs[] = {250, 251, 252, 255, 0, 1, 5};
    reset();
    for (size_t i = 0; i < sizeof seqs; i++) {
        size_t n = frame(0x11, seqs[i], NULL, 0, wire);
        feed(wire, n);
    }
    CHECK_EQ(ngot, 7);
    /* 253, 254 missing; 255 -> 0 wraps without a gap; 2, 3, 4 missing */
    CHECK_EQ(P.st.seq_gaps, 5);
}

int main(void)
{
    RUN(byte_by_byte);
    RUN(leading_and_repeated_delimiters);
    RUN(corruption);
    RUN(truncation);
    RUN(oversize_and_resync);
    RUN(max_payload);
    RUN(len_field_over_512);
    RUN(sequence_gaps);
    return TEST_RESULT();
}
