/* Single-producer / single-consumer byte ring (portable, header only).
 *
 * One side may be an interrupt handler: the producer only writes head, the
 * consumer only writes tail, both are 16-bit (single store on RV32) and
 * volatile. Indices run freely and are masked on access, so the ring holds
 * exactly size bytes. size must be a power of two, at most 32768. */
#ifndef RL_RING_H
#define RL_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint16_t mask;
    volatile uint16_t head; /* written by the producer */
    volatile uint16_t tail; /* written by the consumer */
} rl_ring_t;

static inline void rl_ring_init(rl_ring_t *r, uint8_t *buf, uint16_t size)
{
    r->buf = buf;
    r->mask = (uint16_t)(size - 1u);
    r->head = 0;
    r->tail = 0;
}

static inline uint16_t rl_ring_count(const rl_ring_t *r)
{
    return (uint16_t)(r->head - r->tail);
}

static inline uint16_t rl_ring_space(const rl_ring_t *r)
{
    return (uint16_t)(r->mask + 1u - rl_ring_count(r));
}

/* producer */
static inline bool rl_ring_put(rl_ring_t *r, uint8_t b)
{
    uint16_t h = r->head;
    if ((uint16_t)(h - r->tail) > r->mask)
        return false;
    r->buf[h & r->mask] = b;
    __asm__ volatile("" ::: "memory"); /* data before index */
    r->head = (uint16_t)(h + 1u);
    return true;
}

/* producer: all n bytes or none */
static inline bool rl_ring_write(rl_ring_t *r, const uint8_t *p, size_t n)
{
    uint16_t h = r->head;
    size_t i;
    if (n > rl_ring_space(r))
        return false;
    for (i = 0; i < n; i++)
        r->buf[(uint16_t)(h + i) & r->mask] = p[i];
    __asm__ volatile("" ::: "memory");
    r->head = (uint16_t)(h + n);
    return true;
}

/* consumer */
static inline bool rl_ring_get(rl_ring_t *r, uint8_t *b)
{
    uint16_t t = r->tail;
    if (t == r->head)
        return false;
    *b = r->buf[t & r->mask];
    __asm__ volatile("" ::: "memory"); /* data before index */
    r->tail = (uint16_t)(t + 1u);
    return true;
}

#endif
