#include "radio.h"

#include <string.h>

#include "app.h"
#include "applog.h"
#include "hal.h"
#include "stream.h"

#define RX_CHUNK 64u
/* Device log tee: a line goes out only if the estimated TX backlog after it
 * stays within this; the rest of the ring is the data blocks' (one pass
 * sends at most ~115 wire bytes of blocks, events, STATUS and ACK). */
#define RADIO_LOG_TX_MAX (HAL_UART_TX_RING - 128u)
#define OFF_NOW_MAX_PASSES 200u /* radio_off_now(): ~1 ms each, >> LINK_SHUTDOWN_WAIT_MS */

static struct {
    const radio_ops_t *ops;
    radio_info_t info;
    link_rx_t rx;
    bool want, program;
    uint32_t now;
    uint32_t t_state;       /* entry into the current state */
    uint32_t t_adv;         /* last ADV_START */
    uint8_t shutdown_seq;
    bool shutdown_acked;
    bool status_due;        /* send LINK_STATUS at the next chance */
    uint32_t t_status;
    uint8_t last_status[LINK_STATUS_LEN];
    bool log_torn;          /* a teed line lost its tail: next text starts with '\n' */
} R;

static const char *const k_names[RADIO_STATE_COUNT] = {
    "off", "powering", "retry", "advertising", "connected", "shutdown", "FAULT", "PROGRAM",
};

const char *radio_state_name(radio_state_t s)
{
    return (unsigned)s < RADIO_STATE_COUNT ? k_names[s] : "?";
}
radio_state_t radio_state(void) { return R.info.state; }
bool radio_connected(void) { return R.info.state == RADIO_CONNECTED; }
bool radio_program_mode(void) { return R.program; }

const radio_info_t *radio_info(void)
{
    R.info.rx = R.rx.st;
    return &R.info;
}

static void set_state(radio_state_t s)
{
    R.info.state = s;
    R.t_state = R.now;
}

static void ble_log(const char *text) { log_line("ble", text); }

/* ---- power ----------------------------------------------------------------- */

static void power_on(void)
{
    link_tx_reset();
    link_rx_restart(&R.rx);
    hal_radio_power(true);
    R.info.power_ups++;
    R.info.conn = LINK_CS_IDLE;
    R.info.subs = 0;
}

static void power_off(void)
{
    hal_radio_power(false);
    link_rx_restart(&R.rx);
    R.info.conn = LINK_CS_IDLE;
    R.info.subs = 0;
}

static void start_powering(void)
{
    R.info.attempts = 1;
    power_on();
    set_state(RADIO_POWERING);
    ble_log("radio power on");
}

static void ctrl(uint8_t op)
{
    (void)link_send(LINK_RADIO_CTRL, &op, 1);
}

static void begin_shutdown(const char *why)
{
    uint8_t op = LINK_RC_SHUTDOWN;
    R.shutdown_acked = false;
    (void)link_send(LINK_RADIO_CTRL, &op, 1);
    R.shutdown_seq = link_tx_last_seq();
    set_state(RADIO_SHUTTING_DOWN);
    log_begin("ble");
    log_s(" shutdown (");
    log_s(why);
    log_s(")");
    log_end();
}

static void fault(const char *text)
{
    power_off();
    set_state(RADIO_FAULT);
    log_begin("ble");
    log_s(" radio FAULT: ");
    log_s(text);
    log_end();
}

/* ---- frames to the radio ------------------------------------------------------ */

static void send_status(void)
{
    uint8_t s[LINK_STATUS_LEN];
    R.ops->status(s);
    if (link_send(LINK_STATUS, s, sizeof s))
        memcpy(R.last_status, s, sizeof s);
    R.t_status = R.now;
    R.status_due = false;
}

static void status_poll(void)
{
    uint8_t s[LINK_STATUS_LEN];
    uint32_t dt = R.now - R.t_status;
    if (R.status_due || dt >= LINK_STATUS_PERIOD_MS) {
        send_status();
        return;
    }
    if (dt < RADIO_STATUS_MIN_MS)
        return;
    R.ops->status(s);
    if (memcmp(s, R.last_status, sizeof s) != 0)
        send_status();
}

static void adv_start(void)
{
    ctrl(LINK_RC_ADV_START);
    R.t_adv = R.now;
}

/* READY: identify the MCU, publish the status, advertise. */
static void ready(void)
{
    uint8_t info[2 + sizeof APP_FW_VERSION - 1u];
    info[0] = LINK_RC_SET_INFO;
    info[1] = LINK_INFO_MCU_FW;
    memcpy(info + 2, APP_FW_VERSION, sizeof APP_FW_VERSION - 1u);
    (void)link_send(LINK_RADIO_CTRL, info, sizeof info);
    send_status();
    adv_start();
    R.info.conn = LINK_CS_IDLE;
    R.info.subs = 0;
    set_state(RADIO_ADVERTISING);
}

/* ---- frames from the radio ---------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static char hexc(unsigned v) { return "0123456789ABCDEF"[v & 15u]; }

static void on_hello(const uint8_t *p, size_t n)
{
    radio_info_t *i = &R.info;
    if (n < LINK_HELLO_LEN) {
        R.rx.st.bad_frames++;
        return;
    }
    i->hello = true;
    i->proto = p[0];
    memcpy(i->fw, p + 1, 3);
    memcpy(i->addr, p + 4, 6);
    i->reset_reason = p[10];
    char *e = i->fw_str + sizeof i->fw_str - 1;
    char *b = fmt_u(i->fw_str, e, i->fw[0]);
    b = fmt_s(b, e, ".");
    b = fmt_u(b, e, i->fw[1]);
    b = fmt_s(b, e, ".");
    (void)fmt_u(b, e, i->fw[2]);
    i->id_str[0] = hexc(i->addr[1] >> 4);
    i->id_str[1] = hexc(i->addr[1]);
    i->id_str[2] = hexc(i->addr[0] >> 4);
    i->id_str[3] = hexc(i->addr[0]);
    i->id_str[4] = '\0';
    log_begin("ble");
    log_s(" HELLO fw ");
    log_s(i->fw_str);
    log_s(" proto ");
    log_u(i->proto);
    log_s(" addr ");
    for (int k = 5; k >= 0; k--) {
        log_x(i->addr[k], 2);
        if (k)
            log_s(":");
    }
    log_s(" reset ");
    log_u(i->reset_reason);
    {
        /* raw CH585 values */
        static const char *const why[] = {"software", "power-on", "watchdog", "pin", "?", "wake"};
        log_s(" (");
        log_s(i->reset_reason < sizeof why / sizeof why[0] ? why[i->reset_reason] : "?");
        log_s(")");
    }
    log_s(" name \"ECG Card ");
    log_s(i->id_str);
    log_s("\"");
    log_end();

    switch (i->state) {
    case RADIO_PROGRAM:
    case RADIO_SHUTTING_DOWN:
        return; /* shown only */
    case RADIO_POWERING:
    case RADIO_ADVERTISING:
    case RADIO_CONNECTED:
        if (i->proto != LINK_PROTO_VERSION) {
            fault("link protocol mismatch (radio has another version)");
            return;
        }
        if (i->state != RADIO_POWERING) {
            /* e.g. its ~0.5 s watchdog (reset 2): not a fault, set up again */
            i->restarts++;
            ble_log("radio restarted by itself: link set up again");
        }
        ready();
        return;
    default:
        return;
    }
}

static void on_conn_state(const uint8_t *p, size_t n)
{
    radio_info_t *i = &R.info;
    if (n < LINK_CONN_STATE_LEN) {
        R.rx.st.bad_frames++;
        return;
    }
    if (i->state != RADIO_ADVERTISING && i->state != RADIO_CONNECTED)
        return;
    uint8_t was = i->conn, subs_was = i->subs;
    i->conn = p[0];
    i->mtu = rd16(p + 1);
    i->interval = rd16(p + 3);
    i->subs = p[0] == LINK_CS_CONNECTED ? p[5] : 0u;
    if ((i->subs & LINK_SUB_LOG) && !(subs_was & LINK_SUB_LOG))
        R.log_torn = false; /* a new Log subscriber starts clean */
    i->radio_dropped = rd16(p + 6);
    i->phy = p[8];
    if (p[0] == LINK_CS_CONNECTED) {
        if (i->state != RADIO_CONNECTED || subs_was != i->subs) {
            log_begin("ble");
            log_s(i->state != RADIO_CONNECTED ? " connected mtu " : " subscriptions mtu ");
            log_u(i->mtu);
            log_s(" interval ");
            log_u((uint32_t)i->interval * 5u / 4u);
            log_s(" ms sub 0x");
            log_x(i->subs, 2);
            log_end();
        }
        if (i->state != RADIO_CONNECTED)
            R.status_due = true; /* fresh value for the new client */
        set_state(RADIO_CONNECTED);
    } else if (i->state == RADIO_CONNECTED || (was == LINK_CS_CONNECTED && p[0] == LINK_CS_IDLE)) {
        /* the phone went away: advertise again, new window */
        ble_log("disconnected: advertising again");
        set_state(RADIO_ADVERTISING);
        if (p[0] == LINK_CS_IDLE)
            adv_start();
        else
            R.t_adv = R.now;
    }
}

static void on_cmd(uint8_t seq, const uint8_t *p, size_t n)
{
    link_cmd_t c;
    uint8_t res = link_cmd_parse(p, n, &c);
    if (res == LINK_RES_OK)
        res = R.ops->command(&c);
    R.info.cmds++;
    if (res != LINK_RES_OK)
        R.info.cmd_errors++;
    uint8_t ack[LINK_ACK_LEN] = {seq, res};
    (void)link_send(LINK_ACK, ack, sizeof ack);
    stream_ev_cmd_result(n ? p[0] : 0u, res);
    if (c.cmd == LINK_CMD_GET_STATUS && res == LINK_RES_OK)
        send_status();
    log_begin("ble");
    log_s(" cmd ");
    log_u(n ? p[0] : 0u);
    log_s(" -> ");
    log_u(res);
    log_end();
}

static void on_log(const uint8_t *p, size_t n)
{
    char t[96];
    size_t m = n < sizeof t - 1u ? n : sizeof t - 1u;
    for (size_t k = 0; k < m; k++)
        t[k] = (p[k] >= 0x20u && p[k] < 0x7Fu) ? (char)p[k] : '?';
    while (m && t[m - 1] == '?') /* trailing CR / LF */
        m--;
    t[m] = '\0';
    R.info.radio_logs++;
    log_line_local("radio", t); /* the radio tees its own lines to Log */
}

/* ---- device log tee (Log characteristic) -------------------------------------
 * Every applog line, while a phone is connected with LINK_SUB_LOG, in
 * STREAM_LOG frames of <= LINK_LOG_CHUNK_MAX bytes straight from the line
 * buffer. Lowest priority: the whole line must fit under RADIO_LOG_TX_MAX of
 * estimated TX backlog, else it is dropped whole; if the ring refuses a
 * chunk anyway, the rest of the line is dropped (the head already sent is
 * then ended by a '\n' in front of the next teed text). Never retried, never
 * waits, counted in log_drops. */
static void log_tee(const char *s, size_t n)
{
    if (R.info.state != RADIO_CONNECTED || !(R.info.subs & LINK_SUB_LOG) || n == 0)
        return;
    size_t total = n + (R.log_torn ? 1u : 0u);
    size_t chunks = (total + LINK_LOG_CHUNK_MAX - 1u) / LINK_LOG_CHUNK_MAX;
    if (link_tx_backlog() + total + chunks * LINK_WIRE_LEN(0) > RADIO_LOG_TX_MAX) {
        R.info.log_drops++;
        return;
    }
    uint8_t c[LINK_LOG_CHUNK_MAX];
    size_t k = 0;
    while (k < n) {
        size_t m = 0;
        if (k == 0 && R.log_torn)
            c[m++] = '\n';
        size_t take = n - k < LINK_LOG_CHUNK_MAX - m ? n - k : LINK_LOG_CHUNK_MAX - m;
        memcpy(c + m, s + k, take);
        if (!link_send(STREAM_LOG, c, m + take)) {
            R.info.log_drops++;
            if (k)
                R.log_torn = true;
            return;
        }
        R.log_torn = false;
        k += take;
    }
    R.info.log_lines++;
}

static void on_frame(void *ctx, uint8_t type, uint8_t seq, const uint8_t *p, size_t n)
{
    (void)ctx;
    radio_state_t st = R.info.state;
    bool live = st == RADIO_ADVERTISING || st == RADIO_CONNECTED;
    switch (type) {
    case LINK_HELLO: on_hello(p, n); break;
    case STREAM_LOG: on_log(p, n); break;
    case LINK_CONN_STATE: on_conn_state(p, n); break;
    case LINK_CMD:
        if (live)
            on_cmd(seq, p, n);
        break;
    case LINK_ACK:
        if (n < LINK_ACK_LEN) {
            R.rx.st.bad_frames++;
            break;
        }
        R.info.acks++;
        if (p[1] != LINK_RES_OK)
            R.info.naks++;
        if (st == RADIO_SHUTTING_DOWN && p[0] == R.shutdown_seq)
            R.shutdown_acked = true;
        break;
    default: R.rx.st.unknown_types++; break;
    }
}

/* ---- main ------------------------------------------------------------------------ */

void radio_init(const radio_ops_t *ops)
{
    memset(&R, 0, sizeof R);
    R.ops = ops;
    link_rx_init(&R.rx);
    link_init();
    log_set_tee(log_tee);
    R.info.state = RADIO_OFF;
    hal_radio_power(false); /* already the hal_init() state */
}

void radio_want(bool on) { R.want = on; }

void radio_user(void) { R.info.adv_expired = false; }

void radio_rearm(void)
{
    R.info.adv_expired = false;
    if (R.info.state == RADIO_FAULT)
        set_state(RADIO_OFF);
}

void radio_program(bool on)
{
    if (on == R.program)
        return;
    R.program = on;
    if (on) {
        if (!hal_radio_powered())
            power_on();
        set_state(RADIO_PROGRAM);
        ble_log("program mode ON: RADIO_EN held, link silent");
    } else {
        ble_log("program mode off");
        R.info.adv_expired = false;
        begin_shutdown("program mode off");
    }
}

static void receive(void)
{
    uint8_t b[RX_CHUNK];
    size_t n;
    if (!hal_radio_powered())
        return;
    while ((n = hal_uart_read(b, sizeof b)) != 0)
        link_rx_feed(&R.rx, b, n, on_frame, NULL);
}

void radio_poll(uint32_t now)
{
    R.now = now;
    receive();
    uint32_t dt = now - R.t_state;
    switch (R.info.state) {
    case RADIO_OFF:
        if (R.want && !R.info.adv_expired)
            start_powering();
        break;
    case RADIO_POWERING:
        if (!R.want) {
            power_off();
            set_state(RADIO_OFF);
        } else if (dt > LINK_HELLO_TIMEOUT_MS) { /* ms ticks: > gives the radio >= 500 ms */
            R.info.hello_timeouts++;
            power_off();
            if (R.info.attempts < 2u) {
                ble_log("no HELLO: power-cycling the radio once");
                set_state(RADIO_RETRY_GAP);
            } else {
                fault("no HELLO after 2 power-ups (RADIO_VCC? CH585 firmware?)");
            }
        }
        break;
    case RADIO_RETRY_GAP:
        if (!R.want) {
            set_state(RADIO_OFF);
        } else if (dt >= RADIO_RETRY_OFF_MS) {
            R.info.attempts = 2;
            power_on();
            set_state(RADIO_POWERING);
        }
        break;
    case RADIO_ADVERTISING:
    case RADIO_CONNECTED:
        if (!R.want) {
            begin_shutdown("not wanted");
        } else if (R.info.state == RADIO_ADVERTISING &&
                   now - R.t_adv >= LINK_ADV_TOTAL_SECONDS * 1000u) {
            R.info.adv_expired = true;
            begin_shutdown("advertising timeout: off until a button or BLE On");
        } else {
            status_poll();
        }
        break;
    case RADIO_SHUTTING_DOWN:
        if (R.shutdown_acked || dt >= LINK_SHUTDOWN_WAIT_MS) {
            if (!R.shutdown_acked) {
                R.info.shutdown_timeouts++;
                ble_log("no ACK to SHUTDOWN: power cut after 50 ms");
            }
            power_off();
            set_state(RADIO_OFF);
            ble_log("radio off");
        }
        break;
    case RADIO_FAULT:
    case RADIO_PROGRAM:
    default:
        break;
    }
}

void radio_off_now(void)
{
    R.want = false;
    if (R.program) {
        R.program = false;
        begin_shutdown("standby");
    }
    switch (R.info.state) {
    case RADIO_POWERING:
    case RADIO_RETRY_GAP:
        power_off();
        set_state(RADIO_OFF);
        return;
    case RADIO_ADVERTISING:
    case RADIO_CONNECTED: begin_shutdown("standby"); break;
    default: break;
    }
    for (unsigned k = 0; k < OFF_NOW_MAX_PASSES && R.info.state == RADIO_SHUTTING_DOWN; k++) {
        hal_idle();
        radio_poll(hal_millis());
    }
    if (hal_radio_powered()) { /* backstop: never enter STANDBY with the radio on */
        power_off();
        set_state(RADIO_OFF);
    }
}

void radio_tap(uint8_t type, const uint8_t *p, size_t n)
{
    if (R.info.state != RADIO_CONNECTED || n == 0)
        return;
    uint8_t subs = R.info.subs, need;
    switch (type) {
    case STREAM_ECG_BLOCK: need = LINK_SUB_ECG; break;
    case STREAM_PPG_BLOCK: need = LINK_SUB_PPG; break;
    case STREAM_EVENT:
        /* the radio derives Heart Rate (0x2A37) from R_PEAK / PULSE events */
        need = LINK_SUB_EVENTS | LINK_SUB_HR;
        break;
    default: return;
    }
    if ((subs & need) && !link_send(type, p, n))
        R.info.data_drops++;
}
