#include "mock_hal.h"

#include <string.h>

uint32_t mock_now;
mock_ev_t mock_ev[MOCK_EV_MAX];
int mock_ev_n;
mock_spi_t mock_spi[MOCK_SPI_MAX];
int mock_spi_n;
bool mock_pin[PIN_COUNT];
bool mock_attached;
uint32_t mock_dma_ms = 2;
int mock_viol_high_unpowered, mock_viol_en_fell_driven, mock_viol_spi;
int mock_i2c_recover_count, mock_i2c_xfer_count;

static uint32_t dma_end;
static bool dma_running;
static struct {
    mock_i2c_fn fn;
    void *ctx;
} dev[128];
static int inj_count;
static hal_status_t inj_status;

static void log_ev(mock_ev_kind_t k, int a, int b)
{
    if (mock_ev_n < MOCK_EV_MAX) {
        mock_ev[mock_ev_n++] = (mock_ev_t){k, mock_now, a, b};
    }
}

void mock_reset(void)
{
    mock_now = 1000;
    mock_ev_n = 0;
    mock_spi_n = 0;
    memset(mock_pin, 0, sizeof mock_pin);
    mock_pin[PIN_CHG_PG] = true;
    mock_pin[PIN_CHG_STAT1] = true;
    mock_pin[PIN_BUTTON1] = true;
    mock_pin[PIN_BUTTON2] = true;
    mock_pin[PIN_CW_ALRT] = true;
    mock_attached = false;
    mock_dma_ms = 2;
    dma_running = false;
    mock_viol_high_unpowered = mock_viol_en_fell_driven = mock_viol_spi = 0;
    mock_i2c_recover_count = mock_i2c_xfer_count = 0;
    memset(dev, 0, sizeof dev);
    inj_count = 0;
}

void mock_advance(uint32_t ms) { mock_now += ms; }

int mock_find_pin_ev(int from, board_pin_t pin, int level)
{
    for (int i = from; i < mock_ev_n; i++) {
        if (mock_ev[i].kind == MEV_PIN && mock_ev[i].a == (int)pin &&
            mock_ev[i].b == level) {
            return i;
        }
    }
    return -1;
}

uint32_t hal_millis(void) { return mock_now; }

void hal_delay_ms(uint32_t ms)
{
    log_ev(MEV_DELAY, (int)ms, 0);
    mock_now += ms;
}

void hal_delay_us(uint32_t us) { (void)us; }

static bool oled_pin(board_pin_t p)
{
    return p == PIN_OLED_DC || p == PIN_OLED_CS || p == PIN_OLED_RESET;
}

void hal_pin_write(board_pin_t pin, bool high)
{
    if (pin >= PIN_FIRST_INPUT) {
        return;
    }
    if (oled_pin(pin) && high && !mock_pin[PIN_OLED_EN]) {
        mock_viol_high_unpowered++;
    }
    if (pin == PIN_OLED_EN && !high && mock_pin[PIN_OLED_EN]) {
        if (mock_pin[PIN_OLED_DC] || mock_pin[PIN_OLED_CS] ||
            mock_pin[PIN_OLED_RESET] || mock_attached) {
            mock_viol_en_fell_driven++;
        }
    }
    mock_pin[pin] = high;
    log_ev(MEV_PIN, (int)pin, high);
}

bool hal_pin_read(board_pin_t pin)
{
    return pin < PIN_COUNT ? mock_pin[pin] : false;
}

void hal_oled_bus_attach(bool attached)
{
    if (attached && !mock_pin[PIN_OLED_EN]) {
        mock_viol_high_unpowered++;
    }
    mock_attached = attached;
    log_ev(MEV_ATTACH, attached, 0);
}

static void record_spi(const uint8_t *buf, size_t n, bool async)
{
    if (!mock_attached || mock_pin[PIN_OLED_CS] || !mock_pin[PIN_OLED_EN]) {
        mock_viol_spi++;
    }
    log_ev(async ? MEV_ASYNC : MEV_SPI, mock_spi_n, (int)n);
    for (size_t i = 0; i < n && mock_spi_n < MOCK_SPI_MAX; i++) {
        mock_spi[mock_spi_n++] =
            (mock_spi_t){buf[i], mock_pin[PIN_OLED_DC], mock_pin[PIN_OLED_CS], async};
    }
}

void hal_oled_write(const uint8_t *buf, size_t n) { record_spi(buf, n, false); }

bool hal_oled_write_async(const uint8_t *buf, size_t n)
{
    if (hal_oled_busy()) {
        return false;
    }
    record_spi(buf, n, true);
    dma_running = true;
    dma_end = mock_now + mock_dma_ms;
    return true;
}

bool hal_oled_busy(void)
{
    if (dma_running && (int32_t)(mock_now - dma_end) >= 0) {
        dma_running = false;
    }
    return dma_running;
}

void mock_i2c_attach(uint8_t addr, mock_i2c_fn fn, void *ctx)
{
    dev[addr].fn = fn;
    dev[addr].ctx = ctx;
}

void mock_i2c_inject(int count, hal_status_t st)
{
    inj_count = count;
    inj_status = st;
}

hal_status_t hal_i2c_xfer(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r,
                          size_t rn)
{
    mock_i2c_xfer_count++;
    if (inj_count > 0) {
        inj_count--;
        return inj_status;
    }
    if (addr >= 128 || !dev[addr].fn) {
        return HAL_ERR_NACK;
    }
    return dev[addr].fn(dev[addr].ctx, w, wn, r, rn);
}

void hal_i2c_recover(void) { mock_i2c_recover_count++; }
