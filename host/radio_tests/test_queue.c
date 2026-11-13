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
    RUN(ring);
    return TEST_RESULT();
}
