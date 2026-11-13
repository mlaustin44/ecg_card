/* User settings, shared by the application (storage, behaviour) and the UI
 * (Settings page). Stored in flash by app/ with a CRC. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

typedef enum { GAIN_AUTO, GAIN_LO, GAIN_HI, GAIN_MODE_COUNT } gain_mode_t;
typedef enum { NOTCH_AUTO, NOTCH_50, NOTCH_60, NOTCH_MODE_COUNT } notch_mode_t;
typedef enum { RLD_BOTH, RLD_LEFT, RLD_RIGHT, RLD_OFF, RLD_MODE_COUNT } rld_mode_t;
/* LED current limit per LED. 15 mA is the most the MCP4728 can set on its
 * internal 2.048 V reference (full scale 15.2 mA, board.h LED_MAX_MA). */
typedef enum { LED_LIMIT_5MA, LED_LIMIT_10MA, LED_LIMIT_15MA, LED_LIMIT_COUNT } led_limit_t;
typedef enum { SWEEP_1S5, SWEEP_3S, SWEEP_6S, SWEEP_COUNT } sweep_t;

/* Order of items on the Settings page. */
typedef enum {
    SET_ITEM_GAIN,
    SET_ITEM_NOTCH,
    SET_ITEM_RLD,
    SET_ITEM_LED_LIMIT,
    SET_ITEM_SWEEP,
    SET_ITEM_COUNT
} settings_item_t;

typedef struct {
    uint8_t gain;      /* gain_mode_t */
    uint8_t notch;     /* notch_mode_t */
    uint8_t rld;       /* rld_mode_t */
    uint8_t led_limit; /* led_limit_t */
    uint8_t sweep;     /* sweep_t */
} settings_t;

#define SETTINGS_DEFAULTS \
    { GAIN_AUTO, NOTCH_AUTO, RLD_BOTH, LED_LIMIT_15MA, SWEEP_3S }

/* Milliseconds for one full sweep across the trace area. */
static inline uint16_t sweep_ms(uint8_t sweep)
{
    return sweep == SWEEP_1S5 ? 1500u : sweep == SWEEP_6S ? 6000u : 3000u;
}

static inline uint8_t led_limit_ma(uint8_t limit)
{
    return (uint8_t)(5u * ((limit < LED_LIMIT_COUNT ? limit : LED_LIMIT_15MA) + 1u));
}

#endif
