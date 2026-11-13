/* CH585 board support for the ECG Card radio. */
#include "CONFIG.h"

#include "app_config.h"
#include "radio_board.h"

uint8_t board_reset_reason(void)
{
    return (uint8_t)SYS_GetLastResetSta();
}

void board_pins_init(void)
{
    /* PA10/PA11: 32.768 kHz crystal (X32KI/X32KO). */
    const uint32_t pa_keep = GPIO_Pin_10 | GPIO_Pin_11;
    /* PB14/PB15: 2-wire debug (TIO/TCK); PB12/PB13: UART1 (uart_link_init). */
    const uint32_t pb_keep = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;

    /* Unused pins (NFC, USB, spare, PB22 CH1_BOOT with its 10k pull-up):
     * input with pull-up. */
    GPIOA_ModeCfg(GPIO_Pin_All & ~pa_keep, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All & ~pb_keep & ~BOARD_RADIO_IRQ_PIN, GPIO_ModeIN_PU);

    /* RADIO_IRQ: push-pull, driven low (no wake handshake in v1). */
    GPIOB_ResetBits(BOARD_RADIO_IRQ_PIN);
    GPIOB_ModeCfg(BOARD_RADIO_IRQ_PIN, GPIO_ModeOut_PP_5mA);
}

void board_watchdog_start(void)
{
#if APP_WATCHDOG
    WWDG_SetCounter(0);
    WWDG_ResetCfg(ENABLE);
#endif
}

void board_watchdog_feed(void)
{
#if APP_WATCHDOG
    WWDG_SetCounter(0);
#endif
}
