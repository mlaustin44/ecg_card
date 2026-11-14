/* BLE peripheral TMOS task: GAP role, advertising schedule, connection
 * parameters, GATT services and the notification queue. Structured like
 * WCH's Peripheral and HeartRate examples. */
#ifndef BLE_APP_H
#define BLE_APP_H

#include <stddef.h>
#include <stdint.h>

#include "rl_msg.h"

void ble_app_init(const uint8_t addr[6]);

/* LINK_RADIO_CTRL ops; return a LINK_RES_* code for the LINK_ACK. */
uint8_t ble_app_adv_start(void);
uint8_t ble_app_adv_stop(void);
uint8_t ble_app_disconnect(void);
/* Stop advertising and drop the connection; nothing is sent afterwards. */
void ble_app_shutdown(void);
void ble_app_set_sw_rev(const uint8_t *text, size_t len);

/* LINK_STATUS from the MCU: Status and Battery Level. */
void ble_app_status(const rl_status_t *st);

/* STREAM_ECG_BLOCK / STREAM_PPG_BLOCK / STREAM_EVENT payload from the MCU. */
void ble_app_stream(uint8_t type, const uint8_t *p, size_t n);

/* Device log for the Log characteristic (rl_log.h): an MCU STREAM_LOG
 * frame (link seq, payload <= 32 bytes) and one of the radio's own lines
 * (text without '\n'; sent as "radio: " + text + '\n'). Queued only while a
 * phone has Log enabled and the notification queue has room to spare. */
void ble_app_log_mcu(uint8_t seq, const uint8_t *p, size_t n);
void ble_app_log_own(const char *text, size_t n);

/* Current LINK_CONN_STATE contents. */
void ble_app_conn_state(rl_conn_state_t *cs);

/* Link tick (100 ms): re-reads the CCCDs (the bond manager restores them
 * on reconnection without a write callback) and logs notification
 * statistics every APP_STATS_PERIOD_MS while connected. */
void ble_app_tick(uint32_t now_ms);

#endif
