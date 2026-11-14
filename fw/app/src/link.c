#include "link.h"

#include <string.h>

#include "hal.h"
#include "settings_store.h"

static uint8_t s_seq;
static uint32_t s_frames, s_drops;
static uint32_t s_est, s_est_ms; /* TX backlog estimate (bytes) and when it was aged */

void link_init(void) { s_seq = 0; s_frames = s_drops = 0; s_est = 0; }
void link_tx_reset(void) { s_seq = 0; s_est = 0; }
uint8_t link_tx_last_seq(void) { return (uint8_t)(s_seq - 1u); }
uint32_t link_tx_frames(void) { return s_frames; }
uint32_t link_tx_drops(void) { return s_drops; }

/* Take off what the line can have sent since the last call. */
static void est_age(void)
{
    uint32_t now = hal_millis(), dt = now - s_est_ms;
    s_est_ms = now;
    if (dt >= HAL_UART_TX_RING / LINK_TX_DRAIN_PER_MS + 1u)
        s_est = 0;
    else
        s_est = s_est > dt * LINK_TX_DRAIN_PER_MS ? s_est - dt * LINK_TX_DRAIN_PER_MS : 0u;
}

uint32_t link_tx_backlog(void)
{
    est_age();
    return s_est;
}

bool link_send(uint8_t type, const uint8_t *payload, size_t len)
{
    static uint8_t wire[COBS_MAX(STREAM_FRAME_MAX) + 1u];
    size_t n = stream_build(type, s_seq, payload, len, wire);
    if (n == 0)
        return false;
    s_seq++;
    est_age();
    if (!hal_radio_powered() || hal_uart_write(wire, n) != n) {
        s_drops++;
        s_est = HAL_UART_TX_RING; /* it is full: assume so until it can have drained */
        return false;
    }
    s_est = s_est + n < HAL_UART_TX_RING ? s_est + (uint32_t)n : HAL_UART_TX_RING;
    s_frames++;
    return true;
}

void link_rx_init(link_rx_t *r) { memset(r, 0, sizeof *r); }

void link_rx_restart(link_rx_t *r)
{
    r->n = 0;
    r->overflow = false;
    r->have_seq = false;
}

static void rx_frame(link_rx_t *r, link_frame_fn fn, void *ctx)
{
    uint8_t f[LINK_RX_MAX_PAYLOAD + 6u];
    size_t len = cobs_decode(r->buf, r->n, f, sizeof f);
    if (len == (size_t)-1 || len < 6u || (size_t)(f[2] | (f[3] << 8)) + 6u != len) {
        r->st.bad_frames++;
        return;
    }
    size_t pl = len - 6u;
    if (crc16_ccitt_false(f, 4u + pl, 0xFFFFu) != (uint16_t)(f[4 + pl] | (f[5 + pl] << 8))) {
        r->st.crc_errors++;
        return;
    }
    if (r->have_seq && f[1] != (uint8_t)(r->seq + 1u))
        r->st.seq_gaps++;
    r->have_seq = true;
    r->seq = f[1];
    r->st.frames++;
    if (fn)
        fn(ctx, f[0], f[1], f + 4, pl);
}

void link_rx_feed(link_rx_t *r, const uint8_t *b, size_t n, link_frame_fn fn, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c != 0u) {
            if (r->n < sizeof r->buf)
                r->buf[r->n++] = c;
            else
                r->overflow = true;
            continue;
        }
        /* delimiter: a run of zeros (line idle low, wake bytes) is no frame */
        if (r->overflow)
            r->st.bad_frames++;
        else if (r->n)
            rx_frame(r, fn, ctx);
        r->n = 0;
        r->overflow = false;
    }
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint8_t link_cmd_parse(const uint8_t *p, size_t n, link_cmd_t *out)
{
    memset(out, 0, sizeof *out);
    if (n == 0 || n > LINK_CMD_MAX_LEN)
        return LINK_RES_BAD_ARG;
    out->cmd = p[0];
    switch (p[0]) {
    case LINK_CMD_START:
        if (n != 2u || p[1] < 1u || p[1] > 3u)
            return LINK_RES_BAD_ARG;
        out->mode = p[1];
        return LINK_RES_OK;
    case LINK_CMD_STOP:
    case LINK_CMD_GET_STATUS:
        return n == 1u ? LINK_RES_OK : LINK_RES_BAD_ARG;
    case LINK_CMD_SET: {
        settings_t s;
        settings_defaults(&s);
        if (n != 3u || !settings_set(&s, p[1], p[2]))
            return LINK_RES_BAD_ARG;
        out->item = p[1];
        out->value = p[2];
        return LINK_RES_OK;
    }
    case LINK_CMD_SET_TIME: {
        if (n != 7u)
            return LINK_RES_BAD_ARG;
        int16_t off = (int16_t)(uint16_t)(p[5] | (p[6] << 8));
        uint32_t t = rd32(p + 1);
        if (t < LINK_TIME_MIN || off < LINK_UTC_OFF_MIN || off > LINK_UTC_OFF_MAX)
            return LINK_RES_BAD_ARG;
        out->unix_s = t;
        out->utc_offset_min = off;
        return LINK_RES_OK;
    }
    default:
        return LINK_RES_UNKNOWN;
    }
}
