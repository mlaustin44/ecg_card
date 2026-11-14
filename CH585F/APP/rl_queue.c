/* Bounded notification queue with drop-oldest. Portable C (no WCH headers). */
#include "rl_queue.h"

#include <string.h>

#define RL_Q_MASK (RL_Q_SLOTS - 1u)

void rl_queue_init(rl_queue_t *q)
{
    memset(q, 0, sizeof *q);
}

static void advance_head(rl_queue_t *q)
{
    q->head = (uint16_t)((q->head + 1u) & RL_Q_MASK);
    q->count--;
}

bool rl_queue_push(rl_queue_t *q, uint8_t kind, const uint8_t *p, size_t n)
{
    rl_qent_t *e;

    if (n > RL_Q_DATA) {
        q->too_big++;
        q->dropped++;
        return false;
    }
    if (q->count == RL_Q_SLOTS) {
        advance_head(q);
        q->dropped++;
    }
    e = &q->e[(q->head + q->count) & RL_Q_MASK];
    e->kind = kind;
    e->len = (uint8_t)n;
    if (n)
        memcpy(e->data, p, n);
    q->count++;
    q->pushed++;
    return true;
}

const rl_qent_t *rl_queue_peek(const rl_queue_t *q)
{
    return q->count ? &q->e[q->head] : NULL;
}

void rl_queue_pop(rl_queue_t *q)
{
    if (q->count) {
        advance_head(q);
        q->sent++;
    }
}

void rl_queue_drop_head(rl_queue_t *q)
{
    if (q->count) {
        advance_head(q);
        q->dropped++;
    }
}

void rl_queue_discard_head(rl_queue_t *q)
{
    if (q->count)
        advance_head(q);
}

size_t rl_queue_gather(const rl_queue_t *q, uint8_t *buf, size_t cap,
                       unsigned max_entries, unsigned *entries)
{
    const rl_qent_t *h;
    size_t off = 0;
    unsigned n = 0, i;

    *entries = 0;
    if (!q->count)
        return 0;
    h = &q->e[q->head];
    for (i = 0; i < q->count && n < max_entries; i++) {
        const rl_qent_t *e = &q->e[(q->head + i) & RL_Q_MASK];
        if (e->kind != h->kind || e->len != h->len)
            continue;
        if (off + e->len > cap)
            break;
        memcpy(buf + off, e->data, e->len);
        off += e->len;
        n++;
    }
    *entries = n;
    return off;
}

void rl_queue_pop_gathered(rl_queue_t *q, unsigned entries)
{
    uint8_t kind, len;
    unsigned i, w = 0, taken = 0, count = q->count;

    if (!count || !entries)
        return;
    kind = q->e[q->head].kind;
    len = q->e[q->head].len;
    for (i = 0; i < count; i++) {
        rl_qent_t *e = &q->e[(q->head + i) & RL_Q_MASK];
        if (taken < entries && e->kind == kind && e->len == len) {
            taken++;
            continue;
        }
        if (w != i)
            q->e[(q->head + w) & RL_Q_MASK] = *e;
        w++;
    }
    /* the survivors now sit at head .. head+w-1 */
    q->count = (uint16_t)w;
    q->sent += taken;
}

unsigned rl_queue_flush(rl_queue_t *q)
{
    unsigned n = q->count;
    q->dropped += n;
    q->count = 0;
    q->head = 0;
    return n;
}

unsigned rl_queue_count(const rl_queue_t *q)
{
    return q->count;
}

uint16_t rl_queue_dropped16(const rl_queue_t *q)
{
    return q->dropped > 0xFFFFu ? 0xFFFFu : (uint16_t)q->dropped;
}
