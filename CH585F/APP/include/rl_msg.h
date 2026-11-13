/* Builders and parsers for the link_fmt.h payloads handled by the radio
 * (portable, no WCH headers). Byte offsets follow link_fmt.h exactly; all
 * multi-byte fields are little-endian. */
#ifndef RL_MSG_H
#define RL_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_fmt.h"

/* ---- radio -> MCU ---------------------------------------------------- */

typedef struct {
    uint8_t fw_major, fw_minor, fw_patch;
    uint8_t addr[6];      /* least significant byte first */
    uint8_t reset_reason; /* CH585 R8_RESET_STATUS & RB_RESET_FLAG, raw */
} rl_hello_t;

/* LINK_HELLO payload, LINK_HELLO_LEN (12) bytes. Returns 12. */
size_t rl_msg_hello(const rl_hello_t *h, uint8_t out[LINK_HELLO_LEN]);

typedef struct {
    uint8_t state;     /* LINK_CS_* */
    uint16_t mtu;      /* ATT MTU, 0 when not connected */
    uint16_t interval; /* 1.25 ms units, 0 when not connected */
    uint8_t subscribed; /* LINK_SUB_* */
    uint16_t dropped;  /* saturating */
    uint8_t phy;       /* 1 = 1M, 2 = 2M, 0 = n/a */
} rl_conn_state_t;

/* LINK_CONN_STATE payload, LINK_CONN_STATE_LEN (10) bytes. Returns 10. */
size_t rl_msg_conn_state(const rl_conn_state_t *s, uint8_t out[LINK_CONN_STATE_LEN]);

/* LINK_ACK payload, 2 bytes. Returns 2. */
size_t rl_msg_ack(uint8_t seq, uint8_t result, uint8_t out[LINK_ACK_LEN]);

/* A Control characteristic write is forwarded as LINK_CMD if 1..16 bytes. */
bool rl_msg_cmd_len_ok(size_t n);

/* ---- MCU -> radio ---------------------------------------------------- */

bool rl_msg_parse_ack(const uint8_t *p, size_t n, uint8_t *seq, uint8_t *result);

typedef struct {
    uint8_t proto, mode, meas, batt_pct, flags;
    uint8_t gain, notch, rld, led_limit, sweep;
    uint8_t notch_hz, hr_bpm;
    uint16_t batt_mv;
    uint8_t fw_major, fw_minor, fw_patch;
    uint16_t uptime_min;
    uint8_t raw[LINK_STATUS_LEN]; /* the Status characteristic value */
} rl_status_t;

/* LINK_STATUS: needs at least LINK_STATUS_LEN bytes; any extra bytes (a
 * later protocol version) are ignored. */
bool rl_msg_parse_status(const uint8_t *p, size_t n, rl_status_t *s);

/* Status characteristic value before the first LINK_STATUS arrives:
 * proto 1, idle, meas 0xFF, battery 255 (unknown), everything else 0. */
void rl_msg_status_default(uint8_t out[LINK_STATUS_LEN]);

/* Battery Level (0x2A19) from LINK_STATUS.batt_pct: 255 (unknown) -> 0,
 * values above 100 clamp to 100. */
uint8_t rl_msg_battery_level(uint8_t batt_pct);

#define RL_INFO_MAX 20u /* longest LINK_RC_SET_INFO text kept */

typedef struct {
    uint8_t op;
    uint8_t info_kind;     /* LINK_RC_SET_INFO only */
    const uint8_t *text;   /* LINK_RC_SET_INFO only, not terminated */
    size_t text_len;       /* truncated to RL_INFO_MAX */
} rl_radio_ctrl_t;

/* Parse LINK_RADIO_CTRL. Returns the LINK_ACK result: LINK_RES_OK,
 * LINK_RES_UNKNOWN (unknown op or info kind) or LINK_RES_BAD_ARG (empty
 * payload, arguments on an op that takes none, SET_INFO without kind). */
uint8_t rl_msg_parse_radio_ctrl(const uint8_t *p, size_t n, rl_radio_ctrl_t *rc);

/* STREAM_EVENT R_PEAK (bpm at offset 5) or PULSE (bpm at offset 5).
 * Returns false for other kinds or short payloads. */
bool rl_msg_event_bpm(const uint8_t *p, size_t n, uint8_t *kind, uint16_t *bpm);

/* ---- Heart Rate Measurement ----------------------------------------- */

/* Source selection: R-peaks (ECG) win; PULSE (PPG) is used only when no
 * R-peak has been seen for RL_HR_ECG_HOLD_MS. */
#define RL_HR_ECG_HOLD_MS 5000u

typedef struct {
    bool have_rpeak;
    uint32_t last_rpeak_ms;
} rl_hr_sel_t;

/* Returns true if an event of this kind at now_ms should update the
 * Heart Rate Measurement. */
bool rl_hr_select(rl_hr_sel_t *s, uint8_t kind, uint32_t now_ms);

/* 0x2A37 value: flags 0x00 (u8 format, no contact/energy/RR), u8 bpm
 * (saturates at 255). Returns 2. */
size_t rl_msg_hr_measurement(uint16_t bpm, uint8_t out[2]);

/* ---- LINK_CMD acknowledgement tracking ------------------------------ */

#define RL_ACK_SLOTS 8u
#define RL_ACK_TIMEOUT_MS 500u

typedef struct {
    bool used[RL_ACK_SLOTS];
    uint8_t seq[RL_ACK_SLOTS];
    uint32_t t_ms[RL_ACK_SLOTS];
    uint32_t acked, unmatched, timeouts, overwritten;
} rl_acktrack_t;

void rl_ack_init(rl_acktrack_t *a);
/* A LINK_CMD with this seq was sent at now_ms. If every slot is busy the
 * oldest is counted as a timeout and reused. */
void rl_ack_sent(rl_acktrack_t *a, uint8_t seq, uint32_t now_ms);
/* A LINK_ACK for seq arrived. Returns true if it matched a pending CMD. */
bool rl_ack_received(rl_acktrack_t *a, uint8_t seq);
/* Expire pending CMDs older than RL_ACK_TIMEOUT_MS; returns how many. */
unsigned rl_ack_poll(rl_acktrack_t *a, uint32_t now_ms);

#endif
