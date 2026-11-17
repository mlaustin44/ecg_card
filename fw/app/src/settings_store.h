/* Settings persistence in the NV blob (last flash sector, hal_nv_*).
 *
 * Blob layout (little-endian), SETTINGS_BLOB_LEN bytes:
 *   u16 magic 0x4345 ("EC"), u8 version, u8 payload length,
 *   payload: gain, notch, rld, led_limit, sweep, ble, restore (settings_t order),
 *   u16 CRC-16/CCITT-FALSE over everything before it.
 * Blank (0xFF), bad magic/version/length/CRC or any out-of-range field ->
 * defaults. A version-2 blob (11 bytes, no ble) is still read: its values
 * are kept and Bluetooth gets the default (On); a version-3 blob (12 bytes,
 * no restore) keeps its values with restore = Fast. The next save writes v4. Never written during acquisition (the caller's job: hal_nv_write
 * stalls the CPU for tens of ms). */
#ifndef APP_SETTINGS_STORE_H
#define APP_SETTINGS_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "settings.h"

#define SETTINGS_MAGIC     0x4345u
#define SETTINGS_VERSION   4u     /* 2: LED limit 20 mA option removed; 3: ble; 4: restore */
#define SETTINGS_BLOB_LEN  13u

typedef enum { SETTINGS_LOADED, SETTINGS_BLANK, SETTINGS_CORRUPT } settings_load_t;

void settings_defaults(settings_t *s);
bool settings_valid(const settings_t *s);
void settings_encode(const settings_t *s, uint8_t blob[SETTINGS_BLOB_LEN]);
settings_load_t settings_decode(const uint8_t *blob, settings_t *out); /* out = defaults on failure */

settings_load_t settings_load(settings_t *out);
hal_status_t settings_save(const settings_t *s);

/* Cycle one item to its next value (Settings page BUTTON1). */
void settings_cycle(settings_t *s, uint8_t item);
/* Set one item (LINK_CMD_SET). false, s unchanged: unknown item or value
 * out of range. */
bool settings_set(settings_t *s, uint8_t item, uint8_t value);

#endif
