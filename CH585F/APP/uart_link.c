/* UART1 link driver. Pin set-up and interrupt handling follow WCH's
 * EVT/EXAM/UART1 and BLE/BLE_UART examples; the remap uses RB_PIN_UART1
 * (R16_PIN_ALTERNATE), which moves RXD1/TXD1 from PA8/PA9 to PB12/PB13.
 *
 * Baud: UART1_BaudRateCfg() programs DL = round(Fsys / 8 / baud) with
 * R8_UART1_DIV = 1. At Fsys = 62.4 MHz and 460800 baud DL = 17, giving
 * 458823.5 baud (-0.43 %). */
#include "CONFIG.h"

#include "radio_board.h"
#include "link_fmt.h"
#include "rl_ring.h"
#include "uart_link.h"

static uint8_t s_rx_buf[UART_LINK_RX_SIZE];
static uint8_t s_tx_buf[UART_LINK_TX_SIZE];
static rl_ring_t s_rx, s_tx;
static volatile uart_link_stats_t s_st;

void uart_link_init(void)
{
    rl_ring_init(&s_rx, s_rx_buf, UART_LINK_RX_SIZE);
    rl_ring_init(&s_tx, s_tx_buf, UART_LINK_TX_SIZE);

    /* Configure the IO first, then the UART (UART1 example). TXD idles
     * high: set the level before making it an output. */
    GPIOPinRemap(ENABLE, RB_PIN_UART1);
    GPIOB_SetBits(BOARD_UART_TX_PIN);
    GPIOB_ModeCfg(BOARD_UART_RX_PIN, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(BOARD_UART_TX_PIN, GPIO_ModeOut_PP_5mA);

    UART1_DefInit(); /* 8N1, FIFO on, TX enabled, DIV = 1 */
    UART1_BaudRateCfg(LINK_BAUD);
    /* Interrupt at 2 bytes: leaves 6 bytes (130 us at 460800) of FIFO for
     * interrupt latency; the receive timeout collects the rest. */
    UART1_ByteTrigCfg(UART_2BYTE_TRIG);
    UART1_INTCfg(ENABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT);
    PFIC_EnableIRQ(UART1_IRQn);
}

/* Move bytes from the TX ring into the hardware FIFO; stop the THR-empty
 * interrupt once the ring is empty. Interrupt context, or task context
 * with interrupts disabled. */
__HIGH_CODE
static void uart_link_fill_fifo(void)
{
    uint8_t b;
    while (R8_UART1_TFC < UART_FIFO_SIZE) {
        if (!rl_ring_get(&s_tx, &b)) {
            R8_UART1_IER &= (uint8_t)~RB_IER_THR_EMPTY;
            return;
        }
        R8_UART1_THR = b;
    }
}

bool uart_link_write(const uint8_t *p, size_t n)
{
    uint32_t irq;

    if (!rl_ring_write(&s_tx, p, n)) {
        s_st.tx_full++;
        return false;
    }
    /* Same critical section as BLE_UART's app_uart_process(): a few
     * register accesses with interrupts off. */
    SYS_DisableAllIrq(&irq);
    uart_link_fill_fifo();
    if (rl_ring_count(&s_tx))
        R8_UART1_IER |= RB_IER_THR_EMPTY;
    SYS_RecoverIrq(irq);
    return true;
}

bool uart_link_rx_get(uint8_t *b)
{
    return rl_ring_get(&s_rx, b);
}

bool uart_link_rx_pending(void)
{
    return rl_ring_count(&s_rx) != 0u;
}

bool uart_link_tx_busy(void)
{
    return rl_ring_count(&s_tx) != 0u || !(R8_UART1_LSR & RB_LSR_TX_ALL_EMP);
}

void uart_link_stats(uart_link_stats_t *s)
{
    s->rx_overflow = s_st.rx_overflow;
    s->overrun = s_st.overrun;
    s->frame = s_st.frame;
    s->parity = s_st.parity;
    s->brk = s_st.brk;
    s->tx_full = s_st.tx_full;
}

uint16_t uart_link_divisor(void)
{
    return R16_UART1_DL;
}

uint32_t uart_link_actual_baud(void)
{
    uint32_t dl = R16_UART1_DL, div = R8_UART1_DIV;
    if (dl == 0u || div == 0u)
        return 0;
    return GetSysClock() * 2u / div / 16u / dl;
}

__INTERRUPT
__HIGH_CODE
void UART1_IRQHandler(void)
{
    switch (UART1_GetITFlag()) {
    case UART_II_LINE_STAT: { /* line status error; reading LSR clears it */
        uint8_t lsr = UART1_GetLinSTA();
        if (lsr & RB_LSR_OVER_ERR)
            s_st.overrun++;
        if (lsr & RB_LSR_FRAME_ERR)
            s_st.frame++;
        if (lsr & RB_LSR_PAR_ERR)
            s_st.parity++;
        if (lsr & RB_LSR_BREAK_ERR)
            s_st.brk++;
        break;
    }

    case UART_II_RECV_RDY:  /* FIFO reached the trigger level */
    case UART_II_RECV_TOUT: /* receive timeout: the rest of a burst */
        while (R8_UART1_RFC) {
            uint8_t b = R8_UART1_RBR;
            if (!rl_ring_put(&s_rx, b))
                s_st.rx_overflow++;
        }
        break;

    case UART_II_THR_EMPTY:
        uart_link_fill_fifo();
        break;

    default:
        break;
    }
}
