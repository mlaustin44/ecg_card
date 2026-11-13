/* Render fw/ui screens to PNG for design review.
 *
 *   ui_render_tool [out_dir]      (default build/render)
 *
 * Writes <scenario>.png (x5, OLED-like pixel grid), sweep sequences
 * seq_*.png, font.png, and contact sheets sheet.png / sheet_seq.png. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gfx.h"
#include "trace.h"
#include "png.h"
#include "scenarios.h"
#include "ui_assets.h"
#include "ui_model.h"

#define LIT 235
#define OFF 16
#define GRID 4

static const char *out_dir = "build/render";

static int fb_px(const uint8_t *fb, int x, int y)
{
    return (fb[(y >> 3) * OLED_W + x] >> (y & 7)) & 1;
}

/* Draw a frame into an image at (ox, oy), scale s (grid line if s >= 4). */
static void blit_frame(uint8_t *img, int iw, int ox, int oy, const uint8_t *fb, int s)
{
    for (int y = 0; y < OLED_H * s; y++)
        for (int x = 0; x < OLED_W * s; x++) {
            int on = fb_px(fb, x / s, y / s);
            uint8_t v = on ? LIT : OFF;
            if (s >= 4 && (x % s == s - 1 || y % s == s - 1))
                v = on ? (uint8_t)(LIT - 60) : GRID;
            img[(size_t)(oy + y) * iw + ox + x] = v;
        }
}

/* Label text with the UI's own small font, scale s. */
static void img_text(uint8_t *img, int iw, int ih, int x, int y, const char *str, int s,
                     uint8_t col)
{
    for (; *str; str++) {
        unsigned c = (unsigned char)*str;
        if (c < FONT5_FIRST || c >= FONT5_FIRST + FONT5_COUNT)
            c = '?';
        c -= FONT5_FIRST;
        int w = font5_width[c];
        for (int cx = 0; cx < w; cx++)
            for (int r = 0; r < 8; r++)
                if (font5_bits[c][cx] >> r & 1)
                    for (int dy = 0; dy < s; dy++)
                        for (int dx = 0; dx < s; dx++) {
                            int px = x + cx * s + dx, py = y + r * s + dy;
                            if (px >= 0 && px < iw && py >= 0 && py < ih)
                                img[(size_t)py * iw + px] = col;
                        }
        x += (w + 1) * s;
    }
}

static void write_frame(const char *name, const uint8_t *fb)
{
    const int s = 5;
    int w = OLED_W * s, h = OLED_H * s;
    uint8_t *img = malloc((size_t)w * h);
    blit_frame(img, w, 0, 0, fb, s);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", out_dir, name);
    if (png_write_gray(path, img, w, h) != 0)
        fprintf(stderr, "cannot write %s\n", path);
    free(img);
}

/* ---- contact sheets ------------------------------------------------------- */

#define MAX_TILES 128
typedef struct {
    char name[48];
    uint8_t fb[OLED_FB_BYTES];
} tile_t;

static void write_sheet(const char *file, const tile_t *t, int n, int cols)
{
    const int s = 3, pad = 12, lab = 22;
    int tw = OLED_W * s, th = OLED_H * s;
    int rows = (n + cols - 1) / cols;
    int iw = pad + cols * (tw + pad), ih = pad + rows * (th + lab + pad);
    uint8_t *img = malloc((size_t)iw * ih);
    memset(img, 60, (size_t)iw * ih);
    for (int i = 0; i < n; i++) {
        int cx = pad + (i % cols) * (tw + pad);
        int cy = pad + (i / cols) * (th + lab + pad);
        img_text(img, iw, ih, cx, cy, t[i].name, 2, 250);
        blit_frame(img, iw, cx, cy + lab, t[i].fb, s);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", out_dir, file);
    png_write_gray(path, img, iw, ih);
    free(img);
    printf("%s (%d frames)\n", path, n);
}

static tile_t tiles[MAX_TILES];
static int ntiles;
static tile_t seq[MAX_TILES];
static int nseq;

static void add(tile_t *arr, int *n, const char *name, const uint8_t *fb)
{
    if (*n >= MAX_TILES)
        return;
    snprintf(arr[*n].name, sizeof arr[*n].name, "%s", name);
    memcpy(arr[*n].fb, fb, OLED_FB_BYTES);
    (*n)++;
}

/* ---- sequences -------------------------------------------------------------- */

static void sequence(const char *prefix, ui_screen_t screen, uint8_t sweep,
                     const sim_sig_t *sig, const sim_sig_t *sig2, uint32_t switch_ms,
                     uint32_t step_ms, int frames)
{
    ui_model_t m;
    uint8_t fb[OLED_FB_BYTES];
    scen_defaults(&m);
    m.screen = screen;
    m.settings.sweep = sweep;
    m.live.state = UI_MEAS_RUNNING;
    m.live.quality = 4;
    m.live.hr_bpm = (uint16_t)sig->hr_bpm;
    m.live.pulse_bpm = (uint16_t)sig->hr_bpm;
    m.live.pat_ms = (uint16_t)sig->pat_ms;
    m.live.pi_permille = 18;
    sim_begin(&m, sig);
    uint32_t t = 0;
    for (int i = 0; i < frames; i++) {
        uint32_t next = (uint32_t)(i + 1) * step_ms;
        if (sig2 && t < switch_ms && next >= switch_ms) {
            sim_run(&m, switch_ms - t);
            sim_set(sig2);
            m.live.hr_bpm = m.live.pulse_bpm = (uint16_t)sig2->hr_bpm;
            t = switch_ms;
        }
        sim_run(&m, next - t);
        t = next;
        ui_render(&m, fb);
        char name[64];
        snprintf(name, sizeof name, "%s_%05u", prefix, (unsigned)t);
        write_frame(name, fb);
        char label[64];
        snprintf(label, sizeof label, "%s t=%u.%02us", prefix, (unsigned)(t / 1000),
                 (unsigned)(t % 1000 / 10));
        add(seq, &nseq, label, fb);
    }
}

static void font_sheet(void)
{
    uint8_t fb[OLED_FB_BYTES];
    gfx_begin(fb);
    char line[24];
    int y = 0;
    for (int c = FONT5_FIRST; c < FONT5_FIRST + FONT5_COUNT; c += 21) {
        int k = 0;
        for (int i = c; i < c + 21 && i < FONT5_FIRST + FONT5_COUNT; i++)
            line[k++] = (char)i;
        line[k] = 0;
        gfx_text(0, y, line, TXT_MONO);
        y += LINE_H;
    }
    gfx_big(0, y + 1, "0123456789-", TXT_LEFT);
    write_frame("font", fb);
    add(tiles, &ntiles, "font", fb);

    gfx_begin(fb);
    gfx_text(0, 0, "The quick brown fox", TXT_LEFT);
    gfx_text(0, 9, "jumps over the lazy dog", TXT_LEFT);
    gfx_text(0, 18, "THE QUICK BROWN FOX", TXT_BOLD);
    gfx_text(0, 27, "Bold: Settings ECG Pulse", TXT_BOLD);
    gfx_text(0, 36, "0.5mV 1098x 2.3% 214ms", TXT_LEFT);
    gfx_text(0, 45, "gypsy jq 72 bpm (x){}", TXT_LEFT);
    gfx_text(0, 55, G_SEL "Start " G_NEXT " Next " G_HEART G_MICRO "V", TXT_LEFT);
    write_frame("font_text", fb);
    add(tiles, &ntiles, "font_text", fb);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        out_dir = argv[1];
    if (mkdir(out_dir, 0777) != 0 && errno != EEXIST) {
        perror(out_dir);
        return 1;
    }
    uint8_t fb[OLED_FB_BYTES];
    font_sheet();
    for (int i = 0; i < n_scenarios; i++) {
        ui_model_t m;
        scenarios[i].setup(&m);
        ui_render(&m, fb);
        write_frame(scenarios[i].name, fb);
        add(tiles, &ntiles, scenarios[i].name, fb);
    }
    write_sheet("sheet.png", tiles, ntiles, 5);

    static const sim_sig_t normal = {72, 1000, 1500, 210, 12, 60, 8};
    static const sim_sig_t tiny = {64, 300, 300, 230, 6, 20, 5};
    static const sim_sig_t big = {88, 2400, 3000, 190, 15, 80, 10};
    sequence("seq_ecg3s", UI_SCREEN_ECG, SWEEP_3S, &normal, NULL, 0, 400, 10);
    sequence("seq_ecg1s5", UI_SCREEN_ECG, SWEEP_1S5, &normal, NULL, 0, 400, 5);
    sequence("seq_ecg6s", UI_SCREEN_ECG, SWEEP_6S, &normal, NULL, 0, 1500, 5);
    sequence("seq_rescale", UI_SCREEN_ECG, SWEEP_3S, &tiny, &big, 9000, 1500, 10);
    sequence("seq_both", UI_SCREEN_BOTH, SWEEP_3S, &normal, NULL, 0, 750, 5);
    sequence("seq_ppg1s5", UI_SCREEN_PPG, SWEEP_1S5, &normal, NULL, 0, 500, 5);
    write_sheet("sheet_seq.png", seq, nseq, 5);
    return 0;
}
