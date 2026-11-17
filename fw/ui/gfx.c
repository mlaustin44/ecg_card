#include "gfx.h"

#include <string.h>

static uint8_t *fb_;
uint16_t gfx_text_clipped;
gfx_text_hook_t gfx_text_hook;

void gfx_begin(uint8_t *fb)
{
    fb_ = fb;
    memset(fb, 0, OLED_FB_BYTES);
}

uint8_t *gfx_fb(void) { return fb_; }

static inline void apply(uint8_t *p, uint8_t mask, uint8_t mode)
{
    if (mode == GFX_SET)
        *p |= mask;
    else if (mode == GFX_CLR)
        *p &= (uint8_t)~mask;
    else
        *p ^= mask;
}

void gfx_pixel(int x, int y, uint8_t mode)
{
    if ((unsigned)x >= OLED_W || (unsigned)y >= OLED_H)
        return;
    apply(&fb_[(y >> 3) * OLED_W + x], (uint8_t)(1u << (y & 7)), mode);
}

/* Clipped vertical span in one column: the trace inner loop uses this. */
void gfx_vline(int x, int y0, int y1, uint8_t mode)
{
    if ((unsigned)x >= OLED_W)
        return;
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (y0 < 0)
        y0 = 0;
    if (y1 > OLED_H - 1)
        y1 = OLED_H - 1;
    if (y0 > y1)
        return;
    int p0 = y0 >> 3, p1 = y1 >> 3;
    uint8_t m0 = (uint8_t)(0xFFu << (y0 & 7));
    uint8_t m1 = (uint8_t)(0xFFu >> (7 - (y1 & 7)));
    uint8_t *p = &fb_[p0 * OLED_W + x];
    if (p0 == p1) {
        apply(p, m0 & m1, mode);
        return;
    }
    apply(p, m0, mode);
    for (int pg = p0 + 1; pg < p1; pg++) {
        p += OLED_W;
        apply(p, 0xFF, mode);
    }
    apply(p + OLED_W, m1, mode);
}

void gfx_fill(int x, int y, int w, int h, uint8_t mode)
{
    int x1 = x + w - 1, y1 = y + h - 1;
    if (w <= 0 || h <= 0)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > OLED_W - 1)
        x1 = OLED_W - 1;
    if (y1 > OLED_H - 1)
        y1 = OLED_H - 1;
    if (x > x1 || y > y1)
        return;
    for (int pg = y >> 3; pg <= (y1 >> 3); pg++) {
        uint8_t m = 0xFF;
        if (pg == (y >> 3))
            m &= (uint8_t)(0xFFu << (y & 7));
        if (pg == (y1 >> 3))
            m &= (uint8_t)(0xFFu >> (7 - (y1 & 7)));
        uint8_t *p = &fb_[pg * OLED_W];
        for (int xx = x; xx <= x1; xx++)
            apply(&p[xx], m, mode);
    }
}

void gfx_hline(int x0, int x1, int y, uint8_t mode)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    gfx_fill(x0, y, x1 - x0 + 1, 1, mode);
}

void gfx_dotted_hline(int x0, int x1, int y, int step)
{
    if (step < 1)
        step = 1;
    for (int x = x0; x <= x1; x += step)
        gfx_pixel(x, y, GFX_SET);
}

void gfx_rect(int x, int y, int w, int h, uint8_t mode)
{
    if (w <= 0 || h <= 0)
        return;
    gfx_hline(x, x + w - 1, y, mode);
    if (h > 1)
        gfx_hline(x, x + w - 1, y + h - 1, mode);
    if (h > 2) {
        gfx_vline(x, y + 1, y + h - 2, mode);
        if (w > 1)
            gfx_vline(x + w - 1, y + 1, y + h - 2, mode);
    }
}

void gfx_rrect(int x, int y, int w, int h, uint8_t mode)
{
    if (w < 3 || h < 3) {
        gfx_rect(x, y, w, h, mode);
        return;
    }
    gfx_hline(x + 1, x + w - 2, y, mode);
    gfx_hline(x + 1, x + w - 2, y + h - 1, mode);
    gfx_vline(x, y + 1, y + h - 2, mode);
    gfx_vline(x + w - 1, y + 1, y + h - 2, mode);
}

void gfx_rrect_fill(int x, int y, int w, int h, uint8_t mode)
{
    if (w < 3 || h < 3) {
        gfx_fill(x, y, w, h, mode);
        return;
    }
    gfx_fill(x + 1, y, w - 2, h, mode);
    gfx_vline(x, y + 1, y + h - 2, mode);
    gfx_vline(x + w - 1, y + 1, y + h - 2, mode);
}

#define LINE_LIMIT 2048

void gfx_line(int x0, int y0, int x1, int y1, uint8_t mode)
{
    if (x0 < -LINE_LIMIT || x0 > LINE_LIMIT || x1 < -LINE_LIMIT || x1 > LINE_LIMIT ||
        y0 < -LINE_LIMIT || y0 > LINE_LIMIT || y1 < -LINE_LIMIT || y1 > LINE_LIMIT)
        return; /* never on screen in practice; keeps the loop bounded */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0; /* negative */
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, mode);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_bitmap(int x, int y, const uint8_t *bits, int w, int h, uint8_t mode)
{
    if (w <= 0 || h <= 0 || x >= OLED_W || y >= OLED_H || x + w <= 0 || y + h <= 0)
        return;
    int cx0 = x < 0 ? -x : 0;
    int cx1 = x + w > OLED_W ? OLED_W - x : w;
    int pages = (h + 7) >> 3;
    for (int sp = 0; sp < pages; sp++) {
        int dy = y + sp * 8;               /* top row of this source page */
        if (dy >= OLED_H || dy + 8 <= 0)
            continue;
        uint8_t lastmask = 0xFF;
        if (sp == pages - 1 && (h & 7))
            lastmask = (uint8_t)(0xFFu >> (8 - (h & 7)));
        int dp = dy >= 0 ? dy >> 3 : -((-dy + 7) >> 3); /* floor(dy / 8) */
        int sh = dy - dp * 8;                            /* 0..7 */
        const uint8_t *src = bits + sp * w;
        for (int cx = cx0; cx < cx1; cx++) {
            uint8_t b = src[cx] & lastmask;
            if (!b)
                continue;
            int col = x + cx;
            if (dp >= 0 && dp < OLED_H / 8)
                apply(&fb_[dp * OLED_W + col], (uint8_t)(b << sh), mode);
            if (sh && dp + 1 >= 0 && dp + 1 < OLED_H / 8)
                apply(&fb_[(dp + 1) * OLED_W + col], (uint8_t)(b >> (8 - sh)), mode);
        }
    }
}

void gfx_icon(int x, int y, const ui_icon_t *icon, uint8_t mode)
{
    gfx_bitmap(x, y, icon->bits, icon->w, icon->h, mode);
}

/* ---- small font -------------------------------------------------------- */

static unsigned glyph_index(char c)
{
    unsigned u = (unsigned char)c;
    if (u < FONT5_FIRST || u >= FONT5_FIRST + FONT5_COUNT)
        u = '?';
    return u - FONT5_FIRST;
}

int gfx_char_width(char c, uint8_t flags)
{
    if (flags & TXT_MONO)
        return 5;
    return font5_width[glyph_index(c)] + ((flags & TXT_BOLD) ? 1 : 0);
}

static int text_width_n(const char *s, int n, uint8_t flags)
{
    int w = 0, k = 0;
    for (; k < n && *s; s++, k++)
        w += gfx_char_width(*s, flags) + 1;
    return w ? w - 1 : 0;
}

int gfx_text_width(const char *s, uint8_t flags)
{
    return s ? text_width_n(s, 0x7FFF, flags) : 0;
}

int gfx_text_n(int x, int y, const char *s, int max_chars, uint8_t flags)
{
    if (!s)
        return 0;
    int w = text_width_n(s, max_chars, flags);
    if ((flags & TXT_ALIGN) == TXT_CENTER)
        x -= w / 2;
    else if ((flags & TXT_ALIGN) == TXT_RIGHT)
        x -= w;
    uint8_t mode = (flags & TXT_CLR) ? GFX_CLR : GFX_SET;
    if (gfx_text_hook)
        gfx_text_hook(s, max_chars, x, y, w, FONT5_CAP);
    if (y >= OLED_H || y + FONT5_H <= 0) {
        if (w)
            gfx_text_clipped++;
        return w;
    }
    if (y < 0 || y + FONT5_CAP > OLED_H || x < 0 || x + w > OLED_W)
        gfx_text_clipped++; /* layout check for tests; drawing still clips */
    for (int k = 0; k < max_chars && *s; s++, k++) {
        unsigned gi = glyph_index(*s);
        int gw = font5_width[gi];
        int gx = x;
        if (flags & TXT_MONO)
            gx += (5 - gw + 1) / 2;
        if (gx >= OLED_W)
            break;
        gfx_bitmap(gx, y, font5_bits[gi], gw, FONT5_H, mode);
        if (flags & TXT_BOLD)
            gfx_bitmap(gx + 1, y, font5_bits[gi], gw, FONT5_H, mode);
        x += gfx_char_width(*s, flags) + 1;
    }
    return w;
}

int gfx_text(int x, int y, const char *s, uint8_t flags)
{
    return gfx_text_n(x, y, s, 0x7FFF, flags);
}

/* ---- big digits -------------------------------------------------------- */

static unsigned big_index(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    return c == '-' ? 10u : 11u;
}

int gfx_big_width(const char *s)
{
    int n = (int)strlen(s);
    return n ? n * BIG_ADV - 2 : 0;
}

int gfx_big(int x, int y, const char *s, uint8_t flags)
{
    int w = gfx_big_width(s);
    if ((flags & TXT_ALIGN) == TXT_CENTER)
        x -= w / 2;
    else if ((flags & TXT_ALIGN) == TXT_RIGHT)
        x -= w;
    uint8_t mode = (flags & TXT_CLR) ? GFX_CLR : GFX_SET;
    if (x < 0 || x + w > OLED_W || y < 0 || y + FONTBIG_H > OLED_H)
        gfx_text_clipped++;
    for (; *s; s++, x += BIG_ADV)
        gfx_bitmap(x, y, fontbig_bits[big_index(*s)], FONTBIG_W, FONTBIG_H, mode);
    return w;
}

int gfx_big2_width(const char *s) { return 2 * gfx_big_width(s); }

int gfx_big2(int x, int y, const char *s, uint8_t flags)
{
    int w = gfx_big2_width(s);
    if ((flags & TXT_ALIGN) == TXT_CENTER)
        x -= w / 2;
    else if ((flags & TXT_ALIGN) == TXT_RIGHT)
        x -= w;
    uint8_t mode = (flags & TXT_CLR) ? GFX_CLR : GFX_SET;
    if (x < 0 || x + w > OLED_W || y < 0 || y + 2 * FONTBIG_H > OLED_H)
        gfx_text_clipped++;
    for (; *s; s++, x += 2 * BIG_ADV) {
        const uint8_t *g = fontbig_bits[big_index(*s)];
        for (int c = 0; c < FONTBIG_W; c++)
            for (int r = 0; r < FONTBIG_H; r++)
                if ((g[(r >> 3) * FONTBIG_W + c] >> (r & 7)) & 1u)
                    gfx_fill(x + 2 * c, y + 2 * r, 2, 2, mode);
    }
    return w;
}

/* ---- medium font -------------------------------------------------------- */

/* Glyph of c: index into FONTMED_CHARS and its first byte, or -1. */
static int med_glyph(char c, const uint8_t **bits)
{
    static const char k_chars[] = FONTMED_CHARS;
    unsigned off = 0;
    for (int i = 0; i < FONTMED_COUNT; i++) {
        if (k_chars[i] == c) {
            *bits = fontmed_bits + off;
            return i;
        }
        off += 2u * fontmed_width[i];
    }
    return -1;
}

int gfx_med_width(const char *s)
{
    int w = 0;
    const uint8_t *bits;
    for (; s && *s; s++) {
        int i = med_glyph(*s, &bits);
        if (i >= 0)
            w += fontmed_width[i] + MED_GAP;
    }
    return w ? w - MED_GAP : 0;
}

int gfx_med(int x, int y, const char *s, uint8_t flags)
{
    if (!s)
        return 0;
    int w = gfx_med_width(s);
    if ((flags & TXT_ALIGN) == TXT_CENTER)
        x -= w / 2;
    else if ((flags & TXT_ALIGN) == TXT_RIGHT)
        x -= w;
    uint8_t mode = (flags & TXT_CLR) ? GFX_CLR : GFX_SET;
    if (gfx_text_hook)
        gfx_text_hook(s, 0x7FFF, x, y, w, FONTMED_H);
    if (y < 0 || y + FONTMED_H > OLED_H || x < 0 || x + w > OLED_W)
        gfx_text_clipped++;
    for (; *s; s++) {
        const uint8_t *bits;
        int i = med_glyph(*s, &bits);
        if (i < 0) {
            gfx_text_clipped++; /* no glyph: add it in gen_assets.py */
            continue;
        }
        gfx_bitmap(x, y, bits, fontmed_width[i], FONTMED_H, mode);
        x += fontmed_width[i] + MED_GAP;
    }
    return w;
}

void gfx_progress(int x, int y, int w, int h, unsigned pct)
{
    if (pct > 100)
        pct = 100;
    gfx_rrect(x, y, w, h, GFX_SET);
    int inner = w - 4;
    int fill = inner > 0 ? (int)(((unsigned)inner * pct + 50u) / 100u) : 0;
    gfx_fill(x + 2, y + 2, fill, h - 4, GFX_SET);
}

/* ---- word wrap ---------------------------------------------------------- */

int gfx_text_wrap(int x, int y, int w, const char *s, int max_lines, uint8_t flags)
{
    if (!s || max_lines <= 0)
        return 0;
    int lines = 0;
    while (*s && lines < max_lines) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        /* longest prefix that fits, preferring a break at a space */
        int n = 0, best = 0, width = 0;
        while (s[n] && s[n] != '\n') {
            int cw = gfx_char_width(s[n], flags) + (n ? 1 : 0);
            if (width + cw > w)
                break;
            width += cw;
            n++;
            if (s[n] == ' ' || s[n] == '\n' || s[n] == '\0')
                best = n;
        }
        bool more = s[n] && s[n] != '\n';
        if (!more)
            best = n;
        else if (best == 0)
            best = n ? n : 1; /* a word longer than the line: hard break */
        bool last = lines == max_lines - 1;
        const char *rest = s + best;
        while (*rest == ' ')
            rest++;
        if (*rest == '\n')
            rest++;
        if (last && *rest) {
            /* truncate and add an ellipsis */
            int ew = gfx_char_width(G_ELLIPSIS[0], flags) + 1;
            int k = n;
            while (k > 0 && text_width_n(s, k, flags) + ew > w)
                k--;
            int tw = gfx_text_n(x, y, s, k, flags & (uint8_t)~TXT_ALIGN);
            gfx_text(x + tw + (k ? 1 : 0), y, G_ELLIPSIS, flags & (uint8_t)~TXT_ALIGN);
            return lines + 1;
        }
        int lx = x;
        if ((flags & TXT_ALIGN) == TXT_CENTER)
            lx = x + w / 2;
        else if ((flags & TXT_ALIGN) == TXT_RIGHT)
            lx = x + w;
        gfx_text_n(lx, y, s, best, flags);
        lines++;
        y += LINE_H;
        s = rest;
    }
    return lines;
}

/* ---- strings ------------------------------------------------------------ */

char *str_cpy(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = '\0';
    return d;
}

char *str_u(char *d, unsigned v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < 10);
    while (n)
        *d++ = tmp[--n];
    *d = '\0';
    return d;
}

char *str_u_dec1(char *d, unsigned v)
{
    d = str_u(d, v / 10u);
    *d++ = '.';
    *d++ = (char)('0' + v % 10u);
    *d = '\0';
    return d;
}

char *str_mmss(char *d, unsigned s)
{
    d = str_u(d, s / 60u);
    *d++ = ':';
    unsigned r = s % 60u;
    *d++ = (char)('0' + r / 10u);
    *d++ = (char)('0' + r % 10u);
    *d = '\0';
    return d;
}
