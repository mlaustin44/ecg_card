/* Screens: ui_model_t -> frame. All layout lives here. */
#include <stddef.h>

#include "gfx.h"
#include "trace.h"
#include "ui_model.h"

/* ---- layout ------------------------------------------------------------ */
#define ROW_HINT   56 /* bottom hint row (8 rows incl. descender) */
#define STATUS_H   16 /* live screens: status rows 0..14, trace from 16 */
#define TRACE_Y    STATUS_H

/* ---- beat blink (the only non-trace state) ----------------------------- */
#define BEAT_LIT_MS 180u
static struct {
    uint8_t seq;
    bool init;
    uint32_t t;
} beat_;

static bool beat_lit(const ui_model_t *m)
{
    if (!beat_.init) {
        beat_.init = true;
        beat_.seq = m->live.beat_seq;
        beat_.t = m->now_ms - BEAT_LIT_MS;
    } else if (m->live.beat_seq != beat_.seq) {
        beat_.seq = m->live.beat_seq;
        beat_.t = m->now_ms;
    }
    return (uint32_t)(m->now_ms - beat_.t) < BEAT_LIT_MS;
}

void ui_init(void)
{
    beat_.init = false;
    ui_trace_reset(3000u);
}

/* ---- small widgets ------------------------------------------------------ */

static unsigned clamp_u(unsigned v, unsigned hi) { return v > hi ? hi : v; }

/* Battery 14 x 7 at (x, y) with fill level. */
static void battery_small(int x, int y, const ui_model_t *m)
{
    gfx_rrect(x, y, 12, 7, GFX_SET);
    gfx_fill(x + 12, y + 2, 2, 3, GFX_SET);
    if (m->batt_valid) {
        int lvl = (int)((clamp_u(m->batt_pct, 100) * 8u + 50u) / 100u);
        if (m->batt_pct > 0 && lvl == 0)
            lvl = 1;
        gfx_fill(x + 2, y + 2, lvl, 3, GFX_SET);
    } else {
        gfx_dotted_hline(x + 3, x + 9, y + 3, 3);
    }
}

/* Battery and power-source icons, right-aligned to xr. Returns left edge. */
static int power_icons(int xr, int y, const ui_model_t *m)
{
    int x = xr - 14;
    battery_small(x, y, m);
    if (m->charge_fault) {
        x -= 3;
        gfx_text(x, y - 0, "!", TXT_LEFT);
    } else if (m->charging) {
        x -= 7;
        gfx_icon(x, y, &icon_bolt, GFX_SET);
    } else if (m->usb_present) {
        x -= 11;
        gfx_icon(x, y + 1, &icon_plug, GFX_SET);
    }
    return x;
}

/* Signal quality: 4 bars, 11 x 7, bottom at y + 6. */
static void quality_bars(int x, int y, unsigned q)
{
    static const uint8_t hgt[4] = {2, 3, 5, 7};
    q = clamp_u(q, 4);
    for (unsigned i = 0; i < 4; i++) {
        int bx = x + (int)i * 3;
        if (i < q)
            gfx_fill(bx, y + 7 - hgt[i], 2, hgt[i], GFX_SET);
        else
            gfx_hline(bx, bx + 1, y + 6, GFX_SET);
    }
}

/* Page dots, always in the same place at the bottom right; with_next adds
 * the BUTTON2 glyph after them ("next page"). */
static void page_dots(int page, bool with_next)
{
    const int slot = 6, n = UI_CAROUSEL_PAGES;
    int x0 = OLED_W - 6 - slot * n;
    for (int i = 0; i < n; i++) {
        int cx = x0 + i * slot + slot / 2;
        if (i == page)
            gfx_rrect_fill(cx - 2, ROW_HINT + 1, 4, 5, GFX_SET);
        else
            gfx_fill(cx - 1, ROW_HINT + 2, 2, 3, GFX_SET);
    }
    if (with_next)
        gfx_text(OLED_W, ROW_HINT, G_NEXT, TXT_RIGHT);
}

/* Bottom row, left: "(sel) text" and optionally "(next) text". */
static void hint_row(const char *sel, const char *next, int page)
{
    int x = 0;
    if (sel) {
        x += gfx_text(x, ROW_HINT, G_SEL, TXT_LEFT) + 2;
        x += gfx_text(x, ROW_HINT, sel, TXT_LEFT) + 6;
    }
    if (next) {
        x += gfx_text(x, ROW_HINT, G_NEXT, TXT_LEFT) + 2;
        gfx_text(x, ROW_HINT, next, TXT_LEFT);
    }
    if (page >= 0)
        page_dots(page, next == NULL);
}

static void title_row(const char *title, const ui_model_t *m, bool batt)
{
    gfx_text(0, 0, title, TXT_BOLD);
    if (batt)
        power_icons(OLED_W, 0, m);
}

/* Bluetooth state, right-aligned to xr (7 rows from y). Advertising blinks
 * (0.6 s on, 0.4 s off), connected is steady, a fault adds "!". Returns the
 * left edge (xr when nothing is drawn). */
#define BLE_BLINK_MS 1000u
#define BLE_LIT_MS   600u
static int ble_icon(int xr, int y, const ui_model_t *m)
{
    const ui_icon_t *ic = NULL;
    switch (m->ble_state) {
    case UI_BLE_ADVERTISING:
        if (m->now_ms % BLE_BLINK_MS < BLE_LIT_MS)
            ic = &icon_ble;
        else
            return xr - icon_ble.w; /* keep the space while dark */
        break;
    case UI_BLE_CONNECTED: ic = &icon_ble; break;
    case UI_BLE_FAULT: ic = &icon_ble_fault; break;
    default: return xr;
    }
    gfx_icon(xr - ic->w, y, ic, GFX_SET);
    return xr - ic->w;
}

/* Carousel pages: title, Bluetooth, power icons. */
static void title_row_status(const char *title, const ui_model_t *m)
{
    gfx_text(0, 0, title, TXT_BOLD);
    ble_icon(power_icons(OLED_W, 0, m) - 4, 0, m);
}

static char *bpm_str(char *b, unsigned bpm)
{
    if (bpm == 0)
        return str_cpy(b, "--");
    return str_u(b, clamp_u(bpm, 999));
}

/* ---- pictograms ---------------------------------------------------------- */

typedef struct { int8_t x, y; } pt_t;

/* Polyline with points scaled vertically by ys/8, drawn 2 px wide. */
static void polyline(int x, int y, const pt_t *p, int n, int ys)
{
    for (int i = 1; i < n; i++) {
        int y0 = y + p[i - 1].y * ys / 8, y1 = y + p[i].y * ys / 8;
        gfx_line(x + p[i - 1].x, y0, x + p[i].x, y1, GFX_SET);
        gfx_line(x + p[i - 1].x + 1, y0, x + p[i].x + 1, y1, GFX_SET);
    }
}

static const pt_t ecg_beat[] = {
    {0, 0}, {3, 0}, {5, -3}, {7, -3}, {9, 0}, {11, 0}, {12, 2}, {15, -17},
    {18, 5}, {19, 0}, {23, 0}, {26, -4}, {29, -4}, {32, 0}, {36, 0},
};
static const pt_t ppg_beat[] = {
    {0, 0}, {2, -1}, {6, -15}, {8, -16}, {10, -14}, {13, -8}, {14, -8},
    {16, -9}, {18, -8}, {24, -2}, {27, 0},
};
#define NPTS(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define BEAT_PX 36 /* pictogram beat period */

static void pict_ecg(int x, int y, int beats, int ys)
{
    for (int i = 0; i < beats; i++)
        polyline(x + i * BEAT_PX, y, ecg_beat, NPTS(ecg_beat), ys);
}

static void pict_ppg(int x, int y, int beats, int period, int ys)
{
    for (int i = 0; i < beats; i++) {
        polyline(x + i * period, y, ppg_beat, NPTS(ppg_beat), ys);
        if (period > 27 && i + 1 < beats)
            gfx_fill(x + i * period + 27, y, period - 26, 1, GFX_SET);
    }
}

/* A thumb seen from above, tip (with nail) at ty, widening towards the
 * hand off the bottom of the screen. */
static void thumb(int x, int ty)
{
    gfx_fill(x + 2, ty, 9, 3, GFX_CLR);
    for (int y = ty + 3; y < OLED_H; y++) {
        int o = (int)((unsigned)(y - ty - 3) / 5u);
        gfx_hline(x - o + 1, x + 11 + o, y, GFX_CLR);
        gfx_pixel(x - o, y, GFX_SET);
        gfx_pixel(x + 12 + o, y, GFX_SET);
    }
    gfx_hline(x + 3, x + 9, ty, GFX_SET);
    gfx_pixel(x + 2, ty + 1, GFX_SET);
    gfx_pixel(x + 10, ty + 1, GFX_SET);
    gfx_pixel(x + 1, ty + 2, GFX_SET);
    gfx_pixel(x + 11, ty + 2, GFX_SET);
    /* nail */
    gfx_hline(x + 4, x + 8, ty + 2, GFX_SET);
    gfx_vline(x + 3, ty + 3, ty + 6, GFX_SET);
    gfx_vline(x + 9, ty + 3, ty + 6, GFX_SET);
    gfx_hline(x + 4, x + 8, ty + 7, GFX_SET);
}

/* The card from the front; thumbs rise onto the pads, then contact marks. */
static void pict_contact(int x, int y, bool left, bool right, uint32_t now)
{
    const int cw = 60, ch = 34;
    gfx_rrect(x, y, cw, ch, GFX_SET);
    gfx_rect(x + 22, y + 4, 16, 10, GFX_SET); /* the display */
    const int pad_y = y + 15, pw = 15;
    const int lx = x + 3, rx = x + cw - 3 - pw;
    gfx_rrect(lx, pad_y, pw, 15, GFX_SET);
    gfx_rrect(rx, pad_y, pw, 15, GFX_SET);
    /* 1.6 s loop: rise for 0.7 s, then hold with contact marks */
    uint32_t ph = now % 1600u;
    int lift = ph < 700u ? (int)((700u - ph) * 14u / 700u) : 0;
    bool pressed = ph >= 800u;
    for (int side = 0; side < 2; side++) {
        if (!(side ? right : left))
            continue;
        int px = (side ? rx : lx) + 1;
        int ty = pad_y + 3 + lift;
        thumb(px, ty);
        if (pressed) {
            gfx_line(px - 2, ty, px - 4, ty - 2, GFX_SET);
            gfx_line(px + 14, ty, px + 16, ty - 2, GFX_SET);
            gfx_vline(px + 6, ty - 2, ty - 4, GFX_SET);
        }
    }
}

/* ---- carousel pages ------------------------------------------------------ */

static void screen_home(const ui_model_t *m)
{
    gfx_text(0, 0, "ECG Card", TXT_BOLD);
    int xr = OLED_W;
    if (m->time_valid) {
        char t[6];
        t[0] = (char)('0' + clamp_u(m->hour, 23) / 10u);
        t[1] = (char)('0' + clamp_u(m->hour, 23) % 10u);
        t[2] = ':';
        t[3] = (char)('0' + clamp_u(m->minute, 59) / 10u);
        t[4] = (char)('0' + clamp_u(m->minute, 59) % 10u);
        t[5] = '\0';
        xr -= gfx_text(OLED_W, 0, t, TXT_RIGHT) + 4;
    }
    ble_icon(xr, 0, m);

    /* big battery 30 x 16 with % in big digits */
    const int bx = 10, by = 17, bw = 32, bh = 16;
    gfx_rrect(bx, by, bw, bh, GFX_SET);
    gfx_rect(bx + 1, by + 1, bw - 2, bh - 2, GFX_SET);
    gfx_fill(bx + bw, by + 5, 3, bh - 10, GFX_SET);
    unsigned pct = clamp_u(m->batt_pct, 100);
    if (m->batt_valid) {
        int lvl = (int)((pct * (unsigned)(bw - 6) + 50u) / 100u);
        gfx_fill(bx + 3, by + 3, lvl, bh - 6, GFX_SET);
    }
    if (m->charging)
        gfx_icon(bx + bw / 2 - 2, by + bh / 2 - 3, &icon_bolt, GFX_INV);

    char b[8];
    if (m->batt_valid) {
        str_u(b, pct);
        int w = gfx_big(bx + bw + 10, by + 1, b, TXT_LEFT);
        gfx_text(bx + bw + 10 + w + 2, by + 8, "%", TXT_LEFT);
    } else {
        gfx_big(bx + bw + 10, by + 1, "--", TXT_LEFT);
    }

    const char *st;
    if (m->charge_fault)
        st = "Charge fault";
    else if (m->charging)
        st = "Charging";
    else if (m->usb_present)
        st = (m->batt_valid && pct >= 100) ? "Charged" : "USB power";
    else if (m->batt_valid && pct < BATT_WARN_PCT)
        st = "Battery low";
    else
        st = "On battery";
    gfx_text(OLED_W / 2, 40, st, TXT_CENTER);
    hint_row("hold: off", NULL, UI_SCREEN_HOME);
}

static void screen_card(const ui_model_t *m)
{
    const char *title, *use;
    bool ecg = m->screen != UI_SCREEN_PPG_CARD;
    if (m->screen == UI_SCREEN_ECG_CARD) {
        title = "ECG";
        use = "Both thumbs on the pads";
    } else if (m->screen == UI_SCREEN_PPG_CARD) {
        title = "Pulse";
        use = "Right thumb on sensor";
    } else {
        title = "ECG + Pulse";
        use = "Both thumbs on the pads";
    }
    title_row_status(title, m);

    if (m->screen == UI_SCREEN_ECG_CARD) {
        pict_ecg(9, 32, 3, 8);
    } else if (m->screen == UI_SCREEN_PPG_CARD) {
        pict_ppg(9, 34, 4, 27, 8);
    } else {
        /* ECG above, pulse below, offset by the arrival time */
        const int x = 9, ye = 21, yp = 36, pat = 9;
        pict_ecg(x, ye, 3, 5);
        gfx_hline(x + 15 + pat - 9, x + 15 + pat - 1, yp, GFX_SET);
        pict_ppg(x + 15 + pat, yp, 3, BEAT_PX, 5);
        for (int yy = ye + 2; yy < yp - 1; yy += 2) {
            gfx_pixel(x + BEAT_PX + 15, yy, GFX_SET);
            gfx_pixel(x + BEAT_PX + 15 + pat, yy, GFX_SET);
        }
    }

    if (ecg && m->usb_present) {
        gfx_fill(0, 39, OLED_W, 11, GFX_SET);
        gfx_text(OLED_W / 2, 41, "Unplug USB first", TXT_CENTER | TXT_CLR);
    } else if (m->batt_valid && m->batt_pct < BATT_WARN_PCT) {
        gfx_fill(0, 39, OLED_W, 11, GFX_SET);
        gfx_text(OLED_W / 2, 41, "Battery low", TXT_CENTER | TXT_CLR);
    } else {
        gfx_text(OLED_W / 2, 42, use, TXT_CENTER);
    }
    hint_row("Start", NULL, (int)m->screen);
}

static const char *const set_names[SET_ITEM_COUNT] = {
    "Gain", "Mains notch", "RLD", "LED limit", "Sweep", "Bluetooth", "Restore",
};
static const char *const gain_vals[] = {"Auto", ECG_GAIN_LO_TXT, ECG_GAIN_HI_TXT};
static const char *const notch_vals[] = {"Auto", "50Hz", "60Hz"};
static const char *const rld_vals[] = {"Both", "Left", "Right", "Off"};
static const char *const led_vals[LED_LIMIT_COUNT] = {"5mA", "10mA", "15mA"};
static const char *const sweep_vals[] = {"1.5s", "3s", "6s"};
static const char *const ble_vals[] = {"Off", "On"};
static const char *const restore_vals[] = {"Fast", "Slow", "Off"};

static const char *setting_value(const settings_t *s, unsigned item)
{
    switch (item) {
    case SET_ITEM_GAIN: return s->gain < 3 ? gain_vals[s->gain] : "?";
    case SET_ITEM_NOTCH: return s->notch < 3 ? notch_vals[s->notch] : "?";
    case SET_ITEM_RLD: return s->rld < 4 ? rld_vals[s->rld] : "?";
    case SET_ITEM_LED_LIMIT: return s->led_limit < LED_LIMIT_COUNT ? led_vals[s->led_limit] : "?";
    case SET_ITEM_SWEEP: return s->sweep < 3 ? sweep_vals[s->sweep] : "?";
    case SET_ITEM_BLE: return s->ble < 2 ? ble_vals[s->ble] : "?";
    case SET_ITEM_RESTORE: return s->restore < 3 ? restore_vals[s->restore] : "?";
    default: return "?";
    }
}

#define SET_ROWS 5
static void screen_settings(const ui_model_t *m)
{
    title_row_status("Settings", m);
    unsigned cur = m->settings_cursor < SET_ITEM_COUNT ? m->settings_cursor : 0;
    unsigned first = 0;
    if (cur >= SET_ROWS)
        first = cur - SET_ROWS + 1;
    /* more rows than fit: arrows after the title show where the rest is */
    int ax = gfx_text_width("Settings", TXT_BOLD) + 4;
    if (first > 0)
        gfx_text(ax, 0, G_UP, TXT_LEFT);
    if (first + SET_ROWS < SET_ITEM_COUNT)
        gfx_text(ax + 6, 0, G_DOWN, TXT_LEFT);
    for (unsigned r = 0; r < SET_ROWS && first + r < SET_ITEM_COUNT; r++) {
        unsigned it = first + r;
        int y = 10 + (int)r * LINE_H;
        uint8_t f = 0;
        if (it == cur) {
            gfx_rrect_fill(0, y - 1, OLED_W, LINE_H, GFX_SET);
            f = TXT_CLR;
        }
        gfx_text(3, y, set_names[it], f);
        gfx_text(OLED_W - 3, y, setting_value(&m->settings, it), f | TXT_RIGHT);
    }
    hint_row("Change", "Next", UI_SCREEN_SETTINGS);
}

/* Hardware designer credit: boot and power-down screens, About page. */
#define CREDIT_NAME "Matthew Austin"
#define CREDIT_YEAR "2026"
#define CREDIT_SHORT "M Austin"

static void kv_row(int y, const char *k, const char *v)
{
    gfx_text(0, y, k, TXT_LEFT);
    gfx_text(OLED_W, y, v, TXT_RIGHT);
}

static void screen_about(const ui_model_t *m)
{
    title_row_status("About", m);
    char b[24], *p;
    /* the device name the phone shows, once the radio reported its address */
    p = str_cpy(b, "ECG Card");
    if (m->ble_id) {
        p = str_cpy(p, " ");
        str_cpy(p, m->ble_id);
    }
    kv_row(11, "Device", b);
    kv_row(20, "Firmware", m->fw_version ? m->fw_version : "?");
    kv_row(29, "Radio firmware", m->radio_fw ? m->radio_fw : "n/a");
    /* voltage and charge share a row to leave one for the designer */
    if (m->batt_mv) {
        p = str_u(b, m->batt_mv);
        p = str_cpy(p, " mV");
        if (m->batt_valid) {
            p = str_cpy(p, " ");
            p = str_u(p, clamp_u(m->batt_pct, 100));
            str_cpy(p, "%");
        }
    } else if (m->batt_valid) {
        p = str_u(b, clamp_u(m->batt_pct, 100));
        str_cpy(p, "%");
    } else {
        str_cpy(b, "--");
    }
    kv_row(38, "Battery", b);
    kv_row(47, "Designed by", CREDIT_SHORT);
    hint_row(NULL, NULL, UI_SCREEN_ABOUT);
}

static void screen_diag(const ui_model_t *m)
{
    for (int i = 0; i < UI_DIAG_LINES; i++)
        gfx_text_n(1, i * LINE_H, m->diag[i], UI_DIAG_COLS, TXT_MONO);
    gfx_hline(0, OLED_W - 1, ROW_HINT - 2, GFX_SET);
    gfx_text(0, ROW_HINT, "Diagnostics", TXT_LEFT);
    char b[12], *p = str_u(b, (unsigned)m->diag_page + 1u);
    *p++ = '/';
    str_u(p, m->diag_pages ? m->diag_pages : 1u);
    gfx_text(OLED_W, ROW_HINT, b, TXT_RIGHT);
}

/* ---- measurement screens -------------------------------------------------- */

static const char *mode_title(ui_screen_t s)
{
    return s == UI_SCREEN_ECG ? "ECG" : s == UI_SCREEN_PPG ? "Pulse" : "ECG + Pulse";
}

static void live_wait(const ui_model_t *m)
{
    bool ppg = m->screen == UI_SCREEN_PPG;
    title_row(mode_title(m->screen), m, true);
    pict_contact(2, 12, !ppg, true, m->now_ms);
    const int cx = 96;
    if (ppg) {
        gfx_text(cx, 20, "Place right", TXT_CENTER);
        gfx_text(cx, 29, "thumb on", TXT_CENTER);
        gfx_text(cx, 38, "the sensor", TXT_CENTER);
    } else {
        /* the unit needs ~2 s after every touch or movement (U5's 0.5 s
         * coupling): ask for stillness up front */
        gfx_text(cx, 14, "Place both", TXT_CENTER);
        gfx_text(cx, 23, "thumbs on", TXT_CENTER);
        gfx_text(cx, 32, "the pads", TXT_CENTER);
        gfx_text(cx, 45, "Hold still", TXT_CENTER | TXT_BOLD);
    }
}

static void live_settling(const ui_model_t *m)
{
    title_row(mode_title(m->screen), m, true);
    quality_bars(power_icons(OLED_W, 0, m) - 15, 0, m->live.quality);
    gfx_text(OLED_W / 2, 17, "Hold still", TXT_CENTER | TXT_BOLD);
    gfx_progress(14, 29, 100, 9, clamp_u(m->live.settle_pct, 100));
    gfx_text(OLED_W / 2, 44, "Keep thumbs relaxed", TXT_CENTER);
}

static void live_usb(const ui_model_t *m)
{
    title_row(mode_title(m->screen), m, true);
    gfx_icon(6, 17, &icon_usb_big, GFX_SET);
    gfx_text(30, 17, "Unplug USB", TXT_BOLD);
    gfx_text(30, 27, "to measure ECG", TXT_LEFT);
    gfx_text(30, 40, "ECG runs on", TXT_LEFT);
    gfx_text(30, 49, "battery only", TXT_LEFT);
}

static void live_fault(const ui_model_t *m)
{
    title_row(mode_title(m->screen), m, true);
    gfx_icon(4, 18, &icon_warn, GFX_SET);
    gfx_text(24, 14, "Self-test failed", TXT_BOLD);
    gfx_text_wrap(24, 25, OLED_W - 24, m->fault_text ? m->fault_text : "Analog front end",
                  4, TXT_LEFT);
}

/* Live status rows 0..14: heart + big rate on the left; returns right edge. */
static int live_rate(const ui_model_t *m, unsigned bpm)
{
    gfx_icon(0, 3, beat_lit(m) ? &icon_heart_fill : &icon_heart_line, GFX_SET);
    char b[6];
    bpm_str(b, bpm);
    return 14 + gfx_big(14, 1, b, TXT_LEFT);
}

/* Right block, row 0: [gain] quality bars, power icons. Returns left edge. */
static int live_icons(const ui_model_t *m, bool gain)
{
    int x = power_icons(OLED_W, 0, m) - 15;
    quality_bars(x, 0, m->live.quality);
    if (gain)
        x -= 4 + gfx_text(x - 4, 0, m->live.gain_hi ? ECG_GAIN_HI_TXT : ECG_GAIN_LO_TXT, TXT_RIGHT);
    return x;
}

static void scale_label(char *b, unsigned uv)
{
    if (uv >= 1000u) {
        b = str_u(b, uv / 1000u);
    } else {
        *b++ = '0';
        *b++ = '.';
        *b++ = (char)('0' + uv / 100u);
    }
    str_cpy(b, "mV");
}

/* ECG calibration bar hanging from the top right of the trace. */
static void scale_bar(int y0, int h)
{
    int len = trace_bar_px(h);
    if (len < 2)
        return;
    gfx_fill(OLED_W - 5, y0, 5, len, GFX_CLR);
    gfx_vline(OLED_W - 1, y0, y0 + len - 1, GFX_SET);
    gfx_hline(OLED_W - 3, OLED_W - 1, y0, GFX_SET);
    gfx_hline(OLED_W - 3, OLED_W - 1, y0 + len - 1, GFX_SET);
}

static void live_running(const ui_model_t *m)
{
    char b[12];
    int icons_x = live_icons(m, m->screen == UI_SCREEN_ECG);
    if (m->screen == UI_SCREEN_ECG) {
        int x = live_rate(m, m->live.hr_bpm);
        gfx_text(x + 2, 8, "bpm", TXT_LEFT);
        scale_label(b, trace_bar_uv());
        gfx_text(OLED_W, 8, b, TXT_RIGHT);
        trace_draw(UI_TRACE_ECG, TRACE_Y, OLED_H - TRACE_Y);
        scale_bar(TRACE_Y, OLED_H - TRACE_Y);
    } else if (m->screen == UI_SCREEN_PPG) {
        int x = live_rate(m, m->live.pulse_bpm);
        gfx_text(x + 2, 8, "bpm", TXT_LEFT);
        if (m->live.pi_permille) {
            char *p = str_cpy(b, "PI ");
            p = str_u_dec1(p, clamp_u(m->live.pi_permille, 9999));
            str_cpy(p, "%");
            gfx_text(OLED_W, 8, b, TXT_RIGHT);
        }
        trace_draw(UI_TRACE_PPG, TRACE_Y, OLED_H - TRACE_Y);
    } else {
        int x = live_rate(m, m->live.hr_bpm) + 3;
        char *p = str_cpy(b, "PAT ");
        if (m->live.pat_ms) {
            p = str_u(p, clamp_u(m->live.pat_ms, 999));
            /* the unit only if it clears the quality bars */
            if (x + gfx_text_width(b, 0) + 11 < icons_x - 3)
                str_cpy(p, "ms");
        } else {
            str_cpy(p, "--");
        }
        gfx_text(x, 0, b, TXT_LEFT);
        p = str_cpy(b, "Pulse ");
        if (m->live.pulse_bpm)
            str_u(p, clamp_u(m->live.pulse_bpm, 999));
        else
            str_cpy(p, "--");
        gfx_text(x, 8, b, TXT_LEFT);
        scale_label(b, trace_bar_uv());
        gfx_text(OLED_W, 8, b, TXT_RIGHT);
        const int h1 = 24;
        trace_draw(UI_TRACE_ECG, TRACE_Y, h1);
        scale_bar(TRACE_Y, h1);
        gfx_dotted_hline(0, OLED_W - 1, TRACE_Y + h1, 4);
        trace_draw(UI_TRACE_PPG, TRACE_Y + h1 + 1, OLED_H - TRACE_Y - h1 - 1);
    }
}

static void screen_live(const ui_model_t *m)
{
    switch (m->live.state) {
    case UI_MEAS_WAIT_CONTACT: live_wait(m); break;
    case UI_MEAS_SETTLING: live_settling(m); break;
    case UI_MEAS_RUNNING: live_running(m); break;
    case UI_MEAS_USB_BLOCKED: live_usb(m); break;
    case UI_MEAS_FAULT: live_fault(m); break;
    default: live_wait(m); break;
    }
}

/* ---- summary --------------------------------------------------------------- */

static void kv_at(int x, int xr, int y, const char *k, unsigned v, const char *unit)
{
    char b[12], *p = b;
    gfx_text(x, y, k, TXT_LEFT);
    if (v)
        p = str_u(p, clamp_u(v, 999));
    else
        p = str_cpy(p, "--");
    if (unit && v)
        str_cpy(p, unit);
    gfx_text(xr, y, b, TXT_RIGHT);
}

static void kv_pct(int x, int xr, int y, const char *k, unsigned permille)
{
    char b[12], *p = b;
    if (permille) {
        p = str_u_dec1(p, clamp_u(permille, 999));
        str_cpy(p, "%");
    } else {
        str_cpy(p, "--");
    }
    gfx_text(x, y, k, TXT_LEFT);
    gfx_text(xr, y, b, TXT_RIGHT);
}

static void screen_summary(const ui_model_t *m)
{
    const ui_summary_t *s = &m->summary;
    static const char *const titles[] = {"ECG result", "Pulse result", "ECG+Pulse"};
    char b[16];
    gfx_text(0, 0, titles[s->mode <= UI_MODE_BOTH ? s->mode : 0], TXT_BOLD);
    str_mmss(b, clamp_u(s->duration_s, 99u * 60u + 59u));
    gfx_text(OLED_W, 0, b, TXT_RIGHT);

    if (!s->valid) {
        gfx_text(OLED_W / 2, 17, "Too short", TXT_CENTER | TXT_BOLD);
        gfx_text(OLED_W / 2, 30, "Keep thumbs on the", TXT_CENTER);
        gfx_text(OLED_W / 2, 39, "pads a little longer", TXT_CENTER);
    } else {
        bool ppg = s->mode == UI_MODE_PPG;
        gfx_icon(0, 13, &icon_heart_fill, GFX_SET);
        bpm_str(b, ppg ? s->pulse_avg : s->hr_avg);
        gfx_big(14, 11, b, TXT_LEFT);
        gfx_text(0, 27, "avg bpm", TXT_LEFT);
        /* right block: three rows */
        const int x = 56, xr = OLED_W;
        if (ppg) {
            kv_pct(x, xr, 11, "PI", s->pi_permille);
        } else {
            if (s->mode == UI_MODE_BOTH) {
                char *p = b;
                if (s->hr_min && s->hr_max) {
                    p = str_u(p, clamp_u(s->hr_min, 999));
                    *p++ = '-';
                    str_u(p, clamp_u(s->hr_max, 999));
                } else {
                    str_cpy(p, "--");
                }
                gfx_text(x, 11, "range", TXT_LEFT);
                gfx_text(xr, 11, b, TXT_RIGHT);
                kv_at(x, xr, 20, "beats", s->beats, NULL);
                kv_at(x, xr, 29, "pulse", s->pulse_avg, NULL);
            } else {
                kv_at(x, xr, 11, "min", s->hr_min, NULL);
                kv_at(x, xr, 20, "max", s->hr_max, NULL);
                kv_at(x, xr, 29, "beats", s->beats, NULL);
            }
            gfx_dotted_hline(0, OLED_W - 1, 37, 2);
            kv_at(0, 58, 39, "SDNN", s->sdnn_ms, "ms");
            kv_at(66, xr, 39, "RMSSD", s->rmssd_ms, "ms");
            if (s->mode == UI_MODE_BOTH) {
                kv_at(0, 58, 47, "PAT", s->pat_ms, "ms");
                kv_pct(66, xr, 47, "PI", s->pi_permille);
            }
        }
    }
    if (s->stopped_by_usb) {
        gfx_rrect_fill(0, ROW_HINT - 1, OLED_W, 9, GFX_SET);
        gfx_text(OLED_W / 2, ROW_HINT, "Stopped: USB connected", TXT_CENTER | TXT_CLR);
    } else {
        hint_row("Done", NULL, -1);
    }
}

/* ---- boot / power-down: who designed and built the card ------------------- */

#define SPLASH_NAME CREDIT_NAME
#define SPLASH_BY       "Designed & built by"
#define SPLASH_TITLE_Y  1
#define SPLASH_TRACE_Y  24 /* baseline; the R wave peaks 10 rows higher */
#define SPLASH_BY_Y     30
#define SPLASH_NAME_Y   41
#define SPLASH_FOOT_Y   56
#define SPLASH_X0       6  /* trace from here ... */
#define SPLASH_X1       (OLED_W - 7) /* ... to here */
#define BOOT_DRAW_MS    1000u /* the trace draws itself across */
#define BYE_DOT_MS      400u  /* "Powering off" gains a dot every 0.4 s */

/* A flat line with one heartbeat in the middle, drawn left of column reveal. */
static void splash_trace(int reveal)
{
    const int bx = (OLED_W - BEAT_PX) / 2, y = SPLASH_TRACE_Y;
    gfx_hline(SPLASH_X0, bx, y, GFX_SET);
    for (int i = 1; i < NPTS(ecg_beat); i++) /* 1 px: a crisp spike */
        gfx_line(bx + ecg_beat[i - 1].x, y + ecg_beat[i - 1].y * 5 / 8, bx + ecg_beat[i].x,
                 y + ecg_beat[i].y * 5 / 8, GFX_SET);
    gfx_hline(bx + BEAT_PX, SPLASH_X1, y, GFX_SET);
    gfx_fill(reveal, y - 11, OLED_W, 16, GFX_CLR);
}

static void splash_credit(void)
{
    gfx_med(OLED_W / 2, SPLASH_TITLE_Y, "ECG Card", TXT_CENTER);
    gfx_text(OLED_W / 2, SPLASH_BY_Y, SPLASH_BY, TXT_CENTER);
    gfx_med(OLED_W / 2, SPLASH_NAME_Y, SPLASH_NAME, TXT_CENTER);
}

static void screen_boot(const ui_model_t *m)
{
    splash_credit();
    unsigned t = m->screen_ms < BOOT_DRAW_MS ? m->screen_ms : BOOT_DRAW_MS;
    splash_trace(SPLASH_X0 + (int)(t * (unsigned)(SPLASH_X1 + 2 - SPLASH_X0) / BOOT_DRAW_MS));
    if (m->fw_version) {
        /* "2026  v0.1.0" without copying a string of unknown length */
        int wy = gfx_text_width(CREDIT_YEAR, 0) + 9;
        int wv = gfx_text_width("v", 0) + 1;
        int x = OLED_W / 2 - (wy + wv + gfx_text_width(m->fw_version, 0)) / 2;
        gfx_text(x, SPLASH_FOOT_Y, CREDIT_YEAR, TXT_LEFT);
        gfx_text(x + wy, SPLASH_FOOT_Y, "v", TXT_LEFT);
        gfx_text(x + wy + wv, SPLASH_FOOT_Y, m->fw_version, TXT_LEFT);
    } else {
        gfx_text(OLED_W / 2, SPLASH_FOOT_Y, CREDIT_YEAR, TXT_CENTER);
    }
}

static void screen_power_down(const ui_model_t *m)
{
    splash_credit();
    splash_trace(OLED_W);
    /* the words stay put while the dots appear */
    int x = OLED_W / 2 - gfx_text_width("Powering off...", 0) / 2;
    x += gfx_text(x, SPLASH_FOOT_Y, "Powering off", TXT_LEFT) + 1;
    unsigned dots = 1u + m->screen_ms / BYE_DOT_MS;
    gfx_text_n(x, SPLASH_FOOT_Y, "...", (int)(dots < 3u ? dots : 3u), TXT_LEFT);
}

/* ---- overlays: full-screen, framed ---------------------------------------- */

static void overlay(const ui_model_t *m)
{
    const int tx = 32;
    gfx_begin(gfx_fb());
    gfx_rrect(0, 0, OLED_W, OLED_H, GFX_SET);
    switch (m->overlay) {
    case UI_OVERLAY_USB_STOPPED:
        gfx_icon(8, 22, &icon_usb_big, GFX_SET);
        gfx_text(tx, 10, "Stopped", TXT_BOLD);
        gfx_text(tx, 23, "USB connected.", TXT_LEFT);
        gfx_text(tx, 34, "ECG runs on", TXT_LEFT);
        gfx_text(tx, 43, "battery only.", TXT_LEFT);
        break;
    case UI_OVERLAY_LOW_BATTERY:
    case UI_OVERLAY_BATT_CRITICAL: {
        bool crit = m->overlay == UI_OVERLAY_BATT_CRITICAL;
        gfx_rrect(7, 26, 16, 10, GFX_SET);
        gfx_fill(23, 29, 2, 4, GFX_SET);
        gfx_fill(9, 28, 2, 6, GFX_SET);
        gfx_text(tx, 10, crit ? "Battery empty" : "Battery low", TXT_BOLD);
        gfx_text(tx, 23, crit ? "Shutting down" : "Measuring is off.", TXT_LEFT);
        gfx_text(tx, 34, crit ? "Charge before" : "Charge the card", TXT_LEFT);
        gfx_text(tx, 43, crit ? "next use." : "to measure.", TXT_LEFT);
        break;
    }
    case UI_OVERLAY_POWER_OFF:
        gfx_text(OLED_W / 2, 10, "Power off", TXT_CENTER | TXT_BOLD);
        if (m->hold_progress >= 100) {
            gfx_text(OLED_W / 2, 24, "Release the button", TXT_CENTER);
        } else {
            int w = gfx_text_width("Keep holding  " G_SEL, 0);
            gfx_text(OLED_W / 2 - w / 2, 24, "Keep holding  " G_SEL, TXT_LEFT);
        }
        gfx_progress(14, 40, OLED_W - 28, 10, clamp_u(m->hold_progress, 100));
        break;
    case UI_OVERLAY_FAULT:
        gfx_icon(6, 5, &icon_warn, GFX_SET);
        gfx_text(24, 7, "Fault", TXT_BOLD);
        gfx_text_wrap(6, 21, OLED_W - 12, m->fault_text ? m->fault_text : "Unknown fault", 4,
                      TXT_LEFT);
        break;
    default:
        break;
    }
}

/* ---- entry --------------------------------------------------------------- */

void ui_render(const ui_model_t *m, uint8_t fb[OLED_FB_BYTES])
{
    gfx_begin(fb);
    if (!m)
        return;
    if (m->overlay > UI_OVERLAY_NONE && m->overlay <= UI_OVERLAY_FAULT) {
        overlay(m); /* modal: replaces the screen */
        return;
    }
    switch (m->screen) {
    case UI_SCREEN_HOME: screen_home(m); break;
    case UI_SCREEN_ECG_CARD:
    case UI_SCREEN_PPG_CARD:
    case UI_SCREEN_BOTH_CARD: screen_card(m); break;
    case UI_SCREEN_SETTINGS: screen_settings(m); break;
    case UI_SCREEN_ABOUT: screen_about(m); break;
    case UI_SCREEN_ECG:
    case UI_SCREEN_PPG:
    case UI_SCREEN_BOTH: screen_live(m); break;
    case UI_SCREEN_SUMMARY: screen_summary(m); break;
    case UI_SCREEN_DIAG: screen_diag(m); break;
    case UI_SCREEN_BOOT: screen_boot(m); break;
    case UI_SCREEN_POWER_DOWN: screen_power_down(m); break;
    default: screen_home(m); break;
    }
}
