/* Notification queue (drop-oldest, counters) and the SPSC byte ring. */
#include <string.h>

#include "rl_queue.h"
#include "rl_ring.h"
#include "test.h"

static rl_queue_t Q;

TEST(fifo_order)
{
    uint8_t b[RL_Q_DATA];
    rl_queue_init(&Q);
    CHECK(rl_queue_peek(&Q) == NULL);
    for (unsigned i = 0; i < 5; i++) {
        memset(b, (int)i, sizeof b);
        CHECK(rl_queue_push(&Q, (uint8_t)(i % 3), b, 10 + i));
    }
    CHECK_EQ(rl_queue_count(&Q), 5);
    for (unsigned i = 0; i < 5; i++) {
        const rl_qent_t *e = rl_queue_peek(&Q);
        CHECK(e != NULL);
        CHECK_EQ(e->kind, i % 3);
        CHECK_EQ(e->len, 10 + i);
        CHECK_EQ(e->data[0], i);
        rl_queue_pop(&Q);
    }
    CHECK(rl_queue_peek(&Q) == NULL);
    CHECK_EQ(Q.pushed, 5);
    CHECK_EQ(Q.sent, 5);
    CHECK_EQ(Q.dropped, 0);
    rl_queue_pop(&Q); /* pop on empty is harmless */
    CHECK_EQ(Q.sent, 5);
}

TEST(drop_oldest_when_full)
{
    uint8_t b[4];
    rl_queue_init(&Q);
    /* about 1 s of streaming: 50 ECG + 50 PPG + events must fit */
    CHECK(RL_Q_SLOTS >= 110);
    for (unsigned i = 0; i < RL_Q_SLOTS + 10; i++) {
        b[0] = (uint8_t)i;
        b[1] = (uint8_t)(i >> 8);
        rl_queue_push(&Q, 0, b, 2);
    }
    CHECK_EQ(rl_queue_count(&Q), RL_Q_SLOTS);
    CHECK_EQ(Q.dropped, 10);
    CHECK_EQ(rl_queue_dropped16(&Q), 10);
    const rl_qent_t *e = rl_queue_peek(&Q);
    CHECK_EQ(e->data[0] | (e->data[1] << 8), 10); /* oldest ten are gone */
    /* newest is last */
    unsigned last = 0;
    while ((e = rl_queue_peek(&Q)) != NULL) {
        last = (unsigned)(e->data[0] | (e->data[1] << 8));
        rl_queue_pop(&Q);
    }
    CHECK_EQ(last, RL_Q_SLOTS + 9);
}

TEST(too_big_and_payload_limits)
{
    uint8_t b[RL_Q_DATA + 1];
    memset(b, 7, sizeof b);
    rl_queue_init(&Q);
    CHECK(rl_queue_push(&Q, 1, b, 25));       /* ECG block */
    CHECK(rl_queue_push(&Q, 1, b, 20));       /* Status */
    CHECK(rl_queue_push(&Q, 1, b, RL_Q_DATA));
    CHECK(rl_queue_push(&Q, 1, NULL, 0));
    CHECK(!rl_queue_push(&Q, 1, b, RL_Q_DATA + 1));
    CHECK_EQ(Q.too_big, 1);
    CHECK_EQ(Q.dropped, 1);
    CHECK_EQ(rl_queue_count(&Q), 4);
}

TEST(drop_discard_flush)
{
    uint8_t b[2] = {1, 2};
    rl_queue_init(&Q);
    for (int i = 0; i < 6; i++)
        rl_queue_push(&Q, 0, b, 2);
    rl_queue_drop_head(&Q);    /* undeliverable: counted */
    rl_queue_discard_head(&Q); /* unsubscribed: not counted */
    CHECK_EQ(Q.dropped, 1);
    CHECK_EQ(rl_queue_count(&Q), 4);
    CHECK_EQ(rl_queue_flush(&Q), 4); /* disconnect */
    CHECK_EQ(Q.dropped, 5);
    CHECK_EQ(rl_queue_count(&Q), 0);
    CHECK(rl_queue_peek(&Q) == NULL);
    rl_queue_drop_head(&Q);
    CHECK_EQ(Q.dropped, 5);
    /* usable again after the flush, wrapping the index */
    for (unsigned i = 0; i < 3 * RL_Q_SLOTS; i++) {
        rl_queue_push(&Q, 0, b, 2);
        rl_queue_pop(&Q);
    }
    CHECK_EQ(rl_queue_count(&Q), 0);
    Q.dropped = 70000;
    CHECK_EQ(rl_queue_dropped16(&Q), 0xFFFF);
}

/* interleaved ECG (kind 0, 25 B) and PPG (kind 1, 15 B) blocks plus an event */
static void fill_mixed(unsigned pairs)
{
    uint8_t b[25];
    rl_queue_init(&Q);
    for (unsigned i = 0; i < pairs; i++) {
        memset(b, (int)(0x10 + i), sizeof b);
        rl_queue_push(&Q, 0, b, 25);
        memset(b, (int)(0x80 + i), sizeof b);
        rl_queue_push(&Q, 1, b, 15);
        if (i == 1) {
            memset(b, 0xEE, sizeof b);
            rl_queue_push(&Q, 2, b, 7);
        }
    }
}

TEST(gather_batches_one_stream_in_order)
{
    uint8_t buf[244];
    unsigned n;
    fill_mixed(6); /* 13 entries */
    /* MTU 247: 8 blocks allowed, only 6 ECG queued */
    CHECK_EQ(rl_queue_gather(&Q, buf, sizeof buf, 8, &n), 150);
    CHECK_EQ(n, 6);
    for (unsigned i = 0; i < 6; i++) {
        CHECK_EQ(buf[i * 25], 0x10 + i);
        CHECK_EQ(buf[i * 25 + 24], 0x10 + i);
    }
    CHECK_EQ(rl_queue_count(&Q), 13); /* gather alone changes nothing */
    rl_queue_pop_gathered(&Q, n);
    CHECK_EQ(rl_queue_count(&Q), 7);
    CHECK_EQ(Q.sent, 6);
    CHECK_EQ(Q.dropped, 0);
    /* what is left keeps its order: PPG 0, PPG 1, event, PPG 2.. */
    CHECK_EQ(rl_queue_gather(&Q, buf, sizeof buf, 8, &n), 90);
    CHECK_EQ(n, 6);
    for (unsigned i = 0; i < 6; i++)
        CHECK_EQ(buf[i * 15], 0x80 + i);
    rl_queue_pop_gathered(&Q, n);
    CHECK_EQ(rl_queue_count(&Q), 1);
    CHECK_EQ(rl_queue_peek(&Q)->kind, 2);
    CHECK_EQ(rl_queue_peek(&Q)->len, 7);
    CHECK_EQ(rl_queue_peek(&Q)->data[0], 0xEE);
}

TEST(gather_respects_capacity_and_entry_limit)
{
    uint8_t buf[244];
    unsigned n;
    fill_mixed(12);
    CHECK_EQ(rl_queue_gather(&Q, buf, sizeof buf, 8, &n), 200); /* entry limit */
    CHECK_EQ(n, 8);
    CHECK_EQ(rl_queue_gather(&Q, buf, 60, 8, &n), 50);          /* small MTU */
    CHECK_EQ(n, 2);
    CHECK_EQ(rl_queue_gather(&Q, buf, 20, 8, &n), 0);           /* head does not fit */
    CHECK_EQ(n, 0);
    /* partial pop: the next gather starts at the first block not taken */
    rl_queue_pop_gathered(&Q, 2);
    CHECK_EQ(rl_queue_peek(&Q)->kind, 1);  /* PPG 0 is now the head */
    CHECK_EQ(rl_queue_gather(&Q, buf, sizeof buf, 1, &n), 15);
    rl_queue_pop_gathered(&Q, 1);
    CHECK_EQ(rl_queue_peek(&Q)->kind, 1);  /* PPG 1 (ECG 0, 1 already sent) */
    CHECK_EQ(rl_queue_peek(&Q)->data[0], 0x81);
    rl_queue_pop_gathered(&Q, 1);
    CHECK_EQ(rl_queue_peek(&Q)->kind, 2);
    rl_queue_pop(&Q);
    CHECK_EQ(rl_queue_peek(&Q)->kind, 0);
    CHECK_EQ(rl_queue_peek(&Q)->data[0], 0x12);
    rl_queue_init(&Q);
    CHECK_EQ(rl_queue_gather(&Q, buf, sizeof buf, 8, &n), 0);   /* empty */
    rl_queue_pop_gathered(&Q, 3);
    CHECK_EQ(rl_queue_count(&Q), 0);
}

TEST(gather_across_wraparound_and_push_after)
{
    uint8_t buf[244], b[25];
    unsigned n;
    rl_queue_init(&Q);
    /* move the head near the end of the storage */
    for (unsigned i = 0; i < RL_Q_SLOTS - 3; i++) {
        rl_queue_push(&Q, 3, b, 1);
        rl_queue_discard_head(&Q);
    }
    for (unsigned i = 0; i < 10; i++) {
        memset(b, (int)i, sizeof b);
        rl_queue_push(&Q, (uint8_t)(i & 1), b, 25);
    }
    CHECK_EQ(rl_queue_gather(&Q, buf, sizeof buf, 8, &n), 125);
    CHECK_EQ(n, 5);
    for (unsigned i = 0; i < 5; i++)
        CHECK_EQ(buf[i * 25], 2 * i);
    rl_queue_pop_gathered(&Q, n);
    memset(b, 99, sizeof b);
    rl_queue_push(&Q, 1, b, 25);
    CHECK_EQ(rl_queue_gather(&Q, buf, sizeof buf, 8, &n), 150);
    CHECK_EQ(n, 6);
    for (unsigned i = 0; i < 5; i++)
        CHECK_EQ(buf[i * 25], 2 * i + 1);
    CHECK_EQ(buf[125], 99);
}

TEST(ring)
{
    static uint8_t buf[16];
    rl_ring_t r;
    uint8_t b = 0;
    rl_ring_init(&r, buf, sizeof buf);
    CHECK_EQ(rl_ring_count(&r), 0);
    CHECK_EQ(rl_ring_space(&r), 16);
    CHECK(!rl_ring_get(&r, &b));
    for (int i = 0; i < 16; i++)
        CHECK(rl_ring_put(&r, (uint8_t)i));
    CHECK(!rl_ring_put(&r, 99)); /* full */
    CHECK_EQ(rl_ring_count(&r), 16);
    CHECK(rl_ring_get(&r, &b) && b == 0);
    const uint8_t blk[3] = {7, 8, 9};
    CHECK(!rl_ring_write(&r, blk, 3)); /* all or nothing: only 1 free */
    CHECK_EQ(rl_ring_count(&r), 15);
    for (int i = 1; i < 16; i++)
        CHECK(rl_ring_get(&r, &b) && b == i);
    /* free-running indices across the 16-bit wrap */
    int ok = 1;
    for (unsigned i = 0; i < 70000; i++) {
        uint8_t v;
        ok &= rl_ring_write(&r, blk, 3);
        ok &= rl_ring_get(&r, &v) && v == 7;
        ok &= rl_ring_get(&r, &v) && v == 8;
        ok &= rl_ring_get(&r, &v) && v == 9;
    }
    CHECK(ok);
    CHECK_EQ(rl_ring_count(&r), 0);
}

int main(void)
{
    RUN(fifo_order);
    RUN(drop_oldest_when_full);
    RUN(too_big_and_payload_limits);
    RUN(drop_discard_flush);
    RUN(gather_batches_one_stream_in_order);
    RUN(gather_respects_capacity_and_entry_limit);
    RUN(gather_across_wraparound_and_push_after);
    RUN(ring);
    return TEST_RESULT();
}
