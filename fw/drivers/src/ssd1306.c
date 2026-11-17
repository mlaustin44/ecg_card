#include "ssd1306.h"

#include "hal.h"

static bool s_powered;
static bool s_display_on;
static bool s_flushing;
static uint32_t s_ready_at;

#ifdef SSD1306_FLIP_180
#define SEG_REMAP 0xA0
#define COM_SCAN  0xC0
#else
#define SEG_REMAP 0xA1
#define COM_SCAN  0xC8
#endif

/* Vendor sequence (datasheet 4.2), but horizontal addressing (0x20 0x00) and
 * explicit full-frame window so one DMA transfer carries a whole frame.
 * Charge pump (8D 14) and display on (AF) follow after the RAM clear. */
static const uint8_t k_init[] = {
    0xAE,
    0xD5, 0x80,
    0xA8, 0x3F,
    0xD3, 0x00,
    0x40,
    SEG_REMAP,
    COM_SCAN,
    0xDA, 0x12,
    0x81, SSD1306_CONTRAST_INIT,
    0xD9, 0xF1,
    0xDB, 0x30,
    0xA4,
    0xA6,
    0x20, 0x00,
    0x21, 0x00, 0x7F,
    0x22, 0x00, 0x07,
    0x8D, 0x14,
};

static const uint8_t k_window[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
static const uint8_t k_zero[32];

static void cmd(const uint8_t *c, size_t n)
{
    hal_pin_write(PIN_OLED_DC, false);
    hal_pin_write(PIN_OLED_CS, false);
    hal_oled_write(c, n);
    hal_pin_write(PIN_OLED_CS, true);
}

void ssd1306_power_on(void)
{
    /* OLED_EN reads back low after an MCU reset (or a host-sim reboot that
     * keeps this module's statics): then the panel is unpowered whatever
     * s_powered says, so start from scratch. */
    if (s_powered && hal_pin_read(PIN_OLED_EN)) {
        return;
    }
    s_powered = false;
    s_flushing = false;
    hal_pin_write(PIN_OLED_RESET, false);
    hal_pin_write(PIN_OLED_EN, true);
    hal_delay_ms(6); /* >= 5 ms */
    hal_oled_bus_attach(true);
    hal_pin_write(PIN_OLED_CS, true);
    hal_pin_write(PIN_OLED_DC, false);
    hal_pin_write(PIN_OLED_RESET, true);
    hal_delay_ms(3);
    s_powered = true;

    cmd(k_init, sizeof k_init);

    /* Clear GDDRAM so no random content shows when the panel turns on. */
    hal_pin_write(PIN_OLED_DC, true);
    hal_pin_write(PIN_OLED_CS, false);
    for (unsigned i = 0; i < OLED_FB_BYTES / sizeof k_zero; i++) {
        hal_oled_write(k_zero, sizeof k_zero);
    }
    hal_pin_write(PIN_OLED_CS, true);

    static const uint8_t on = 0xAF;
    cmd(&on, 1);
    s_display_on = true;
    s_ready_at = hal_millis() + SSD1306_SETTLE_MS;
}

void ssd1306_power_off(void)
{
    if (!s_powered) {
        return;
    }
    for (unsigned i = 0; i < 50 && hal_oled_busy(); i++) {
        hal_delay_ms(1);
    }
    s_flushing = false;
    hal_pin_write(PIN_OLED_CS, true);

    if (!hal_oled_busy()) {
        static const uint8_t off[] = {0xAE, 0x8D, 0x10};
        cmd(off, sizeof off);
        hal_delay_ms(100);
    }
    /* else the SPI never finished (hardware fault): no command can be sent;
     * the pin-safe sequence below must still run, so the rail never stays up
     * and no pin drives an unpowered module. */

    hal_pin_write(PIN_OLED_DC, false);
    hal_pin_write(PIN_OLED_CS, false);
    hal_pin_write(PIN_OLED_RESET, false);
    hal_oled_bus_attach(false);
    hal_pin_write(PIN_OLED_EN, false);
    s_powered = false;
    s_display_on = false;
}

bool ssd1306_ready(void)
{
    return s_powered && (int32_t)(hal_millis() - s_ready_at) >= 0;
}

bool ssd1306_is_on(void) { return s_powered && s_display_on; }
bool ssd1306_is_powered(void) { return s_powered; }

bool ssd1306_flush_start(const uint8_t fb[OLED_FB_BYTES])
{
    if (!s_powered || ssd1306_flush_poll()) {
        return false;
    }
    cmd(k_window, sizeof k_window);
    hal_pin_write(PIN_OLED_DC, true);
    hal_pin_write(PIN_OLED_CS, false);
    if (!hal_oled_write_async(fb, OLED_FB_BYTES)) {
        hal_pin_write(PIN_OLED_CS, true);
        return false;
    }
    s_flushing = true;
    return true;
}

bool ssd1306_flush_poll(void)
{
    if (s_flushing && !hal_oled_busy()) {
        hal_pin_write(PIN_OLED_CS, true);
        s_flushing = false;
    }
    return s_flushing;
}

bool ssd1306_busy(void) { return ssd1306_flush_poll(); }

bool ssd1306_set_contrast(uint8_t contrast)
{
    if (!s_powered || ssd1306_flush_poll()) {
        return false;
    }
    const uint8_t c[] = {0x81, contrast};
    cmd(c, sizeof c);
    return true;
}

bool ssd1306_display_on(bool on)
{
    if (!s_powered || ssd1306_flush_poll()) {
        return false;
    }
    const uint8_t c = on ? 0xAF : 0xAE;
    cmd(&c, 1);
    s_display_on = on;
    return true;
}
