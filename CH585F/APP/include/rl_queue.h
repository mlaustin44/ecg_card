/* Bounded notification queue with drop-oldest (portable, no WCH headers).
 *
 * One FIFO for every notification the radio sends (ECG, PPG, Events,
 * Status, Heart Rate, Battery), so they leave in arrival order. 128 slots
 * hold about 1 s of streaming (50 ECG + 50 PPG blocks + events). When it is
 * full the oldest entry is discarded and counted; entries that cannot be
 * delivered (too long for the slot or for the MTU, flushed on disconnect)
 * are counted as dropped too. Task context only (not interrupt safe). */
#ifndef RL_QUEUE_H
#define RL_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RL_Q_SLOTS 128u /* power of two */
#define RL_Q_DATA  32u  /* largest entry: ECG block 25 B, Status 20 B */

typedef struct {
    uint8_t kind; /* caller-defined (which characteristic) */
    uint8_t len;
    uint8_t data[RL_Q_DATA];
} rl_qent_t;

typedef struct {
    rl_qent_t e[RL_Q_SLOTS];
    uint16_t head;  /* index of the oldest entry */
    uint16_t count;
    uint32_t pushed;   /* accepted by rl_queue_push */
    uint32_t sent;     /* removed by rl_queue_pop (delivered) */
    uint32_t dropped;  /* overwritten, too big, flushed or dropped at head */
    uint32_t too_big;  /* subset of dropped: did not fit a slot */
} rl_queue_t;

void rl_queue_init(rl_queue_t *q);

/* Append; if full, the oldest entry is dropped first. Returns false (and
 * counts a drop) only if n > RL_Q_DATA. */
bool rl_queue_push(rl_queue_t *q, uint8_t kind, const uint8_t *p, size_t n);

/* Oldest entry, or NULL if empty. Stays queued until pop or drop_head. */
const rl_qent_t *rl_queue_peek(const rl_queue_t *q);
void rl_queue_pop(rl_queue_t *q);       /* delivered */
void rl_queue_drop_head(rl_queue_t *q); /* not deliverable: counted */
void rl_queue_discard_head(rl_queue_t *q); /* not wanted (unsubscribed): not counted */
/* Drop everything (counted). Returns the number of entries dropped. */
unsigned rl_queue_flush(rl_queue_t *q);
unsigned rl_queue_count(const rl_queue_t *q);
/* dropped, saturated to 16 bits for LINK_CONN_STATE */
uint16_t rl_queue_dropped16(const rl_queue_t *q);

#endif
