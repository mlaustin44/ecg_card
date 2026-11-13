/* hal.h radio link: RADIO_EN (PB18) and UART0 to the CH585 (PA10 TX,
 * PA11 RX), interrupt-driven rings.
 *
 * Modelled on TI's LP_MSPM0G3507 DriverLib examples (SDK 2.11.00.07):
 *   uart_rw_multibyte_fifo_poll   ti_msp_dl_config.c SYSCFG_DL_UART_0_init():
 *       BUSCLK / divide 1, DL_UART_Main_init (normal, TX+RX, no flow control,
 *       8N1), 16x oversampling, setBaudRateDivisor, enableFIFOs, FIFO
 *       thresholds, enable; SYSCFG_DL_initPower(): reset, enablePower, delay
 *   uart_rx_multibyte_fifo_dma_interrupts  RX threshold ONE_ENTRY
 *   uart_echo_interrupts_standby  the RX interrupt / IIDX handler pattern
 *   uart_external_loopback_interrupt  SYSCFG_DL_GPIO_init(): PA10 =
 *       IOMUX_PINCM21_PF_UART0_TX, PA11 = IOMUX_PINCM22_PF_UART0_RX
 *
 * Clock: UART0 is a PD0 peripheral, so its BUSCLK is ULPCLK: 32 MHz, or
 * 40 MHz while MCLK is 80 MHz (SysConfig generates UART_0_INST_FREQUENCY
 * 40000000 for UART0 at MCLK 80 MHz / ULPCLK DIV_2 in the SDK's
 * LP_MSPM0G3519 bq769x2_TIDA010247 demo). With 16x oversampling the
 * divider (same rounding as DL_UART_configBaudRate) is
 *   32 MHz: 32e6 / (16 * 460800) = 4.3403 -> IBRD 4, FBRD 22 (4.34375)
 *           = 460432 baud, -0.080 %
 *   40 MHz: 40e6 / (16 * 460800) = 5.4253 -> IBRD 5, FBRD 27 (5.421875)
 *           = 461095 baud, +0.064 %
 * The CH585 side runs at 458823 baud (-0.43 %), so the two ends differ by
 * 0.35 % (32 MHz) and 0.49 % (40 MHz): well under the ~2 % an 8N1 link
 * tolerates in total.
 * MFCLK (4 MHz) is too slow for 16x (8.68 clocks per bit) and LFCLK far
 * too slow, so BUSCLK it is, and hal_clock_fast() reprograms the divider
 * (bsp_uart_retime(), like bsp_i2c_retime()). Before the switch it waits,
 * bounded, for the TX FIFO and shifter to empty (bsp_uart_drain_tx()) so no
 * transmitted byte is cut; a byte the radio sends while ULPCLK changes is
 * received wrong (its frame fails the CRC). */
#include "bsp_internal.h"

#define RADIO_EN_PIN  DL_GPIO_PIN_18 /* GPIOB */
#define UART_TX_PIN   DL_GPIO_PIN_10 /* GPIOA */
#define UART_TX_PINCM IOMUX_PINCM21
#define UART_RX_PINCM IOMUX_PINCM22
#define BSP_UART_BAUD 460800u        /* link_fmt.h LINK_BAUD */

#define TX_MASK (HAL_UART_TX_RING - 1u)
#define RX_MASK (HAL_UART_RX_RING - 1u)
#if (HAL_UART_TX_RING & TX_MASK) || (HAL_UART_RX_RING & RX_MASK) || HAL_UART_TX_RING > 32768u
#error "UART rings must be powers of two (free-running uint16_t indices)"
#endif

/* Single producer / single consumer: TX head and RX tail are written by the
 * main loop only, TX tail and RX head by the UART0 ISR only. */
static uint8_t s_tx[HAL_UART_TX_RING];
static uint8_t s_rx[HAL_UART_RX_RING];
static volatile uint16_t s_tx_head, s_tx_tail, s_rx_head, s_rx_tail;
static volatile bool s_on;
static volatile hal_uart_stats_t s_st;

static const DL_UART_Main_ClockConfig k_uart_clk = {
    .clockSel = DL_UART_MAIN_CLOCK_BUSCLK,
    .divideRatio = DL_UART_MAIN_CLOCK_DIVIDE_RATIO_1,
};

static const DL_UART_Main_Config k_uart_cfg = {
    .mode = DL_UART_MAIN_MODE_NORMAL,
    .direction = DL_UART_MAIN_DIRECTION_TX_RX,
    .flowControl = DL_UART_MAIN_FLOW_CONTROL_NONE,
    .parity = DL_UART_MAIN_PARITY_NONE,
    .wordLength = DL_UART_MAIN_WORD_LENGTH_8_BITS,
    .stopBits = DL_UART_MAIN_STOP_BITS_ONE,
};

/* 16x oversampling divider for the current ULPCLK, rounded as in
 * DL_UART_configBaudRate(): 64 * brd + 1/2. */
static void uart_set_divisor(void)
{
    uint32_t div = ((g_ulpclk_hz * 8u) / BSP_UART_BAUD + 1u) / 2u;
    DL_UART_Main_setBaudRateDivisor(UART0, div >> 6, div & 0x3Fu);
}

/* Move ring bytes into the TX FIFO. Called from the ISR or with interrupts
 * locked. Leaves either the ring empty (TX interrupt off) or the FIFO full
 * (the TX interrupt fires when it drains through the threshold). */
static void tx_fill(void)
{
    uint16_t tail = s_tx_tail;
    while (tail != s_tx_head && !DL_UART_Main_isTXFIFOFull(UART0)) {
        DL_UART_Main_transmitData(UART0, s_tx[tail & TX_MASK]);
        tail++;
    }
    s_tx_tail = tail;
    if (tail == s_tx_head)
        DL_UART_Main_disableInterrupt(UART0, DL_UART_MAIN_INTERRUPT_TX);
}

static void rx_drain(void)
{
    while (!DL_UART_Main_isRXFIFOEmpty(UART0)) {
        /* One read of RXDATA: data and its error flags (DL_UART_receiveData
         * and DL_UART_getErrorStatus would each pop a byte). */
        uint32_t w = UART0->RXDATA;
        if (w & UART_RXDATA_OVRERR_MASK)
            s_st.rx_overruns++;
        if (w & (UART_RXDATA_FRMERR_MASK | UART_RXDATA_PARERR_MASK | UART_RXDATA_BRKERR_MASK))
            s_st.rx_errors++;
        uint16_t head = s_rx_head;
        if ((uint16_t)(head - s_rx_tail) >= HAL_UART_RX_RING) {
            s_st.rx_overruns++;
            continue;
        }
        s_rx[head & RX_MASK] = (uint8_t)(w & UART_RXDATA_DATA_MASK);
        s_rx_head = (uint16_t)(head + 1u);
        s_st.rx_bytes++;
    }
}

void UART0_IRQHandler(void)
{
    for (;;) {
        /* reading IIDX clears the interrupt it reports */
        switch (DL_UART_Main_getPendingInterrupt(UART0)) {
        case DL_UART_MAIN_IIDX_RX: rx_drain(); break;
        case DL_UART_MAIN_IIDX_TX: tx_fill(); break;
        case DL_UART_MAIN_IIDX_NO_INTERRUPT: return;
        default: break;
        }
    }
}

void hal_radio_power(bool on)
{
    if (on) {
        if (s_on)
            return;
        /* 1. supply first: PA10 is GPIO low, PA11 input with pull-down */
        DL_GPIO_setPins(GPIOB, RADIO_EN_PIN);
        /* 2. pins to UART0 (TI: GPIO init before UART init). RX pulled up so
         *    the line idles high until the CH585 drives it. */
        DL_GPIO_initPeripheralOutputFunction(UART_TX_PINCM, IOMUX_PINCM21_PF_UART0_TX);
        DL_GPIO_initPeripheralInputFunctionFeatures(UART_RX_PINCM, IOMUX_PINCM22_PF_UART0_RX,
            DL_GPIO_INVERSION_DISABLE, DL_GPIO_RESISTOR_PULL_UP, DL_GPIO_HYSTERESIS_DISABLE,
            DL_GPIO_WAKEUP_DISABLE);
        /* 3. UART0 */
        DL_UART_Main_reset(UART0);
        DL_UART_Main_enablePower(UART0);
        bsp_delay_cycles(32u); /* POWER_STARTUP_DELAY (SysConfig uses 16) */
        DL_UART_Main_setClockConfig(UART0, &k_uart_clk);
        DL_UART_Main_init(UART0, &k_uart_cfg);
        DL_UART_Main_setOversampling(UART0, DL_UART_MAIN_OVERSAMPLING_RATE_16X);
        uart_set_divisor();
        DL_UART_Main_enableFIFOs(UART0);
        DL_UART_Main_setRXFIFOThreshold(UART0, DL_UART_MAIN_RX_FIFO_LEVEL_ONE_ENTRY);
        DL_UART_Main_setTXFIFOThreshold(UART0, DL_UART_TX_FIFO_LEVEL_1_2_EMPTY);
        s_tx_head = s_tx_tail = 0u;
        s_rx_head = s_rx_tail = 0u;
        DL_UART_Main_clearInterruptStatus(UART0,
            DL_UART_MAIN_INTERRUPT_RX | DL_UART_MAIN_INTERRUPT_TX);
        DL_UART_Main_enableInterrupt(UART0, DL_UART_MAIN_INTERRUPT_RX);
        DL_UART_Main_enable(UART0);
        NVIC_SetPriority(UART0_INT_IRQn, BSP_PRIO_UART);
        NVIC_ClearPendingIRQ(UART0_INT_IRQn);
        NVIC_EnableIRQ(UART0_INT_IRQn);
        s_on = true;
    } else {
        /* 1. UART0 off */
        NVIC_DisableIRQ(UART0_INT_IRQn);
        if (s_on) {
            DL_UART_Main_disableInterrupt(UART0,
                DL_UART_MAIN_INTERRUPT_RX | DL_UART_MAIN_INTERRUPT_TX);
            DL_UART_Main_disable(UART0);
        }
        s_on = false;
        /* 2. pins: the hal_init() off-state */
        bsp_radio_pins_safe();
        if (DL_UART_Main_isPowerEnabled(UART0))
            DL_UART_Main_disablePower(UART0);
        /* 3. supply */
        DL_GPIO_clearPins(GPIOB, RADIO_EN_PIN);
    }
}

/* PA10 GPIO output low, PA11 input with pull-down (as in gpio_init()). Also
 * on the fault path (bsp_safe_state), before RADIO_EN goes low. */
void bsp_radio_pins_safe(void)
{
    DL_GPIO_clearPins(GPIOA, UART_TX_PIN);
    DL_GPIO_initDigitalOutput(UART_TX_PINCM);
    DL_GPIO_enableOutput(GPIOA, UART_TX_PIN);
    DL_GPIO_initDigitalInputFeatures(UART_RX_PINCM, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_DOWN, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
}

bool hal_radio_powered(void)
{
    return s_on;
}

size_t hal_uart_write(const uint8_t *buf, size_t n)
{
    uint32_t pm;
    uint16_t head, i;
    if (!s_on || buf == NULL || n == 0u ||
        n > (size_t) (HAL_UART_TX_RING - (uint16_t) (s_tx_head - s_tx_tail)))
        return 0u;
    head = s_tx_head;
    for (i = 0u; i < (uint16_t) n; i++)
        s_tx[(uint16_t) (head + i) & TX_MASK] = buf[i];
    pm = hal_irq_lock();
    s_tx_head = (uint16_t) (head + (uint16_t) n);
    tx_fill();
    if (s_tx_tail != s_tx_head)
        DL_UART_Main_enableInterrupt(UART0, DL_UART_MAIN_INTERRUPT_TX);
    s_st.tx_bytes += (uint32_t) n;
    hal_irq_unlock(pm);
    return n;
}

size_t hal_uart_read(uint8_t *buf, size_t max)
{
    size_t n = 0u;
    uint16_t tail = s_rx_tail;
    while (n < max && tail != s_rx_head) {
        buf[n++] = s_rx[tail & RX_MASK];
        tail++;
    }
    s_rx_tail = tail;
    return n;
}

void hal_uart_stats(hal_uart_stats_t *out)
{
    uint32_t pm;
    if (out == NULL)
        return;
    pm = hal_irq_lock();
    out->rx_bytes = s_st.rx_bytes;
    out->tx_bytes = s_st.tx_bytes;
    out->rx_overruns = s_st.rx_overruns;
    out->rx_errors = s_st.rx_errors;
    hal_irq_unlock(pm);
}

/* hal_clock_fast(), interrupts locked, BEFORE ULPCLK changes: let the TX
 * FIFO and shifter empty (<= 5 bytes = 109 us at 460800; the ISR cannot
 * refill it now). Bounded: >= 160 us at 80 MHz, >= 400 us at 32 MHz.
 * Also waits for a received byte in progress (BUSY covers both). */
void bsp_uart_drain_tx(void)
{
    uint32_t k;
    if (!s_on)
        return;
    for (k = 0u; k < 400u && (!DL_UART_Main_isTXFIFOEmpty(UART0) || DL_UART_Main_isBusy(UART0));
         k++)
        bsp_delay_cycles(32u);
}

/* hal_clock_fast(), interrupts locked, AFTER the switch: divider for the
 * new ULPCLK (g_ulpclk_hz already updated). */
void bsp_uart_retime(void)
{
    if (!s_on)
        return;
    DL_UART_Main_disable(UART0);
    uart_set_divisor();
    DL_UART_Main_enable(UART0);
}
