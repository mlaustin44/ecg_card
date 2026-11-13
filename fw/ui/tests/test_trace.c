/* Trace widgets: envelope, sweep timing, erase bar, autoscale. */
#include <string.h>

#include "gfx.h"
#include "test.h"
#include "trace.h"

static void push_const(ui_trace_t t, int16_t v, uint32_t n)
{
    int16_t b[16];
    for (int i = 0; i < 16; i++)
        b[i] = v;
    while (n) {
        uint16_t k = n > 16 ? 16 : (uint16_t)n;
        ui_trace_push(t, b, k);
        n -= k;
    }
}

/* Alternating +a / -a: peak-to-peak 2a, mean 0. */
static void push_alt(ui_trace_t t, int16_t a, uint32_t n)
{
    static uint32_t phase;
    for (uint32_t i = 0; i < n; i++) {
        int16_t v = (phase++ & 1) ? a : (int16_t)-a;
        ui_trace_push(t, &v, 1);
    }
}

static bool col_empty(const trace_state_t *s, int c) { return s->lo[c] > s->hi[c]; }

static uint8_t fb[OLED_FB_BYTES];
static int px(int x, int y) { return (fb[(y >> 3) * OLED_W + x] >> (y & 7)) & 1; }

TEST(spike_one_column)
{
    ui_trace_reset(3000);
    const trace_state_t *s = trace_state(UI_TRACE_ECG);
    const uint32_t n = 1500, at = 600;
    push_const(UI_TRACE_ECG, 0, at);
    int16_t spike = 1000; /* 1 mV */
    ui_trace_push(UI_TRACE_ECG, &spike, 1);
    push_const(UI_TRACE_ECG, 0, 300);
    int col = (int)(at * TRACE_W / n);
    CHECK_EQ(col, 51);
    uint8_t v0 = s->lo[10];
    CHECK_EQ(s->hi[10], v0);
    int raised = 0;
    for (int c = 0; c < TRACE_W; c++)
        if (!col_empty(s, c) && s->hi[c] > v0) {
            raised++;
            CHECK_EQ(c, col);
        }
    CHECK_EQ(raised, 1);
    /* 1 mV at 2 mV per trace height = half of the 0..255 scale */
    CHECK_NEAR(s->hi[col] - v0, 128, 1);
    CHECK_EQ(s->lo[col], v0);

    /* on screen: 48-px trace, 46 rows of travel -> 23 rows above baseline,
     * drawn by the 2-px pen in columns col and col+1 only */
    gfx_begin(fb);
    trace_draw(UI_TRACE_ECG, 16, 48);
    int base = 16 + 46 - (int)((v0 * 46u * 257u + 32768u) >> 16);
    CHECK(px(0, base) && px(0, base + 1));
    for (int x = 0; x < OLED_W; x++) {
        int top = -1;
        for (int y = 16; y < OLED_H && top < 0; y++)
            if (px(x, y))
                top = y;
        if (x == col || x == col + 1)
            CHECK_NEAR(base - top, 23, 1);
        else if (top >= 0)
            CHECK(top >= base);
    }
    CHECK_EQ(trace_bar_uv(), 1000);
    CHECK_EQ(trace_bar_px(48), 24);
}

TEST(sweep_wraps_exactly)
{
    static const uint16_t sweeps[] = {1500, 3000, 6000};
    for (int si = 0; si < 3; si++)
        for (int t = 0; t < UI_TRACE_COUNT; t++) {
            uint32_t fs = t == UI_TRACE_ECG ? ECG_HZ : PPG_HZ;
            uint32_t n = fs * sweeps[si] / 1000u;
            ui_trace_reset(sweeps[si]);
            const trace_state_t *s = trace_state((ui_trace_t)t);
            CHECK_EQ(s->n_sweep, n);
            push_const((ui_trace_t)t, 5, n / 2);
            CHECK_EQ(s->pen, TRACE_W / 2);
            push_const((ui_trace_t)t, 5, n - 1 - n / 2);
            CHECK_EQ(s->wraps, 0);
            CHECK_EQ(s->pen, TRACE_W - 1);
            push_const((ui_trace_t)t, 5, 1);
            CHECK_EQ(s->wraps, 1);
            CHECK_EQ(s->pen, 0);
            push_const((ui_trace_t)t, 5, n);
            CHECK_EQ(s->wraps, 2);
            CHECK_EQ(s->pen, 0);
            /* the other trace did not move */
            CHECK_EQ(trace_state((ui_trace_t)(1 - t))->pen, 0);
            CHECK_EQ(trace_state((ui_trace_t)(1 - t))->wraps, 0);
        }
}

TEST(erase_bar)
{
    ui_trace_reset(3000);
    const trace_state_t *s = trace_state(UI_TRACE_ECG);
    push_const(UI_TRACE_ECG, 0, 1500 + 750 + 1);
    int p = s->pen;
    CHECK_EQ(p, 64);
    CHECK(!col_empty(s, p));
    for (int c = p + 1; c <= p + TRACE_GAP; c++)
        CHECK(col_empty(s, c));
    CHECK(!col_empty(s, p + TRACE_GAP + 1)); /* previous sweep still shown */
    CHECK(!col_empty(s, TRACE_W - 1));
    for (int c = 0; c < p; c++)
        CHECK(!col_empty(s, c));
    /* the gap wraps around the right edge */
    while (s->pen != TRACE_W - 2)
        push_const(UI_TRACE_ECG, 0, 1);
    for (int k = 1; k <= TRACE_GAP; k++)
        CHECK(col_empty(s, (TRACE_W - 2 + k) % TRACE_W));
    CHECK(!col_empty(s, (TRACE_W - 2 + TRACE_GAP + 1) % TRACE_W));
}

TEST(ecg_autoscale_steps_and_hysteresis)
{
    ui_trace_reset(3000);
    const trace_state_t *s = trace_state(UI_TRACE_ECG);
    const uint32_t n = 1500;
    CHECK_EQ(s->step, ECG_STEP_DEFAULT);
    push_alt(UI_TRACE_ECG, 300, n); /* p2p 0.6 mV: first wrap goes straight to fit */
    CHECK_EQ(s->wraps, 1);
    CHECK_EQ(trace_ecg_steps_uv[s->step], 1000);
    CHECK_EQ(trace_bar_uv(), 500);
    push_alt(UI_TRACE_ECG, 750, n); /* 1.5 mV: up at once */
    CHECK_EQ(trace_ecg_steps_uv[s->step], 2000);
    push_alt(UI_TRACE_ECG, 300, n); /* fits the smaller step: wait one more */
    CHECK_EQ(trace_ecg_steps_uv[s->step], 2000);
    push_alt(UI_TRACE_ECG, 300, n);
    CHECK_EQ(trace_ecg_steps_uv[s->step], 1000);
    /* inside the hysteresis band: fits 1 mV but without 20 % to spare */
    push_alt(UI_TRACE_ECG, 750, n);
    CHECK_EQ(trace_ecg_steps_uv[s->step], 2000);
    for (int i = 0; i < 4; i++) {
        push_alt(UI_TRACE_ECG, 380, n); /* p2p 0.76 mV */
        CHECK_EQ(trace_ecg_steps_uv[s->step], 2000);
    }
    /* an interrupted run of small sweeps does not step down */
    push_alt(UI_TRACE_ECG, 300, n);
    push_alt(UI_TRACE_ECG, 380, n);
    push_alt(UI_TRACE_ECG, 300, n);
    CHECK_EQ(trace_ecg_steps_uv[s->step], 2000);
    /* saturates at the largest step */
    push_alt(UI_TRACE_ECG, 15000, n);
    CHECK_EQ(s->step, ECG_STEPS - 1);
    CHECK_EQ(trace_bar_uv(), 5000);
    /* the scale only ever changes at a wrap */
    ui_trace_reset(3000);
    push_alt(UI_TRACE_ECG, 15000, n - 1);
    CHECK_EQ(s->step, ECG_STEP_DEFAULT);
    push_alt(UI_TRACE_ECG, 15000, 1);
    CHECK_EQ(s->step, ECG_STEPS - 1);
}

TEST(ecg_baseline_follows_data)
{
    /* R waves up: data from -0.3 to +1.2 mV is centred at +0.45 mV */
    ui_trace_reset(3000);
    const trace_state_t *s = trace_state(UI_TRACE_ECG);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 1500; i++) {
            int16_t v = (i % 100 == 0) ? 1200 : (i % 100 == 1) ? -300 : 0;
            ui_trace_push(UI_TRACE_ECG, &v, 1);
        }
    CHECK_EQ(s->center, 450);
    CHECK_EQ(s->range, 2000);
    /* baseline sits in the lower part of the trace */
    CHECK(s->lo[TRACE_W / 2] < 128 - 40);
}

TEST(ppg_autoscale)
{
    ui_trace_reset(3000);
    const trace_state_t *s = trace_state(UI_TRACE_PPG);
    const uint32_t n = 300;
    push_alt(UI_TRACE_PPG, 500, n); /* p2p 1000 */
    CHECK_EQ(s->range, 1500);
    push_alt(UI_TRACE_PPG, 600, n); /* 80 % of range: keep */
    CHECK_EQ(s->range, 1500);
    push_alt(UI_TRACE_PPG, 700, n); /* 93 %: open up */
    CHECK_EQ(s->range, 2100);
    push_alt(UI_TRACE_PPG, 200, n); /* 19 %: close in */
    CHECK_EQ(s->range, 600);
    push_alt(UI_TRACE_PPG, 0, n);   /* flat: minimum range, no divide by zero */
    CHECK(s->range >= 8);
}

TEST(ppg_first_sweep_opens_on_clip)
{
    ui_trace_reset(3000);
    const trace_state_t *s = trace_state(UI_TRACE_PPG);
    int32_t r0 = s->range;
    push_alt(UI_TRACE_PPG, 20000, 10);
    CHECK(s->range > r0);
    CHECK(s->range >= 40000);
}

static void check_continuous(const trace_state_t *s)
{
    int bad = 0;
    for (int c = 1; c < s->pen; c++) {
        if (col_empty(s, c) || col_empty(s, c - 1)) {
            bad++;
            continue;
        }
        if (s->lo[c] > s->hi[c - 1] + 1 || s->hi[c] + 1 < s->lo[c - 1])
            bad++;
    }
    CHECK_EQ(bad, 0);
}

TEST(envelope_is_continuous)
{
    /* steep 20 Hz ECG sine: a column holds about half a cycle */
    ui_trace_reset(3000);
    for (int i = 0; i < 1400; i++) {
        int16_t v = (int16_t)(900.0 * sin(2.0 * 3.14159265 * 20.0 * i / 500.0));
        ui_trace_push(UI_TRACE_ECG, &v, 1);
    }
    check_continuous(trace_state(UI_TRACE_ECG));
    /* PPG at 1.5 s sweep: about one sample per column, big swings */
    ui_trace_reset(1500);
    push_alt(UI_TRACE_PPG, 3000, 150); /* settle the scale */
    for (int i = 0; i < 140; i++) {
        int16_t v = (int16_t)(2500.0 * sin(2.0 * 3.14159265 * 2.5 * i / 100.0));
        ui_trace_push(UI_TRACE_PPG, &v, 1);
    }
    check_continuous(trace_state(UI_TRACE_PPG));
}

TEST(sparse_columns_are_filled)
{
    /* sweep faster than the sample rate covers columns (100 samples, 128 px) */
    ui_trace_reset(1000);
    const trace_state_t *s = trace_state(UI_TRACE_PPG);
    CHECK_EQ(s->n_sweep, 100);
    push_const(UI_TRACE_PPG, 0, 99);
    int empty = 0;
    for (int c = 0; c < s->pen; c++) {
        int ahead = (c - s->pen + TRACE_W) % TRACE_W; /* erase bar wraps to the left */
        if (ahead < 1 || ahead > TRACE_GAP)
            empty += col_empty(s, c);
    }
    CHECK_EQ(empty, 0);
    push_const(UI_TRACE_PPG, 0, 1);
    CHECK_EQ(s->wraps, 1);
    CHECK_EQ(s->pen, 0);
}

TEST(bad_input)
{
    ui_trace_reset(0); /* treated as 3 s */
    CHECK_EQ(trace_state(UI_TRACE_ECG)->n_sweep, 1500);
    ui_trace_push(UI_TRACE_COUNT, NULL, 5);
    ui_trace_push((ui_trace_t)200, (const int16_t *)"abcd", 2);
    ui_trace_push(UI_TRACE_ECG, NULL, 100);
    int16_t ext[4] = {32767, -32768, 32767, -32768};
    ui_trace_push(UI_TRACE_ECG, ext, 4);
    ui_trace_push(UI_TRACE_PPG, ext, 4);
    gfx_begin(fb);
    trace_draw(UI_TRACE_ECG, -10, 100);
    trace_draw(UI_TRACE_PPG, 60, 2);
    trace_draw((ui_trace_t)7, 0, 64);
    CHECK(1);
}

int main(void)
{
    RUN(spike_one_column);
    RUN(sweep_wraps_exactly);
    RUN(erase_bar);
    RUN(ecg_autoscale_steps_and_hysteresis);
    RUN(ecg_baseline_follows_data);
    RUN(ppg_autoscale);
    RUN(ppg_first_sweep_opens_on_clip);
    RUN(envelope_is_continuous);
    RUN(sparse_columns_are_filled);
    RUN(bad_input);
    return TEST_RESULT();
}
