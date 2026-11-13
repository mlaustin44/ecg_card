/* Settings blob: round trip, blank, every kind of corruption -> defaults. */
#include <string.h>

#include "settings_store.h"
#include "sim.h"
#include "test.h"

static const settings_t D = SETTINGS_DEFAULTS;

TEST(defaults_have_15ma)
{
    CHECK_EQ(D.led_limit, LED_LIMIT_15MA);
    CHECK_EQ(led_limit_ma(D.led_limit), 15);
    CHECK_EQ(LED_LIMIT_COUNT, 3);
    CHECK_EQ(led_limit_ma(LED_LIMIT_5MA), 5);
    CHECK_EQ(led_limit_ma(99), 15);
}

TEST(roundtrip_nv)
{
    sim_reset();
    settings_t s, o;
    CHECK(settings_load(&o) == SETTINGS_BLANK);
    CHECK(memcmp(&o, &D, sizeof o) == 0);
    s = (settings_t){GAIN_HI, NOTCH_50, RLD_RIGHT, LED_LIMIT_5MA, SWEEP_6S};
    CHECK(settings_save(&s) == HAL_OK);
    CHECK(settings_load(&o) == SETTINGS_LOADED);
    CHECK(memcmp(&o, &s, sizeof o) == 0);
}

TEST(corruption)
{
    settings_t s = {GAIN_LO, NOTCH_60, RLD_OFF, LED_LIMIT_10MA, SWEEP_1S5}, o;
    uint8_t b[SETTINGS_BLOB_LEN];
    settings_encode(&s, b);
    CHECK(settings_decode(b, &o) == SETTINGS_LOADED);
    for (unsigned i = 0; i < SETTINGS_BLOB_LEN; i++)
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t c[SETTINGS_BLOB_LEN];
            memcpy(c, b, sizeof c);
            c[i] ^= (uint8_t)(1u << bit);
            CHECK(settings_decode(c, &o) == SETTINGS_CORRUPT);
            CHECK(memcmp(&o, &D, sizeof o) == 0);
        }
    /* valid CRC but out-of-range value (e.g. the old 20 mA option = 3) */
    settings_t bad = s;
    bad.led_limit = 3;
    settings_encode(&bad, b);
    CHECK(settings_decode(b, &o) == SETTINGS_CORRUPT);
    CHECK(memcmp(&o, &D, sizeof o) == 0);
    /* old version */
    settings_encode(&s, b);
    b[2] = 1;
    CHECK(settings_decode(b, &o) == SETTINGS_CORRUPT);
    /* all zeros */
    memset(b, 0, sizeof b);
    CHECK(settings_decode(b, &o) == SETTINGS_CORRUPT);
}

TEST(cycle)
{
    settings_t s = D;
    for (int i = 0; i < 3; i++)
        settings_cycle(&s, SET_ITEM_LED_LIMIT);
    CHECK_EQ(s.led_limit, D.led_limit);
    settings_cycle(&s, SET_ITEM_LED_LIMIT);
    CHECK_EQ(s.led_limit, LED_LIMIT_5MA);
    settings_cycle(&s, SET_ITEM_RLD);
    CHECK_EQ(s.rld, RLD_LEFT);
    for (int i = 0; i < 4; i++)
        settings_cycle(&s, SET_ITEM_RLD);
    CHECK_EQ(s.rld, RLD_LEFT);
    CHECK(settings_valid(&s));
}

int main(void)
{
    RUN(defaults_have_15ma);
    RUN(roundtrip_nv);
    RUN(corruption);
    RUN(cycle);
    return TEST_RESULT();
}
