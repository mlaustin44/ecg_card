/* Radio manager: the CH585 link state machine (fw/include/link_fmt.h).
 * Non-blocking; radio_poll() runs once per main-loop pass. Only
 * radio_off_now() (STANDBY entry) waits, for at most LINK_SHUTDOWN_WAIT_MS.
 *
 *   OFF --want--> POWERING --HELLO--> (READY: SET_INFO, STATUS, ADV_START)
 *                    |                    -> ADVERTISING <--CONN_STATE--> CONNECTED
 *                    | no HELLO in LINK_HELLO_TIMEOUT_MS
 *                    v
 *                RETRY_GAP (unpowered RADIO_RETRY_OFF_MS) -> POWERING again
 *                    | no HELLO again (or HELLO with a wrong protocol)
 *                    v
 *                  FAULT (unpowered; "ble:" log line, UI fault icon)
 *   ADVERTISING / CONNECTED --!want, adv timeout--> SHUTTING_DOWN
 *       (LINK_RC_SHUTDOWN, then the ACK or LINK_SHUTDOWN_WAIT_MS) -> OFF
 *   any --radio_program(true)--> PROGRAM (RADIO_EN held, MCU silent, no
 *       timeouts; HELLO and radio log lines are still shown)
 *       --radio_program(false)--> SHUTTING_DOWN -> OFF
 *
 * Policy inputs (the app decides, radio.c executes):
 *   radio_want(on)     the radio should be up (BLE setting On, card awake,
 *                      battery not low). Ignored in PROGRAM.
 *   radio_user()       a button press: re-arms after an advertising timeout.
 *   radio_rearm()      wake from STANDBY / BLE turned On / program mode left:
 *                      clears the advertising timeout and FAULT.
 *   radio_adv_hold(on) HR monitor session: the advertising window does not
 *                      expire (a sports app may connect at any time, and
 *                      again after a disconnect); on also clears an
 *                      expired window. Released, the window counts afresh.
 * Advertising window: LINK_ADV_TOTAL_SECONDS from each ADV_START (power-up,
 * and again after a phone disconnects); if nobody connects in that time the
 * radio is shut down and stays off until radio_user() / radio_rearm().
 * A connection never times out here.
 *
 * Data: radio_tap() is the stream tap. ECG / PPG blocks and events go to the
 * radio only while CONNECTED with the matching LINK_SUB_* bit; events with
 * LINK_SUB_EVENTS or LINK_SUB_HR (the radio derives Heart Rate from R_PEAK /
 * PULSE). Notification payloads stay <= 32 bytes (CH585 limit; ECG 25, PPG
 * 15, events <= 11). An unexpected HELLO (the radio restarted, e.g. its
 * ~0.5 s watchdog: reset_reason 2) is logged and the link set up again. LINK_STATUS goes out in ADVERTISING and CONNECTED: on change (at
 * most every RADIO_STATUS_MIN_MS), every LINK_STATUS_PERIOD_MS and after
 * LINK_CMD_GET_STATUS. Radio STREAM_LOG lines are printed as "radio: ...".
 *
 * Device log: while CONNECTED with LINK_SUB_LOG, every applog line (RTT
 * channel 0) is also sent as STREAM_LOG frames of <= LINK_LOG_CHUNK_MAX
 * bytes for the Log characteristic, except the "radio: ..." lines (the
 * radio tees its own). RTT is unaffected. Lowest priority on the UART: a
 * line is sent only if the estimated TX backlog (link_tx_backlog) after it
 * stays <= HAL_UART_TX_RING - 128, else dropped whole; if the ring still
 * refuses a chunk, the rest of that line is dropped and the next teed text
 * starts with '\n'. Dropped lines are counted (log_drops), never retried. */
#ifndef APP_RADIO_H
#define APP_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link.h"

#define RADIO_RETRY_OFF_MS  100u /* RADIO_VCC discharge before the second try */
#define RADIO_STATUS_MIN_MS 250u

typedef enum {
    RADIO_OFF,
    RADIO_POWERING,
    RADIO_RETRY_GAP,
    RADIO_ADVERTISING,
    RADIO_CONNECTED,
    RADIO_SHUTTING_DOWN,
    RADIO_FAULT,
    RADIO_PROGRAM,
    RADIO_STATE_COUNT
} radio_state_t;

typedef struct {
    /* the app executes a validated phone command (link_cmd_parse() == OK)
     * and returns LINK_RES_* */
    uint8_t (*command)(const link_cmd_t *c);
    /* fill a LINK_STATUS payload */
    void (*status)(uint8_t out[LINK_STATUS_LEN]);
} radio_ops_t;

typedef struct {
    radio_state_t state;
    bool adv_expired;          /* off after the advertising window */
    uint8_t attempts;          /* power-ups in the current POWERING sequence */
    /* last HELLO (kept after power-off) */
    bool hello;
    uint8_t proto, fw[3], addr[6], reset_reason;
    char fw_str[12];           /* "1.2.3" */
    char id_str[5];            /* last two address bytes, hex: "E5F6" (device name) */
    /* last LINK_CONN_STATE */
    uint8_t conn;              /* LINK_CS_* */
    uint16_t mtu, interval;    /* interval in 1.25 ms units */
    uint8_t subs, phy;
    uint16_t radio_dropped;    /* notifications the radio dropped */
    /* counters since boot */
    uint32_t power_ups, hello_timeouts, shutdown_timeouts;
    uint32_t restarts;         /* unexpected HELLOs (radio reset by itself) */
    uint32_t cmds, cmd_errors; /* phone commands; those not answered OK */
    uint32_t acks, naks;       /* radio ACKs to LINK_RADIO_CTRL (naks: result != OK) */
    uint32_t radio_logs;
    uint32_t data_drops;       /* data frames for the phone that did not fit the TX ring */
    uint32_t log_lines;        /* log lines teed to the phone in full */
    uint32_t log_drops;        /* ... dropped whole or cut short (TX backlog) */
    link_rx_stats_t rx;
} radio_info_t;

void radio_init(const radio_ops_t *ops);
void radio_poll(uint32_t now_ms);
void radio_want(bool on);
void radio_user(void);
void radio_rearm(void);
void radio_adv_hold(bool on);
void radio_program(bool on);
bool radio_program_mode(void);
/* STANDBY entry: shutdown handshake now, waiting (hal_idle) for the ACK or
 * LINK_SHUTDOWN_WAIT_MS; leaves the radio unpowered and program mode off. */
void radio_off_now(void);

void radio_tap(uint8_t type, const uint8_t *payload, size_t len);

radio_state_t radio_state(void);
bool radio_connected(void);
const radio_info_t *radio_info(void);
const char *radio_state_name(radio_state_t s);

#endif
