/* Link manager TMOS task (see link_mgr.h). The frame parser, message
 * layouts and ACK tracking are the portable rl_*.c modules, tested on the
 * host; this file only wires them to TMOS, the UART and the BLE task. */
#include <stdarg.h>
#include <stdbool.h>

#include "CONFIG.h"

#include "app_config.h"
#include "ble_app.h"
#include "radio_board.h"
#include "link_mgr.h"
#include "rl_codec.h"
#include "rl_fmt.h"
#include "rl_msg.h"
#include "rl_parser.h"
#include "uart_link.h"

/* Task events */
#define LM_EVT_RX         0x0001
#define LM_EVT_HELLO      0x0002
#define LM_EVT_TICK       0x0004
#define LM_EVT_CONN_STATE 0x0008

/* Bytes parsed per RX event before yielding to the other tasks. */
#define LM_RX_BUDGET 600u

static tmosTaskID s_task = INVALID_TASK_ID;
static rl_parser_t s_parser;
static rl_acktrack_t s_acks;
static uint8_t s_wire[RL_WIRE_MAX];
static uint8_t s_tx_seq;
static bool s_shutdown;
static bool s_rx_scheduled;
static uint32_t s_ms;

static uint8_t s_reset_reason;
static uint8_t s_addr[6];

static rl_conn_state_t s_cs_sent;
static uint32_t s_cs_sent_ms;

/* error and traffic counters */
static uint32_t s_tx_frames, s_tx_drops;
static uint32_t s_rx_unknown, s_rx_bad_payload, s_rx_logs;
static uint32_t s_stats_sig, s_stats_ms;

static tmosEvents lm_process_event(tmosTaskID task_id, tmosEvents events);

uint32_t link_now_ms(void)
{
    return s_ms;
}

bool link_send(uint8_t type, const uint8_t *payload, size_t len)
{
    size_t n;

    if (s_shutdown)
        return false;
    n = rl_frame_encode(type, s_tx_seq, payload, len, s_wire, sizeof s_wire);
    if (n == 0u) {
        s_tx_drops++;
        return false;
    }
    /* seq advances for every frame produced, so the MCU counts a frame
     * dropped here as a gap */
    s_tx_seq++;
    if (!uart_link_write(s_wire, n)) {
        s_tx_drops++;
        return false;
    }
    s_tx_frames++;
    return true;
}

void link_log(const char *fmt, ...)
{
    char buf[APP_LOG_MAX + 1u];
    va_list ap;
    size_t n;

    if (s_shutdown)
        return;
    va_start(ap, fmt);
    n = rl_vfmt(buf, sizeof buf, fmt, ap);
    va_end(ap);
    link_send(STREAM_LOG, (const uint8_t *)buf, n);
    ble_app_log_own(buf, n); /* the phone's Log, if enabled */
}

void link_send_cmd(const uint8_t *p, size_t n)
{
    uint8_t seq = s_tx_seq;
    if (!rl_msg_cmd_len_ok(n))
        return;
    if (link_send(LINK_CMD, p, n))
        rl_ack_sent(&s_acks, seq, s_ms);
}

void link_conn_state_changed(void)
{
    if (s_task != INVALID_TASK_ID)
        tmos_set_event(s_task, LM_EVT_CONN_STATE);
}

static void lm_send_conn_state(void)
{
    uint8_t b[LINK_CONN_STATE_LEN];
    rl_conn_state_t cs;

    ble_app_conn_state(&cs);
    rl_msg_conn_state(&cs, b);
    if (link_send(LINK_CONN_STATE, b, sizeof b)) {
        s_cs_sent = cs;
        s_cs_sent_ms = s_ms;
    }
}

static void lm_send_hello(void)
{
    rl_hello_t h;
    uint8_t b[LINK_HELLO_LEN];

    h.fw_major = APP_FW_MAJOR;
    h.fw_minor = APP_FW_MINOR;
    h.fw_patch = APP_FW_PATCH;
    tmos_memcpy(h.addr, s_addr, sizeof h.addr);
    h.reset_reason = s_reset_reason;
    rl_msg_hello(&h, b);
    link_send(LINK_HELLO, b, sizeof b);
    link_log("boot: radio fw %s, reset %u, %s, Fsys %u Hz, UART DL %u = %u baud",
             APP_FW_STRING, s_reset_reason, (const char *)VER_LIB,
             (unsigned)GetSysClock(), uart_link_divisor(), (unsigned)uart_link_actual_baud());
}

static void lm_radio_ctrl(const rl_frame_t *f)
{
    rl_radio_ctrl_t rc;
    uint8_t ack[LINK_ACK_LEN];
    uint8_t res = rl_msg_parse_radio_ctrl(f->payload, f->len, &rc);

    if (res == LINK_RES_OK) {
        switch (rc.op) {
        case LINK_RC_ADV_START:
            res = ble_app_adv_start();
            break;
        case LINK_RC_ADV_STOP:
            res = ble_app_adv_stop();
            break;
        case LINK_RC_DISCONNECT:
            res = ble_app_disconnect();
            break;
        case LINK_RC_SET_INFO:
            ble_app_set_sw_rev(rc.text, rc.text_len);
            break;
        case LINK_RC_SHUTDOWN:
            /* stop advertising, drop the connection, ACK, then silence */
            ble_app_shutdown();
            rl_msg_ack(f->seq, LINK_RES_OK, ack);
            link_send(LINK_ACK, ack, sizeof ack);
            s_shutdown = true;
            return;
        default:
            res = LINK_RES_UNKNOWN;
            break;
        }
    }
    rl_msg_ack(f->seq, res, ack);
    link_send(LINK_ACK, ack, sizeof ack);
    if (res != LINK_RES_OK)
        link_log("link: radio_ctrl op %u -> result %u", f->len ? f->payload[0] : 0u, res);
}

static void lm_handle_frame(const rl_frame_t *f)
{
    switch (f->type) {
    case LINK_STATUS: {
        rl_status_t st;
        if (rl_msg_parse_status(f->payload, f->len, &st))
            ble_app_status(&st);
        else
            s_rx_bad_payload++;
        break;
    }
    case STREAM_ECG_BLOCK:
    case STREAM_PPG_BLOCK:
    case STREAM_EVENT:
        ble_app_stream(f->type, f->payload, f->len);
        break;
    case LINK_ACK: {
        uint8_t seq, result;
        if (rl_msg_parse_ack(f->payload, f->len, &seq, &result))
            rl_ack_received(&s_acks, seq);
        else
            s_rx_bad_payload++;
        break;
    }
    case LINK_RADIO_CTRL:
        lm_radio_ctrl(f);
        break;
    case STREAM_LOG:
        /* the MCU's device log tee: to Log (or dropped) */
        s_rx_logs++;
        ble_app_log_mcu(f->seq, f->payload, f->len);
        break;
    default:
        s_rx_unknown++;
        break;
    }
}

static void lm_rx(void)
{
    unsigned n = 0;
    uint8_t b;
    rl_frame_t f;

    while (n < LM_RX_BUDGET && uart_link_rx_get(&b)) {
        n++;
        if (rl_parser_feed(&s_parser, b, &f))
            lm_handle_frame(&f);
    }
}

void link_mgr_poll(void)
{
    if (!s_rx_scheduled && uart_link_rx_pending()) {
        s_rx_scheduled = true;
        tmos_set_event(s_task, LM_EVT_RX);
    }
}

static void lm_stats(void)
{
    uart_link_stats_t u;
    const rl_parser_stats_t *p = &s_parser.st;
    uint32_t sig;

    uart_link_stats(&u);
    sig = p->crc_err + p->cobs_err + p->len_err + p->oversize + p->seq_gaps + u.rx_overflow +
          u.overrun + u.frame + u.parity + u.brk + u.tx_full + s_tx_drops + s_rx_unknown +
          s_rx_bad_payload + s_acks.timeouts + s_acks.unmatched;
    if (sig == s_stats_sig)
        return;
    s_stats_sig = sig;
    link_log("link: rx %u crc %u cobs %u len %u big %u gap %u unk %u bad %u",
             (unsigned)p->frames, (unsigned)p->crc_err, (unsigned)p->cobs_err,
             (unsigned)p->len_err, (unsigned)p->oversize, (unsigned)p->seq_gaps,
             (unsigned)s_rx_unknown, (unsigned)s_rx_bad_payload);
    link_log("link: uart ovr %u fe %u pe %u brk %u full %u | tx %u drop %u | ack %u to %u unm %u",
             (unsigned)u.overrun, (unsigned)u.frame, (unsigned)u.parity, (unsigned)u.brk,
             (unsigned)u.rx_overflow, (unsigned)s_tx_frames, (unsigned)(s_tx_drops + u.tx_full),
             (unsigned)s_acks.acked, (unsigned)s_acks.timeouts, (unsigned)s_acks.unmatched);
}

static void lm_tick(void)
{
    rl_conn_state_t cs;
    unsigned expired;

    s_ms += (APP_TICK * 5u) / 8u; /* 0.625 ms units -> ms */
    board_watchdog_feed();

    expired = rl_ack_poll(&s_acks, s_ms);
    if (expired)
        link_log("link: %u LINK_CMD without ACK", expired);

    ble_app_tick(s_ms);

    /* 'dropped' changes at up to 100/s: report it at most once a second */
    ble_app_conn_state(&cs);
    if (cs.dropped != s_cs_sent.dropped &&
        (uint32_t)(s_ms - s_cs_sent_ms) >= APP_DROP_REPORT_MS)
        lm_send_conn_state();

    if ((uint32_t)(s_ms - s_stats_ms) >= APP_STATS_PERIOD_MS) {
        s_stats_ms = s_ms;
        lm_stats();
    }
}

void link_mgr_init(uint8_t reset_reason, const uint8_t addr[6])
{
    s_reset_reason = reset_reason;
    tmos_memcpy(s_addr, addr, sizeof s_addr);
    rl_parser_init(&s_parser);
    rl_ack_init(&s_acks);

    s_task = TMOS_ProcessEventRegister(lm_process_event);
    tmos_set_event(s_task, LM_EVT_HELLO);
    tmos_start_task(s_task, LM_EVT_TICK, APP_TICK);
}

static tmosEvents lm_process_event(tmosTaskID task_id, tmosEvents events)
{
    (void)task_id;

    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg;
        if ((pMsg = tmos_msg_receive(s_task)) != NULL)
            tmos_msg_deallocate(pMsg);
        return (events ^ SYS_EVENT_MSG);
    }

    if (events & LM_EVT_HELLO) {
        lm_send_hello();
        return (events ^ LM_EVT_HELLO);
    }

    if (events & LM_EVT_RX) {
        s_rx_scheduled = false;
        lm_rx();
        return (events ^ LM_EVT_RX);
    }

    if (events & LM_EVT_CONN_STATE) {
        lm_send_conn_state();
        return (events ^ LM_EVT_CONN_STATE);
    }

    if (events & LM_EVT_TICK) {
        tmos_start_task(s_task, LM_EVT_TICK, APP_TICK);
        lm_tick();
        return (events ^ LM_EVT_TICK);
    }

    /* Discard unknown events */
    return 0;
}
