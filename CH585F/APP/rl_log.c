/* Device log on the Log characteristic (see rl_log.h). Portable C (no WCH
 * headers). */
#include "rl_log.h"

#include <string.h>

#define PREFIX_LEN (sizeof RL_LOG_PREFIX - 1u)

static uint8_t next_idx(uint8_t idx)
{
    /* 1..7 for continuations: 0 always means "a line starts" */
    return idx >= 7u ? 1u : (uint8_t)(idx + 1u);
}

void rl_log_init(rl_log_t *l, uint8_t kind)
{
    memset(l, 0, sizeof *l);
    l->kind = (uint8_t)(kind & RL_LOG_KIND_MASK);
    l->tx_next = 0xFFu;
}

bool rl_log_mcu_open(const rl_log_t *l)
{
    return l->in_idx != 0u || l->in_dropping;
}

/* "radio: " + text + '\n' in RL_Q_DATA chunks, all or nothing. */
static void own_push(rl_log_t *l, rl_queue_t *q, const char *text, size_t n)
{
    size_t total = PREFIX_LEN + n + 1u;
    size_t chunks = (total + RL_Q_DATA - 1u) / RL_Q_DATA;
    size_t k = 0;
    uint8_t idx = 0;

    if (rl_queue_count(q) + chunks > RL_LOG_START_MAX) {
        l->own_drops++;
        return;
    }
    while (k < total) {
        uint8_t c[RL_Q_DATA];
        size_t m = 0;
        while (m < RL_Q_DATA && k < total) {
            c[m++] = (uint8_t)(k < PREFIX_LEN       ? RL_LOG_PREFIX[k]
                               : k < PREFIX_LEN + n ? (uint8_t)text[k - PREFIX_LEN]
                                                    : '\n');
            k++;
        }
        rl_queue_push(q, (uint8_t)(l->kind | (idx << RL_LOG_IDX_SHIFT)), c, m);
        idx = next_idx(idx);
    }
    l->own_lines++;
}

static void own_flush(rl_log_t *l, rl_queue_t *q, bool on)
{
    if (!l->own_waiting)
        return;
    l->own_waiting = false;
    if (on)
        own_push(l, q, l->own, l->own_n);
}

void rl_log_mcu(rl_log_t *l, rl_queue_t *q, bool on, uint8_t seq, const uint8_t *p, size_t n,
                uint32_t now_ms)
{
    bool ends = n != 0u && p[n - 1u] == '\n';
    bool next_seq = l->in_have_seq && seq == (uint8_t)(l->in_seq + 1u);
    bool start, ok = false;

    if (l->in_abandoned) {
        /* the given-up line's late tail comes with the next seq */
        l->in_abandoned = false;
        if (next_seq)
            l->in_dropping = true;
    }
    if (!rl_log_mcu_open(l)) {
        start = true;
    } else if (next_seq) {
        start = false; /* continuation, or more of a line being dropped */
    } else if (n != 0u && p[0] == '\n') {
        start = true; /* the MCU cut its last line itself and says so */
        l->in_dropping = false;
    } else {
        start = false; /* frames lost inside a line: this is a tail */
        l->in_dropping = true;
    }
    l->in_have_seq = true;
    l->in_seq = seq;
    l->in_ms = now_ms;

    if (!l->in_dropping && on && n != 0u && n <= RL_Q_DATA) {
        unsigned c = rl_queue_count(q);
        uint8_t idx = start ? 0u : l->in_idx;
        ok = start ? c <= RL_LOG_START_MAX : c < RL_LOG_CONT_MAX;
        if (ok) {
            rl_queue_push(q, (uint8_t)(l->kind | (idx << RL_LOG_IDX_SHIFT)), p, n);
            l->in_chunks++;
            l->in_idx = ends ? 0u : next_idx(idx);
        }
    }
    if (!ok) {
        if (on)
            l->in_drops++;
        l->in_idx = 0;
        l->in_dropping = !ends;
    }
    if (!rl_log_mcu_open(l))
        own_flush(l, q, on);
}

void rl_log_own(rl_log_t *l, rl_queue_t *q, bool on, const char *text, size_t n, uint32_t now_ms)
{
    if (!on)
        return;
    if (n > RL_LOG_OWN_MAX)
        n = RL_LOG_OWN_MAX;
    rl_log_poll(l, q, on, now_ms);
    if (rl_log_mcu_open(l)) {
        if (l->own_waiting) {
            l->own_drops++; /* one line waits; this one is lost */
            return;
        }
        memcpy(l->own, text, n);
        l->own_n = (uint8_t)n;
        l->own_waiting = true;
        return;
    }
    own_flush(l, q, on); /* keep the order */
    own_push(l, q, text, n);
}

void rl_log_poll(rl_log_t *l, rl_queue_t *q, bool on, uint32_t now_ms)
{
    if (rl_log_mcu_open(l) && (uint32_t)(now_ms - l->in_ms) >= RL_LOG_STALE_MS) {
        /* the rest of that MCU line is not coming: give it up */
        l->in_idx = 0;
        l->in_dropping = false;
        l->in_abandoned = true;
    }
    if (!rl_log_mcu_open(l))
        own_flush(l, q, on);
}

rl_log_tx_t rl_log_tx(const rl_log_t *l, const rl_qent_t *e)
{
    uint8_t idx = (uint8_t)(e->kind >> RL_LOG_IDX_SHIFT);
    if (idx == 0u)
        return l->tx_open ? RL_LOG_TX_NEWLINE : RL_LOG_TX_SEND;
    return idx == l->tx_next ? RL_LOG_TX_SEND : RL_LOG_TX_SKIP;
}

void rl_log_tx_sent(rl_log_t *l, const rl_qent_t *e)
{
    bool ends;
    if (e == NULL) {
        l->tx_open = false;
        l->tx_next = 0xFFu;
        return;
    }
    ends = e->len != 0u && e->data[e->len - 1u] == '\n';
    l->tx_chunks++;
    l->tx_open = !ends;
    l->tx_next = ends ? 0xFFu : next_idx((uint8_t)(e->kind >> RL_LOG_IDX_SHIFT));
}

void rl_log_tx_skip(rl_log_t *l)
{
    l->tx_skips++;
    l->tx_next = 0xFFu; /* the rest of this line too */
}

void rl_log_tx_lost(rl_log_t *l)
{
    l->tx_next = 0xFFu;
}

void rl_log_tx_reset(rl_log_t *l)
{
    l->tx_open = false;
    l->tx_next = 0xFFu;
}
