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
