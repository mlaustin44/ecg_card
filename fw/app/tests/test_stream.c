/* COBS + CRC framing against known vectors, and the frame emitters. */
#include <string.h>

#include "sim.h"
#include "stream.h"
#include "stream_fmt.h"
#include "test.h"

static bool roundtrip(const uint8_t *in, size_t n)
{
    static uint8_t enc[COBS_MAX(600)], dec[600];
    size_t e = cobs_encode(in, n, enc);
    if (e > COBS_MAX(n))
        return false;
    for (size_t i = 0; i < e; i++)
        if (enc[i] == 0)
            return false;
    size_t d = cobs_decode(enc, e, dec, sizeof dec);
    return d == n && memcmp(dec, in, n) == 0;
}

TEST(crc_vectors)
{
    CHECK_EQ(crc16_ccitt_false((const uint8_t *)"123456789", 9, 0xFFFF), 0x29B1);
    CHECK_EQ(crc16_ccitt_false(NULL, 0, 0xFFFF), 0xFFFF);
    const uint8_t a[] = {0x00};
    CHECK_EQ(crc16_ccitt_false(a, 1, 0xFFFF), 0xE1F0);
}

TEST(cobs_vectors)
{
    /* Cheshire & Baker / Wikipedia examples */
    struct { const uint8_t *in; size_t n; const uint8_t *out; size_t m; } v[] = {
        {(const uint8_t *)"\x00", 1, (const uint8_t *)"\x01\x01", 2},
        {(const uint8_t *)"\x00\x00", 2, (const uint8_t *)"\x01\x01\x01", 3},
        {(const uint8_t *)"\x00\x11\x00", 3, (const uint8_t *)"\x01\x02\x11\x01", 4},
        {(const uint8_t *)"\x11\x22\x00\x33", 4, (const uint8_t *)"\x03\x11\x22\x02\x33", 5},
        {(const uint8_t *)"\x11\x22\x33\x44", 4, (const uint8_t *)"\x05\x11\x22\x33\x44", 5},
        {(const uint8_t *)"\x11\x00\x00\x00", 4, (const uint8_t *)"\x02\x11\x01\x01\x01", 5},
        {(const uint8_t *)"", 0, (const uint8_t *)"\x01", 1},
    };
    uint8_t out[16];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        size_t m = cobs_encode(v[i].in, v[i].n, out);
        CHECK_EQ(m, v[i].m);
        CHECK(memcmp(out, v[i].out, v[i].m) == 0);
        CHECK(roundtrip(v[i].in, v[i].n));
    }
    /* 254 / 255 non-zero bytes: block split */
    uint8_t big[600];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i % 255 + 1);
    static uint8_t enc[COBS_MAX(600)];
    CHECK_EQ(cobs_encode(big, 254, enc), 256); /* FF + 254 + trailing 01 */
    CHECK_EQ(enc[0], 0xFF);
    CHECK_EQ(enc[255], 0x01);
    CHECK_EQ(cobs_encode(big, 255, enc), 257);
    CHECK(roundtrip(big, 253) && roundtrip(big, 254) && roundtrip(big, 255) && roundtrip(big, 600));
    /* pseudo-random with zeros */
    uint32_t x = 1;
    for (int k = 0; k < 200; k++) {
        size_t n = (size_t)(k * 3 % 590);
        for (size_t i = 0; i < n; i++) {
            x = x * 1103515245u + 12345u;
            big[i] = (x >> 24) % 5 == 0 ? 0 : (uint8_t)(x >> 16);
        }
        CHECK(roundtrip(big, n));
    }
    /* malformed */
    uint8_t dec[8];
    CHECK(cobs_decode((const uint8_t *)"\x05\x11", 2, dec, sizeof dec) == (size_t)-1);
    CHECK(cobs_decode((const uint8_t *)"\x02\x00", 2, dec, sizeof dec) == (size_t)-1);
    CHECK(cobs_decode((const uint8_t *)"", 0, dec, sizeof dec) == (size_t)-1);
}

TEST(frame_layout)
{
    uint8_t wire[COBS_MAX(STREAM_FRAME_MAX) + 1];
    const uint8_t pl[] = {STREAM_EV_LEAD, 1};
    size_t n = stream_build(STREAM_EVENT, 7, pl, sizeof pl, wire);
    CHECK(n > 0 && wire[n - 1] == 0);
    uint8_t f[64];
    size_t m = cobs_decode(wire, n - 1, f, sizeof f);
    CHECK_EQ(m, 4 + 2 + 2);
    CHECK_EQ(f[0], STREAM_EVENT);
    CHECK_EQ(f[1], 7);
    CHECK_EQ(f[2], 2);
    CHECK_EQ(f[3], 0);
    CHECK_EQ(f[4], STREAM_EV_LEAD);
    uint16_t crc = crc16_ccitt_false(f, 6, 0xFFFF);
    CHECK_EQ(f[6], crc & 0xFF);
    CHECK_EQ(f[7], crc >> 8);
    /* whole frame including the CRC checks to zero residue? CCITT-FALSE is
     * not residue-zero in LE order; check explicitly instead */
    CHECK_EQ(stream_build(STREAM_LOG, 0, pl, STREAM_APP_MAX_PAYLOAD + 1, wire), 0);
}

TEST(emitters_and_seq)
{
    sim_reset();
    stream_init();
    int16_t s[10] = {0, -1, 2, -3, 32767, -32768, 6, 7, 8, 9};
    stream_ecg_block(1234, 5, s);
    int16_t cf[4] = {100, -200, 300, -400};
    stream_ppg_block(56, 1344, 3, cf);
    stream_ev_r_peak(99, 72);
    stream_ev_pulse(100, 71, 205, 18);
    stream_ev_state(3, 50, 2048, 80, 1);
    stream_ev_button(2, 1);
    CHECK_EQ(stream_frames(), 6);
    CHECK_EQ(stream_drops(), 0);
    size_t n;
    const uint8_t *d = sim_stream_data(&n);
    uint8_t f[64];
    size_t i = 0, k = 0;
    const size_t want_len[] = {25, 15, 7, 11, 11, 3};
    while (i < n) {
        size_t j = i;
        while (d[j]) j++;
        size_t m = cobs_decode(d + i, j - i, f, sizeof f);
        CHECK(m != (size_t)-1);
        CHECK_EQ(f[1], k);                 /* one global sequence counter */
        CHECK_EQ(f[2] | f[3] << 8, want_len[k]);
        if (k == 0) {
            CHECK_EQ(f[4] | f[5] << 8 | f[6] << 16 | f[7] << 24, 1234);
            CHECK_EQ(f[8], 5);
            CHECK_EQ((int16_t)(f[9 + 2 * 5] | f[10 + 2 * 5] << 8), -32768);
        }
        if (k == 1) {
            CHECK_EQ(f[8] | f[9] << 8, 1344);
            CHECK_EQ((int16_t)(f[11 + 2] | f[12 + 2] << 8), -200);
        }
        i = j + 1;
        k++;
    }
    CHECK_EQ(k, 6);
}

int main(void)
{
    RUN(crc_vectors);
    RUN(cobs_vectors);
    RUN(frame_layout);
    RUN(emitters_and_seq);
    return TEST_RESULT();
}
