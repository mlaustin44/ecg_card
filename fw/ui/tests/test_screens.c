/* Every screen x state x overlay renders inside the buffer (ASan: the frame
 * buffer is an exact-size heap block), no text is clipped in the reference
 * scenarios, and random models never crash. */
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "scenarios.h"
#include "test.h"
#include "trace.h"

static uint8_t *fb;

static int lit(const uint8_t *b)
{
    int n = 0;
    for (int i = 0; i < OLED_FB_BYTES; i++)
        n += __builtin_popcount(b[i]);
    return n;
}

static int px(const uint8_t *b, int x, int y) { return (b[(y >> 3) * OLED_W + x] >> (y & 7)) & 1; }

TEST(scenarios_render_without_clipping)
{
    for (int i = 0; i < n_scenarios; i++) {
        ui_model_t m;
        scenarios[i].setup(&m);
        gfx_text_clipped = 0;
        ui_render(&m, fb);
        if (gfx_text_clipped)
            printf("  clipped text in %s\n", scenarios[i].name);
        CHECK_EQ(gfx_text_clipped, 0);
        CHECK(lit(fb) > 40);
        /* rendering is a pure function of model + trace state */
        uint8_t again[OLED_FB_BYTES];
        ui_render(&m, again);
        CHECK(memcmp(fb, again, OLED_FB_BYTES) == 0);
    }
}

static const char *long_text =
    "An extremely long fault description that goes on and on without any useful "
    "line breaks, WithAnUnbreakableWordThatIsMuchWiderThanTheScreenItself, more.";

TEST(every_screen_state_overlay)
{
    static const uint8_t battv[] = {0, 1, 4, 5, 99, 100, 101, 255};
    static const uint16_t rates[] = {0, 1, 72, 99, 100, 205, 999, 1000, 65535};
    ui_model_t m;
    scen_defaults(&m);
    sim_sig_t sig = {72, 1000, 1500, 210, 10, 50, 8};
    m.screen = UI_SCREEN_BOTH;
    m.live.state = UI_MEAS_RUNNING;
    sim_begin(&m, &sig);
    sim_run(&m, 4000);
    int frames = 0;
    for (int s = 0; s <= UI_SCREEN_COUNT; s++)
        for (int st = 0; st <= UI_MEAS_FAULT + 1; st++)
            for (int ov = 0; ov <= UI_OVERLAY_FAULT + 1; ov++)
                for (int v = 0; v < 9; v++) {
                    m.screen = (ui_screen_t)s;
                    m.live.state = (ui_meas_state_t)st;
                    m.overlay = (ui_overlay_t)ov;
                    m.batt_pct = battv[v % 8];
                    m.batt_valid = v != 3;
                    m.usb_present = v & 1;
                    m.charging = v & 2;
                    m.charge_fault = v == 7;
                    m.time_valid = v == 5;
                    m.hour = (uint8_t)(v * 7);
                    m.minute = (uint8_t)(v * 13);
                    m.hold_progress = (uint8_t)(v * 30);
                    m.fault_text = v == 0 ? NULL : v & 1 ? long_text : "short";
                    m.fw_version = v == 2 ? NULL : "0.1.0";
                    m.ble_state = (ui_ble_t)(v % 5); /* incl. one out of range */
                    m.radio_fw = v & 1 ? "255.255.255" : NULL;
                    m.ble_id = v & 2 ? "FFFF" : NULL;
                    m.settings_cursor = (uint8_t)(v % (SET_ITEM_COUNT + 1));
                    m.live.hr_bpm = m.live.pulse_bpm = rates[v];
                    m.live.pat_ms = rates[8 - v];
                    m.live.pi_permille = rates[v];
                    m.live.quality = (uint8_t)v;
                    m.live.settle_pct = (uint8_t)(v * 20);
                    m.live.gain_hi = v & 1;
                    m.live.beat_seq = (uint8_t)v;
                    m.now_ms += 37;
                    m.settings_cursor = (uint8_t)v;
                    m.settings.gain = m.settings.notch = m.settings.rld = (uint8_t)v;
                    m.settings.led_limit = m.settings.sweep = (uint8_t)v;
                    m.summary.mode = (ui_mode_t)(v % 4);
                    m.summary.valid = v != 4;
                    m.summary.stopped_by_usb = v & 1;
                    m.summary.hr_avg = m.summary.hr_min = m.summary.hr_max = rates[v];
                    m.summary.beats = m.summary.sdnn_ms = m.summary.rmssd_ms = rates[8 - v];
                    m.summary.pulse_avg = m.summary.pat_ms = m.summary.pi_permille = rates[v];
                    m.summary.duration_s = rates[v];
                    m.diag_page = (uint8_t)v;
                    m.diag_pages = (uint8_t)(v * 31);
                    ui_render(&m, fb);
                    frames++;
                }
    CHECK(frames > 1000);
}

TEST(random_models)
{
    uint32_t r = 1;
    for (int i = 0; i < 20000; i++) {
        ui_model_t m;
        uint8_t *p = (uint8_t *)&m;
        for (size_t k = 0; k < sizeof m; k++) {
            r = r * 1103515245u + 12345u;
            p[k] = (uint8_t)(r >> 16);
        }
        /* bools must hold 0/1 (anything else is UB before the UI sees it) */
        m.batt_valid = r & 0x10;
        m.usb_present = r & 0x20;
        m.charging = r & 0x40;
        m.charge_fault = r & 0x80;
        m.time_valid = r & 0x100;
        m.live.gain_hi = r & 0x200;
        m.summary.stopped_by_usb = r & 0x400;
        m.summary.valid = r & 0x800;
        m.fault_text = (r & 3) == 0 ? NULL : (r & 3) == 1 ? long_text : "x";
        m.fw_version = (r & 4) ? NULL : "9.99.999-rc1+build.12345";
        /* diag lines deliberately not terminated: the UI reads <= 21 chars */
        if (i % 3 == 0) {
            m.screen = UI_SCREEN_DIAG;
            m.overlay = UI_OVERLAY_NONE;
        }
        ui_render(&m, fb);
        int16_t smp[8];
        memcpy(smp, p + (i % 16), sizeof smp);
        ui_trace_push((ui_trace_t)(i & 1), smp, 8);
        if (i % 977 == 0)
            ui_trace_reset((uint16_t)r);
    }
    ui_render(NULL, fb);
    CHECK_EQ(lit(fb), 0);
}

TEST(page_dots_track_the_page)
{
    int prev = -1;
    for (int pg = 0; pg < UI_CAROUSEL_PAGES; pg++) {
        ui_model_t m;
        scen_defaults(&m);
        m.screen = (ui_screen_t)pg;
        ui_render(&m, fb);
        /* the active dot is the tallest mark in the dot strip */
        int active = -1;
        for (int x = 80; x < OLED_W - 6; x++)
            if (px(fb, x, 57) && px(fb, x, 61))
                active = x;
        CHECK(active > prev);
        prev = active;
    }
}

static bool region_equal(const uint8_t *a, const uint8_t *b, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (px(a, x, y) != px(b, x, y))
                return false;
    return true;
}

TEST(heart_rate_digits)
{
    static uint8_t want[OLED_FB_BYTES];
    const struct { uint16_t bpm; const char *txt; } cases[] = {
        {0, "--"}, {7, "7"}, {72, "72"}, {205, "205"}, {1500, "999"},
    };
    for (int i = 0; i < 5; i++) {
        ui_model_t m;
        scen_defaults(&m);
        m.screen = UI_SCREEN_ECG;
        m.live.state = UI_MEAS_RUNNING;
        m.live.hr_bpm = cases[i].bpm;
        ui_trace_reset(3000);
        ui_render(&m, fb);
        gfx_begin(want);
        gfx_big(14, 1, cases[i].txt, TXT_LEFT);
        CHECK(region_equal(fb, want, 13, 0, 14 + gfx_big_width(cases[i].txt), 15));
    }
}

TEST(heart_blinks_on_beat)
{
    ui_model_t m;
    scen_defaults(&m);
    m.screen = UI_SCREEN_ECG;
    m.live.state = UI_MEAS_RUNNING;
    m.live.hr_bpm = 60;
    ui_render(&m, fb);
    int idle = lit(fb);
    m.live.beat_seq++;
    m.now_ms += 10;
    ui_render(&m, fb);
    int beat = lit(fb);
    CHECK(beat > idle); /* filled heart */
    m.now_ms += 400;
    ui_render(&m, fb);
    CHECK_EQ(lit(fb), idle);
}

int main(void)
{
    fb = malloc(OLED_FB_BYTES);
    RUN(scenarios_render_without_clipping);
    RUN(every_screen_state_overlay);
    RUN(random_models);
    RUN(page_dots_track_the_page);
    RUN(heart_rate_digits);
    RUN(heart_blinks_on_beat);
    free(fb);
    return TEST_RESULT();
}
