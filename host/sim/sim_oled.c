/* SPI1 bus to the OLED and an SSD1306 model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_priv.h"

static struct {
    bool powered, in_reset;
    bool reset_seen_since_power; /* in reset at/after power-up */
    bool reset_pulse_seen;
    bool dirty_counted;
    uint64_t power_on_us;
    uint8_t ram[8][128];
    /* registers */
    uint8_t mode; /* 0 horizontal, 1 vertical, 2 page */
    uint8_t col_lo, col_hi;
    uint8_t col_start, col_end, page_start, page_end;
    uint8_t col, page;
    uint8_t contrast, start_line, mux, offset;
    bool seg_remap, com_rev, entire_on, inverse, display_on, pump_on;
    /* command parser */
    uint8_t cmd, argc, argn;
    uint8_t args[8];
    /* visibility */
    bool lit;
    uint64_t lit_since_us;
    unsigned settle_ms;
    sim_oled_info_t info;
    /* DMA job */
    bool dma_active;
    const uint8_t *dma_buf;
    uint8_t *dma_snap;
    size_t dma_n, dma_done;
    uint64_t dma_start_us;
} O;

static void regs_reset(void)
{
    O.mode = 2;
    O.col_lo = O.col_hi = 0;
    O.col_start = 0;
    O.col_end = 127;
    O.page_start = 0;
    O.page_end = 7;
    O.col = 0;
    O.page = 0;
    O.contrast = 0x7F;
    O.start_line = 0;
    O.mux = 63;
    O.offset = 0;
    O.seg_remap = O.com_rev = O.entire_on = O.inverse = false;
    O.display_on = false;
    O.pump_on = false;
    O.cmd = 0;
    O.argc = O.argn = 0;
}

static void update_lit(void)
{
    bool lit = O.powered && !O.in_reset && O.display_on && O.pump_on;
    if (lit && !O.lit) O.lit_since_us = sim_now_us();
    O.lit = lit;
}

void oled_reset(void)
{
    if (O.dma_snap) free(O.dma_snap);
    memset(&O, 0, sizeof O);
    O.settle_ms = 100;
    regs_reset();
}

void oled_mcu_reset(void)
{
    if (O.dma_snap) free(O.dma_snap);
    O.dma_snap = NULL;
    O.dma_active = false;
}

static void fill_garbage(void)
{
    for (int p = 0; p < 8; p++)
        for (int c = 0; c < 128; c++) O.ram[p][c] = (uint8_t)(sim_rng_next() >> 24);
}

void oled_pin_changed(board_pin_t p, bool old_level, bool nw)
{
    (void)old_level;
    if (p == PIN_OLED_EN) {
        if (nw) {
            O.powered = true;
            O.power_on_us = sim_now_us();
            O.info.power_ups++;
            regs_reset();
            fill_garbage();
            O.in_reset = !sim_pin(PIN_OLED_RESET);
            O.reset_seen_since_power = O.in_reset;
            O.reset_pulse_seen = false;
            O.dirty_counted = false;
        } else {
            if (!O.dirty_counted && (O.display_on || O.pump_on)) O.info.dirty_power_offs++;
            O.dirty_counted = false;
            O.powered = false;
            O.in_reset = false;
            memset(O.ram, 0, sizeof O.ram);
            regs_reset();
        }
    } else if (p == PIN_OLED_RESET && O.powered) {
        if (!nw) {
            if (!O.dirty_counted && (O.display_on || O.pump_on)) {
                O.info.dirty_power_offs++;
                O.dirty_counted = true;
            }
            O.in_reset = true;
            O.reset_seen_since_power = true;
            regs_reset();
        } else if (O.in_reset) {
            O.in_reset = false;
            if (O.reset_seen_since_power) O.reset_pulse_seen = true;
            O.info.resets_released++;
            if (sim_now_us() - O.power_on_us < 5000) O.info.early_reset_releases++;
        }
    }
    update_lit();
}

/* -------------------------------------------------------------------- */

static void data_byte(uint8_t b)
{
    O.ram[O.page & 7][O.col & 127] = b;
    if (O.mode == 2) { /* page addressing: column wraps within the page */
        O.col = (uint8_t)((O.col + 1) & 127);
    } else if (O.mode == 0) { /* horizontal */
        if (O.col >= O.col_end) {
            O.col = O.col_start;
            O.page = (O.page >= O.page_end) ? O.page_start : (uint8_t)(O.page + 1);
        } else {
            O.col++;
        }
    } else { /* vertical */
        if (O.page >= O.page_end) {
            O.page = O.page_start;
            O.col = (O.col >= O.col_end) ? O.col_start : (uint8_t)(O.col + 1);
        } else {
            O.page++;
        }
    }
}

static void apply_cmd(void)
{
    uint8_t a0 = O.args[0], a1 = O.args[1];
    switch (O.cmd) {
    case 0x81: O.contrast = a0; break;
    case 0x20: O.mode = a0 & 3; break;
    case 0x21:
        O.col_start = a0 & 127;
        O.col_end = a1 & 127;
        O.col = O.col_start;
        break;
    case 0x22:
        O.page_start = a0 & 7;
        O.page_end = a1 & 7;
        O.page = O.page_start;
        break;
    case 0xA8: O.mux = a0 & 63; break;
    case 0xD3: O.offset = a0 & 63; break;
    case 0x8D: O.pump_on = (a0 & 0x04) != 0; break;
    default: break; /* clock, precharge, com pins, vcomh, scroll setup: accepted, no visual effect */
    }
}

static unsigned cmd_argc(uint8_t c)
{
    switch (c) {
    case 0x81: case 0x20: case 0xA8: case 0xD3: case 0xD5: case 0xD9: case 0xDA:
    case 0xDB: case 0x8D: case 0xAD: return 1;
    case 0x21: case 0x22: case 0xA3: return 2;
    case 0x26: case 0x27: return 6;
    case 0x29: case 0x2A: return 5;
    default: return 0;
    }
}

static void command_byte(uint8_t b)
{
    if (O.argc) {
        O.args[O.argn++] = b;
        if (O.argn == O.argc) {
            apply_cmd();
            O.argc = 0;
        }
        return;
    }
    if (b <= 0x0F) {
        O.col_lo = b & 0x0F;
        if (O.mode == 2) O.col = (uint8_t)((O.col_hi << 4) | O.col_lo);
    } else if (b <= 0x1F) {
        O.col_hi = b & 0x0F;
        if (O.mode == 2) O.col = (uint8_t)((O.col_hi << 4) | O.col_lo);
    } else if (b >= 0x40 && b <= 0x7F) {
        O.start_line = b & 63;
    } else if (b >= 0xB0 && b <= 0xB7) {
        O.page = b & 7;
    } else {
        switch (b) {
        case 0xA0: O.seg_remap = false; break;
        case 0xA1: O.seg_remap = true; break;
        case 0xA4: O.entire_on = false; break;
        case 0xA5: O.entire_on = true; break;
        case 0xA6: O.inverse = false; break;
        case 0xA7: O.inverse = true; break;
        case 0xAE: O.display_on = false; break;
        case 0xAF: O.display_on = true; break;
        case 0xC0: O.com_rev = false; break;
        case 0xC8: O.com_rev = true; break;
        case 0x2E: case 0x2F: case 0xE3: break;
        default:
            if (cmd_argc(b)) {
                O.cmd = b;
                O.argc = (uint8_t)cmd_argc(b);
                O.argn = 0;
            }
            break;
        }
    }
    update_lit();
}

/* One byte clocked in with the pin levels as they are now. */
static void spi_byte(uint8_t b)
{
    sim_facts_t *f = sim_facts_raw();
    if (!O.powered || O.in_reset || sim_pin(PIN_OLED_CS) || !sim_oled_bus_attached()) {
        O.info.ignored_bytes++;
        f->oled_ignored_bytes++;
        return;
    }
    if (sim_pin(PIN_OLED_DC)) {
        O.info.data_bytes++;
        f->oled_data_bytes++;
        data_byte(b);
    } else {
        O.info.cmd_bytes++;
        f->oled_cmd_bytes++;
        command_byte(b);
        update_lit();
    }
}

/* -------------------------------------------------------------------- */
/* HAL                                                                    */

void hal_oled_bus_attach(bool attached)
{
    sim_service();
    oled_progress();
    if (attached && !sim_pin(PIN_OLED_EN))
        sim_viol(SIM_V_OLED_SIGNAL_UNPOWERED, "SPI bus attached while OLED_EN is low");
    if (!attached && O.dma_active)
        sim_viol(SIM_V_OLED_DMA_MISUSE, "SPI bus detached during a DMA transfer");
    sim_core_set_oled_attached(attached);
}

static void check_spi_write(void)
{
    if (!sim_pin(PIN_OLED_EN))
        sim_viol(SIM_V_OLED_SPI_UNPOWERED, "SPI write while OLED_EN is low");
    if (!sim_oled_bus_attached())
        sim_viol(SIM_V_OLED_SPI_DETACHED, "SPI write while the OLED bus is detached");
}

void hal_oled_write(const uint8_t *buf, size_t n)
{
    sim_service();
    oled_progress();
    if (O.dma_active)
        sim_viol(SIM_V_OLED_DMA_MISUSE, "blocking SPI write while a DMA transfer is running");
    check_spi_write();
    for (size_t i = 0; i < n; i++) spi_byte(buf[i]);
    sim_advance_us(n); /* 8 MHz: 1 us per byte */
}

static void dma_finish(void)
{
    if (O.dma_snap && O.dma_buf && memcmp(O.dma_snap, O.dma_buf, O.dma_n) != 0)
        sim_viol(SIM_V_OLED_DMA_MISUSE, "DMA source buffer was modified during the transfer");
    free(O.dma_snap);
    O.dma_snap = NULL;
    O.dma_active = false;
}

void oled_progress(void)
{
    if (!O.dma_active) return;
    uint64_t el = sim_now_us() - O.dma_start_us;
    size_t upto = el >= O.dma_n ? O.dma_n : (size_t)el;
    while (O.dma_done < upto) spi_byte(O.dma_snap[O.dma_done++]);
    if (O.dma_done >= O.dma_n) dma_finish();
}

bool oled_dma_active(void) { return O.dma_active; }

bool hal_oled_write_async(const uint8_t *buf, size_t n)
{
    sim_service();
    oled_progress();
    if (O.dma_active) return false;
    sim_facts_t *f = sim_facts_raw();
    f->oled_async_writes++;
    check_spi_write();
    if (n == 0) return true;
    O.dma_snap = malloc(n);
    if (!O.dma_snap) sim_fatal("out of memory");
    memcpy(O.dma_snap, buf, n);
    O.dma_buf = buf;
    O.dma_n = n;
    O.dma_done = 0;
    O.dma_start_us = sim_now_us();
    O.dma_active = true;
    return true;
}

bool hal_oled_busy(void)
{
    sim_service();
    oled_progress();
    return O.dma_active;
}

/* -------------------------------------------------------------------- */
/* views                                                                  */

void sim_oled_set_settle_ms(unsigned ms) { O.settle_ms = ms; }

sim_oled_info_t sim_oled_info(void)
{
    oled_progress();
    sim_oled_info_t i = O.info;
    i.powered = O.powered;
    i.in_reset = O.in_reset;
    i.reset_pulse_seen = O.reset_pulse_seen;
    i.display_on = O.display_on;
    i.charge_pump = O.pump_on;
    i.contrast = O.contrast;
    i.init_complete = O.lit;
    i.image_settled = O.lit && sim_now_us() >= O.lit_since_us + (uint64_t)O.settle_ms * 1000;
    return i;
}

uint8_t sim_oled_ram_byte(int page, int col) { return O.ram[page & 7][col & 127]; }

void sim_oled_image(uint8_t out[64][128])
{
    memset(out, 0, 64 * 128);
    oled_progress();
    sim_oled_info_t i = sim_oled_info();
    if (!i.image_settled) return;
    for (int m = 0; m <= O.mux && m < 64; m++) {
        int ram_row = (O.start_line + m + 64 - O.offset) & 63;
        int y = O.com_rev ? m : 63 - m;
        for (int c = 0; c < 128; c++) {
            int x = O.seg_remap ? c : 127 - c;
            bool on = (O.ram[ram_row >> 3][c] >> (ram_row & 7)) & 1;
            if (O.entire_on) on = true;
            else if (O.inverse) on = !on;
            out[y][x] = on ? 1 : 0;
        }
    }
}

bool sim_oled_save_pgm(const char *path, int scale)
{
    static uint8_t img[64][128];
    if (scale < 1) scale = 1;
    sim_oled_image(img);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P5\n%d %d\n255\n", 128 * scale, 64 * scale);
    for (int y = 0; y < 64 * scale; y++)
        for (int x = 0; x < 128 * scale; x++) fputc(img[y / scale][x / scale] ? 255 : 0, f);
    fclose(f);
    return true;
}

static uint32_t crc_table[256];
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void png_chunk(FILE *f, const char *type, const uint8_t *d, size_t n)
{
    uint8_t h[8];
    put32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_upd(0, h + 4, 4);
    c = n ? crc32_upd(c, d, n) : c;
    uint8_t t[4];
    put32(t, c);
    fwrite(t, 1, 4, f);
}

bool sim_oled_save_png(const char *path, int scale)
{
    static uint8_t img[64][128];
    if (scale < 1) scale = 1;
    sim_oled_image(img);
    size_t w = (size_t)128 * (size_t)scale, h = (size_t)64 * (size_t)scale;
    size_t rawn = (w + 1) * h;
    uint8_t *raw = malloc(rawn);
    if (!raw) return false;
    for (size_t y = 0; y < h; y++) {
        raw[y * (w + 1)] = 0;
        for (size_t x = 0; x < w; x++) raw[y * (w + 1) + 1 + x] = img[y / (size_t)scale][x / (size_t)scale] ? 255 : 0;
    }
    /* zlib stream of stored blocks */
    size_t nblk = (rawn + 65534) / 65535;
    size_t zn = 2 + rawn + nblk * 5 + 4;
    uint8_t *z = malloc(zn);
    if (!z) { free(raw); return false; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < rawn; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t pos = 0; pos < rawn;) {
        size_t len = rawn - pos > 65535 ? 65535 : rawn - pos;
        z[o++] = (pos + len == rawn) ? 1 : 0;
        z[o++] = (uint8_t)len; z[o++] = (uint8_t)(len >> 8);
        z[o++] = (uint8_t)~len; z[o++] = (uint8_t)(~len >> 8);
        memcpy(z + o, raw + pos, len);
        o += len; pos += len;
    }
    put32(z + o, (b << 16) | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return false; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, o);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return true;
}
