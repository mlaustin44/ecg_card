/* link_fmt.h payload builders and parsers. Portable C (no WCH headers). */
#include "rl_msg.h"

#include <string.h>

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

size_t rl_msg_hello(const rl_hello_t *h, uint8_t out[LINK_HELLO_LEN])
{
    out[0] = (uint8_t)LINK_PROTO_VERSION;
    out[1] = h->fw_major;
    out[2] = h->fw_minor;
    out[3] = h->fw_patch;
    memcpy(&out[4], h->addr, 6);
    out[10] = h->reset_reason;
    out[11] = 0u;
    return LINK_HELLO_LEN;
}

size_t rl_msg_conn_state(const rl_conn_state_t *s, uint8_t out[LINK_CONN_STATE_LEN])
{
    uint8_t *p = out;
    *p++ = s->state;
    p = put16(p, s->mtu);
    p = put16(p, s->interval);
    *p++ = s->subscribed;
    p = put16(p, s->dropped);
    *p++ = s->phy;
    *p = 0u;
    return LINK_CONN_STATE_LEN;
}

size_t rl_msg_ack(uint8_t seq, uint8_t result, uint8_t out[LINK_ACK_LEN])
{
    out[0] = seq;
    out[1] = result;
    return LINK_ACK_LEN;
}

bool rl_msg_cmd_len_ok(size_t n)
{
    return n >= 1u && n <= LINK_CMD_MAX_LEN;
}

bool rl_msg_parse_ack(const uint8_t *p, size_t n, uint8_t *seq, uint8_t *result)
{
    if (n != LINK_ACK_LEN)
        return false;
    *seq = p[0];
    *result = p[1];
    return true;
}

bool rl_msg_parse_status(const uint8_t *p, size_t n, rl_status_t *s)
{
    if (n < LINK_STATUS_LEN)
        return false;
    s->proto = p[0];
    s->mode = p[1];
    s->meas = p[2];
    s->batt_pct = p[3];
    s->flags = p[4];
    s->gain = p[5];
    s->notch = p[6];
    s->rld = p[7];
    s->led_limit = p[8];
    s->sweep = p[9];
    s->notch_hz = p[10];
    s->hr_bpm = p[11];
    s->batt_mv = get16(&p[12]);
    s->fw_major = p[14];
    s->fw_minor = p[15];
    s->fw_patch = p[16];
    /* p[17] reserved */
    s->uptime_min = get16(&p[18]);
    memcpy(s->raw, p, LINK_STATUS_LEN);
    return true;
}

void rl_msg_status_default(uint8_t out[LINK_STATUS_LEN])
{
    memset(out, 0, LINK_STATUS_LEN);
    out[0] = (uint8_t)LINK_PROTO_VERSION;
    out[2] = 0xFFu; /* meas: no mode active */
    out[3] = 0xFFu; /* batt_pct: unknown */
}

uint8_t rl_msg_battery_level(uint8_t batt_pct)
{
    if (batt_pct == 0xFFu)
        return 0u;
    return batt_pct > 100u ? 100u : batt_pct;
}

uint8_t rl_msg_parse_radio_ctrl(const uint8_t *p, size_t n, rl_radio_ctrl_t *rc)
{
    memset(rc, 0, sizeof *rc);
    if (n < 1u)
        return LINK_RES_BAD_ARG;
    rc->op = p[0];
    switch (p[0]) {
    case LINK_RC_ADV_START:
    case LINK_RC_ADV_STOP:
    case LINK_RC_DISCONNECT:
    case LINK_RC_SHUTDOWN:
        return n == 1u ? LINK_RES_OK : LINK_RES_BAD_ARG;
    case LINK_RC_SET_INFO:
        if (n < 2u)
            return LINK_RES_BAD_ARG;
        rc->info_kind = p[1];
        if (p[1] != LINK_INFO_MCU_FW)
            return LINK_RES_UNKNOWN;
        rc->text = &p[2];
        rc->text_len = n - 2u > RL_INFO_MAX ? RL_INFO_MAX : n - 2u;
        return LINK_RES_OK;
    default:
        return LINK_RES_UNKNOWN;
    }
}

bool rl_msg_event_bpm(const uint8_t *p, size_t n, uint8_t *kind, uint16_t *bpm)
{
    if (n < 1u)
        return false;
    if (p[0] == STREAM_EV_R_PEAK && n >= 7u) {
        *kind = p[0];
        *bpm = get16(&p[5]);
        return true;
    }
    if (p[0] == STREAM_EV_PULSE && n >= 11u) {
        *kind = p[0];
        *bpm = get16(&p[5]);
        return true;
    }
    return false;
}

bool rl_hr_select(rl_hr_sel_t *s, uint8_t kind, uint32_t now_ms)
{
    if (kind == STREAM_EV_R_PEAK) {
        s->have_rpeak = true;
        s->last_rpeak_ms = now_ms;
        return true;
    }
    if (kind == STREAM_EV_PULSE)
        return !s->have_rpeak || (uint32_t)(now_ms - s->last_rpeak_ms) > RL_HR_ECG_HOLD_MS;
    return false;
}

void rl_ack_init(rl_acktrack_t *a)
{
    memset(a, 0, sizeof *a);
}

void rl_ack_sent(rl_acktrack_t *a, uint8_t seq, uint32_t now_ms)
{
    unsigned i, oldest = 0;
    for (i = 0; i < RL_ACK_SLOTS; i++) {
        if (!a->used[i])
            break;
        if ((uint32_t)(now_ms - a->t_ms[i]) > (uint32_t)(now_ms - a->t_ms[oldest]))
            oldest = i;
    }
    if (i == RL_ACK_SLOTS) {
        i = oldest;
        a->overwritten++;
        a->timeouts++;
    }
    a->used[i] = true;
    a->seq[i] = seq;
    a->t_ms[i] = now_ms;
}

bool rl_ack_received(rl_acktrack_t *a, uint8_t seq)
{
    unsigned i;
    for (i = 0; i < RL_ACK_SLOTS; i++) {
        if (a->used[i] && a->seq[i] == seq) {
            a->used[i] = false;
            a->acked++;
            return true;
        }
    }
    a->unmatched++;
    return false;
}

unsigned rl_ack_poll(rl_acktrack_t *a, uint32_t now_ms)
{
    unsigned i, n = 0;
    for (i = 0; i < RL_ACK_SLOTS; i++) {
        if (a->used[i] && (uint32_t)(now_ms - a->t_ms[i]) > RL_ACK_TIMEOUT_MS) {
            a->used[i] = false;
            a->timeouts++;
            n++;
        }
    }
    return n;
}
