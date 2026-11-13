#include "settings_store.h"

#include "stream.h" /* crc16_ccitt_false */

#define PAYLOAD_LEN 5u

void settings_defaults(settings_t *s)
{
    const settings_t d = SETTINGS_DEFAULTS;
    *s = d;
}

bool settings_valid(const settings_t *s)
{
    return s->gain < GAIN_MODE_COUNT && s->notch < NOTCH_MODE_COUNT && s->rld < RLD_MODE_COUNT &&
           s->led_limit < LED_LIMIT_COUNT && s->sweep < SWEEP_COUNT;
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
    uint16_t crc = crc16_ccitt_false(b, 9u, 0xFFFFu);
    b[9] = (uint8_t)crc;
    b[10] = (uint8_t)(crc >> 8);
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
    uint16_t crc = (uint16_t)(b[9] | (b[10] << 8));
    if (magic != SETTINGS_MAGIC || b[2] != SETTINGS_VERSION || b[3] != PAYLOAD_LEN ||
        crc != crc16_ccitt_false(b, 9u, 0xFFFFu))
        return SETTINGS_CORRUPT;
    settings_t s = {b[4], b[5], b[6], b[7], b[8]};
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
    switch (item) {
    case SET_ITEM_GAIN: s->gain = (uint8_t)((s->gain + 1u) % GAIN_MODE_COUNT); break;
    case SET_ITEM_NOTCH: s->notch = (uint8_t)((s->notch + 1u) % NOTCH_MODE_COUNT); break;
    case SET_ITEM_RLD: s->rld = (uint8_t)((s->rld + 1u) % RLD_MODE_COUNT); break;
    case SET_ITEM_LED_LIMIT: s->led_limit = (uint8_t)((s->led_limit + 1u) % LED_LIMIT_COUNT); break;
    case SET_ITEM_SWEEP: s->sweep = (uint8_t)((s->sweep + 1u) % SWEEP_COUNT); break;
    default: break;
    }
}
