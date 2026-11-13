/* SSD1306 128x64 OLED driver, 4-wire SPI, via hal.h only.
 *
 * Frame format: OLED_FB_BYTES (1024) bytes, page-major (8 pages of 128
 * columns, LSB = top row of the page), sent in HORIZONTAL addressing mode.
 *
 * Compile-time option: define SSD1306_FLIP_180 to rotate the image 180
 * degrees (SEG remap 0xA0 / COM scan 0xC0 instead of the vendor 0xA1/0xC8).
 *
 * Blocking times (at 8 MHz SPI):
 *   ssd1306_power_on()   about 9 ms (6 ms rail settle + 3 ms reset) + 1.5 ms
 *                        of SPI (init commands and RAM clear). The 100 ms the
 *                        panel needs before the image is reliable is NOT
 *                        waited for: poll ssd1306_ready().
 *   ssd1306_power_off()  about 100 ms (datasheet wait after charge pump off),
 *                        plus up to 50 ms if a flush is still running.
 *                        The watchdog window (2 s) is not at risk.
 *   flush_start()        about 15 us (window commands); the 1024-byte data
 *                        transfer (~1 ms) runs by DMA.
 *   set_contrast / display_on  a few microseconds.
 *
 * Safety rule: no pin may drive a chip whose OLED_EN rail is off. power_off()
 * drives DC, CS, RESET low and detaches SCK/MOSI before OLED_EN falls.
 */
#ifndef SSD1306_H
#define SSD1306_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

#define SSD1306_SETTLE_MS 100u /* power_on() to a reliable image */

/* Rail-up and init sequence; leaves the display ON (showing black). */
void ssd1306_power_on(void);
/* Safe power-down sequence (see header). Waits up to 50 ms for a running
 * flush; if the SPI is still busy then (hardware fault) the display-off
 * commands are skipped, but the pin-safe sequence (DC, CS, RESET low, bus
 * detached, then OLED_EN low) always completes. */
void ssd1306_power_off(void);

/* True once SSD1306_SETTLE_MS have elapsed since power_on() (non-blocking). */
bool ssd1306_ready(void);
/* Powered and the display is switched on (0xAF). */
bool ssd1306_is_on(void);
bool ssd1306_is_powered(void);

/* Start sending a whole frame by DMA. Returns false if not powered or a
 * flush is still running. THE CALLER MUST KEEP fb VALID AND UNCHANGED until
 * ssd1306_busy() returns false (the DMA reads it directly). */
bool ssd1306_flush_start(const uint8_t fb[OLED_FB_BYTES]);
/* Finish a running flush when the DMA is done (raises CS). Call from the main
 * loop; returns true while still busy. */
bool ssd1306_flush_poll(void);
/* Same as flush_poll(): true while a flush is in progress. */
bool ssd1306_busy(void);

/* These return false (and do nothing) when not powered or while flushing. */
bool ssd1306_set_contrast(uint8_t contrast);
/* 0xAF / 0xAE. Charge pump stays on; use to blank during PPG captures. */
bool ssd1306_display_on(bool on);

#endif
