/* gfx: clipping, primitives, text measurement, number formatting. */
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "test.h"

/* Frame buffer with guard bytes either side (ASan also sees the exact-size
 * heap buffer used in test_screens). */
#define GUARD 64
static uint8_t mem[GUARD + OLED_FB_BYTES + GUARD];
static uint8_t *fb = mem + GUARD;

static void begin(void)
{
    memset(mem, 0xA5, sizeof mem);
    gfx_begin(fb);
}

static bool guards_ok(void)
{
    for (int i = 0; i < GUARD; i++)
        if (mem[i] != 0xA5 || mem[GUARD + OLED_FB_BYTES + i] != 0xA5)
            return false;
    return true;
}

static int px(int x, int y) { return (fb[(y >> 3) * OLED_W + x] >> (y & 7)) & 1; }

static int count(void)
{
    int n = 0;
    for (int i = 0; i < OLED_FB_BYTES; i++)
        n += __builtin_popcount(fb[i]);
    return n;
}

static int count_rect(int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            n += px(x, y);
    return n;
}

TEST(pixel_clipping)
{
    begin();
    const int xs[] = {-100000, -1, 0, 127, 128, 100000};
    const int ys[] = {-100000, -1, 0, 63, 64, 100000};
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++)
            gfx_pixel(xs[i], ys[j], GFX_SET);
    CHECK(guards_ok());
    CHECK_EQ(count(), 4); /* only the four corners */
    CHECK(px(0, 0) && px(127, 0) && px(0, 63) && px(127, 63));
    gfx_pixel(0, 0, GFX_INV);
    gfx_pixel(127, 63, GFX_CLR);
    CHECK_EQ(count(), 2);
}

TEST(fill_clipping)
{
    begin();
    gfx_fill(-10, -10, 20, 20, GFX_SET);
    CHECK_EQ(count(), 100);
    CHECK_EQ(count_rect(0, 0, 9, 9), 100);
    gfx_fill(120, 60, 50, 50, GFX_SET);
    CHECK_EQ(count(), 100 + 8 * 4);
    gfx_fill(-5000, -5000, 10000, 10000, GFX_SET);
    CHECK_EQ(count(), OLED_W * OLED_H);
    gfx_fill(0, 0, OLED_W, OLED_H, GFX_INV);
    CHECK_EQ(count(), 0);
    gfx_fill(10, 10, 0, 5, GFX_SET);
    gfx_fill(10, 10, -3, 5, GFX_SET);
    gfx_fill(200, 10, 5, 5, GFX_SET);
    CHECK_EQ(count(), 0);
    CHECK(guards_ok());
}

TEST(vline_every_span)
{
    /* every (y0, y1) pair, including off-screen ends, against a reference */
    for (int y0 = -3; y0 < OLED_H + 3; y0++)
        for (int y1 = -3; y1 < OLED_H + 3; y1++) {
            begin();
            gfx_vline(5, y0, y1, GFX_SET);
            int lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
            if (lo < 0)
                lo = 0;
            if (hi > OLED_H - 1)
                hi = OLED_H - 1;
            int want = hi >= lo ? hi - lo + 1 : 0;
            if (count() != want || (want && (!px(5, lo) || !px(5, hi)))) {
                CHECK_EQ(count(), want);
                return;
            }
        }
    CHECK(guards_ok());
}

TEST(lines_and_shapes_off_screen)
{
    begin();
    gfx_line(-1000, -1000, 1000, 1000, GFX_SET);
    gfx_line(-100000, 5, 100000, 5, GFX_SET); /* out of range: ignored */
    gfx_line(127, -50, 0, 120, GFX_SET);
    gfx_rect(-5, -5, 300, 300, GFX_SET);
    gfx_rrect(100, 50, 60, 60, GFX_SET);
    gfx_rrect_fill(-20, 40, 30, 40, GFX_INV);
    gfx_progress(-50, 60, 400, 20, 250);
    gfx_dotted_hline(-10, 300, 63, 3);
    CHECK(guards_ok());
    begin();
    gfx_line(0, 0, 63, 63, GFX_SET); /* diagonal: one pixel per column */
    CHECK_EQ(count(), 64);
    CHECK(px(63, 63) && px(31, 31));
}

TEST(bitmap_clipping)
{
    static const uint8_t bits[3 * 2] = {0xFF, 0xFF, 0xFF, 0x0F, 0x0F, 0x0F}; /* 3 x 12 */
    begin();
    gfx_bitmap(10, 3, bits, 3, 12, GFX_SET);
    CHECK_EQ(count(), 36);
    CHECK_EQ(count_rect(10, 3, 12, 14), 36);
    begin();
    for (int x = -5; x <= OLED_W + 2; x++)
        for (int y = -14; y <= OLED_H + 2; y += 3)
            gfx_bitmap(x, y, bits, 3, 12, GFX_INV);
    CHECK(guards_ok());
    begin();
    gfx_bitmap(-1, -5, bits, 3, 12, GFX_SET); /* 2 cols x 7 rows visible */
    CHECK_EQ(count(), 14);
    gfx_icon(120, 58, &icon_warn, GFX_SET);
    gfx_icon(-6, -6, &icon_usb_big, GFX_SET);
    CHECK(guards_ok());
}

TEST(text_measure_and_align)
{
    CHECK_EQ(gfx_text_width("", 0), 0);
    CHECK_EQ(gfx_text_width("A", 0), 5);
    CHECK_EQ(gfx_text_width("AB", 0), 11);
    CHECK_EQ(gfx_text_width("A", TXT_BOLD), 6);
    CHECK_EQ(gfx_text_width("i", 0), 1);
    CHECK_EQ(gfx_text_width("il", TXT_MONO), 11);
    CHECK_EQ(gfx_text_width(NULL, 0), 0);
    /* all digits are the same width (numbers don't jitter) */
    for (char c = '0'; c <= '9'; c++)
        CHECK_EQ(gfx_char_width(c, 0), 5);
    begin();
    gfx_text(OLED_W, 0, "H", TXT_RIGHT);
    CHECK(px(127, 0) && px(123, 0) && !px(122, 3));
    begin();
    gfx_text(64, 0, "H", TXT_CENTER); /* 5 wide: x 62..66 */
    CHECK(px(62, 3) && px(66, 3) && !px(61, 3) && !px(67, 3));
    begin();
    int w = gfx_text(0, 0, "H", TXT_CLR);
    CHECK_EQ(w, 5);
    CHECK_EQ(count(), 0);
    /* unknown characters draw as '?' and never index out of the font */
    begin();
    gfx_text(0, 0, "\x01\x7f\xff\x90", TXT_LEFT);
    CHECK(count() > 0);
}

TEST(text_clipping)
{
    begin();
    gfx_text_clipped = 0;
    gfx_text(-3, -4, "Clipped text", TXT_BOLD);
    gfx_text(100, 60, "Clipped", TXT_LEFT);
    gfx_text(-1000, 1000, "x", TXT_LEFT);
    gfx_text_n(0, 0, "abc", -5, TXT_LEFT);
    gfx_big(120, 55, "205", TXT_LEFT);
    gfx_big(-30, -10, "-8-", TXT_RIGHT);
    CHECK(guards_ok());
    CHECK_EQ(gfx_text_clipped, 5);
    gfx_text_clipped = 0;
    begin();
    gfx_text(0, 0, "fits", TXT_LEFT);
    gfx_text(OLED_W, 56, "fits", TXT_RIGHT);
    CHECK_EQ(gfx_text_clipped, 0);
}

TEST(big_digits)
{
    CHECK_EQ(gfx_big_width("72"), 2 * BIG_ADV - 2);
    begin();
    gfx_big(0, 0, "8", TXT_LEFT);
    CHECK(count_rect(0, 0, FONTBIG_W - 1, FONTBIG_H - 1) == count());
    CHECK(px(0, 3) && px(8, 3) && px(4, 0) && px(4, 13));
    begin();
    gfx_big(0, 0, " ", TXT_LEFT);
    CHECK_EQ(count(), 0);
}

TEST(wrap)
{
    begin();
    CHECK_EQ(gfx_text_wrap(0, 0, 128, "short", 3, 0), 1);
    CHECK_EQ(gfx_text_wrap(0, 0, 40, "one two three four", 4, 0), 3);
    begin();
    /* overflow: truncated with an ellipsis on the last line, nothing below */
    int n = gfx_text_wrap(0, 0, 60, "this text is far too long to fit in two lines here", 2, 0);
    CHECK_EQ(n, 2);
    CHECK_EQ(count_rect(0, 18, 127, 63), 0);
    begin();
    CHECK_EQ(gfx_text_wrap(0, 0, 20, "Supercalifragilistic", 5, 0), 5); /* hard breaks */
    CHECK_EQ(gfx_text_wrap(0, 0, 50, NULL, 3, 0), 0);
    CHECK_EQ(gfx_text_wrap(0, 0, 50, "   ", 3, 0), 0);
    CHECK_EQ(gfx_text_wrap(0, 0, 0, "xy", 3, 0), 2); /* zero width: 1 char per line */
    CHECK(guards_ok());
}

TEST(number_formatting)
{
    char b[24];
    str_u(b, 0);
    CHECK(strcmp(b, "0") == 0);
    str_u(b, 7);
    CHECK(strcmp(b, "7") == 0);
    str_u(b, 65535);
    CHECK(strcmp(b, "65535") == 0);
    str_u(b, 4294967295u);
    CHECK(strcmp(b, "4294967295") == 0);
    str_u_dec1(b, 23);
    CHECK(strcmp(b, "2.3") == 0);
    str_u_dec1(b, 5);
    CHECK(strcmp(b, "0.5") == 0);
    str_u_dec1(b, 1000);
    CHECK(strcmp(b, "100.0") == 0);
    str_mmss(b, 0);
    CHECK(strcmp(b, "0:00") == 0);
    str_mmss(b, 65);
    CHECK(strcmp(b, "1:05") == 0);
    str_mmss(b, 3599);
    CHECK(strcmp(b, "59:59") == 0);
    char *e = str_cpy(b, "PAT ");
    e = str_u(e, 214);
    str_cpy(e, "ms");
    CHECK(strcmp(b, "PAT 214ms") == 0);
}

int main(void)
{
    RUN(pixel_clipping);
    RUN(fill_clipping);
    RUN(vline_every_span);
    RUN(lines_and_shapes_off_screen);
    RUN(bitmap_clipping);
    RUN(text_measure_and_align);
    RUN(text_clipping);
    RUN(big_digits);
    RUN(wrap);
    RUN(number_formatting);
    return TEST_RESULT();
}
