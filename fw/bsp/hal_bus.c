/* hal.h buses: I2C0 controller (400 kHz) and SPI1 to the OLED (mode 0, 8 MHz). */
#include "bsp_internal.h"

/* ======================================================================== */
/* I2C0: PA0 SDA (PINCM1, PF 3), PA1 SCL (PINCM2, PF 3)                      */
/* ======================================================================== */
#define I2C_HZ          400000u
#define I2C_TIMEOUT_MS  10u
#define I2C_LOOP_CAP    400000u /* backstop if SysTick is not advancing */
#define I2C_FIFO_DEPTH  8u
#define I2C_MAX_LEN     0xFFFu  /* MBLEN is 12 bits */

/* I2C0 lives in PD0, so its BUSCLK is ULPCLK (32 MHz, or 40 MHz when MCLK is
 * 80 MHz). SCL = BUSCLK / (10 * (TPR + 1)): 32 MHz -> TPR 7, 40 MHz -> TPR 9,
 * both exactly 400 kHz. */
static uint32_t i2c_tpr(void)
{
    return g_ulpclk_hz / (10u * I2C_HZ) - 1u;
}

static void i2c_pins_peripheral(void)
{
    DL_GPIO_initPeripheralInputFunctionFeatures(IOMUX_PINCM1,
        IOMUX_PINCM1_PF_I2C0_SDA, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_DISABLE,
        DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_initPeripheralInputFunctionFeatures(IOMUX_PINCM2,
        IOMUX_PINCM2_PF_I2C0_SCL, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_DISABLE,
        DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_enableHiZ(IOMUX_PINCM1);
    DL_GPIO_enableHiZ(IOMUX_PINCM2);
}

void bsp_i2c_init(void)
{
    static const DL_I2C_ClockConfig clk = {
        .clockSel = DL_I2C_CLOCK_BUSCLK,
        .divideRatio = DL_I2C_CLOCK_DIVIDE_1,
    };
    DL_I2C_reset(I2C0);
    DL_I2C_enablePower(I2C0);
    bsp_delay_cycles(32u);
    i2c_pins_peripheral();
    DL_I2C_setClockConfig(I2C0, &clk);
    DL_I2C_disableAnalogGlitchFilter(I2C0); /* as SysConfig generates */
    DL_I2C_resetControllerTransfer(I2C0);
    DL_I2C_setTimerPeriod(I2C0, i2c_tpr());
    DL_I2C_setControllerTXFIFOThreshold(I2C0, DL_I2C_TX_FIFO_LEVEL_EMPTY);
    DL_I2C_setControllerRXFIFOThreshold(I2C0, DL_I2C_RX_FIFO_LEVEL_BYTES_1);
    DL_I2C_enableControllerClockStretching(I2C0);
    DL_I2C_enableController(I2C0);
}

void bsp_i2c_retime(void)
{
    DL_I2C_disableController(I2C0);
    DL_I2C_setTimerPeriod(I2C0, i2c_tpr());
    DL_I2C_enableController(I2C0);
}

typedef struct {
    uint32_t t0;
    uint32_t loops;
} i2c_deadline_t;

static bool i2c_expired(i2c_deadline_t *d)
{
    /* > 10 ms on the 1 ms tick, or the loop backstop. */
    return (hal_millis() - d->t0) > I2C_TIMEOUT_MS || ++d->loops > I2C_LOOP_CAP;
}

/* I2C_ERR_13: wait 3 I2C functional clocks after starting a transfer before
 * trusting the controller status. 64 MCLK cycles covers 3 ULPCLK cycles. */
static void i2c_errata_delay(void)
{
    bsp_delay_cycles(64u);
}

static hal_status_t i2c_error(uint32_t st)
{
    if (st & DL_I2C_CONTROLLER_STATUS_ARBITRATION_LOST) return HAL_ERR_BUS;
    if (st & (DL_I2C_CONTROLLER_STATUS_ADDR_ACK | DL_I2C_CONTROLLER_STATUS_DATA_ACK))
        return HAL_ERR_NACK;
    return HAL_ERR_BUS;
}

/* After an error or timeout: give the controller 1 ms to finish its STOP,
 * otherwise re-initialise it. FIFOs are flushed either way. */
static void i2c_cleanup(bool force_reinit)
{
    i2c_deadline_t d = {hal_millis(), 0u};
    while (!force_reinit &&
           !(DL_I2C_getControllerStatus(I2C0) & DL_I2C_CONTROLLER_STATUS_IDLE)) {
        if ((hal_millis() - d.t0) > 1u || ++d.loops > I2C_LOOP_CAP / 8u) {
            force_reinit = true;
        }
    }
    if (force_reinit) {
        bsp_i2c_init();
    } else {
        DL_I2C_flushControllerTXFIFO(I2C0);
        DL_I2C_flushControllerRXFIFO(I2C0);
    }
}

#define I2C_RIS_ALL (DL_I2C_INTERRUPT_CONTROLLER_TX_DONE | \
                     DL_I2C_INTERRUPT_CONTROLLER_RX_DONE | \
                     DL_I2C_INTERRUPT_CONTROLLER_NACK | \
                     DL_I2C_INTERRUPT_CONTROLLER_ARBITRATION_LOST | \
                     DL_I2C_INTERRUPT_CONTROLLER_STOP)

hal_status_t hal_i2c_xfer(uint8_t addr, const uint8_t *w, size_t wn,
                          uint8_t *r, size_t rn)
{
    i2c_deadline_t d = {hal_millis(), 0u};
    uint32_t st;
    size_t i;

    if (addr > 0x7Fu || (wn != 0u && w == NULL) || (rn != 0u && r == NULL) ||
        wn > I2C_MAX_LEN || rn > I2C_MAX_LEN)
        return HAL_ERR_PARAM;

    while (!(DL_I2C_getControllerStatus(I2C0) & DL_I2C_CONTROLLER_STATUS_IDLE)) {
        if (i2c_expired(&d)) {
            i2c_cleanup(true);
            return HAL_ERR_BUS;
        }
    }
    DL_I2C_flushControllerTXFIFO(I2C0);
    DL_I2C_flushControllerRXFIFO(I2C0);
    DL_I2C_clearInterruptStatus(I2C0, I2C_RIS_ALL);

    /* Write phase. With wn == rn == 0 this is an address-only probe. */
    if (wn != 0u || rn == 0u) {
        i = 0u;
        while (i < wn && i < I2C_FIFO_DEPTH) {
            DL_I2C_transmitControllerData(I2C0, w[i]);
            i++;
        }
        DL_I2C_startControllerTransferAdvanced(I2C0, addr,
            DL_I2C_CONTROLLER_DIRECTION_TX, (uint16_t) wn,
            DL_I2C_CONTROLLER_START_ENABLE,
            rn == 0u ? DL_I2C_CONTROLLER_STOP_ENABLE : DL_I2C_CONTROLLER_STOP_DISABLE,
            DL_I2C_CONTROLLER_ACK_DISABLE);
        i2c_errata_delay();
        for (;;) {
            while (i < wn && !DL_I2C_isControllerTXFIFOFull(I2C0)) {
                DL_I2C_transmitControllerData(I2C0, w[i]);
                i++;
            }
            st = DL_I2C_getControllerStatus(I2C0);
            if (st & DL_I2C_CONTROLLER_STATUS_ERROR) {
                i2c_cleanup(false);
                return i2c_error(st);
            }
            if (DL_I2C_getRawInterruptStatus(I2C0, DL_I2C_INTERRUPT_CONTROLLER_TX_DONE) &&
                !(st & DL_I2C_CONTROLLER_STATUS_BUSY))
                break;
            if (i2c_expired(&d)) {
                i2c_cleanup(true);
                return HAL_ERR_TIMEOUT;
            }
        }
    }

    /* Read phase, after a repeated START if there was a write. The last byte
     * is NACKed and followed by STOP. */
    if (rn != 0u) {
        DL_I2C_startControllerTransferAdvanced(I2C0, addr,
            DL_I2C_CONTROLLER_DIRECTION_RX, (uint16_t) rn,
            DL_I2C_CONTROLLER_START_ENABLE, DL_I2C_CONTROLLER_STOP_ENABLE,
            DL_I2C_CONTROLLER_ACK_DISABLE);
        i2c_errata_delay();
        i = 0u;
        for (;;) {
            while (i < rn && !DL_I2C_isControllerRXFIFOEmpty(I2C0)) {
                r[i] = DL_I2C_receiveControllerData(I2C0);
                i++;
            }
            st = DL_I2C_getControllerStatus(I2C0);
            if (st & DL_I2C_CONTROLLER_STATUS_ERROR) {
                i2c_cleanup(false);
                return i2c_error(st);
            }
            if (i == rn &&
                DL_I2C_getRawInterruptStatus(I2C0, DL_I2C_INTERRUPT_CONTROLLER_RX_DONE))
                break;
            if (i2c_expired(&d)) {
                i2c_cleanup(true);
                return HAL_ERR_TIMEOUT;
            }
        }
    }

    /* Wait for the STOP to complete so back-to-back calls start clean. */
    while (DL_I2C_getControllerStatus(I2C0) & DL_I2C_CONTROLLER_STATUS_BUSY_BUS) {
        if (i2c_expired(&d)) {
            i2c_cleanup(true);
            return HAL_ERR_TIMEOUT;
        }
    }
    return HAL_OK;
}

/* Bus-hang recovery (NXP UM10204 3.1.16): clock SCL 9 times as an open-drain
 * GPIO so a target stuck mid-byte releases SDA, then a STOP, then re-init. */
void hal_i2c_recover(void)
{
    const uint32_t sda = DL_GPIO_PIN_0, scl = DL_GPIO_PIN_1;
    const uint32_t half_us = 5u; /* ~100 kHz */
    int k;

    DL_I2C_disableController(I2C0);

    DL_GPIO_setPins(GPIOA, sda | scl); /* released (open drain: high = Hi-Z) */
    DL_GPIO_initDigitalOutputFeatures(IOMUX_PINCM1, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_DRIVE_STRENGTH_LOW, DL_GPIO_HIZ_ENABLE);
    DL_GPIO_initDigitalOutputFeatures(IOMUX_PINCM2, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_DRIVE_STRENGTH_LOW, DL_GPIO_HIZ_ENABLE);
    DL_GPIO_enableOutput(GPIOA, sda | scl);
    hal_delay_us(half_us);

    for (k = 0; k < 9; k++) { /* always 9: finishes any byte + ACK in flight */
        DL_GPIO_clearPins(GPIOA, scl);
        hal_delay_us(half_us);
        DL_GPIO_setPins(GPIOA, scl);
        hal_delay_us(half_us);
    }

    /* STOP: SDA low while SCL low, release SCL, then release SDA. */
    DL_GPIO_clearPins(GPIOA, scl);
    hal_delay_us(half_us);
    DL_GPIO_clearPins(GPIOA, sda);
    hal_delay_us(half_us);
    DL_GPIO_setPins(GPIOA, scl);
    hal_delay_us(half_us);
    DL_GPIO_setPins(GPIOA, sda);
    hal_delay_us(half_us);

    DL_GPIO_disableOutput(GPIOA, sda | scl);
    bsp_i2c_init(); /* restores the I2C pin functions too */
}

/* ======================================================================== */
/* SPI1 -> OLED: PB8 PICO (PINCM25, PF 3), PB9 SCK (PINCM26, PF 3)           */
/* ======================================================================== */
#define SPI_HZ 8000000u
static bool s_oled_attached;

static const DL_SPI_Config k_spi_cfg = {
    .mode = DL_SPI_MODE_CONTROLLER,
    .frameFormat = DL_SPI_FRAME_FORMAT_MOTO3_POL0_PHA0, /* mode 0, no HW CS */
    .parity = DL_SPI_PARITY_NONE,
    .dataSize = DL_SPI_DATA_SIZE_8,
    .bitOrder = DL_SPI_BIT_ORDER_MSB_FIRST,
    .chipSelectPin = DL_SPI_CHIP_SELECT_NONE,
};

/* SPI1 is in PD1: BUSCLK = MCLK. SCK = MCLK / (2 * (SCR + 1)):
 * 32 MHz -> SCR 1, 80 MHz -> SCR 4, both exactly 8 MHz. */
static uint32_t spi_scr(void)
{
    return g_mclk_hz / (2u * SPI_HZ) - 1u;
}

void bsp_spi_init(void)
{
    static const DL_SPI_ClockConfig clk = {
        .clockSel = DL_SPI_CLOCK_BUSCLK,
        .divideRatio = DL_SPI_CLOCK_DIVIDE_RATIO_1,
    };
    DL_SPI_reset(SPI1);
    DL_SPI_enablePower(SPI1);
    bsp_delay_cycles(32u);
    DL_SPI_setClockConfig(SPI1, &clk);
    DL_SPI_init(SPI1, &k_spi_cfg);
    DL_SPI_setBitRateSerialClockDivider(SPI1, spi_scr());
    DL_SPI_setFIFOThreshold(SPI1, DL_SPI_RX_FIFO_LEVEL_ONE_FRAME,
                            DL_SPI_TX_FIFO_LEVEL_ONE_FRAME);
    DL_SPI_enable(SPI1);
    hal_oled_bus_attach(s_oled_attached); /* re-apply pin state */
}

void bsp_spi_retime(void)
{
    DL_SPI_disable(SPI1);
    DL_SPI_setBitRateSerialClockDivider(SPI1, spi_scr());
    DL_SPI_enable(SPI1);
}

static void spi_drain_rx(void)
{
    while (!DL_SPI_isRXFIFOEmpty(SPI1)) (void) DL_SPI_receiveData8(SPI1);
}

bool hal_oled_busy(void)
{
    return DL_DMA_isChannelEnabled(DMA, BSP_DMA_CH_OLED) || DL_SPI_isBusy(SPI1);
}

/* Bounded waits on SPI1 (review finding: hal_oled_write spun forever if the
 * SPI never finished). A full frame takes ~1 ms at 8 MHz; OLED_WAIT_MS is
 * time-based (SysTick runs in the main loop), OLED_WAIT_SPINS a backstop in
 * case SysTick is masked (each pass is a few register reads: well over
 * 10 ms at 80 MHz). */
#define OLED_WAIT_MS    10u
#define OLED_WAIT_SPINS 400000u

typedef struct {
    uint32_t t0, spins;
} oled_deadline_t;

static oled_deadline_t oled_deadline(void)
{
    oled_deadline_t d = {hal_millis(), 0u};
    return d;
}

static bool oled_expired(oled_deadline_t *d)
{
    return (uint32_t) (hal_millis() - d->t0) > OLED_WAIT_MS || ++d->spins > OLED_WAIT_SPINS;
}

static bool oled_wait_idle(void)
{
    oled_deadline_t d = oled_deadline();
    while (hal_oled_busy()) {
        if (oled_expired(&d)) return false;
    }
    return true;
}

/* The SPI did not finish: stop the DMA and reset SPI1 (FIFOs, shifter);
 * bsp_spi_init() re-applies the attached / detached pin state. */
static void oled_abort(void)
{
    DL_DMA_disableChannel(DMA, BSP_DMA_CH_OLED);
    DL_SPI_disableDMATransmitEvent(SPI1);
    bsp_spi_init();
}

void hal_oled_bus_attach(bool attached)
{
    if (!oled_wait_idle()) {
        DL_DMA_disableChannel(DMA, BSP_DMA_CH_OLED);
        DL_SPI_disableDMATransmitEvent(SPI1);
        /* the pin state below takes the bus away from SPI1 either way */
    }
    if (attached) {
        DL_GPIO_initPeripheralOutputFunction(IOMUX_PINCM25, IOMUX_PINCM25_PF_SPI1_PICO);
        DL_GPIO_initPeripheralOutputFunction(IOMUX_PINCM26, IOMUX_PINCM26_PF_SPI1_SCLK);
    } else {
        DL_DMA_disableChannel(DMA, BSP_DMA_CH_OLED);
        DL_GPIO_clearPins(GPIOB, DL_GPIO_PIN_8 | DL_GPIO_PIN_9);
        DL_GPIO_initDigitalOutput(IOMUX_PINCM25);
        DL_GPIO_initDigitalOutput(IOMUX_PINCM26);
        DL_GPIO_enableOutput(GPIOB, DL_GPIO_PIN_8 | DL_GPIO_PIN_9);
    }
    s_oled_attached = attached;
}

void hal_oled_write(const uint8_t *buf, size_t n)
{
    size_t i;
    oled_deadline_t d;
    if (!s_oled_attached || buf == NULL || n == 0u) return;
    if (!oled_wait_idle()) {
        oled_abort(); /* a DMA transfer that never finished */
        return;
    }
    spi_drain_rx();
    d = oled_deadline();
    for (i = 0; i < n; i++) {
        while (DL_SPI_isTXFIFOFull(SPI1)) {
            spi_drain_rx();
            if (oled_expired(&d)) {
                oled_abort();
                return;
            }
        }
        DL_SPI_transmitData8(SPI1, buf[i]);
        spi_drain_rx(); /* TX-only: discard what the shifter clocked in */
    }
    while (DL_SPI_isBusy(SPI1)) { /* last bit out of the shifter */
        spi_drain_rx();
        if (oled_expired(&d)) {
            oled_abort();
            return;
        }
    }
    spi_drain_rx();
}

bool hal_oled_write_async(const uint8_t *buf, size_t n)
{
    static const DL_DMA_Config cfg = {
        .transferMode = DL_DMA_SINGLE_TRANSFER_MODE,
        .extendedMode = DL_DMA_NORMAL_MODE,
        .destIncrement = DL_DMA_ADDR_UNCHANGED,
        .srcIncrement = DL_DMA_ADDR_INCREMENT,
        .destWidth = DL_DMA_WIDTH_BYTE,
        .srcWidth = DL_DMA_WIDTH_BYTE,
        .trigger = DMA_SPI1_TX_TRIG,
        .triggerType = DL_DMA_TRIGGER_TYPE_EXTERNAL,
    };
    if (hal_oled_busy()) return false;
    if (!s_oled_attached || buf == NULL || n > 0xFFFFu) return false;
    if (n == 0u) return true;
    spi_drain_rx();
    DL_DMA_initChannel(DMA, BSP_DMA_CH_OLED, &cfg);
    DL_DMA_setSrcAddr(DMA, BSP_DMA_CH_OLED, (uint32_t) buf);
    DL_DMA_setDestAddr(DMA, BSP_DMA_CH_OLED, (uint32_t) &SPI1->TXDATA);
    DL_DMA_setTransferSize(DMA, BSP_DMA_CH_OLED, (uint16_t) n);
    DL_SPI_enableDMATransmitEvent(SPI1);
    /* The SPI TX trigger is already asserted (FIFO has room), so enabling
     * the channel starts the transfer. DMAEN self-clears after n bytes. */
    DL_DMA_enableChannel(DMA, BSP_DMA_CH_OLED);
    return true;
}
