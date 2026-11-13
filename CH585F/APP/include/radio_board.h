/* CH585 board support for the ECG Card radio (ecg-fw.md, "CH585
 * configuration"): pin states, watchdog, reset reason. */
#ifndef RADIO_BOARD_H
#define RADIO_BOARD_H

#include <stdint.h>

/* RADIO_IRQ (PB4, active high). Held low in this version. */
#define BOARD_RADIO_IRQ_PIN GPIO_Pin_4
/* UART1 remapped: RXD1_ = PB12, TXD1_ = PB13 (RB_PIN_UART1). */
#define BOARD_UART_RX_PIN   GPIO_Pin_12
#define BOARD_UART_TX_PIN   GPIO_Pin_13

/* Read once at boot, before anything else: R8_RESET_STATUS & RB_RESET_FLAG
 * (RST_FLAG_SW 0, RPOR 1, WTR 2 watchdog, MR 3, GPWSM 5). */
uint8_t board_reset_reason(void);

/* Every pin to a defined state: unused pins input pull-up (as WCH's
 * examples do before sleeping), RADIO_IRQ output low. The crystal pins
 * (PA10/PA11) and 2-wire debug pins (PB14/PB15) are left alone. The UART
 * pins are configured by uart_link_init(). */
void board_pins_init(void);

/* Start / feed the watchdog (no-ops when APP_WATCHDOG is 0). */
void board_watchdog_start(void);
void board_watchdog_feed(void);

#endif
