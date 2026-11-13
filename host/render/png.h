#ifndef RENDER_PNG_H
#define RENDER_PNG_H

#include <stdint.h>

/* Write an 8-bit greyscale PNG. Returns 0 on success. */
int png_write_gray(const char *path, const uint8_t *pix, int w, int h);

#endif
