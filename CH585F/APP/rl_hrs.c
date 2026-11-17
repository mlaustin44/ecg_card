/* Heart Rate Measurement (0x2A37) state. Portable C (no WCH headers). */
#include "rl_hrs.h"

#include <string.h>

#include "link_fmt.h"
#include "stream_fmt.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void rl_hrs_init(rl_hrs_t *h)
{
    memset(h, 0, sizeof *h);
}

bool rl_hrs_enabled(const rl_hrs_t *h)
{
    return h->active && h->subscribed;
}

/* Call before a change of active / subscribed: restarts the period and
 * drops the RR backlog when the measurement becomes enabled. */
static void set_enabled(rl_hrs_t *h, bool was, uint32_t now_ms)
{
    if (!was && rl_hrs_enabled(h)) {
        h->sent = false;
        h->ref_ms = now_ms;
        h->n_rr = 0;
    }
}

void rl_hrs_status(rl_hrs_t *h, uint8_t mode, uint8_t meas, uint32_t now_ms)
{
    bool was = rl_hrs_enabled(h);
    bool active = mode != LINK_MODE_IDLE;
    bool contact = active && meas == RL_HRS_MEAS_RUNNING;

    if (active != h->active) {
        /* a mode starts or ends: nothing carries over */
        memset(&h->sel, 0, sizeof h->sel);
        h->bpm = 0;
        h->have_beat = false;
        h->have_prev = false;
        h->n_rr = 0;
        h->active = active;
    }
    if (contact != h->contact) {
        /* gained: the next beat only starts an interval; lost: RR stop */
        h->have_prev = false;
        h->n_rr = 0;
        h->contact = contact;
    }
    set_enabled(h, was, now_ms);
}

void rl_hrs_subscribed(rl_hrs_t *h, bool on, uint32_t now_ms)
{
    bool was = rl_hrs_enabled(h);
    h->subscribed = on;
    set_enabled(h, was, now_ms);
}

bool rl_hrs_rr_1024(uint32_t samples, uint16_t *out)
{
    /* ms = samples * 1000 / HZ; the first test also keeps the products
     * below in range */
    if (samples > (RL_HRS_RR_MAX_MS * RL_HRS_SAMPLE_HZ) / 1000u ||
        samples * 1000u > RL_HRS_RR_MAX_MS * RL_HRS_SAMPLE_HZ ||
        samples * 1000u < RL_HRS_RR_MIN_MS * RL_HRS_SAMPLE_HZ)
        return false;
    /* round(samples * 1024 / HZ) */
    *out = (uint16_t)((samples * 2048u + RL_HRS_SAMPLE_HZ) / (2u * RL_HRS_SAMPLE_HZ));
    return true;
}

static void push_rr(rl_hrs_t *h, uint16_t rr)
{
    if (h->n_rr == RL_HRS_RR_MAX) {
        memmove(&h->rr[0], &h->rr[1], (RL_HRS_RR_MAX - 1u) * sizeof h->rr[0]);
        h->n_rr--;
        h->rr_overflow++;
    }
    h->rr[h->n_rr++] = rr;
}

bool rl_hrs_event(rl_hrs_t *h, const uint8_t *p, size_t n, uint32_t now_ms)
{
    uint8_t kind;
    uint16_t bpm;

    if (!rl_msg_event_bpm(p, n, &kind, &bpm))
        return false;
    if (!h->active)
        return false; /* stale event after the mode ended */
    if (kind == STREAM_EV_R_PEAK && h->contact) {
        uint32_t idx = get32(&p[1]);
        uint16_t rr;
        if (h->have_prev) {
            if (rl_hrs_rr_1024(idx - h->prev_idx, &rr)) {
                if (rl_hrs_enabled(h))
                    push_rr(h, rr);
            } else {
                h->rr_rejected++;
            }
        }
        h->prev_idx = idx;
        h->have_prev = true;
    }
    if (!rl_hr_select(&h->sel, kind, now_ms))
        return false;
    h->bpm = bpm;
    h->have_beat = true;
    h->beat_ms = now_ms;
    return rl_hrs_enabled(h) &&
           (!h->sent || (uint32_t)(now_ms - h->ref_ms) >= RL_HRS_PERIOD_MS);
}

uint32_t rl_hrs_wait_ms(const rl_hrs_t *h, uint32_t now_ms)
{
    uint32_t el;

    if (!rl_hrs_enabled(h))
        return RL_HRS_NEVER;
    el = (uint32_t)(now_ms - h->ref_ms);
    return el >= RL_HRS_PERIOD_MS ? 0u : RL_HRS_PERIOD_MS - el;
}

size_t rl_hrs_take(rl_hrs_t *h, uint32_t now_ms, uint8_t out[RL_HRS_MAX_LEN])
{
    size_t n = 2;
    unsigned i;
    uint8_t flags = RL_HRS_FLAG_SUPPORTED;

    if (h->have_beat && (uint32_t)(now_ms - h->beat_ms) >= RL_HRS_BPM_STALE_MS) {
        h->bpm = 0; /* no beat for 5 s: do not freeze the last value */
        h->have_beat = false;
    }
    if (h->contact)
        flags |= RL_HRS_FLAG_CONTACT;
    if (h->n_rr)
        flags |= RL_HRS_FLAG_RR;
    out[0] = flags;
    out[1] = h->bpm > 255u ? 255u : (uint8_t)h->bpm;
    for (i = 0; i < h->n_rr; i++) {
        out[n++] = (uint8_t)h->rr[i];
        out[n++] = (uint8_t)(h->rr[i] >> 8);
    }
    h->n_rr = 0;
    h->sent = true;
    h->ref_ms = now_ms;
    return n;
}
