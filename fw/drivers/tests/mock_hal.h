/* Test mock of hal.h: virtual time, pin/event log, SPI stream, I2C dispatch.
 * Only the functions the drivers use are implemented. */
#ifndef MOCK_HAL_H
#define MOCK_HAL_H

#include "hal.h"

typedef enum { MEV_PIN, MEV_ATTACH, MEV_SPI, MEV_ASYNC, MEV_DELAY } mock_ev_kind_t;
typedef struct {
    mock_ev_kind_t kind;
    uint32_t t;
    int a; /* PIN: pin; ATTACH: attached; SPI/ASYNC: first byte index; DELAY: ms */
    int b; /* PIN: level; SPI/ASYNC: byte count */
} mock_ev_t;

typedef struct {
    uint8_t byte;
    bool dc, cs, async;
} mock_spi_t;

#define MOCK_EV_MAX  8192
#define MOCK_SPI_MAX 16384

extern uint32_t mock_now;
extern mock_ev_t mock_ev[MOCK_EV_MAX];
extern int mock_ev_n;
extern mock_spi_t mock_spi[MOCK_SPI_MAX];
extern int mock_spi_n;
extern bool mock_pin[PIN_COUNT];
extern bool mock_attached;
extern uint32_t mock_dma_ms; /* async transfer duration (default 2) */
/* Safety-invariant violation counters (OLED rail model). */
extern int mock_viol_high_unpowered;  /* OLED pin/bus driven while OLED_EN low */
extern int mock_viol_en_fell_driven;  /* OLED_EN fell with a pin high/bus attached */
extern int mock_viol_spi;             /* SPI write detached / CS high / unpowered */

void mock_reset(void);
void mock_advance(uint32_t ms);
int mock_find_pin_ev(int from, board_pin_t pin, int level); /* index or -1 */

/* I2C */
typedef hal_status_t (*mock_i2c_fn)(void *ctx, const uint8_t *w, size_t wn,
                                    uint8_t *r, size_t rn);
void mock_i2c_attach(uint8_t addr, mock_i2c_fn fn, void *ctx);
void mock_i2c_inject(int count, hal_status_t st); /* fail the next count xfers */
extern int mock_i2c_recover_count;
extern int mock_i2c_xfer_count;

#endif
