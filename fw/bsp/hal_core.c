/* hal.h core: init, reset cause, watchdog, clocks, time, GPIO, events,
 * SLEEP/STANDBY and the RTC. */
#include <string.h>

#include "bsp_internal.h"

/* ======================================================================== */
/* Shared state                                                             */
/* ======================================================================== */
volatile uint32_t g_mclk_hz = BSP_SYSOSC_HZ;
volatile uint32_t g_ulpclk_hz = BSP_SYSOSC_HZ;
volatile uint32_t g_events;
volatile uint32_t g_wake_events;

static volatile uint32_t s_millis;
static volatile uint32_t s_rtc_secs;
static volatile bool s_in_standby;
static volatile bool s_rtc_deadline_armed;
static volatile uint32_t s_rtc_deadline;
static volatile uint32_t s_chg_pg_last; /* last CHG_PG level seen by the ISR */
static hal_reset_cause_t s_reset_cause = HAL_RESET_OTHER;
static bool s_inited;
static bool s_fast;
static DL_SYSCTL_FLASH_WAIT_STATE s_flash_ws_slow = DL_SYSCTL_FLASH_WAIT_STATE_1;

/* ======================================================================== */
/* Pin map (verified against mspm0g350x.h IOMUX_PINCMx_PF_GPIOx_DIOy)        */
/* ======================================================================== */
#define PA ((GPIO_Regs *) GPIOA_BASE)
#define PB ((GPIO_Regs *) GPIOB_BASE)

const bsp_pin_t g_bsp_pins[PIN_COUNT] = {
    [PIN_FAST_RESTORE] = {PA, DL_GPIO_PIN_2, IOMUX_PINCM7},
    [PIN_MEASURE_REQ]  = {PA, DL_GPIO_PIN_3, IOMUX_PINCM8},
    [PIN_BUCK_PS_SYNC] = {PA, DL_GPIO_PIN_4, IOMUX_PINCM9},
    [PIN_OLED_EN]      = {PB, DL_GPIO_PIN_2, IOMUX_PINCM15},
    [PIN_VLED_EN]      = {PB, DL_GPIO_PIN_3, IOMUX_PINCM16},
    [PIN_ANA_EN]       = {PA, DL_GPIO_PIN_8, IOMUX_PINCM19},
    [PIN_GAIN_HI]      = {PA, DL_GPIO_PIN_9, IOMUX_PINCM20},
    [PIN_OLED_DC]      = {PB, DL_GPIO_PIN_6, IOMUX_PINCM23},
    [PIN_OLED_CS]      = {PB, DL_GPIO_PIN_7, IOMUX_PINCM24},
    [PIN_OLED_RESET]   = {PB, DL_GPIO_PIN_14, IOMUX_PINCM31},
    [PIN_RADIO_EN]     = {PB, DL_GPIO_PIN_18, IOMUX_PINCM44},
    [PIN_RLD_L_EN]     = {PB, DL_GPIO_PIN_19, IOMUX_PINCM45},
    [PIN_RLD_R_EN]     = {PB, DL_GPIO_PIN_20, IOMUX_PINCM48},
    [PIN_CHG_PG]       = {PA, DL_GPIO_PIN_28, IOMUX_PINCM3},
    [PIN_CHG_STAT1]    = {PA, DL_GPIO_PIN_5, IOMUX_PINCM10},
    [PIN_BUTTON1]      = {PA, DL_GPIO_PIN_31, IOMUX_PINCM6},
    [PIN_BUTTON2]      = {PB, DL_GPIO_PIN_15, IOMUX_PINCM32},
    [PIN_RADIO_IRQ]    = {PB, DL_GPIO_PIN_16, IOMUX_PINCM33},
    [PIN_CW_ALRT]      = {PA, DL_GPIO_PIN_13, IOMUX_PINCM35},
};

/* Board-signal outputs per port (all have board pull-downs). */
#define A_BOARD_OUT (DL_GPIO_PIN_2 | DL_GPIO_PIN_3 | DL_GPIO_PIN_4 | \
                     DL_GPIO_PIN_8 | DL_GPIO_PIN_9)
#define B_BOARD_OUT (DL_GPIO_PIN_2 | DL_GPIO_PIN_3 | DL_GPIO_PIN_6 | \
                     DL_GPIO_PIN_7 | DL_GPIO_PIN_14 | DL_GPIO_PIN_18 | \
                     DL_GPIO_PIN_19 | DL_GPIO_PIN_20)
/* Radio off-state TX (PA10) and the unused pins, driven low. */
#define A_UART_TX   DL_GPIO_PIN_10
#define A_UNUSED    (DL_GPIO_PIN_6 | DL_GPIO_PIN_7 | DL_GPIO_PIN_21 | \
                     DL_GPIO_PIN_23 | DL_GPIO_PIN_24 | DL_GPIO_PIN_25)
#define B_UNUSED    DL_GPIO_PIN_24
/* OLED SPI pins while detached (PB8 PICO, PB9 SCK). */
#define B_OLED_BUS  (DL_GPIO_PIN_8 | DL_GPIO_PIN_9)

/* Interrupt sources. */
#define A_IRQ_CHG_PG   DL_GPIO_PIN_28
#define A_IRQ_STAT1    DL_GPIO_PIN_5
#define A_IRQ_BTN1     DL_GPIO_PIN_31
#define A_IRQ_CW_ALRT  DL_GPIO_PIN_13
#define B_IRQ_BTN2     DL_GPIO_PIN_15
#define B_IRQ_RADIO    DL_GPIO_PIN_16
#define A_IRQ_ALL (A_IRQ_CHG_PG | A_IRQ_STAT1 | A_IRQ_BTN1 | A_IRQ_CW_ALRT)
/* RADIO_IRQ is configured (pull-down, rising edge, FastWake) but left masked
 * in this build: the radio is never powered, and hal_init() does not list it
 * as a wake source. Set to 1 when the radio link is brought up. */
#define BSP_RADIO_IRQ_ENABLED 0
#if BSP_RADIO_IRQ_ENABLED
#define B_IRQ_ALL (B_IRQ_BTN2 | B_IRQ_RADIO)
#else
#define B_IRQ_ALL (B_IRQ_BTN2)
#endif

static const uint8_t k_unused_pincm[] = {
    IOMUX_PINCM11, /* PA6 */  IOMUX_PINCM14, /* PA7 */
    IOMUX_PINCM46, /* PA21 */ IOMUX_PINCM53, /* PA23 */
    IOMUX_PINCM54, /* PA24 */ IOMUX_PINCM55, /* PA25 */
    IOMUX_PINCM52, /* PB24 */
};
static const uint8_t k_analog_pincm[] = {
    IOMUX_PINCM36, /* PA14 ECG_ADC  ADC0 ch12 */
    IOMUX_PINCM37, /* PA15 DAC12 out (unconnected) */
    IOMUX_PINCM38, /* PA16 PPG_HI   OPA1_OUT / ADC1 ch1 */
    IOMUX_PINCM39, /* PA17 PPG_DCC  OPA1_IN1- */
    IOMUX_PINCM43, /* PB17 VMID_MON ADC1 ch4 */
    IOMUX_PINCM47, /* PA22 TIA_OUT  OPA0_OUT / ADC1 ch8 */
    IOMUX_PINCM59, /* PA26 PD_BIAS  unused analog */
    IOMUX_PINCM60, /* PA27 PD_SUM   OPA0_IN0- */
};

void bsp_led_blank_gpio_high(void)
{
    DL_GPIO_setPins(BSP_LED_BLANK_PORT, BSP_LED_BLANK_PIN);
    DL_GPIO_initDigitalOutput(BSP_LED_BLANK_IOMUX);
    DL_GPIO_enableOutput(BSP_LED_BLANK_PORT, BSP_LED_BLANK_PIN);
}

static void gpio_init(void)
{
    size_t i;

    /* Output latches first, so no pin glitches high when DOE is set. */
    DL_GPIO_clearPins(GPIOA, A_BOARD_OUT | A_UART_TX | A_UNUSED);
    DL_GPIO_clearPins(GPIOB, B_BOARD_OUT | B_OLED_BUS | B_UNUSED);
    DL_GPIO_setPins(GPIOA, BSP_LED_BLANK_PIN); /* LEDs off */

    for (i = 0; i < (size_t) PIN_FIRST_INPUT; i++)
        DL_GPIO_initDigitalOutput(g_bsp_pins[i].pincm);
    for (i = 0; i < sizeof k_unused_pincm; i++)
        DL_GPIO_initDigitalOutput(k_unused_pincm[i]);
    DL_GPIO_initDigitalOutput(IOMUX_PINCM21); /* PA10 UART0 TX off-state: low */
    DL_GPIO_initDigitalOutput(IOMUX_PINCM25); /* PB8 OLED MOSI detached: low */
    DL_GPIO_initDigitalOutput(IOMUX_PINCM26); /* PB9 OLED SCK detached: low */
    DL_GPIO_initDigitalOutput(BSP_LED_BLANK_IOMUX);

    DL_GPIO_enableOutput(GPIOA, A_BOARD_OUT | A_UART_TX | A_UNUSED |
                                    BSP_LED_BLANK_PIN);
    DL_GPIO_enableOutput(GPIOB, B_BOARD_OUT | B_OLED_BUS | B_UNUSED);

    /* Inputs. CHG_PG, CHG_STAT1 and CW_ALRT have board pull-ups. The buttons
     * get the internal pull-up as well (harmless if a board pull-up exists,
     * required if not). RADIO_IRQ floats while the radio is off: pull-down. */
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM3, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM10, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM6, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_UP, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM32, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_UP, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM33, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_DOWN, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM35, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
    /* PA11 UART0 RX off-state: input with pull-down. */
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM22, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_PULL_DOWN, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);
    /* PA18 BSL_INVOKE: input, board pull-down R26, never driven. */
    DL_GPIO_initDigitalInputFeatures(IOMUX_PINCM40, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_ENABLE, DL_GPIO_WAKEUP_DISABLE);

    /* Analog pins: IOMUX function 0 (PC unconnected), input buffer off. */
    for (i = 0; i < sizeof k_analog_pincm; i++)
        DL_GPIO_initPeripheralAnalogFunction(k_analog_pincm[i]);

    /* PA19/PA20 (SWD) are left in their reset (SWD) function. */

    /* Edge selection. These write whole POLARITY registers. */
    DL_GPIO_setLowerPinsPolarity(GPIOA,
        DL_GPIO_PIN_5_EDGE_RISE_FALL | DL_GPIO_PIN_13_EDGE_FALL);
    DL_GPIO_setUpperPinsPolarity(GPIOA,
        DL_GPIO_PIN_28_EDGE_RISE_FALL | DL_GPIO_PIN_31_EDGE_RISE_FALL);
    DL_GPIO_setLowerPinsPolarity(GPIOB, DL_GPIO_PIN_15_EDGE_RISE_FALL);
    DL_GPIO_setUpperPinsPolarity(GPIOB, DL_GPIO_PIN_16_EDGE_RISE);

    /* FastWake on the STANDBY wake pins (edge detection without ULPCLK). */
    DL_GPIO_enableFastWakePins(GPIOA, A_IRQ_CHG_PG | A_IRQ_BTN1 | A_IRQ_CW_ALRT);
    DL_GPIO_enableFastWakePins(GPIOB, B_IRQ_BTN2 | B_IRQ_RADIO);

    s_chg_pg_last = DL_GPIO_readPins(GPIOA, A_IRQ_CHG_PG);
    DL_GPIO_clearInterruptStatus(GPIOA, 0xFFFFFFFFu);
    DL_GPIO_clearInterruptStatus(GPIOB, 0xFFFFFFFFu);
    DL_GPIO_enableInterrupt(GPIOA, A_IRQ_ALL);
    DL_GPIO_enableInterrupt(GPIOB, B_IRQ_ALL);

    NVIC_SetPriority(GPIOA_INT_IRQn, BSP_PRIO_GPIO); /* = INT_GROUP1 */
    NVIC_ClearPendingIRQ(GPIOA_INT_IRQn);
    NVIC_EnableIRQ(GPIOA_INT_IRQn);
}

/* GPIOA and GPIOB share INT_GROUP1. */
void GROUP1_IRQHandler(void)
{
    uint32_t a = DL_GPIO_getEnabledInterruptStatus(GPIOA, A_IRQ_ALL);
    uint32_t b = DL_GPIO_getEnabledInterruptStatus(GPIOB, B_IRQ_ALL);
    uint32_t ev = 0;

    if (a & A_IRQ_CHG_PG) {
        /* Safety path first. Clear the flag before sampling the level so an
         * edge after the sample re-raises the interrupt. */
        DL_GPIO_clearInterruptStatus(GPIOA, A_IRQ_CHG_PG);
        uint32_t lvl = DL_GPIO_readPins(GPIOA, A_IRQ_CHG_PG);
        if (lvl == 0u) {
            /* Falling edge: USB plugged in. MEASURE_REQ low before latching. */
            DL_GPIO_clearPins(GPIOA, DL_GPIO_PIN_3);
            ev |= HAL_EV_USB_PLUGGED;
        } else if (s_chg_pg_last != 0u) {
            /* High before and high now: a low pulse came and went before we
             * sampled. Treat it as plug + unplug, safety action included. */
            DL_GPIO_clearPins(GPIOA, DL_GPIO_PIN_3);
            ev |= HAL_EV_USB_PLUGGED | HAL_EV_USB_REMOVED;
        } else {
            ev |= HAL_EV_USB_REMOVED;
        }
        s_chg_pg_last = lvl;
    }
    if (a & (A_IRQ_BTN1)) ev |= HAL_EV_BUTTON;
    if (a & A_IRQ_STAT1) ev |= HAL_EV_CHG_STAT;
    if (a & A_IRQ_CW_ALRT) ev |= HAL_EV_CW_ALRT;
    if (b & B_IRQ_BTN2) ev |= HAL_EV_BUTTON;
    if (b & B_IRQ_RADIO) ev |= HAL_EV_RADIO_IRQ;

    DL_GPIO_clearInterruptStatus(GPIOA, a & ~A_IRQ_CHG_PG);
    DL_GPIO_clearInterruptStatus(GPIOB, b);
    bsp_latch(ev);
}

/* ======================================================================== */
/* Clocks                                                                   */
/* ======================================================================== */
/* SYSPLL from SYSOSC (TI sysctl_mclk_syspll example values):
 * 32 MHz / PDIV 2 = 16 MHz ref, VCO = 16 * (QDIV + 1) = 160 MHz,
 * CLK2X = 160 / (RDIVCLK2X + 1) / 2 * 2 = 80 MHz -> MCLK,
 * CLK1  = 160 / ((RDIVCLK1 + 1) * 2) = 40 MHz (used only by the FCC check). */
static const DL_SYSCTL_SYSPLLConfig k_pll80 = {
    .inputFreq   = DL_SYSCTL_SYSPLL_INPUT_FREQ_16_32_MHZ,
    .rDivClk2x   = 3,
    .rDivClk1    = 1,
    .rDivClk0    = 0,
    .enableCLK2x = DL_SYSCTL_SYSPLL_CLK2X_ENABLE,
    .enableCLK1  = DL_SYSCTL_SYSPLL_CLK1_ENABLE,
    .enableCLK0  = DL_SYSCTL_SYSPLL_CLK0_DISABLE,
    .sysPLLMCLK  = DL_SYSCTL_SYSPLL_MCLK_CLK2X,
    .sysPLLRef   = DL_SYSCTL_SYSPLL_REF_SYSOSC,
    .qDiv        = 9,
    .pDiv        = DL_SYSCTL_SYSPLL_PDIV_2,
};

/* SYSPLL_ERR_01 workaround (TI erratum, as generated by SysConfig): measure
 * SYSPLLCLK1 and SYSOSC against one LFCLK period with the FCC and require the
 * ratio 40/32 = 1.250 within +-0.3 %. Runs at 32 MHz MCLK. */
static uint32_t fcc_measure(DL_SYSCTL_FCC_CLOCK_SOURCE src)
{
    uint32_t n;
    DL_SYSCTL_configFCC(DL_SYSCTL_FCC_TRIG_TYPE_RISE_RISE,
                        DL_SYSCTL_FCC_TRIG_SOURCE_LFCLK, src);
    DL_SYSCTL_startFCC();
    for (n = 0; n < 66u && !DL_SYSCTL_isFCCDone(); n++)
        bsp_delay_cycles(977u); /* ~1 LFCLK period at 32 MHz */
    return DL_SYSCTL_readFCC();
}

static bool pll_lock_ok(void)
{
    uint32_t pll, osc, ratio;
    DL_SYSCTL_setFCCPeriods(DL_SYSCTL_FCC_TRIG_CNT_01);
    pll = fcc_measure(DL_SYSCTL_FCC_CLOCK_SOURCE_SYSPLLCLK1);
    osc = fcc_measure(DL_SYSCTL_FCC_CLOCK_SOURCE_SYSOSC);
    if (osc == 0u) return false;
    ratio = (pll * 1000u) / osc;
    return ratio > 1246u && ratio < 1254u; /* 1250 * (1 -+ 0.003) */
}

static bool pll_start(void)
{
    unsigned tries;
    uint32_t t;

    DL_SYSCTL_configSYSPLL(&k_pll80); /* waits for SYSPLLGOOD */
    for (tries = 0; tries < 8u; tries++) {
        if (pll_lock_ok()) return true;
        DL_SYSCTL_disableSYSPLL();
        DL_SYSCTL_enableSYSPLL();
        for (t = 0; t < 100000u; t++) {
            if ((DL_SYSCTL_getClockStatus() & SYSCTL_CLKSTATUS_SYSPLLGOOD_MASK) ==
                DL_SYSCTL_CLK_STATUS_SYSPLL_GOOD)
                break;
        }
    }
    DL_SYSCTL_disableSYSPLL();
    return false;
}

/* Wait (bounded) for the OLED DMA so SPI1 is never retimed mid-transfer. */
static void wait_oled_idle(void)
{
    uint32_t t;
    for (t = 0; t < 2000000u && hal_oled_busy(); t++) {
    }
}

void hal_clock_fast(bool fast)
{
    uint32_t pm;

    if (fast == s_fast || hal_acq_running() != 0u) return;
    wait_oled_idle();

    if (fast) {
        DL_SYSCTL_setFlashWaitState(DL_SYSCTL_FLASH_WAIT_STATE_2);
        if (!pll_start()) {
            DL_SYSCTL_setFlashWaitState(s_flash_ws_slow);
            static const char msg[] = "bsp: SYSPLL lock check failed, staying at 32 MHz\n";
            (void) hal_log_write(msg, sizeof msg - 1u);
            return;
        }
        pm = hal_irq_lock();
        /* ULPCLK must stay <= 40 MHz: divide before MCLK goes to 80 MHz. */
        DL_SYSCTL_setULPCLKDivider(DL_SYSCTL_ULPCLK_DIV_2);
        DL_SYSCTL_switchMCLKfromSYSOSCtoHSCLK(DL_SYSCTL_HSCLK_SOURCE_SYSPLL);
        g_mclk_hz = BSP_PLL_HZ;
        g_ulpclk_hz = BSP_PLL_HZ / 2u;
        bsp_systick_config();
        bsp_spi_retime();
        bsp_i2c_retime();
        s_fast = true;
        hal_irq_unlock(pm);
    } else {
        pm = hal_irq_lock();
        DL_SYSCTL_switchMCLKfromHSCLKtoSYSOSC();
        DL_SYSCTL_setULPCLKDivider(DL_SYSCTL_ULPCLK_DIV_1);
        g_mclk_hz = BSP_SYSOSC_HZ;
        g_ulpclk_hz = BSP_SYSOSC_HZ;
        bsp_systick_config();
        bsp_spi_retime();
        bsp_i2c_retime();
        s_fast = false;
        hal_irq_unlock(pm);
        DL_SYSCTL_disableSYSPLL();
        DL_SYSCTL_setFlashWaitState(s_flash_ws_slow);
    }
}

static void sysctl_init(void)
{
    uint32_t t;

    DL_SYSCTL_setBORThreshold(DL_SYSCTL_BOR_THRESHOLD_LEVEL_0);
    /* A warm reset can leave MCLK on HSCLK: get back to SYSOSC first. */
    if ((SYSCTL->SOCLOCK.MCLKCFG & SYSCTL_MCLKCFG_USEHSCLK_MASK) != 0u)
        DL_SYSCTL_switchMCLKfromHSCLKtoSYSOSC();
    DL_SYSCTL_disableSYSPLL();
    DL_SYSCTL_disableHFXT(); /* no crystals: HFXT/LFXT pins are GPIO */
    DL_SYSCTL_setSYSOSCFreq(DL_SYSCTL_SYSOSC_FREQ_BASE);
    DL_SYSCTL_setULPCLKDivider(DL_SYSCTL_ULPCLK_DIV_1);
    DL_SYSCTL_setMCLKDivider(DL_SYSCTL_MCLK_DIVIDER_DISABLE);
    s_flash_ws_slow = DL_SYSCTL_getFlashWaitState();
    /* SYSOSC frequency correction loop, INTERNAL resistor mode. The external
     * ROSC mode would claim PA2, which is FAST_RESTORE on this board. */
    DL_SYSCTL_enableSYSOSCFCL();
    /* LFCLK = LFOSC (reset default; LFXT is never enabled, PA3/PA4 are GPIO). */
    for (t = 0; t < 1000000u; t++) {
        if ((DL_SYSCTL_getClockStatus() & DL_SYSCTL_CLK_STATUS_LFOSC_GOOD) ==
            DL_SYSCTL_CLK_STATUS_LFOSC_GOOD)
            break;
    }
    g_mclk_hz = BSP_SYSOSC_HZ;
    g_ulpclk_hz = BSP_SYSOSC_HZ;
    s_fast = false;
}

/* ======================================================================== */
/* Time                                                                     */
/* ======================================================================== */
void bsp_systick_config(void)
{
    SysTick->CTRL = 0u;
    SysTick->LOAD = g_mclk_hz / 1000u - 1u;
    SysTick->VAL = 0u;
    NVIC_SetPriority(SysTick_IRQn, BSP_PRIO_SYSTICK);
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
}

void SysTick_Handler(void)
{
    s_millis++;
}

uint32_t hal_millis(void)
{
    return s_millis;
}

void bsp_delay_cycles(uint32_t cycles)
{
    if (cycles != 0u) delay_cycles(cycles);
}

void hal_delay_us(uint32_t us)
{
    uint32_t per_us = g_mclk_hz / 1000000u;
    while (us > 50000u) { /* keep us * per_us well inside 32 bits */
        bsp_delay_cycles(50000u * per_us);
        us -= 50000u;
    }
    bsp_delay_cycles(us * per_us);
}

void hal_delay_ms(uint32_t ms)
{
    while (ms--) hal_delay_us(1000u);
}

/* ======================================================================== */
/* RTC: LFOSC, 1 Hz periodic interrupt -> monotonic seconds                  */
/* ======================================================================== */
static void rtc_init(void)
{
    static const DL_RTC_Calendar cal = {
        .seconds = 0, .minutes = 0, .hours = 0, .dayOfWeek = 0,
        .dayOfMonth = 1, .month = 1, .year = 2026,
    };
    DL_RTC_initCalendar(RTC, cal, DL_RTC_FORMAT_BINARY);
    /* Prescaler 1 interval: 1 Hz (same setting as TI's
     * rtc_periodic_alarm_lfosc_standby example). */
    DL_RTC_setPeriodicAlarm1(RTC, DL_RTC_PRESCALER1_DIVIDE_128);
    DL_RTC_clearInterruptStatus(RTC, DL_RTC_INTERRUPT_PRESCALER1);
    DL_RTC_enableInterrupt(RTC, DL_RTC_INTERRUPT_PRESCALER1);
    NVIC_SetPriority(RTC_INT_IRQn, BSP_PRIO_RTC);
    NVIC_ClearPendingIRQ(RTC_INT_IRQn);
    NVIC_EnableIRQ(RTC_INT_IRQn);
    DL_RTC_enableClockControl(RTC);
}

void RTC_IRQHandler(void)
{
    if (DL_RTC_getPendingInterrupt(RTC) == DL_RTC_IIDX_PRESCALER1) {
        uint32_t s = s_rtc_secs + 1u;
        s_rtc_secs = s;
        if (!s_in_standby)
            bsp_latch(HAL_EV_RTC);
        else if (s_rtc_deadline_armed && (int32_t) (s - s_rtc_deadline) >= 0)
            bsp_latch(HAL_EV_RTC);
        /* else: internal tick inside hal_standby(): not reported */
    }
}

uint32_t hal_rtc_seconds(void)
{
    return s_rtc_secs;
}

/* ======================================================================== */
/* Reset, watchdog, interrupt lock                                          */
/* ======================================================================== */
static hal_reset_cause_t map_reset_cause(DL_SYSCTL_RESET_CAUSE c)
{
    switch (c) {
    case DL_SYSCTL_RESET_CAUSE_POR_HW_FAILURE:
        return HAL_RESET_POWER_ON;
    case DL_SYSCTL_RESET_CAUSE_BOR_SUPPLY_FAILURE:
        return HAL_RESET_BROWNOUT;
    case DL_SYSCTL_RESET_CAUSE_POR_EXTERNAL_NRST:
    case DL_SYSCTL_RESET_CAUSE_BOOTRST_EXTERNAL_NRST:
        return HAL_RESET_PIN;
    case DL_SYSCTL_RESET_CAUSE_SYSRST_WWDT0_VIOLATION:
    case DL_SYSCTL_RESET_CAUSE_SYSRST_WWDT1_VIOLATION:
        return HAL_RESET_WATCHDOG;
    case DL_SYSCTL_RESET_CAUSE_POR_SW_TRIGGERED:
    case DL_SYSCTL_RESET_CAUSE_BOOTRST_SW_TRIGGERED:
    case DL_SYSCTL_RESET_CAUSE_SYSRST_SW_TRIGGERED:
    case DL_SYSCTL_RESET_CAUSE_CPURST_SW_TRIGGERED:
        return HAL_RESET_SOFTWARE;
    default:
        return HAL_RESET_OTHER; /* debug, BSL, ECC, lockup, clock fault, ... */
    }
}

hal_reset_cause_t hal_reset_cause(void)
{
    return s_reset_cause;
}

static void wdt_init(void)
{
    /* LFCLK 32768 Hz / 2 = 16384 Hz, 2^15 counts -> 2.0 s, no closed window.
     * Counts during SLEEP/STANDBY (hal_standby kicks it on every 1 Hz RTC
     * wake); paused while the debugger halts the core. */
    DL_WWDT_setCoreHaltBehavior(WWDT0, DL_WWDT_CORE_HALT_STOP);
    DL_WWDT_initWatchdogMode(WWDT0, DL_WWDT_CLOCK_DIVIDE_2,
        DL_WWDT_TIMER_PERIOD_15_BITS, DL_WWDT_RUN_IN_SLEEP,
        DL_WWDT_WINDOW_PERIOD_0, DL_WWDT_WINDOW_PERIOD_0);
    DL_WWDT_setActiveWindow(WWDT0, DL_WWDT_WINDOW0);
}

void hal_wdt_kick(void)
{
    DL_WWDT_restart(WWDT0);
}

void hal_system_reset(void)
{
    bsp_safe_state();
    DL_SYSCTL_resetDevice(DL_SYSCTL_RESET_BOOT);
    for (;;) {
    }
}

uint32_t hal_irq_lock(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

void hal_irq_unlock(uint32_t state)
{
    if ((state & 1u) == 0u) __enable_irq();
}

/* ======================================================================== */
/* GPIO                                                                     */
/* ======================================================================== */
void hal_pin_write(board_pin_t pin, bool high)
{
    if ((unsigned) pin >= (unsigned) PIN_FIRST_INPUT) return; /* inputs, range */
    const bsp_pin_t *p = &g_bsp_pins[pin];
    if (high)
        DL_GPIO_setPins(p->port, p->mask); /* DOUTSET: atomic, ISR-safe */
    else
        DL_GPIO_clearPins(p->port, p->mask);
}

bool hal_pin_read(board_pin_t pin)
{
    if ((unsigned) pin >= (unsigned) PIN_COUNT) return false;
    const bsp_pin_t *p = &g_bsp_pins[pin];
    if ((unsigned) pin < (unsigned) PIN_FIRST_INPUT)
        return (p->port->DOUT31_0 & p->mask) != 0u; /* output: read back latch */
    return DL_GPIO_readPins(p->port, p->mask) != 0u;
}

/* ======================================================================== */
/* Events, SLEEP, STANDBY                                                   */
/* ======================================================================== */
uint32_t hal_events_take(void)
{
    uint32_t pm = hal_irq_lock();
    uint32_t ev = g_events;
    g_events = 0u;
    hal_irq_unlock(pm);
    return ev;
}

void hal_idle(void)
{
    uint32_t pm = hal_irq_lock();
    if (g_events == 0u) {
        SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk; /* SLEEP0: clocks keep running */
        __WFI(); /* wakes on any pending IRQ even with PRIMASK set */
    }
    hal_irq_unlock(pm);
}

#define STANDBY_WAKE_MASK (HAL_EV_BUTTON | HAL_EV_USB_PLUGGED | \
                           HAL_EV_USB_REMOVED | HAL_EV_CW_ALRT | HAL_EV_RTC | \
                           HAL_EV_RADIO_IRQ)

uint32_t hal_standby(uint32_t wake_after_s)
{
    uint32_t pm, wake, stat_before, stat_after;

    if (hal_acq_running() != 0u) hal_acq_stop();
    if (s_fast) hal_clock_fast(false);
    wait_oled_idle();

    /* CHG_STAT1 is not a wake source (a 2 Hz fault toggle would keep the
     * card awake): mask it, and report a level change on exit instead. */
    stat_before = DL_GPIO_readPins(GPIOA, A_IRQ_STAT1);
    DL_GPIO_disableInterrupt(GPIOA, A_IRQ_STAT1);

    /* SysTick does not run in STANDBY; stop it so it cannot pend. */
    SysTick->CTRL = 0u;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

    pm = hal_irq_lock();
    /* hal.h: a stale 1 Hz tick is discarded; any other event still pending
     * (e.g. a button edge after the caller's last hal_events_take()) is a
     * wake reason, so the loop below returns at once instead of losing it. */
    g_events &= ~(uint32_t) HAL_EV_RTC;
    g_wake_events = g_events;
    s_rtc_deadline = s_rtc_secs + wake_after_s;
    s_rtc_deadline_armed = (wake_after_s != 0u);
    s_in_standby = true;
    hal_irq_unlock(pm);

    /* STANDBY0: PD0 (GPIO, RTC, WWDT, I2C0) keeps ULPCLK/LFCLK at 32 kHz, so
     * the RTC 1 Hz interrupt and GPIO edges are serviced; PD1 loses MCLK. */
    DL_SYSCTL_setPowerPolicySTANDBY0();
    for (;;) {
        hal_wdt_kick();
        pm = hal_irq_lock();
        if ((g_wake_events & STANDBY_WAKE_MASK) != 0u) {
            hal_irq_unlock(pm);
            break;
        }
        __WFI();
        hal_irq_unlock(pm); /* pending ISRs (RTC tick, GPIO) run here */
    }
    SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk; /* back to RUN0/SLEEP0 */

    pm = hal_irq_lock();
    s_in_standby = false;
    s_rtc_deadline_armed = false;
    wake = g_wake_events;
    hal_irq_unlock(pm);

    /* Restore what STANDBY does not keep. MCLK is SYSOSC again on exit.
     * SPI1 (PD1) loses its registers; I2C0 is re-initialised for symmetry. */
    bsp_systick_config();
    bsp_spi_init();
    bsp_i2c_init();

    DL_GPIO_clearInterruptStatus(GPIOA, A_IRQ_STAT1);
    DL_GPIO_enableInterrupt(GPIOA, A_IRQ_STAT1);
    stat_after = DL_GPIO_readPins(GPIOA, A_IRQ_STAT1);
    if (stat_after != stat_before) {
        pm = hal_irq_lock();
        bsp_latch(HAL_EV_CHG_STAT);
        hal_irq_unlock(pm);
        wake |= HAL_EV_CHG_STAT;
    }
    return wake & (STANDBY_WAKE_MASK | HAL_EV_CHG_STAT);
}

/* ======================================================================== */
/* Init and fault safety                                                    */
/* ======================================================================== */
void bsp_safe_state(void)
{
    /* Everything that powers or connects something: low. LEDs blanked. */
    DL_GPIO_clearPins(GPIOA, A_BOARD_OUT | A_UART_TX);
    DL_GPIO_clearPins(GPIOB, B_BOARD_OUT | B_OLED_BUS);
    DL_GPIO_initDigitalOutput(IOMUX_PINCM25);
    DL_GPIO_initDigitalOutput(IOMUX_PINCM26);
    DL_GPIO_enableOutput(GPIOB, B_OLED_BUS);
    bsp_led_blank_gpio_high();
}

void hal_init(void)
{
    if (s_inited) return; /* idempotent: main() and app_main() may both call */
    __disable_irq();

    s_reset_cause = map_reset_cause(DL_SYSCTL_getResetCause());

    /* Watchdog first, so a fault anywhere below (a clock that never comes
     * up, a peripheral that never answers) resets instead of hanging.
     * TRM SLAU846 41.2: the WWDT is disabled and cleared by every SYSRST
     * (and every BOOTRST / POR above it), so this first WWDTCTL0 write
     * enables it; a second one would be a violation, and hal_init() is
     * idempotent. Window: both closed-window periods are 0 %, so a restart
     * is valid at any time (no early-kick reset). Timing: the rest of
     * hal_init() takes well under 0.5 s (the longest step is the bounded
     * LFOSC-good poll in sysctl_init(), 1e6 iterations, normally a few
     * passes), then app_init() (MCP4728 EEPROM
     * programming ~50-100 ms, I2C timeouts of 10 ms per failed transfer,
     * OLED power-up ~10 ms) until the first hal_wdt_kick() in app_step(),
     * against a 2 s period. The WWDT counts LFCLK; LFOSC runs from reset. */
    DL_WWDT_reset(WWDT0);
    DL_WWDT_enablePower(WWDT0);
    bsp_delay_cycles(32u); /* POWER_STARTUP_DELAY (SysConfig uses 16) */
    wdt_init();

    DL_GPIO_reset(GPIOA);
    DL_GPIO_reset(GPIOB);
    DL_GPIO_enablePower(GPIOA);
    DL_GPIO_enablePower(GPIOB);
    DL_RTC_reset(RTC);
    DL_RTC_enablePower(RTC);
    bsp_delay_cycles(32u); /* POWER_STARTUP_DELAY (SysConfig uses 16) */

    gpio_init();
    sysctl_init();
    bsp_systick_config();
    rtc_init();
    bsp_i2c_init();
    bsp_spi_init();
    bsp_acq_init();
    bsp_rtt_init();
    hal_wdt_kick(); /* the period starts over for app_init() */

    g_events = 0u;
    s_inited = true;
    __enable_irq();
}
