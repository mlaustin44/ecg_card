/* Link manager TMOS task: the radio's side of the UART link (link_fmt.h).
 *
 * - LINK_HELLO once at boot;
 * - drains the UART RX ring through the frame parser (task context);
 * - LINK_RADIO_CTRL ops answered with LINK_ACK;
 * - LINK_STATUS stored (Status and Battery Level characteristics);
 * - STREAM_ECG_BLOCK / PPG_BLOCK / EVENT handed to the BLE task;
 * - LINK_CMD (Control writes) sent to the MCU, LINK_ACKs counted;
 * - LINK_CONN_STATE on every change; STREAM_LOG lines;
 * - sequence and error counters; watchdog feed. */
#ifndef LINK_MGR_H
#define LINK_MGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void link_mgr_init(uint8_t reset_reason, const uint8_t addr[6]);

/* Main-loop hook (after TMOS_SystemProcess): schedules the RX event when
 * bytes are waiting. */
void link_mgr_poll(void);

/* Send one frame (COBS + CRC, next seq). False if the TX ring is full or
 * after LINK_RC_SHUTDOWN. */
bool link_send(uint8_t type, const uint8_t *payload, size_t len);

/* A Control write: forwarded as LINK_CMD, ACK tracked. */
void link_send_cmd(const uint8_t *p, size_t n);

/* STREAM_LOG line (printf subset, see rl_fmt.h), at most APP_LOG_MAX. */
void link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Something in LINK_CONN_STATE changed: send it (coalesced in task context). */
void link_conn_state_changed(void);

/* Milliseconds since boot, from the 100 ms link tick (wrap-safe diffs). */
uint32_t link_now_ms(void);

#endif
