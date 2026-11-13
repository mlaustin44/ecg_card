/* CRC-16/CCITT-FALSE, COBS and frame encode/check of the radio link. */
#include <string.h>

#include "rl_codec.h"
#include "test.h"

static int roundtrip(const uint8_t *in, size_t n)
{
    static uint8_t enc[RL_COBS_MAX(1200)], dec[1200];
    size_t e = rl_cobs_encode(in, n, enc), d, i;
    if (e > RL_COBS_MAX(n))
        return 0;
    for (i = 0; i < e; i++)
        if (enc[i] == 0)
            return 0;
    d = rl_cobs_decode(enc, e, dec, sizeof dec);
    return d == n && memcmp(dec, in, n) == 0;
}

TEST(crc_vectors)
{
    const uint8_t z[] = {0x00};
    CHECK_EQ(rl_crc16((const uint8_t *)"123456789", 9, RL_CRC_INIT), 0x29B1);
    CHECK_EQ(rl_crc16(NULL, 0, RL_CRC_INIT), 0xFFFF);
    CHECK_EQ(rl_crc16(z, 1, RL_CRC_INIT), 0xE1F0);
    /* chaining equals one pass */
    CHECK_EQ(rl_crc16((const uint8_t *)"6789", 4, rl_crc16((const uint8_t *)"12345", 5, RL_CRC_INIT)),
             0x29B1);
}

TEST(cobs_vectors)
{
    struct { const char *in; size_t n; const char *out; size_t m; } v[] = {
        {"\x00", 1, "\x01\x01", 2},
        {"\x00\x00", 2, "\x01\x01\x01", 3},
        {"\x00\x11\x00", 3, "\x01\x02\x11\x01", 4},
        {"\x11\x22\x00\x33", 4, "\x03\x11\x22\x02\x33", 5},
        {"\x11\x22\x33\x44", 4, "\x05\x11\x22\x33\x44", 5},
        {"\x11\x00\x00\x00", 4, "\x02\x11\x01\x01\x01", 5},
        {"", 0, "\x01", 1},
    };
    uint8_t out[16];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        size_t m = rl_cobs_encode((const uint8_t *)v[i].in, v[i].n, out);
        CHECK_EQ(m, v[i].m);
        CHECK(memcmp(out, v[i].out, v[i].m) == 0);
        CHECK(roundtrip((const uint8_t *)v[i].in, v[i].n));
    }
}

TEST(cobs_block_split)
{
    static uint8_t big[1200], enc[RL_COBS_MAX(1200)];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i % 255 + 1);
    CHECK_EQ(rl_cobs_encode(big, 254, enc), 256); /* FF + 254 + trailing 01 */
    CHECK_EQ(enc[0], 0xFF);
    CHECK_EQ(enc[255], 0x01);
    CHECK_EQ(rl_cobs_encode(big, 255, enc), 257);
    CHECK(roundtrip(big, 253) && roundtrip(big, 254) && roundtrip(big, 255));
    CHECK(roundtrip(big, 508) && roundtrip(big, 1200));
    /* pseudo-random content with zeros, every length up to 600 */
    uint32_t x = 12345;
    int ok = 1;
    for (size_t n = 0; n <= 600; n++) {
        for (size_t i = 0; i < n; i++) {
            x = x * 1103515245u + 12345u;
            big[i] = (x >> 16) % 5 == 0 ? 0 : (uint8_t)(x >> 8);
        }
        ok &= roundtrip(big, n);
    }
    CHECK(ok);
}

TEST(cobs_decode_errors)
{
    uint8_t out[8];
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"", 0, out, sizeof out), RL_COBS_ERROR);
    /* zero inside a block */
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"\x03\x11\x00", 3, out, sizeof out), RL_COBS_ERROR);
    /* code byte zero */
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"\x00", 1, out, sizeof out), RL_COBS_ERROR);
    /* code runs past the end */
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"\x05\x11\x22", 3, out, sizeof out), RL_COBS_ERROR);
    /* does not fit */
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"\x05\x11\x22\x33\x44", 5, out, 3), RL_COBS_ERROR);
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"\x05\x11\x22\x33\x44", 5, out, 4), 4);
    /* implied trailing zero needs room too */
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"\x02\x11\x01", 3, out, 1), RL_COBS_ERROR);
    CHECK_EQ(rl_cobs_decode((const uint8_t *)"\x02\x11\x01", 3, out, 2), 2);
}

TEST(frame_encode_layout)
{
    const uint8_t pl[] = {0x07, 0x00, 0x2A};
    uint8_t wire[RL_WIRE_SIZE(3)], dec[16];
    size_t n = rl_frame_encode(0x21, 5, pl, 3, wire, sizeof wire);
    CHECK_EQ(n, 11); /* 9 frame bytes + 1 COBS overhead + delimiter */
    CHECK_EQ(wire[n - 1], 0x00);
    for (size_t i = 0; i + 1 < n; i++)
        CHECK(wire[i] != 0);
    size_t d = rl_cobs_decode(wire, n - 1, dec, sizeof dec);
    CHECK_EQ(d, 9);
    CHECK_EQ(dec[0], 0x21);
    CHECK_EQ(dec[1], 5);
    CHECK_EQ(dec[2], 3);
    CHECK_EQ(dec[3], 0);
    CHECK(memcmp(&dec[4], pl, 3) == 0);
    uint16_t crc = rl_crc16(dec, 7, RL_CRC_INIT);
    CHECK_EQ(dec[7], crc & 0xFF);
    CHECK_EQ(dec[8], crc >> 8);

    rl_frame_t f;
    CHECK_EQ(rl_frame_parse(dec, d, &f), RL_FRAME_OK);
    CHECK_EQ(f.type, 0x21);
    CHECK_EQ(f.seq, 5);
    CHECK_EQ(f.len, 3);
    CHECK(f.payload == dec + 4);
}

TEST(frame_encode_limits)
{
    static uint8_t pl[RL_MAX_PAYLOAD + 1], wire[RL_WIRE_MAX + 8], dec[RL_FRAME_MAX];
    for (size_t i = 0; i < sizeof pl; i++)
        pl[i] = (uint8_t)i;
    /* 512-byte payload: fits RL_WIRE_MAX exactly or less */
    size_t n = rl_frame_encode(0x10, 0, pl, RL_MAX_PAYLOAD, wire, RL_WIRE_MAX);
    CHECK(n > 0 && n <= RL_WIRE_MAX);
    size_t d = rl_cobs_decode(wire, n - 1, dec, sizeof dec);
    CHECK_EQ(d, RL_FRAME_MAX);
    rl_frame_t f;
    CHECK_EQ(rl_frame_parse(dec, d, &f), RL_FRAME_OK);
    CHECK_EQ(f.len, RL_MAX_PAYLOAD);
    /* too long a payload, or too small a buffer */
    CHECK_EQ(rl_frame_encode(0x10, 0, pl, RL_MAX_PAYLOAD + 1, wire, sizeof wire), 0);
    CHECK_EQ(rl_frame_encode(0x10, 0, pl, 3, wire, RL_WIRE_SIZE(3) - 1), 0);
    CHECK(rl_frame_encode(0x10, 0, NULL, 0, wire, RL_WIRE_SIZE(0)) > 0);
}

TEST(frame_parse_errors)
{
    uint8_t f[16] = {0x01, 0x00, 0x02, 0x00, 0xAA, 0xBB};
    uint16_t crc = rl_crc16(f, 6, RL_CRC_INIT);
    rl_frame_t out;
    f[6] = (uint8_t)crc;
    f[7] = (uint8_t)(crc >> 8);
    CHECK_EQ(rl_frame_parse(f, 8, &out), RL_FRAME_OK);
    CHECK_EQ(rl_frame_parse(f, 5, &out), RL_FRAME_SHORT);
    CHECK_EQ(rl_frame_parse(f, 9, &out), RL_FRAME_LEN); /* one byte too many */
    CHECK_EQ(rl_frame_parse(f, 7, &out), RL_FRAME_LEN);
    f[4] ^= 1;
    CHECK_EQ(rl_frame_parse(f, 8, &out), RL_FRAME_CRC);
    f[4] ^= 1;
    /* len field above 512 */
    uint8_t g[6] = {0x01, 0x00, 0x01, 0x02, 0, 0};
    CHECK_EQ(rl_frame_parse(g, 6, &out), RL_FRAME_LEN);
}

int main(void)
{
    RUN(crc_vectors);
    RUN(cobs_vectors);
    RUN(cobs_block_split);
    RUN(cobs_decode_errors);
    RUN(frame_encode_layout);
    RUN(frame_encode_limits);
    RUN(frame_parse_errors);
    return TEST_RESULT();
}
