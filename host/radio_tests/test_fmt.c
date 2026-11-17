/* Log formatter, device name / serial, advertising payload budgets. */
#include <string.h>

#include "link_fmt.h"
#include "rl_ble_fmt.h"
#include "rl_fmt.h"
#include "test.h"

#define FMT_IS(expect, ...)                                   \
    do {                                                      \
        char fb_[64];                                         \
        size_t fn_ = rl_fmt(fb_, sizeof fb_, __VA_ARGS__);    \
        CHECK_EQ(fn_, strlen(expect));                        \
        CHECK(strcmp(fb_, expect) == 0);                      \
    } while (0)

TEST(formatter)
{
    FMT_IS("plain", "plain");
    FMT_IS("u=42 d=-7 x=ff X=FF", "u=%u d=%d x=%x X=%X", 42u, -7, 255u, 255u);
    FMT_IS("0x0A 007 -05", "0x%02X %03u %03d", 10u, 7u, -5);
    FMT_IS("[  12]", "[%4u]", 12u);
    FMT_IS("s=abc c=Z %", "s=%s c=%c %%", "abc", 'Z');
    FMT_IS("4294967295 -2147483648", "%u %d", 4294967295u, -2147483647 - 1);
    FMT_IS("123456 7", "%lu %li", 123456ul, 7l);
    FMT_IS("0", "%u", 0u);
    FMT_IS("(null)", "%s", (const char *)NULL);

    /* truncation keeps the terminator */
    char b[6];
    CHECK_EQ(rl_fmt(b, sizeof b, "abcdefgh"), 5);
    CHECK(strcmp(b, "abcde") == 0);
    CHECK_EQ(rl_fmt(b, sizeof b, "%u", 1234567u), 5);
    CHECK(strcmp(b, "12345") == 0);
    CHECK_EQ(rl_fmt(b, 1, "x"), 0);
    CHECK_EQ(b[0], '\0');
}

TEST(name_and_serial)
{
    const uint8_t addr[6] = {0xEF, 0xBE, 0xAD, 0xDE, 0x34, 0x12}; /* 12:34:DE:AD:BE:EF */
    char name[RL_NAME_LEN + 1], serial[RL_SERIAL_LEN + 1];
    rl_ble_name(addr, name);
    CHECK(strcmp(name, "ECG Card BEEF") == 0);
    CHECK_EQ(strlen(name), RL_NAME_LEN);
    rl_ble_serial(addr, serial);
    CHECK(strcmp(serial, "1234DEADBEEF") == 0);
}

TEST(adv_data)
{
    static const uint8_t uuid[16] = LINK_UUID128(LINK_UUID_SERVICE);
    /* d5780001-8a28-493a-ad8d-6b741ba09ea7, least significant byte first */
    static const uint8_t expect_uuid[16] = {0xa7, 0x9e, 0xa0, 0x1b, 0x74, 0x6b, 0x8d, 0xad,
                                            0x3a, 0x49, 0x28, 0x8a, 0x01, 0x00, 0x78, 0xd5};
    uint8_t a[RL_ADV_MAX];
    size_t n = rl_ble_adv_data(a);
    CHECK(memcmp(uuid, expect_uuid, 16) == 0);
    CHECK_EQ(n, 25);
    CHECK(n <= RL_ADV_MAX);
    CHECK_EQ(a[0], 2);
    CHECK_EQ(a[1], 0x01);
    CHECK_EQ(a[2], 0x06);
    /* 16-bit service UUIDs, complete: Heart Rate 0x180D, LSB first */
    CHECK_EQ(a[3], 3);
    CHECK_EQ(a[4], 0x03);
    CHECK_EQ(a[5], 0x0D);
    CHECK_EQ(a[6], 0x18);
    CHECK_EQ(a[7], 17);
    CHECK_EQ(a[8], 0x07);
    CHECK(memcmp(&a[9], expect_uuid, 16) == 0);
}

TEST(scan_response)
{
    uint8_t s[RL_ADV_MAX];
    size_t n = rl_ble_scan_rsp("ECG Card BEEF", 16, 16, 0, s);
    CHECK_EQ(n, 24);
    CHECK(n <= RL_ADV_MAX);
    CHECK_EQ(s[0], 14);
    CHECK_EQ(s[1], 0x09);
    CHECK(memcmp(&s[2], "ECG Card BEEF", 13) == 0);
    CHECK_EQ(s[15], 5);
    CHECK_EQ(s[16], 0x12);
    CHECK_EQ(s[17], 16);
    CHECK_EQ(s[18], 0);
    CHECK_EQ(s[19], 16);
    CHECK_EQ(s[20], 0);
    CHECK_EQ(s[21], 2);
    CHECK_EQ(s[22], 0x0A);
    CHECK_EQ(s[23], 0);
    /* a name that would not fit is refused, not truncated */
    CHECK_EQ(rl_ble_scan_rsp("012345678901234567890", 16, 16, 0, s), 0);  /* 21 chars */
    CHECK_EQ(rl_ble_scan_rsp("01234567890123456789", 16, 16, 0, s), 31); /* 20 chars */
}

int main(void)
{
    RUN(formatter);
    RUN(name_and_serial);
    RUN(adv_data);
    RUN(scan_response);
    return TEST_RESULT();
}
