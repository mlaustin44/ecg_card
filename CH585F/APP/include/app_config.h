/* CH585 radio firmware: application constants.
 *
 * BLE stack settings (CONFIG.h overrides) are NOT here: following WCH's
 * convention they are preprocessor defines in the project settings
 * (.cproject, C Compiler > Defined symbols), so that HAL/ and LIB/ see the
 * same values. See README.md. */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "link_fmt.h"

/* Radio firmware version, reported in LINK_HELLO and Device Information. */
#define APP_FW_MAJOR 0u
#define APP_FW_MINOR 1u
#define APP_FW_PATCH 0u
#define APP_FW_STRING "0.1.0"

/* Device Information strings. */
#define APP_DIS_MANUFACTURER "ECG Card"
#define APP_DIS_MODEL        "ECG Card rev 1"

/* Advertising (0.625 ms units): 100 ms for 30 s, then 1 s (link_fmt.h). */
#define APP_ADV_FAST_INT      ((LINK_ADV_FAST_MS * 8u) / 5u)       /* 160 */
#define APP_ADV_FAST_DURATION (LINK_ADV_FAST_SECONDS * 1600u)      /* 48000 = 30 s */
#define APP_ADV_SLOW_INT      ((LINK_ADV_SLOW_MS * 8u) / 5u)       /* 1600 */
#define APP_ADV_TX_POWER_DBM  0                                    /* BLE_TX_POWER default */

/* Connection request (link_fmt.h): 20 ms, latency 0, 4 s supervision. */
#define APP_CONN_INTERVAL 16u  /* 1.25 ms units */
#define APP_CONN_LATENCY  0u
#define APP_CONN_TIMEOUT  400u /* 10 ms units */
/* Delay before the connection parameter update request (as WCH's
 * HeartRate example: 1600 x 0.625 ms = 1 s). */
#define APP_PARAM_UPDATE_DELAY 1600u

/* Notification pump: entries sent per pass, and the back-off (0.625 ms
 * units) after the stack reports no buffer. */
#define APP_NOTIFY_BURST    8u
#define APP_NOTIFY_BACKOFF  8u /* 5 ms */

/* Link manager tick (0.625 ms units): watchdog feed, ACK timeouts. */
#define APP_TICK            160u /* 100 ms */
/* Error summary log period and CONN_STATE rate limit for 'dropped'. */
#define APP_STATS_PERIOD_MS 10000u
#define APP_DROP_REPORT_MS  1000u

/* Watchdog: R8_WDOG_COUNT counts Fsys/131072 and resets on overflow,
 * 256 x 131072 / 62.4 MHz = 538 ms; fed every APP_TICK. Set to 0 while
 * single-stepping with the debugger. */
#ifndef APP_WATCHDOG
#define APP_WATCHDOG 1
#endif

/* Peripheral-initiated ATT MTU exchange right after connecting, as WCH's
 * HID_BTSSP_NFC peripheral example does (needs GATT_InitClient()). */
#ifndef APP_MTU_EXCHANGE
#define APP_MTU_EXCHANGE 1
#endif

/* Longest STREAM_LOG line (bytes). */
#define APP_LOG_MAX 120u

#endif
