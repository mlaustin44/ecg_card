/* UART0 + fake CH585 radio. See sim.h "radio link".
 *
 * Two byte-timed lines at LINK_BAUD:
 *   MCU -> radio: the MCU's TX ring (hal_uart_write) drains into the fake
 *     radio's frame parser;
 *   radio -> MCU: the fake radio's output queue drains into the MCU's RX
 *     ring (hal_uart_read).
 * The fake radio has its own COBS / CRC codec (it must not share app code). */
#include <stdlib.h>
#include <string.h>

#include "sim_priv.h"

#define NEVER UINT64_MAX
#define RADIO_OUT_CAP 8192u
#define RADIO_IN_MAX  700u
#define LOG_MAX       128u
#define ACKS_MAX      64u
#define TIMERS_MAX    16u
#define HW_FIFO       4u
#define TRACE_MAX     512u

typedef enum { T_HELLO, T_SEND_ACK, T_SHUTDOWN_ACK, T_ADV_SLOW, T_BOOT_LOG } timer_kind_t;
typedef struct {
    bool used;
    uint64_t t_us;
    timer_kind_t kind;
    uint8_t a, b;
    uint32_t gen;
} timer_t_;

/* A byte-timed line. Byte k (0-based) of a burst ends at start + (k+1) * 21.7 us. */
typedef struct {
    bool busy;
    uint64_t start_us;
    uint64_t n; /* bytes completed in this burst */
} line_t;

static uint64_t line_done_us(const line_t *l)
{
    return l->start_us + ((l->n + 1u) * (uint64_t)SIM_UART_BYTE_NS + 999u) / 1000u;
}

static struct {
    /* ---- MCU side ---- */
    bool uart_on;
    uint8_t tx[HAL_UART_TX_RING];
    size_t tx_head, tx_n;
    uint8_t rx[HAL_UART_RX_RING];
    size_t rx_head, rx_n;
    uint8_t hw_fifo[HW_FIFO];
    size_t hw_n;
    uint32_t masked_tx_budget;
    hal_uart_stats_t st;
    line_t to_radio, to_mcu;
    bool to_mcu_corrupt_inflight;
    bool tx_stall;

    /* ---- trace ---- */
    sim_rt_t tr[TRACE_MAX];
    uint64_t tr_t[TRACE_MAX];
    size_t tr_n;

    /* ---- fake radio ---- */
    sim_radio_cfg_t cfg;
    sim_radio_info_t info;
    uint32_t gen;            /* power-up generation: stale timers are ignored */
    uint32_t silent_left;
    uint8_t seq_out;
    bool have_seq_in;
    uint8_t seq_in;
    uint8_t out[RADIO_OUT_CAP];
    size_t out_head, out_n;
    uint8_t in[RADIO_IN_MAX];
    size_t in_n;
    bool in_overflow;
    timer_t_ tm[TIMERS_MAX];
    uint64_t shutdown_rx_us;
    bool was_connected;
    bool adv_stopped;        /* last advertising command was ADV_STOP */
    uint32_t next_ecg, next_ppg;
    bool have_ecg, have_ppg;
    /* injection */
    uint32_t corrupt_mcu_n, drop_mcu_n, corrupt_radio_n;
    uint8_t corrupt_mcu_x, corrupt_radio_x;
    /* records */
    sim_link_frame_t log[LOG_MAX];
    size_t log_n; /* total, ring of LOG_MAX */
    struct { uint8_t seq, res; } acks[ACKS_MAX];
    size_t acks_n;
    bool have_result[256];
    uint8_t result[256];
} R;

static const char k_boot_log[] = "CH585 link up";

/* Text the phone received on the Log characteristic (kept outside R, which
 * radio_reset() clears with a memset; 1 MB). */
#define PHONE_LOG_CAP (1u << 20)
static char s_phone_log[PHONE_LOG_CAP + 1];
static size_t s_phone_log_n;
static uint32_t s_phone_log_lost;

static void phone_log_add(const void *p, size_t n)
{
    if (s_phone_log_n + n > PHONE_LOG_CAP) {
        s_phone_log_lost += (uint32_t)n;
        return;
    }
    memcpy(s_phone_log + s_phone_log_n, p, n);
    s_phone_log_n += n;
    s_phone_log[s_phone_log_n] = 0;
}

sim_radio_cfg_t sim_radio_defaults(void)
{
    sim_radio_cfg_t c;
    memset(&c, 0, sizeof c);
    c.present = true;
    c.hello_delay_ms = 20;
    c.proto = LINK_PROTO_VERSION;
    c.fw[0] = 1;
    c.fw[1] = 2;
    c.fw[2] = 3;
    static const uint8_t addr[6] = {0xF6, 0xE5, 0xD4, 0xC3, 0xB2, 0xA1};
    memcpy(c.addr, addr, 6);
    c.boot_log = k_boot_log;
    c.ack_delay_us = 300;
    c.ack_shutdown = true;
    c.reading = true;
    return c;
}

void sim_radio_config(const sim_radio_cfg_t *cfg)
{
    R.cfg = *cfg;
    R.silent_left = cfg->silent_boots;
}

const sim_radio_info_t *sim_radio_info(void) { return &R.info; }
bool sim_uart_enabled(void) { return R.uart_on; }
bool sim_radio_tx_pin(void) { return R.uart_on; }
size_t sim_radio_trace_count(void) { return R.tr_n; }
sim_rt_t sim_radio_trace_get(size_t i, uint64_t *t_us)
{
    if (i >= R.tr_n || i >= TRACE_MAX) {
        if (t_us) *t_us = 0;
        return SIM_RT_RADIO_EN_LOW;
    }
    if (t_us) *t_us = R.tr_t[i];
    return R.tr[i];
}
static void trace(sim_rt_t e)
{
    if (R.tr_n < TRACE_MAX) {
        R.tr[R.tr_n] = e;
        R.tr_t[R.tr_n] = sim_now_us();
    }
    R.tr_n++;
}

/* ------------------------------------------------------------------------ */
/* the fake radio's own codec                                                */

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFFu;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int b = 0; b < 8; b++)
            c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
    }
    return c;
}

static size_t cobs_enc(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 1, code_at = 0;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
        if (in[i]) {
            out[o++] = in[i];
            if (++code == 0xFF) {
                out[code_at] = code;
                code_at = o++;
                code = 1;
            }
        } else {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        }
    }
    out[code_at] = code;
    return o;
}

static long cobs_dec(const uint8_t *in, size_t n, uint8_t *out, size_t max)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t code = in[i++];
        if (!code || i + code - 1 > n) return -1;
        for (int k = 1; k < code; k++) {
            if (o >= max || !in[i]) return -1;
            out[o++] = in[i++];
        }
        if (code != 0xFF && i < n) {
            if (o >= max) return -1;
            out[o++] = 0;
        }
    }
    return (long)o;
}

/* ------------------------------------------------------------------------ */
/* radio output queue -> line                                                */

static void out_push(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (R.out_n == RADIO_OUT_CAP) sim_fatal("fake radio output queue overflow");
        R.out[(R.out_head + R.out_n) % RADIO_OUT_CAP] = b[i];
        R.out_n++;
    }
    if (!R.to_mcu.busy && R.out_n) {
        R.to_mcu.busy = true;
        R.to_mcu.start_us = sim_now_us();
        R.to_mcu.n = 0;
    }
}

static void radio_send(uint8_t type, const uint8_t *p, size_t n)
{
    uint8_t f[600], w[620];
    if (n > 512) sim_fatal("fake radio frame too long");
    f[0] = type;
    f[1] = R.seq_out++;
    f[2] = (uint8_t)n;
    f[3] = (uint8_t)(n >> 8);
    if (n) memcpy(f + 4, p, n);
    uint16_t c = crc16(f, n + 4);
    f[n + 4] = (uint8_t)c;
    f[n + 5] = (uint8_t)(c >> 8);
    size_t m = cobs_enc(f, n + 6, w);
    w[m++] = 0;
    out_push(w, m);
}

static bool radio_alive(void) { return R.info.powered && R.cfg.present && !R.info.shut_down; }

static void send_conn_state(void)
{
    uint8_t p[LINK_CONN_STATE_LEN] = {0};
    bool c = R.info.conn_state == LINK_CS_CONNECTED;
    p[0] = R.info.conn_state;
    p[1] = (uint8_t)(c ? R.info.mtu : 0u); /* the CH585 reports 0 when not connected */
    p[2] = (uint8_t)((c ? R.info.mtu : 0u) >> 8);
    p[3] = (uint8_t)(c ? R.info.interval : 0u);
    p[4] = (uint8_t)((c ? R.info.interval : 0u) >> 8);
    p[5] = c ? R.info.subs : 0u;
    p[8] = c ? 2u : 0u;
    if (radio_alive()) radio_send(LINK_CONN_STATE, p, sizeof p);
}

static void set_conn(uint8_t st)
{
    if (R.info.conn_state == st) return;
    R.info.conn_state = st;
    send_conn_state();
}

static void timer_add(uint64_t t_us, timer_kind_t k, uint8_t a, uint8_t b);
static void timer_cancel(timer_kind_t k);

static void adv_fast(void)
{
    set_conn(LINK_CS_ADV_FAST);
    timer_cancel(T_ADV_SLOW);
    timer_add(sim_now_us() + LINK_ADV_FAST_SECONDS * 1000000ull, T_ADV_SLOW, 0, 0);
}

/* ------------------------------------------------------------------------ */
/* timers                                                                    */

static void timer_add(uint64_t t_us, timer_kind_t k, uint8_t a, uint8_t b)
{
    for (size_t i = 0; i < TIMERS_MAX; i++) {
        if (!R.tm[i].used) {
            R.tm[i] = (timer_t_){true, t_us, k, a, b, R.gen};
            return;
        }
    }
    sim_fatal("fake radio: timer table full");
}

static void timer_cancel(timer_kind_t k)
{
    for (size_t i = 0; i < TIMERS_MAX; i++)
        if (R.tm[i].used && R.tm[i].kind == k) R.tm[i].used = false;
}

static void send_hello(void)
{
    uint8_t p[LINK_HELLO_LEN] = {0};
    p[0] = R.cfg.proto;
    p[1] = R.cfg.fw[0];
    p[2] = R.cfg.fw[1];
    p[3] = R.cfg.fw[2];
    memcpy(p + 4, R.cfg.addr, 6);
    p[10] = R.cfg.reset_reason;
    radio_send(LINK_HELLO, p, sizeof p);
    R.info.hello_sent = true;
    R.info.hellos++;
}

static void timer_fire(const timer_t_ *t)
{
    if (t->gen != R.gen || !R.info.powered || !R.cfg.present) return;
    if (t->kind == T_SHUTDOWN_ACK) {
        uint8_t p[2] = {t->a, t->b};
        radio_send(LINK_ACK, p, 2);
        return;
    }
    if (!radio_alive()) return;
    switch (t->kind) {
    case T_HELLO:
        send_hello();
        if (R.cfg.boot_log) timer_add(sim_now_us() + 200, T_BOOT_LOG, 0, 0);
        break;
    case T_BOOT_LOG:
        radio_send(STREAM_LOG, (const uint8_t *)R.cfg.boot_log, strlen(R.cfg.boot_log));
        break;
    case T_SEND_ACK: {
        uint8_t p[2] = {t->a, t->b};
        radio_send(LINK_ACK, p, 2);
        break;
    }
    case T_SHUTDOWN_ACK: break;
    case T_ADV_SLOW:
        if (R.info.conn_state == LINK_CS_ADV_FAST) set_conn(LINK_CS_ADV_SLOW);
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* frames from the MCU                                                       */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void log_frame(uint8_t type, uint8_t seq, const uint8_t *p, size_t n)
{
    sim_link_frame_t *f = &R.log[R.log_n % LOG_MAX];
    memset(f, 0, sizeof *f);
    f->t_us = sim_now_us();
    f->type = type;
    f->seq = seq;
    f->len = (uint16_t)n;
    memcpy(f->p, p, n < sizeof f->p ? n : sizeof f->p);
    R.log_n++;
}

static void on_ctrl(uint8_t seq, const uint8_t *p, size_t n)
{
    uint8_t res = LINK_RES_OK;
    uint8_t op = n ? p[0] : 0xFF;
    if (op < 8) R.info.ctrl_ops[op]++;
    uint64_t t_ack = sim_now_us() + R.cfg.ack_delay_us;
    /* as the CH585 firmware: extra bytes on an op without arguments -> BAD_ARG */
    if ((op == LINK_RC_ADV_START || op == LINK_RC_ADV_STOP || op == LINK_RC_DISCONNECT ||
         op == LINK_RC_SHUTDOWN) && n != 1) {
        timer_add(t_ack, T_SEND_ACK, seq, LINK_RES_BAD_ARG);
        return;
    }
    switch (op) {
    case LINK_RC_ADV_START:
        R.adv_stopped = false;
        if (R.info.conn_state == LINK_CS_CONNECTED) {
            res = LINK_RES_BUSY;
        } else if (R.info.conn_state == LINK_CS_IDLE) {
            adv_fast();
        } /* already advertising: OK, schedule not restarted */
        break;
    case LINK_RC_ADV_STOP:
        R.adv_stopped = true;
        if (R.info.conn_state != LINK_CS_CONNECTED) set_conn(LINK_CS_IDLE);
        timer_cancel(T_ADV_SLOW);
        break;
    case LINK_RC_DISCONNECT:
        if (R.info.conn_state == LINK_CS_CONNECTED) {
            R.info.subs = 0;
            set_conn(LINK_CS_IDLE);
        }
        break;
    case LINK_RC_SHUTDOWN:
        /* stop advertising, drop the connection, ACK, then send nothing more */
        timer_cancel(T_ADV_SLOW);
        R.info.conn_state = LINK_CS_IDLE;
        R.info.subs = 0;
        R.info.shutdowns++;
        R.info.shut_down = true;
        R.shutdown_rx_us = sim_now_us();
        if (R.cfg.ack_shutdown) timer_add(t_ack, T_SHUTDOWN_ACK, seq, LINK_RES_OK);
        return;
    case LINK_RC_SET_INFO:
        if (n >= 2 && p[1] == LINK_INFO_MCU_FW) {
            size_t m = n - 2 < 20u ? n - 2 : 20u; /* the CH585 keeps at most 20 bytes */
            memcpy(R.info.mcu_info, p + 2, m);
            R.info.mcu_info[m] = 0;
        } else {
            res = LINK_RES_BAD_ARG;
        }
        break;
    default: res = LINK_RES_UNKNOWN; break;
    }
    timer_add(t_ack, T_SEND_ACK, seq, res);
}

static bool subscribed(uint8_t sub)
{
    return R.info.conn_state == LINK_CS_CONNECTED && (R.info.subs & sub);
}

static void on_frame(const uint8_t *f, size_t len)
{
    uint8_t type = f[0], seq = f[1];
    size_t n = (size_t)(f[2] | f[3] << 8);
    const uint8_t *p = f + 4;
    R.info.frames++;
    R.info.by_type[type]++;
    if (R.have_seq_in && seq != (uint8_t)(R.seq_in + 1u)) R.info.seq_gaps++;
    R.have_seq_in = true;
    R.seq_in = seq;
    (void)len;
    if (R.info.shut_down) {
        R.info.after_shutdown++;
        return;
    }
    if (type != STREAM_ECG_BLOCK && type != STREAM_PPG_BLOCK && type != STREAM_LOG)
        log_frame(type, seq, p, n);
    uint64_t now = sim_now_us();
    if ((type == STREAM_ECG_BLOCK || type == STREAM_PPG_BLOCK || type == STREAM_EVENT ||
         type == STREAM_LOG) && n > LINK_LOG_CHUNK_MAX)
        R.info.oversize_notify++; /* the CH585 forwards at most 32 bytes */
    switch (type) {
    case STREAM_ECG_BLOCK:
        if (!subscribed(LINK_SUB_ECG)) R.info.unsolicited++;
        if (n == 25) {
            uint32_t st = rd32(p);
            if (st == 0) R.info.ecg_runs++;
            else if (!R.have_ecg || st != R.next_ecg) R.info.ecg_gaps++;
            R.have_ecg = true;
            R.next_ecg = st + 10u;
        }
        if (!R.info.ecg_blocks) R.info.ecg_first_us = now;
        R.info.ecg_last_us = now;
        R.info.ecg_blocks++;
        break;
    case STREAM_PPG_BLOCK:
        if (!subscribed(LINK_SUB_PPG)) R.info.unsolicited++;
        if (n == 15) {
            uint32_t st = rd32(p);
            if (st == 0) R.info.ppg_runs++;
            else if (!R.have_ppg || st != R.next_ppg) R.info.ppg_gaps++;
            R.have_ppg = true;
            R.next_ppg = st + 2u;
        }
        if (!R.info.ppg_blocks) R.info.ppg_first_us = now;
        R.info.ppg_last_us = now;
        R.info.ppg_blocks++;
        break;
    case STREAM_EVENT:
        R.info.events++;
        if (n && p[0] < 8) R.info.events_by_kind[p[0]]++;
        {
            /* the radio derives Heart Rate from R_PEAK / PULSE: events are
             * wanted with LINK_SUB_EVENTS or LINK_SUB_HR */
            if (!subscribed(LINK_SUB_EVENTS) && !subscribed(LINK_SUB_HR)) R.info.unsolicited++;
        }
        if (n >= 3 && p[0] == STREAM_EV_CMD_RESULT) {
            R.have_result[p[1]] = true;
            R.result[p[1]] = p[2];
        }
        break;
    case LINK_STATUS:
        R.info.status_frames++;
        if (n == LINK_STATUS_LEN) memcpy(R.info.last_status, p, n);
        break;
    case LINK_ACK:
        if (n == LINK_ACK_LEN) {
            R.acks[R.acks_n % ACKS_MAX].seq = p[0];
            R.acks[R.acks_n % ACKS_MAX].res = p[1];
            R.acks_n++;
        }
        break;
    case LINK_RADIO_CTRL: on_ctrl(seq, p, n); break;
    case STREAM_LOG:
        /* device log tee: forwarded to Log while subscribed, else dropped */
        R.info.log_frames++;
        R.info.log_bytes += (uint32_t)n;
        if (!subscribed(LINK_SUB_LOG)) R.info.unsolicited++;
        else if (n <= LINK_LOG_CHUNK_MAX) phone_log_add(p, n);
        break;
    default: R.info.unknown_types++; break;
    }
}

static void radio_rx_byte(uint8_t b)
{
    if (!R.info.powered || !R.cfg.present || !R.cfg.reading) return;
    if (b != 0) {
        if (R.in_n < RADIO_IN_MAX) R.in[R.in_n++] = b;
        else R.in_overflow = true;
        return;
    }
    if (R.in_n && !R.in_overflow) {
        uint8_t f[RADIO_IN_MAX];
        long m = cobs_dec(R.in, R.in_n, f, sizeof f);
        if (m < 6 || (size_t)(f[2] | f[3] << 8) + 6u != (size_t)m ||
            crc16(f, (size_t)m - 2) != (uint16_t)(f[m - 2] | f[m - 1] << 8))
            R.info.crc_errors++;
        else
            on_frame(f, (size_t)m);
    } else if (R.in_overflow) {
        R.info.crc_errors++;
    }
    R.in_n = 0;
    R.in_overflow = false;
}

/* ------------------------------------------------------------------------ */
/* lines                                                                     */

static void mcu_rx_store(uint8_t b)
{
    if (R.rx_n == HAL_UART_RX_RING) {
        R.st.rx_overruns++;
        return;
    }
    R.rx[(R.rx_head + R.rx_n) % HAL_UART_RX_RING] = b;
    R.rx_n++;
    R.st.rx_bytes++;
}

/* the RX "ISR" runs again: empty the hardware FIFO into the ring */
static void mcu_rx_unmask(void)
{
    for (size_t i = 0; i < R.hw_n; i++)
        mcu_rx_store(R.hw_fifo[i]);
    R.hw_n = 0;
}

static void deliver_to_mcu(uint8_t b, bool corrupt)
{
    if (R.drop_mcu_n) {
        R.drop_mcu_n--;
        return;
    }
    if (R.corrupt_mcu_n) {
        R.corrupt_mcu_n--;
        b ^= R.corrupt_mcu_x;
    }
    if (corrupt) {
        b ^= 0x5Au;
        R.st.rx_errors++;
    }
    if (!R.uart_on) return;
    if (sim_irq_masked()) {
        if (R.hw_n < HW_FIFO) R.hw_fifo[R.hw_n++] = b;
        else R.st.rx_overruns++;
        return;
    }
    if (R.hw_n) mcu_rx_unmask();
    mcu_rx_store(b);
}

static void deliver_to_radio(uint8_t b)
{
    if (R.corrupt_radio_n) {
        R.corrupt_radio_n--;
        b ^= R.corrupt_radio_x;
    }
    if (R.info.powered) radio_rx_byte(b);
}

static void tx_kick(void)
{
    if (!R.to_radio.busy && R.tx_n && R.uart_on && !R.tx_stall) {
        R.to_radio.busy = true;
        R.to_radio.start_us = sim_now_us();
        R.to_radio.n = 0;
    }
}

uint64_t radio_next_event_us(void)
{
    uint64_t t = NEVER;
    if (R.to_radio.busy) {
        uint64_t d = line_done_us(&R.to_radio);
        if (d < t) t = d;
    }
    if (R.to_mcu.busy) {
        uint64_t d = line_done_us(&R.to_mcu);
        if (d < t) t = d;
    }
    for (size_t i = 0; i < TIMERS_MAX; i++)
        if (R.tm[i].used && R.tm[i].t_us < t) t = R.tm[i].t_us;
    if (!sim_irq_masked() && (R.hw_n || (!R.to_radio.busy && R.tx_n && R.uart_on && !R.tx_stall)))
        t = sim_now_us();
    return t;
}

void radio_run_events(uint64_t t)
{
    if (!sim_irq_masked()) {
        if (R.hw_n) mcu_rx_unmask();
        tx_kick();
    }
    for (;;) {
        bool any = false;
        if (R.to_radio.busy && line_done_us(&R.to_radio) <= t) {
            any = true;
            if (R.tx_stall || (sim_irq_masked() && R.masked_tx_budget == 0)) {
                /* the ISR cannot refill the FIFO: the line goes idle */
                R.to_radio.busy = false;
            } else {
                if (sim_irq_masked()) R.masked_tx_budget--;
                uint8_t b = R.tx[R.tx_head];
                R.tx_head = (R.tx_head + 1) % HAL_UART_TX_RING;
                R.tx_n--;
                R.to_radio.n++;
                deliver_to_radio(b);
                if (!R.tx_n || !R.uart_on) R.to_radio.busy = false;
            }
        }
        if (R.to_mcu.busy && line_done_us(&R.to_mcu) <= t) {
            any = true;
            uint8_t b = R.out[R.out_head];
            R.out_head = (R.out_head + 1) % RADIO_OUT_CAP;
            R.out_n--;
            R.to_mcu.n++;
            bool c = R.to_mcu_corrupt_inflight;
            R.to_mcu_corrupt_inflight = false;
            deliver_to_mcu(b, c);
            if (!R.out_n) R.to_mcu.busy = false;
        }
        for (size_t i = 0; i < TIMERS_MAX; i++) {
            if (R.tm[i].used && R.tm[i].t_us <= t) {
                timer_t_ tm = R.tm[i];
                R.tm[i].used = false;
                any = true;
                timer_fire(&tm);
            }
        }
        if (!any) break;
    }
}

/* ------------------------------------------------------------------------ */
/* hooks from sim_core                                                       */

static void radio_power_cut(void)
{
    R.gen++;
    if (R.info.hello_sent) {
        if (R.info.shut_down) {
            uint64_t w = sim_now_us() - R.shutdown_rx_us;
            R.info.last_shutdown_wait_us = w;
            if (w > R.info.max_shutdown_wait_us) R.info.max_shutdown_wait_us = w;
            if (w <= (LINK_SHUTDOWN_WAIT_MS + 1u) * 1000u) R.info.clean_offs++;
            else R.info.unclean_offs++;
        } else {
            R.info.unclean_offs++;
        }
    }
    R.info.powered = false;
    R.info.hello_sent = false;
    R.info.shut_down = false;
    R.info.conn_state = LINK_CS_IDLE;
    R.info.subs = 0;
    R.out_n = 0;
    R.to_mcu.busy = false;
    R.in_n = 0;
    R.have_seq_in = false;
    R.seq_out = 0;
    for (size_t i = 0; i < TIMERS_MAX; i++) R.tm[i].used = false;
}

static void radio_boot(void)
{
    R.gen++;
    R.info.powered = true;
    R.info.power_ups++;
    R.info.hello_sent = false;
    R.info.shut_down = false;
    R.info.conn_state = LINK_CS_IDLE;
    R.info.subs = 0;
    R.have_seq_in = false;
    R.seq_out = 0;
    R.in_n = 0;
    if (!R.cfg.present) return;
    if (R.silent_left) {
        if (R.silent_left != UINT32_MAX) R.silent_left--;
        return;
    }
    timer_add(sim_now_us() + (uint64_t)R.cfg.hello_delay_ms * 1000u, T_HELLO, 0, 0);
}

void radio_pin_changed(board_pin_t p, bool old, bool level)
{
    if (p != PIN_RADIO_EN || old == level) return;
    trace(level ? SIM_RT_RADIO_EN_HIGH : SIM_RT_RADIO_EN_LOW);
    if (level) radio_boot();
    else radio_power_cut();
}

bool radio_uart_on(void) { return R.uart_on; }

void radio_clock_switch(void)
{
    /* the BSP drains TX first; a byte the radio is sending is hit */
    if (R.uart_on && R.to_mcu.busy) R.to_mcu_corrupt_inflight = true;
}

void radio_irq_mask_begin(void) { R.masked_tx_budget = HW_FIFO + 1u; }

void radio_mcu_reset(void)
{
    /* pins already low (without checks): UART off, radio unpowered */
    if (R.uart_on) trace(SIM_RT_UART_OFF);
    R.uart_on = false;
    R.tx_n = R.rx_n = R.hw_n = 0;
    R.to_radio.busy = false;
    if (R.info.powered) {
        trace(SIM_RT_RADIO_EN_LOW);
        R.info.hello_sent = false; /* an MCU reset is not a protocol error */
        radio_power_cut();
    }
    memset(&R.st, 0, sizeof R.st);
}

void radio_reset(void)
{
    memset(&R, 0, sizeof R);
    R.cfg = sim_radio_defaults();
    sim_phone_log_clear();
}

/* ------------------------------------------------------------------------ */
/* HAL                                                                       */

void hal_radio_power(bool on)
{
    sim_service();
    if (on) {
        if (!sim_pin(PIN_RADIO_EN)) sim_radio_pin_write(true);
        if (!R.uart_on) {
            if (!sim_pin(PIN_RADIO_EN))
                sim_viol(SIM_V_RADIO_BACKFEED, "UART0 enabled while RADIO_EN is low");
            R.uart_on = true;
            R.tx_n = R.rx_n = R.hw_n = 0;
            R.to_radio.busy = false;
            trace(SIM_RT_UART_ON);
        }
    } else {
        if (R.uart_on) {
            R.uart_on = false;
            R.tx_n = R.rx_n = R.hw_n = 0;
            R.to_radio.busy = false;
            trace(SIM_RT_UART_OFF);
        }
        if (sim_pin(PIN_RADIO_EN)) sim_radio_pin_write(false);
    }
}

bool hal_radio_powered(void) { return R.uart_on && sim_pin(PIN_RADIO_EN); }

size_t hal_uart_write(const uint8_t *buf, size_t n)
{
    sim_service();
    if (!R.uart_on) {
        sim_viol(SIM_V_UART_RADIO_OFF, "hal_uart_write(%zu bytes) while the radio is off", n);
        return 0;
    }
    if (!buf || n == 0 || n > HAL_UART_TX_RING - R.tx_n) return 0;
    for (size_t i = 0; i < n; i++)
        R.tx[(R.tx_head + R.tx_n + i) % HAL_UART_TX_RING] = buf[i];
    R.tx_n += n;
    R.st.tx_bytes += (uint32_t)n;
    tx_kick();
    return n;
}

size_t hal_uart_read(uint8_t *buf, size_t max)
{
    sim_service();
    size_t n = 0;
    while (n < max && R.rx_n) {
        buf[n++] = R.rx[R.rx_head];
        R.rx_head = (R.rx_head + 1) % HAL_UART_RX_RING;
        R.rx_n--;
    }
    return n;
}

void hal_uart_stats(hal_uart_stats_t *out)
{
    if (out) *out = R.st;
}

/* ------------------------------------------------------------------------ */
/* test API                                                                  */

size_t sim_radio_log_count(void) { return R.log_n < LOG_MAX ? R.log_n : LOG_MAX; }
const sim_link_frame_t *sim_radio_log_get(size_t i)
{
    size_t n = sim_radio_log_count();
    if (i >= n) return NULL;
    size_t first = R.log_n - n;
    return &R.log[(first + i) % LOG_MAX];
}

bool sim_radio_ack_of(uint8_t seq, uint8_t *result)
{
    size_t n = R.acks_n < ACKS_MAX ? R.acks_n : ACKS_MAX;
    for (size_t k = 0; k < n; k++) {
        size_t i = (R.acks_n - 1 - k) % ACKS_MAX;
        if (R.acks[i].seq == seq) {
            if (result) *result = R.acks[i].res;
            return true;
        }
    }
    return false;
}

bool sim_radio_cmd_result(uint8_t cmd, uint8_t *result)
{
    if (!R.have_result[cmd]) return false;
    if (result) *result = R.result[cmd];
    return true;
}

bool sim_phone_connect(uint16_t mtu, uint16_t interval, uint8_t subs)
{
    sim_service();
    if (!radio_alive() || (R.info.conn_state != LINK_CS_ADV_FAST && R.info.conn_state != LINK_CS_ADV_SLOW))
        return false;
    timer_cancel(T_ADV_SLOW);
    R.info.mtu = mtu;
    R.info.interval = interval;
    R.info.subs = subs;
    set_conn(LINK_CS_CONNECTED);
    return true;
}

void sim_phone_subscribe(uint8_t subs)
{
    sim_service();
    if (!radio_alive() || R.info.conn_state != LINK_CS_CONNECTED || R.info.subs == subs) return;
    R.info.subs = subs;
    send_conn_state();
}

void sim_phone_disconnect(void)
{
    sim_service();
    if (!radio_alive() || R.info.conn_state != LINK_CS_CONNECTED) return;
    R.info.subs = 0;
    /* the CH585 resumes fast advertising by itself unless the MCU's last
     * advertising command was ADV_STOP */
    if (R.adv_stopped) set_conn(LINK_CS_IDLE);
    else adv_fast();
}

int sim_phone_command(const uint8_t *payload, size_t n)
{
    sim_service();
    if (!radio_alive() || R.info.conn_state != LINK_CS_CONNECTED) return -1;
    int seq = R.seq_out;
    /* forget older ACKs with this seq (the counter wraps / restarts) */
    for (size_t i = 0; i < ACKS_MAX; i++)
        if (R.acks[i].seq == (uint8_t)seq) R.acks[i].seq = (uint8_t)(seq + 128);
    if (n) R.have_result[payload[0]] = false;
    radio_send(LINK_CMD, payload, n);
    return seq;
}

void sim_radio_send_frame(uint8_t type, const uint8_t *payload, size_t n)
{
    sim_service();
    if (R.info.powered && R.cfg.present) radio_send(type, payload, n);
}

void sim_radio_send_raw(const uint8_t *bytes, size_t n)
{
    sim_service();
    if (R.info.powered) out_push(bytes, n);
}

void sim_radio_log(const char *text)
{
    sim_radio_send_frame(STREAM_LOG, (const uint8_t *)text, strlen(text));
    /* as the CH585: its own lines also go to Log, prefixed, between MCU lines */
    if (R.info.powered && R.cfg.present && subscribed(LINK_SUB_LOG) &&
        (s_phone_log_n == 0 || s_phone_log[s_phone_log_n - 1] == '\n')) {
        phone_log_add("radio: ", 7);
        phone_log_add(text, strlen(text));
        phone_log_add("\n", 1);
    }
}

const char *sim_phone_log_text(size_t *len)
{
    if (len) *len = s_phone_log_n;
    return s_phone_log;
}

void sim_phone_log_clear(void)
{
    s_phone_log_n = 0;
    s_phone_log[0] = 0;
    s_phone_log_lost = 0;
}

void sim_uart_stall_tx(bool stall)
{
    sim_service();
    R.tx_stall = stall;
}

void sim_radio_corrupt_to_mcu(uint32_t n, uint8_t x) { R.corrupt_mcu_n = n; R.corrupt_mcu_x = x; }
void sim_radio_drop_to_mcu(uint32_t n) { R.drop_mcu_n = n; }
void sim_radio_corrupt_to_radio(uint32_t n, uint8_t x) { R.corrupt_radio_n = n; R.corrupt_radio_x = x; }

void sim_radio_self_reset(void)
{
    sim_service();
    if (!R.info.powered) return;
    R.info.hello_sent = false; /* not the MCU's fault */
    radio_power_cut();
    R.info.powered = true;
    R.info.power_ups--; /* radio_boot counts again */
    radio_boot();
}
