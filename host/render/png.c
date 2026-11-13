/* Minimal PNG writer: 8-bit greyscale, zlib stored (uncompressed) blocks. */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    c ^= 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uint8_t *tmp = malloc(len + 4);
    memcpy(tmp, type, 4);
    if (len)
        memcpy(tmp + 4, data, len);
    uint32_t c = crc(0, tmp, len + 4);
    free(tmp);
    uint8_t t[4];
    be32(t, c);
    fwrite(t, 1, 4, f);
}

int png_write_gray(const char *path, const uint8_t *pix, int w, int h)
{
    static int init;
    if (!init) {
        crc_init();
        init = 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 0;  /* greyscale */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (size_t)(w + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (w + 1)] = 0; /* filter: none */
        memcpy(raw + (size_t)y * (w + 1) + 1, pix + (size_t)y * w, (size_t)w);
    }
    size_t nblocks = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(z_len), *q = z;
    *q++ = 0x78;
    *q++ = 0x01;
    size_t off = 0;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    do {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *q++ = (uint8_t)(off + n == raw_len ? 1 : 0);
        *q++ = (uint8_t)n;
        *q++ = (uint8_t)(n >> 8);
        *q++ = (uint8_t)~n;
        *q++ = (uint8_t)(~n >> 8);
        memcpy(q, raw + off, n);
        q += n;
        off += n;
    } while (off < raw_len);
    be32(q, (b << 16) | a);
    q += 4;
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    free(raw);
    free(z);
    return fclose(f);
}
