#include "settings_store.h"

#include "stream.h" /* crc16_ccitt_false */

#define PAYLOAD_LEN    7u
#define V3_PAYLOAD_LEN 6u
#define V3_BLOB_LEN    12u
#define V2_PAYLOAD_LEN 5u
#define V2_BLOB_LEN    11u

void settings_defaults(settings_t *s)
{
    const settings_t d = SETTINGS_DEFAULTS;
    *s = d;
}

/* Number of values of each item (Settings page order). */
static uint8_t item_count(uint8_t item)
{
    static const uint8_t n[SET_ITEM_COUNT] = {GAIN_MODE_COUNT, NOTCH_MODE_COUNT, RLD_MODE_COUNT,
                                              LED_LIMIT_COUNT, SWEEP_COUNT, BLE_MODE_COUNT,
                                              RESTORE_MODE_COUNT};
    return item < SET_ITEM_COUNT ? n[item] : 0u;
}

static uint8_t *item_ref(settings_t *s, uint8_t item)
{
    switch (item) {
    case SET_ITEM_GAIN: return &s->gain;
    case SET_ITEM_NOTCH: return &s->notch;
    case SET_ITEM_RLD: return &s->rld;
    case SET_ITEM_LED_LIMIT: return &s->led_limit;
    case SET_ITEM_SWEEP: return &s->sweep;
    case SET_ITEM_BLE: return &s->ble;
    case SET_ITEM_RESTORE: return &s->restore;
    default: return 0;
    }
}

bool settings_valid(const settings_t *s)
{
    return s->gain < GAIN_MODE_COUNT && s->notch < NOTCH_MODE_COUNT && s->rld < RLD_MODE_COUNT &&
           s->led_limit < LED_LIMIT_COUNT && s->sweep < SWEEP_COUNT && s->ble < BLE_MODE_COUNT &&
           s->restore < RESTORE_MODE_COUNT;
}

void settings_encode(const settings_t *s, uint8_t b[SETTINGS_BLOB_LEN])
{
    b[0] = (uint8_t)SETTINGS_MAGIC;
    b[1] = (uint8_t)(SETTINGS_MAGIC >> 8);
    b[2] = SETTINGS_VERSION;
    b[3] = PAYLOAD_LEN;
    b[4] = s->gain;
    b[5] = s->notch;
    b[6] = s->rld;
    b[7] = s->led_limit;
    b[8] = s->sweep;
    b[9] = s->ble;
    b[10] = s->restore;
    uint16_t crc = crc16_ccitt_false(b, 11u, 0xFFFFu);
    b[11] = (uint8_t)crc;
    b[12] = (uint8_t)(crc >> 8);
}

static bool crc_ok(const uint8_t *b, unsigned n)
{
    return (uint16_t)(b[n] | (b[n + 1u] << 8)) == crc16_ccitt_false(b, n, 0xFFFFu);
}

settings_load_t settings_decode(const uint8_t *b, settings_t *out)
{
    settings_defaults(out);
    bool blank = true;
    for (unsigned i = 0; i < SETTINGS_BLOB_LEN; i++)
        if (b[i] != 0xFFu)
            blank = false;
    if (blank)
        return SETTINGS_BLANK;
    uint16_t magic = (uint16_t)(b[0] | (b[1] << 8));
    if (magic != SETTINGS_MAGIC)
        return SETTINGS_CORRUPT;
    settings_t s;
    if (b[2] == SETTINGS_VERSION && b[3] == PAYLOAD_LEN && crc_ok(b, 11u)) {
        s = (settings_t){b[4], b[5], b[6], b[7], b[8], b[9], b[10]};
    } else if (b[2] == 3u && b[3] == V3_PAYLOAD_LEN && crc_ok(b, V3_BLOB_LEN - 2u)) {
        /* version 3 (before restore): keep the values, restore default */
        s = (settings_t){b[4], b[5], b[6], b[7], b[8], b[9], RESTORE_OFF};
    } else if (b[2] == 2u && b[3] == V2_PAYLOAD_LEN && crc_ok(b, V2_BLOB_LEN - 2u)) {
        /* version 2 (before Bluetooth): keep the values, Bluetooth default */
        s = (settings_t){b[4], b[5], b[6], b[7], b[8], BLE_ON, RESTORE_OFF};
    } else {
        return SETTINGS_CORRUPT;
    }
    if (!settings_valid(&s))
        return SETTINGS_CORRUPT;
    *out = s;
    return SETTINGS_LOADED;
}

settings_load_t settings_load(settings_t *out)
{
    uint8_t b[SETTINGS_BLOB_LEN];
    hal_nv_read(b, sizeof b);
    return settings_decode(b, out);
}

hal_status_t settings_save(const settings_t *s)
{
    uint8_t b[SETTINGS_BLOB_LEN];
    settings_encode(s, b);
    return hal_nv_write(b, sizeof b);
}

void settings_cycle(settings_t *s, uint8_t item)
{
    uint8_t *v = item_ref(s, item);
    if (v)
        *v = (uint8_t)((*v + 1u) % item_count(item));
}

bool settings_set(settings_t *s, uint8_t item, uint8_t value)
{
    uint8_t *v = item_ref(s, item);
    if (!v || value >= item_count(item))
        return false;
    *v = value;
    return true;
}
