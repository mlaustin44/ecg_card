/* Settings blob: round trip, blank, every kind of corruption -> defaults. */
#include <string.h>

#include "settings_store.h"
#include "stream.h"
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
    s = (settings_t){GAIN_HI, NOTCH_50, RLD_RIGHT, LED_LIMIT_5MA, SWEEP_6S, BLE_OFF, RESTORE_OFF};
    CHECK(settings_save(&s) == HAL_OK);
    CHECK(settings_load(&o) == SETTINGS_LOADED);
    CHECK(memcmp(&o, &s, sizeof o) == 0);
}

TEST(corruption)
{
    settings_t s = {GAIN_LO, NOTCH_60, RLD_OFF, LED_LIMIT_10MA, SWEEP_1S5, BLE_ON, RESTORE_SLOW}, o;
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

/* A blob written by the previous build (v2, no Bluetooth item) keeps its
 * values; Bluetooth comes up with its default. */
TEST(v2_migration)
{
    uint8_t b[SETTINGS_BLOB_LEN];
    memset(b, 0xFF, sizeof b);
    b[0] = (uint8_t)SETTINGS_MAGIC;
    b[1] = (uint8_t)(SETTINGS_MAGIC >> 8);
    b[2] = 2;
    b[3] = 5;
    b[4] = GAIN_HI; b[5] = NOTCH_60; b[6] = RLD_LEFT; b[7] = LED_LIMIT_10MA; b[8] = SWEEP_6S;
    uint16_t c = crc16_ccitt_false(b, 9, 0xFFFF);
    b[9] = (uint8_t)c;
    b[10] = (uint8_t)(c >> 8);
    settings_t o;
    CHECK(settings_decode(b, &o) == SETTINGS_LOADED);
    CHECK_EQ(o.gain, GAIN_HI);
    CHECK_EQ(o.notch, NOTCH_60);
    CHECK_EQ(o.rld, RLD_LEFT);
    CHECK_EQ(o.led_limit, LED_LIMIT_10MA);
    CHECK_EQ(o.sweep, SWEEP_6S);
    CHECK_EQ(o.ble, BLE_ON);
    b[5] = 7; /* v2 with a bad CRC */
    CHECK(settings_decode(b, &o) == SETTINGS_CORRUPT);
}

/* A blob written by the previous build (v3, no restore item) keeps its
 * values; restore comes up Fast (the behaviour of that build). */
TEST(v3_migration)
{
    uint8_t b[SETTINGS_BLOB_LEN];
    memset(b, 0xFF, sizeof b);
    b[0] = (uint8_t)SETTINGS_MAGIC;
    b[1] = (uint8_t)(SETTINGS_MAGIC >> 8);
    b[2] = 3;
    b[3] = 6;
    b[4] = GAIN_LO; b[5] = NOTCH_50; b[6] = RLD_RIGHT; b[7] = LED_LIMIT_5MA; b[8] = SWEEP_1S5;
    b[9] = BLE_OFF;
    uint16_t c = crc16_ccitt_false(b, 10, 0xFFFF);
    b[10] = (uint8_t)c;
    b[11] = (uint8_t)(c >> 8);
    settings_t o;
    CHECK(settings_decode(b, &o) == SETTINGS_LOADED);
    CHECK_EQ(o.gain, GAIN_LO);
    CHECK_EQ(o.notch, NOTCH_50);
    CHECK_EQ(o.rld, RLD_RIGHT);
    CHECK_EQ(o.led_limit, LED_LIMIT_5MA);
    CHECK_EQ(o.sweep, SWEEP_1S5);
    CHECK_EQ(o.ble, BLE_OFF);
    CHECK_EQ(o.restore, RESTORE_FAST);
    b[6] = RLD_LEFT; /* v3 with a bad CRC */
    CHECK(settings_decode(b, &o) == SETTINGS_CORRUPT);
}

TEST(set_validation)
{
    settings_t s = D;
    CHECK(settings_set(&s, SET_ITEM_BLE, BLE_OFF));
    CHECK_EQ(s.ble, BLE_OFF);
    CHECK(!settings_set(&s, SET_ITEM_BLE, 2));
    CHECK(!settings_set(&s, SET_ITEM_GAIN, GAIN_MODE_COUNT));
    CHECK(!settings_set(&s, SET_ITEM_LED_LIMIT, 3));
    CHECK(!settings_set(&s, SET_ITEM_COUNT, 0));
    CHECK(settings_set(&s, SET_ITEM_RESTORE, RESTORE_OFF));
    CHECK_EQ(s.restore, RESTORE_OFF);
    CHECK(!settings_set(&s, SET_ITEM_RESTORE, RESTORE_MODE_COUNT));
    CHECK(!settings_set(&s, 255, 0));
    CHECK(settings_set(&s, SET_ITEM_SWEEP, SWEEP_1S5));
    CHECK(settings_set(&s, SET_ITEM_RLD, RLD_OFF));
    CHECK(settings_set(&s, SET_ITEM_NOTCH, NOTCH_50));
    CHECK_EQ(s.ble, BLE_OFF);
    CHECK_EQ(s.sweep, SWEEP_1S5);
    CHECK_EQ(s.rld, RLD_OFF);
    CHECK(settings_valid(&s));
    settings_cycle(&s, SET_ITEM_BLE);
    CHECK_EQ(s.ble, BLE_ON);
    settings_cycle(&s, SET_ITEM_BLE);
    CHECK_EQ(s.ble, BLE_OFF);
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
    RUN(v2_migration);
    RUN(v3_migration);
    RUN(set_validation);
    return TEST_RESULT();
}
