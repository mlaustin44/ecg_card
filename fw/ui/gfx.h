/* Minimal 1-bpp graphics on the SSD1306 page-layout frame buffer.
 * Every primitive clips to the 128 x 64 screen, so any coordinates are safe. */
#ifndef UI_GFX_H
#define UI_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "ui_assets.h"

enum { GFX_SET = 0, GFX_CLR = 1, GFX_INV = 2 };

/* Text flags */
#define TXT_LEFT   0x00u
#define TXT_CENTER 0x01u /* x is the centre */
#define TXT_RIGHT  0x02u /* x is one past the right edge */
#define TXT_ALIGN  0x03u
#define TXT_BOLD   0x04u /* double-struck, 1 px wider per glyph */
#define TXT_MONO   0x08u /* 6 px cells (Diagnostics) */
#define TXT_CLR    0x10u /* draw dark pixels (on a lit background) */

#define FONT5_H     8   /* cell height incl. descender row */
#define FONT5_CAP   7   /* cap height */
#define LINE_H      9   /* line pitch */

void gfx_begin(uint8_t *fb); /* sets the target and clears it */

/* Count of text runs drawn partly off-screen (layout check for tests). */
extern uint16_t gfx_text_clipped;
uint8_t *gfx_fb(void);

void gfx_pixel(int x, int y, uint8_t mode);
void gfx_hline(int x0, int x1, int y, uint8_t mode);
void gfx_vline(int x, int y0, int y1, uint8_t mode);
void gfx_fill(int x, int y, int w, int h, uint8_t mode);
void gfx_rect(int x, int y, int w, int h, uint8_t mode);
void gfx_rrect(int x, int y, int w, int h, uint8_t mode);      /* 1-px corner cut */
void gfx_rrect_fill(int x, int y, int w, int h, uint8_t mode);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t mode);
void gfx_dotted_hline(int x0, int x1, int y, int step);

/* Page-major bitmap (see ui_assets.h), lit bits drawn with mode. */
void gfx_bitmap(int x, int y, const uint8_t *bits, int w, int h, uint8_t mode);
void gfx_icon(int x, int y, const ui_icon_t *icon, uint8_t mode);

/* Small font. Returns the drawn width. */
int gfx_text(int x, int y, const char *s, uint8_t flags);
int gfx_text_n(int x, int y, const char *s, int max_chars, uint8_t flags);
int gfx_text_width(const char *s, uint8_t flags);
int gfx_char_width(char c, uint8_t flags);

/* Big digits ('0'-'9', '-', ' '). Returns the drawn width. */
#define BIG_ADV (FONTBIG_W + 2)
int gfx_big(int x, int y, const char *s, uint8_t flags);
int gfx_big_width(const char *s);

/* Horizontal progress bar with 1 px border; pct 0..100. */
void gfx_progress(int x, int y, int w, int h, unsigned pct);

/* Word-wrap s into at most max_lines lines of width w starting at (x, y),
 * line pitch LINE_H. Overflow ends with an ellipsis. Returns lines used. */
int gfx_text_wrap(int x, int y, int w, const char *s, int max_lines, uint8_t flags);

/* ---- string building without printf ---------------------------------- */
char *str_cpy(char *d, const char *s);
char *str_u(char *d, unsigned v);               /* decimal */
char *str_u_dec1(char *d, unsigned v_tenths);   /* "12.3" */
char *str_mmss(char *d, unsigned seconds);      /* "1:05", "65:00" */

#endif
