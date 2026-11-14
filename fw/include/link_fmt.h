/* Inter-processor link and BLE payload contract (protocol version 1).
 *
 * Shared by the MSPM0 application, the CH585 radio firmware (which keeps a
 * byte-identical copy in CH585F/APP/include; a host test compares them) and
 * the browser client. Plain C89-compatible defines only.
 *
 * ---- UART link ----------------------------------------------------------
 * MSPM0 UART0 <-> CH585 UART1 (remapped PB12/PB13), 460800 baud 8N1, no flow
 * control. Framing is exactly stream_fmt.h: COBS(frame) + 0x00, frame =
 * type u8, seq u8, len u16 LE, payload, CRC-16/CCITT-FALSE u16 LE. Each
 * direction has its own seq counter over all frame types. Frames with a bad
 * CRC are dropped and counted. All multi-byte fields are little-endian.
 *
 * The MSPM0 is the master: it powers the radio, owns every setting and
 * decides when to advertise. The radio never acts on measurement settings.
 * In this version the radio stays awake while powered (no wake handshake);
 * it drives RADIO_IRQ low.
 *
 * ---- Frames -------------------------------------------------------------
 * LINK_HELLO (0x01) radio -> MCU, once after boot, 12 bytes:
 *   u8 proto (LINK_PROTO_VERSION), u8 fw_major, u8 fw_minor, u8 fw_patch,
 *   u8 ble_addr[6] (least significant byte first), u8 reset_reason,
 *   u8 reserved (0)
 *
 * LINK_STATUS (0x02) MCU -> radio, on change, every 5 s, and on
 *   LINK_CMD_GET_STATUS; also the value of the Status characteristic,
 *   20 bytes:
 *   u8 proto, u8 mode (0 idle, 1 ECG, 2 PPG, 3 both),
 *   u8 meas (ui_meas_state_t while a mode is active, else 0xFF),
 *   u8 batt_pct (0..100, 255 = unknown),
 *   u8 flags (LINK_ST_*),
 *   u8 gain, u8 notch, u8 rld, u8 led_limit, u8 sweep  (settings.h enums),
 *   u8 notch_hz (50/60: in use), u8 hr_bpm (0 = n/a; saturates at 255),
 *   u16 batt_mv (0 = unknown),
 *   u8 fw_major, u8 fw_minor, u8 fw_patch, u8 reserved (0),
 *   u16 uptime_min
 *
 * STREAM_ECG_BLOCK (0x10), STREAM_PPG_BLOCK (0x11), STREAM_EVENT (0x12)
 *   MCU -> radio, payloads exactly as in stream_fmt.h. The MCU sends them
 *   only while LINK_CONN_STATE reports a connection with the matching
 *   subscription. The radio forwards the payload unchanged as a
 *   notification and never parses it, except STREAM_EVENT kinds R_PEAK and
 *   PULSE, from which it also updates Heart Rate Measurement (0x2A37).
 *
 * LINK_CMD (0x20) radio -> MCU: a phone write to the Control characteristic,
 *   forwarded unchanged. Payload: u8 cmd, then arguments:
 *   LINK_CMD_START (1): u8 mode (1 ECG, 2 PPG, 3 both)
 *   LINK_CMD_STOP (2): no arguments
 *   LINK_CMD_SET (3): u8 item (settings_item_t), u8 value
 *   LINK_CMD_SET_TIME (4): u32 unix_seconds (UTC), i16 utc_offset_minutes
 *   LINK_CMD_GET_STATUS (5): no arguments
 *   The MCU answers every LINK_CMD with LINK_ACK, and also emits
 *   STREAM_EVENT kind STREAM_EV_CMD_RESULT { u8 cmd, u8 result } so the
 *   phone sees the outcome on the Events characteristic.
 *
 * LINK_ACK (0x21) both directions, 2 bytes: u8 seq of the frame being
 *   acknowledged, u8 result (LINK_RES_*). Sent for LINK_CMD (by the MCU) and
 *   LINK_RADIO_CTRL (by the radio). Data blocks are never acknowledged.
 *
 * STREAM_LOG (0x30) both directions: UTF-8 text. The radio has no debug
 *   port, so its log lines travel to the MCU, which prints them on RTT
 *   channel 0 prefixed "radio: ".
 *   MCU -> radio (device log over BLE): only while LINK_CONN_STATE reports a
 *   connection with LINK_SUB_LOG. The MCU tees its RTT channel 0 text (not
 *   the "radio: " lines it prints for the radio) as a byte stream cut into
 *   frames of at most 32 payload bytes; lines end with '\n' and a line may
 *   span several frames, sent back to back (consecutive seq: a gap inside
 *   a line tells the radio a piece was lost). The radio forwards each payload unchanged as one
 *   Log notification while Log is subscribed and drops it otherwise. The
 *   radio also emits its own log lines on Log, while subscribed, as
 *   "radio: " + text + '\n' in chunks of at most 32 bytes, between MCU
 *   lines. Best-effort on both ends: under back-pressure log text is
 *   dropped (never retried) and counted, and it never delays or displaces
 *   ECG / PPG / Events / Status data. Text is dropped as whole lines or as
 *   the rest of a line (never a tail without its start); a line cut short
 *   is ended by a '\n' before the next text, so a client can show it as a
 *   short line.
 *
 * LINK_RADIO_CTRL (0x40) MCU -> radio: u8 op, then arguments:
 *   LINK_RC_ADV_START (1): start connectable advertising. The radio runs
 *     the schedule itself: 100 ms interval for 30 s, then 1 s interval.
 *   LINK_RC_ADV_STOP (2)
 *   LINK_RC_DISCONNECT (3)
 *   LINK_RC_SHUTDOWN (5): stop advertising, drop the connection, ACK, then
 *     send nothing more. The MCU cuts power 50 ms later.
 *   LINK_RC_SET_INFO (6): u8 kind (1 = MCU firmware revision string for the
 *     Device Information service), then text without terminator.
 *
 * LINK_CONN_STATE (0x41) radio -> MCU, on every change, 10 bytes:
 *   u8 state (LINK_CS_*), u16 mtu, u16 conn_interval (1.25 ms units, 0 when
 *   not connected), u8 subscribed (LINK_SUB_*), u16 dropped (notifications
 *   dropped since boot, saturating), u8 phy (1 = 1M, 2 = 2M, 0 = n/a),
 *   u8 reserved (0)
 *
 * ---- Power-up and power-down (ecg-fw.md section 5) ------------------------
 *   up:   MCU enables UART pins, RADIO_EN high; radio sends LINK_HELLO within
 *         500 ms (MCU retries the power cycle once, then reports a fault).
 *         MCU sends LINK_RC_SET_INFO, LINK_STATUS, then LINK_RC_ADV_START.
 *   down: MCU sends LINK_RC_SHUTDOWN, waits for the ACK or 50 ms, stops the
 *         UART, drives TX low / RX pull-down, RADIO_EN low.
 *
 * ---- BLE ------------------------------------------------------------------
 * Device name "ECG Card XXXX" (last two bytes of the BLE address, hex).
 * Advertising data carries the 128-bit ECG Card service UUID so a client can
 * filter on it. Connection: request 20 ms interval, latency 0, 4 s
 * supervision timeout, data length 251, ATT MTU 247. Just Works bonding.
 *
 * Standard services: Device Information 0x180A (manufacturer, model,
 * firmware revision = radio version, software revision = MCU version from
 * LINK_RC_SET_INFO, serial = BLE address), Battery 0x180F (0x2A19 read +
 * notify, from LINK_STATUS; 255/unknown reported as 0), Heart Rate 0x180D
 * (0x2A37 notify: flags 0x00, u8 bpm).
 *
 * ECG Card service, base d578xxxx-8a28-493a-ad8d-6b741ba09ea7:
 *   service        d5780001-8a28-493a-ad8d-6b741ba09ea7
 *   ECG stream     d5780002-...  notify            STREAM_ECG_BLOCK payload
 *   PPG stream     d5780003-...  notify            STREAM_PPG_BLOCK payload
 *   Events         d5780004-...  notify            STREAM_EVENT payload
 *   Status         d5780005-...  read, notify      LINK_STATUS payload
 *   Control        d5780006-...  write (with response)  LINK_CMD payload
 *   Log            d5780007-...  notify            device log text (UTF-8,
 *                                                  STREAM_LOG chunks, <= 32 B)
 * A Control write is accepted at ATT level if it is 1..16 bytes; the command
 * result arrives on Events as STREAM_EV_CMD_RESULT.
 * Log is optional for clients (radio firmware before it has no 0x0007). It
 * has the lowest priority: the radio starts queueing a line only while its
 * notification queue is at most half full (else the line, or its rest, is
 * dropped), log text never makes it drop data, and log text it refuses is
 * counted apart from LINK_CONN_STATE.dropped (which still counts queued
 * entries of any kind lost when the queue overflows).
 * If notifications back up (link stall), the radio keeps about 1 s of data,
 * drops the oldest blocks and counts them in LINK_CONN_STATE.dropped.
 *
 * ---- Clarifications (as implemented on both ends) ---------------------------
 * - Device name digits: ble_addr[1] then ble_addr[0] of LINK_HELLO, upper-case
 *   hex (address A1:B2:C3:D4:E5:F6 gives "ECG Card E5F6").
 * - LINK_HELLO.reset_reason is the raw CH585 value: 0 software, 1 power-on,
 *   2 watchdog, 3 reset pin, 5 wake. A HELLO after the first means the radio
 *   restarted: the MCU logs it and repeats SET_INFO, STATUS, ADV_START.
 * - STREAM_EVENT frames are sent while LINK_SUB_EVENTS or LINK_SUB_HR is set;
 *   the radio notifies the Events characteristic only if it is subscribed.
 * - Notification payloads are at most 32 bytes.
 * - LINK_CONN_STATE: mtu is 0 when not connected; a changing dropped count is
 *   reported at most once per second.
 * - After a disconnect the radio resumes fast advertising by itself unless
 *   the last advertising command was LINK_RC_ADV_STOP.
 * - LINK_RC_ADV_START while connected is answered LINK_RES_BUSY; while
 *   advertising, LINK_RES_OK without restarting the schedule. A
 *   LINK_RADIO_CTRL op with unexpected extra bytes gets LINK_RES_BAD_ARG.
 *   LINK_RC_SET_INFO text is at most 20 bytes.
 * - Nothing is acknowledged after LINK_RC_SHUTDOWN.
 * - LINK_CMD_SET item 5 (SET_ITEM_BLE) is Bluetooth on/off. Gain, notch, RLD
 *   and LED limit are refused with LINK_RES_BUSY during a session.
 * - LINK_CMD_SET_TIME is refused with LINK_RES_BAD_ARG before 2024-01-01 or
 *   with an offset outside -720..+840 minutes.
 * - LINK_CMD_START during a session is answered LINK_RES_BUSY (stop first).
 * - Actual baud: radio 458823 (-0.43 %), MCU 460432 or 461095. */
#ifndef LINK_FMT_H
#define LINK_FMT_H

#include "stream_fmt.h"

#define LINK_PROTO_VERSION 1u
#define LINK_BAUD          460800u

/* frame types (in addition to STREAM_* in stream_fmt.h) */
#define LINK_HELLO      0x01u
#define LINK_STATUS     0x02u
#define LINK_CMD        0x20u
#define LINK_ACK        0x21u
#define LINK_RADIO_CTRL 0x40u
#define LINK_CONN_STATE 0x41u

#define LINK_HELLO_LEN      12u
#define LINK_STATUS_LEN     20u
#define LINK_ACK_LEN        2u
#define LINK_CONN_STATE_LEN 10u
#define LINK_CMD_MAX_LEN    16u

/* LINK_STATUS.flags */
#define LINK_ST_USB          0x01u
#define LINK_ST_CHARGING     0x02u
#define LINK_ST_CHARGE_FAULT 0x04u
#define LINK_ST_NO_CONTACT   0x08u
#define LINK_ST_BATT_LOW     0x10u
#define LINK_ST_GAIN_HI      0x20u
#define LINK_ST_TIME_VALID   0x40u

/* LINK_CMD.cmd */
#define LINK_CMD_START      1u
#define LINK_CMD_STOP       2u
#define LINK_CMD_SET        3u
#define LINK_CMD_SET_TIME   4u
#define LINK_CMD_GET_STATUS 5u

/* LINK_ACK.result and STREAM_EV_CMD_RESULT.result */
#define LINK_RES_OK           0u
#define LINK_RES_UNKNOWN      1u /* unknown command or op */
#define LINK_RES_BAD_ARG      2u
#define LINK_RES_REFUSED_USB  3u /* ECG while USB power is present */
#define LINK_RES_REFUSED_BATT 4u
#define LINK_RES_BUSY         5u
#define LINK_RES_FAULT        6u

/* LINK_RADIO_CTRL.op */
#define LINK_RC_ADV_START  1u
#define LINK_RC_ADV_STOP   2u
#define LINK_RC_DISCONNECT 3u
#define LINK_RC_SHUTDOWN   5u
#define LINK_RC_SET_INFO   6u
#define LINK_INFO_MCU_FW   1u

/* LINK_CONN_STATE.state */
#define LINK_CS_IDLE      0u
#define LINK_CS_ADV_FAST  1u
#define LINK_CS_ADV_SLOW  2u
#define LINK_CS_CONNECTED 3u

/* LINK_CONN_STATE.subscribed */
#define LINK_SUB_ECG    0x01u
#define LINK_SUB_PPG    0x02u
#define LINK_SUB_EVENTS 0x04u
#define LINK_SUB_STATUS 0x08u
#define LINK_SUB_HR     0x10u
#define LINK_SUB_LOG    0x20u

/* STREAM_LOG frames on the link carry at most this many payload bytes when
 * they are meant for the Log characteristic (the MCU's tee, the radio's own
 * lines on Log). */
#define LINK_LOG_CHUNK_MAX 32u

/* STREAM_EVENT kind added by the link: { u8 cmd, u8 result } */
#define STREAM_EV_CMD_RESULT 6u

/* Timing */
#define LINK_HELLO_TIMEOUT_MS    500u
#define LINK_SHUTDOWN_WAIT_MS    50u
#define LINK_STATUS_PERIOD_MS    5000u
#define LINK_ADV_FAST_MS         100u
#define LINK_ADV_FAST_SECONDS    30u
#define LINK_ADV_SLOW_MS         1000u
#define LINK_ADV_TOTAL_SECONDS   120u /* MCU powers the radio off after this */

/* ECG Card service UUIDs, as 16 bytes least significant first (the order
 * BLE stacks and advertising data use). Short id goes in bytes 12..13. */
#define LINK_UUID128(short_id)                                              \
    { 0xa7, 0x9e, 0xa0, 0x1b, 0x74, 0x6b, 0x8d, 0xad, 0x3a, 0x49, 0x28,     \
      0x8a, (uint8_t)((short_id) & 0xffu), (uint8_t)((short_id) >> 8),      \
      0x78, 0xd5 }
#define LINK_UUID_SERVICE 0x0001u
#define LINK_UUID_ECG     0x0002u
#define LINK_UUID_PPG     0x0003u
#define LINK_UUID_EVENTS  0x0004u
#define LINK_UUID_STATUS  0x0005u
#define LINK_UUID_CONTROL 0x0006u
#define LINK_UUID_LOG     0x0007u

#endif
