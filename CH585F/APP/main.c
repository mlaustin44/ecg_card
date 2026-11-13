/* CH585 radio firmware for the ECG Card: BLE peripheral + UART bridge to
 * the MSPM0 (fw/include/link_fmt.h, copied in APP/include).
 *
 * Start-up order follows WCH's BLE/Peripheral/APP/peripheral_main.c:
 * DC-DC, 32 MHz crystal load, system clock, then CH58x_BLEInit(),
 * HAL_Init(), GAPRole_PeripheralInit(), application tasks, TMOS loop.
 * No DEBUG define: UART1 is the link and there is no debug UART; logs go
 * to the MCU as STREAM_LOG frames. */
#include "CONFIG.h"
#include "HAL.h"

#include "app_config.h"
#include "ble_app.h"
#include "radio_board.h"
#include "link_mgr.h"
#include "uart_link.h"

/* BLE stack heap (CONFIG.h: BLE_MEMHEAP_SIZE, set in the project defines) */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if (defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/*********************************************************************
 * @fn      Main_Circulation
 *
 * @brief   Main loop: TMOS, then schedule the link RX event if bytes wait.
 */
__HIGH_CODE
__attribute__((noinline))
void Main_Circulation(void)
{
    while (1) {
        TMOS_SystemProcess();
        link_mgr_poll();
    }
}

int main(void)
{
    uint8_t reset_reason = board_reset_reason();
    uint8_t addr[6];

#if (defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif
    /* X2: 12 pF crystal; RB_XT32M_C_LOAD 0001b = 12 pF (reset 10 pF).
     * Trim at bring-up from the measured carrier offset. */
    HSECFG_Capacitance(HSECap_12p);
    SetSysClock(SYSCLK_FREQ);

    board_pins_init();

    /* X3: 12.5 pF crystal; RB_XT32K_C_LOAD 0000b = 12 pF (reset 1100b,
     * about 24 pF). Set before HAL_TimeInit() powers the oscillator
     * (CLK_OSC32K = 0). Trim at bring-up from RTC drift. */
    LSECFG_Capacitance(LSECap_12p);

    uart_link_init();

    /* Public address the stack uses (BLE_MAC FALSE: the chip's MAC),
     * least significant byte first, as CH58x_BLEInit() passes it. */
    GetMACAddress(addr);

    CH58x_BLEInit();
    HAL_Init();
    GAPRole_PeripheralInit();
    link_mgr_init(reset_reason, addr);
    ble_app_init(addr);
    board_watchdog_start();
    Main_Circulation();
    return 0;
}
