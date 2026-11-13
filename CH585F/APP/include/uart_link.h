/* UART1 (remapped to PB12 RX / PB13 TX) at LINK_BAUD, 8N1, no flow control.
 *
 * RX: FIFO trigger + receive-timeout interrupts copy bytes into a ring
 * (WCH UART1 / BLE_UART example pattern); the task drains it.
 * TX: frames are copied into a ring and sent by the THR-empty interrupt;
 * uart_link_write never waits. */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UART_LINK_RX_SIZE 1024u /* power of two */
#define UART_LINK_TX_SIZE 1024u /* power of two */

typedef struct {
    uint32_t rx_overflow; /* bytes lost: RX ring full */
    uint32_t overrun;     /* LSR overrun (hardware FIFO) */
    uint32_t frame;       /* LSR framing error */
    uint32_t parity;      /* LSR parity error */
    uint32_t brk;         /* LSR break */
    uint32_t tx_full;     /* frames refused: TX ring full */
} uart_link_stats_t;

void uart_link_init(void);

/* Queue n bytes for transmission: all or nothing. */
bool uart_link_write(const uint8_t *p, size_t n);

/* Task side of the RX ring. */
bool uart_link_rx_get(uint8_t *b);
bool uart_link_rx_pending(void);

/* True while bytes are queued or still in the TX FIFO / shift register. */
bool uart_link_tx_busy(void);

void uart_link_stats(uart_link_stats_t *s);

/* Divider actually programmed and the resulting baud rate. */
uint16_t uart_link_divisor(void);
uint32_t uart_link_actual_baud(void);

#endif
