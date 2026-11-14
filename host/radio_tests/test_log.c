/* Device log on the Log characteristic (CH585F/APP/rl_log.c): priority gate,
 * line integrity in and out of the notification queue, the radio's own
 * lines between MCU lines. */
#include <stdio.h>
#include <string.h>

#include "rl_log.h"
#include "rl_queue.h"
#include "test.h"

#define K_LOG  4u
#define K_DATA 0u

static rl_queue_t Q;
static rl_log_t L;
static char out[4096]; /* what the phone got on Log */
static size_t out_n;
static unsigned data_out;
static uint8_t seq;

static void reset(void)
{
    rl_queue_init(&Q);
    rl_log_init(&L, K_LOG);
    out_n = 0;
    out[0] = 0;
    data_out = 0;
    seq = 0;
}

static void phone(const uint8_t *p, size_t n)
{
    memcpy(out + out_n, p, n);
    out_n += n;
    out[out_n] = 0;
}

/* The pump of ble_app.c, every entry delivered. */
static void pump(void)
{
    const rl_qent_t *e;
    while ((e = rl_queue_peek(&Q)) != NULL) {
        if ((e->kind & RL_LOG_KIND_MASK) != K_LOG) {
            data_out++;
            rl_queue_pop(&Q);
            continue;
        }
        switch (rl_log_tx(&L, e)) {
        case RL_LOG_TX_SKIP:
            rl_log_tx_skip(&L);
            rl_queue_discard_head(&Q);
            break;
        case RL_LOG_TX_NEWLINE:
            phone((const uint8_t *)"\n", 1);
            rl_log_tx_sent(&L, NULL);
            break;
        case RL_LOG_TX_SEND:
            CHECK(e->len <= 32);
            phone(e->data, e->len);
            rl_log_tx_sent(&L, e);
            rl_queue_pop(&Q);
            break;
        }
    }
}

/* MCU tee: one line in <= 32-byte chunks with consecutive seqs. */
static void mcu_line(const char *s, bool on, uint32_t t)
{
    size_t n = strlen(s);
    for (size_t k = 0; k < n; k += 32)
        rl_log_mcu(&L, &Q, on, seq++, (const uint8_t *)s + k, n - k < 32 ? n - k : 32, t);
    seq += 3; /* other frames (blocks) between lines */
}

static void fill_data(unsigned count)
{
    uint8_t b[25] = {0};
    while (rl_queue_count(&Q) < count)
        rl_queue_push(&Q, K_DATA, b, sizeof b);
}

static const char *LINE_E =
    "[52310] ecg: wait rail=68% min=0 max=4086 avg=1495 fr=3 beats=0 hr=0 mains=4917 lost=4 rs=0\n";
static const char *LINE_P = "[52311] ppg: on led=1344 dac=1290 dk=304 on=1225 hi=1944/1869-2046 "
                            "c=921 agc=0 dcc=1 p=21 pr=73 lost=0\n";

TEST(lines_across_chunks)
{
    reset();
    mcu_line(LINE_E, true, 0);
    CHECK_EQ(rl_queue_count(&Q), 3); /* 93 bytes: 32 + 32 + 29 */
    CHECK_EQ(Q.e[0].kind, K_LOG);
    CHECK_EQ(Q.e[1].kind, K_LOG | 1u << 4);
    CHECK_EQ(Q.e[2].kind, K_LOG | 2u << 4);
    mcu_line(LINE_P, true, 1);
    pump();
    char want[512];
    snprintf(want, sizeof want, "%s%s", LINE_E, LINE_P);
    CHECK(strcmp(out, want) == 0);
    CHECK_EQ(L.in_drops + L.tx_skips + L.own_drops, 0);
    CHECK_EQ(L.tx_chunks, L.in_chunks);
    /* a line longer than 8 chunks keeps going (indices wrap to 1..7) */
    reset();
    char lng[300];
    memset(lng, 'x', 299);
    lng[0] = '[';
    lng[298] = '\n';
    lng[299] = 0;
    mcu_line(lng, true, 0);
    pump();
    CHECK(strcmp(out, lng) == 0);
}

TEST(priority_gate)
{
    reset();
    fill_data(RL_LOG_START_MAX + 1u); /* more than half full: no new line */
    mcu_line(LINE_E, true, 0);
    CHECK_EQ(rl_queue_count(&Q), RL_LOG_START_MAX + 1u);
    CHECK_EQ(L.in_drops, 3);
    CHECK_EQ(Q.dropped, 0); /* log never evicts */
    pump();
    CHECK_EQ(out_n, 0);
    CHECK_EQ(data_out, RL_LOG_START_MAX + 1u);
    /* exactly half full: the line starts, and finishes below 3/4 */
    fill_data(RL_LOG_START_MAX);
    mcu_line(LINE_P, true, 1);
    CHECK_EQ(rl_queue_count(&Q), RL_LOG_START_MAX + 4u);
    pump();
    CHECK(strcmp(out, LINE_P) == 0);
}

TEST(cut_mid_line)
{
    reset();
    fill_data(RL_LOG_START_MAX);
    size_t n = strlen(LINE_P);
    rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_P, 32, 0); /* head in */
    fill_data(RL_LOG_CONT_MAX);                                     /* then data floods */
    for (size_t k = 32; k < n; k += 32)
        rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_P + k, n - k < 32 ? n - k : 32, 0);
    CHECK_EQ(L.in_drops, 3);
    CHECK(!rl_log_mcu_open(&L)); /* the line end was seen */
    rl_queue_init(&Q);           /* (drain the data) */
    rl_queue_push(&Q, K_LOG, (const uint8_t *)LINE_P, 32);
    mcu_line(LINE_E, true, 1);
    pump();
    /* head, ended by '\n', then the next line whole */
    char want[256];
    snprintf(want, sizeof want, "%.32s\n%s", LINE_P, LINE_E);
    CHECK(strcmp(out, want) == 0);
}

TEST(seq_gap_inside_a_line)
{
    reset();
    size_t n = strlen(LINE_E);
    rl_log_mcu(&L, &Q, true, 10, (const uint8_t *)LINE_E, 32, 0);
    /* frame 11 lost on the UART: 12 is a tail */
    rl_log_mcu(&L, &Q, true, 12, (const uint8_t *)LINE_E + 64, n - 64, 0);
    CHECK_EQ(L.in_drops, 1);
    CHECK(!rl_log_mcu_open(&L));
    seq = 20;
    mcu_line(LINE_P, true, 1);
    /* the MCU cut a line itself: its next text starts with '\n' */
    rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_E, 32, 2);
    char marked[200];
    snprintf(marked, sizeof marked, "\n%s", LINE_P);
    seq += 5;
    mcu_line(marked, true, 3);
    pump();
    char want[512];
    snprintf(want, sizeof want, "%.32s\n%s%.32s\n\n%s", LINE_E, LINE_P, LINE_E, LINE_P);
    CHECK(strcmp(out, want) == 0);
}

TEST(own_lines)
{
    reset();
    const char *t = "ble: sent 4711 drop 0 q 3 busy 0 unsub 0 big 0 err 0/0x00";
    rl_log_own(&L, &Q, true, t, strlen(t), 0);
    CHECK_EQ(rl_queue_count(&Q), 3); /* 7 + 57 + 1 = 65 bytes */
    pump();
    char want[1024];
    snprintf(want, sizeof want, "radio: %s\n", t);
    CHECK(strcmp(out, want) == 0);
    /* while an MCU line is open the own line waits for its end */
    reset();
    size_t n = strlen(LINE_E);
    rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_E, 32, 100);
    rl_log_own(&L, &Q, true, "ble: one", 8, 101);
    rl_log_own(&L, &Q, true, "ble: two", 8, 102); /* only one waits */
    CHECK_EQ(L.own_drops, 1);
    CHECK_EQ(rl_queue_count(&Q), 1);
    for (size_t k = 32; k < n; k += 32)
        rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_E + k, n - k < 32 ? n - k : 32, 103);
    rl_log_own(&L, &Q, true, "ble: three", 10, 104);
    pump();
    snprintf(want, sizeof want, "%sradio: ble: one\nradio: ble: three\n", LINE_E);
    CHECK(strcmp(out, want) == 0);
    /* a stuck MCU line is given up after RL_LOG_STALE_MS; its late tail is dropped */
    reset();
    seq = 50;
    rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_E, 32, 1000);
    rl_log_own(&L, &Q, true, "ble: waits", 10, 1001);
    rl_log_poll(&L, &Q, true, 1000 + RL_LOG_STALE_MS - 1);
    CHECK_EQ(rl_queue_count(&Q), 1);
    rl_log_poll(&L, &Q, true, 1000 + RL_LOG_STALE_MS);
    CHECK_EQ(rl_queue_count(&Q), 2);
    rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_E + 32, 32, 1100);     /* late */
    rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_E + 64, n - 64, 1100); /* tail */
    seq += 2;
    mcu_line(LINE_P, true, 1200);
    pump();
    snprintf(want, sizeof want, "%.32s\nradio: ble: waits\n%s", LINE_E, LINE_P);
    CHECK(strcmp(out, want) == 0);
    /* a long own line is cut to RL_LOG_OWN_MAX; refused above half full */
    reset();
    char lng[200];
    memset(lng, 'y', sizeof lng);
    rl_log_own(&L, &Q, true, lng, sizeof lng, 0);
    CHECK_EQ(rl_queue_count(&Q), 4); /* 7 + 120 + 1 = 128 */
    pump();
    CHECK_EQ(out_n, 7u + RL_LOG_OWN_MAX + 1u);
    fill_data(RL_LOG_START_MAX - 2u);
    rl_log_own(&L, &Q, true, "ble: x", 6, 0); /* one chunk: fits */
    rl_log_own(&L, &Q, true, lng, 100, 0);    /* four: not */
    CHECK_EQ(L.own_lines, 2);
    CHECK_EQ(L.own_drops, 1);
    /* not subscribed: nothing at all */
    rl_log_own(&L, &Q, false, "ble: x", 6, 0);
    CHECK_EQ(L.own_lines + L.own_drops, 3);
}

TEST(eviction_never_leaves_a_tail)
{
    /* the line's first chunk evicted by data: its other chunks are skipped */
    reset();
    mcu_line(LINE_E, true, 0);
    fill_data(RL_Q_SLOTS);
    uint8_t b[25] = {0};
    rl_queue_push(&Q, K_DATA, b, sizeof b); /* evicts chunk 0 */
    CHECK_EQ(Q.dropped, 1);
    pump();
    CHECK_EQ(out_n, 0);
    CHECK_EQ(L.tx_skips, 2);
    /* chunk 0 sent, chunk 1 evicted: the head is ended before the next line */
    reset();
    mcu_line(LINE_E, true, 0);
    const rl_qent_t *e = rl_queue_peek(&Q);
    CHECK_EQ(rl_log_tx(&L, e), RL_LOG_TX_SEND);
    phone(e->data, e->len);
    rl_log_tx_sent(&L, e);
    rl_queue_pop(&Q);
    fill_data(RL_Q_SLOTS);
    rl_queue_push(&Q, K_DATA, b, sizeof b); /* evicts chunk 1 */
    pump();
    mcu_line(LINE_P, true, 1);
    pump();
    char want[256];
    snprintf(want, sizeof want, "%.32s\n%s", LINE_E, LINE_P);
    CHECK(strcmp(out, want) == 0);
    /* a notification error on a chunk: the same */
    reset();
    mcu_line(LINE_E, true, 0);
    mcu_line(LINE_P, true, 0);
    rl_log_tx_lost(&L); /* chunk 0 of LINE_E dropped by the pump */
    rl_queue_drop_head(&Q);
    pump();
    CHECK(strcmp(out, LINE_P) == 0);
    /* flushed on disconnect: a new connection starts clean */
    reset();
    mcu_line(LINE_E, true, 0);
    e = rl_queue_peek(&Q);
    rl_log_tx_sent(&L, e);
    rl_queue_flush(&Q);
    rl_log_tx_reset(&L);
    mcu_line(LINE_P, true, 1);
    pump();
    CHECK(strcmp(out, LINE_P) == 0);
}

TEST(subscription_follows_lines)
{
    reset();
    size_t n = strlen(LINE_E);
    /* Log enabled in the middle of a line: only from the next line on */
    rl_log_mcu(&L, &Q, false, seq++, (const uint8_t *)LINE_E, 32, 0);
    for (size_t k = 32; k < n; k += 32)
        rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)LINE_E + k, n - k < 32 ? n - k : 32, 0);
    CHECK_EQ(rl_queue_count(&Q), 0);
    mcu_line(LINE_P, true, 0);
    /* too long for a notification: dropped, with the rest of its line */
    uint8_t big[40];
    memset(big, 'z', sizeof big);
    rl_log_mcu(&L, &Q, true, seq++, big, sizeof big, 0);
    rl_log_mcu(&L, &Q, true, seq++, (const uint8_t *)"tail\n", 5, 0);
    seq += 2;
    mcu_line(LINE_E, true, 0);
    pump();
    char want[512];
    snprintf(want, sizeof want, "%s%s", LINE_P, LINE_E);
    CHECK(strcmp(out, want) == 0);
    CHECK_EQ(L.in_drops, 4); /* LINE_E's two later chunks, the big one, its tail */
}

int main(void)
{
    RUN(lines_across_chunks);
    RUN(priority_gate);
    RUN(cut_mid_line);
    RUN(seq_gap_inside_a_line);
    RUN(own_lines);
    RUN(eviction_never_leaves_a_tail);
    RUN(subscription_follows_lines);
    return TEST_RESULT();
}
