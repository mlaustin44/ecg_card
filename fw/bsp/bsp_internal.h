/* BSP-private declarations shared between the fw/bsp translation units.
 * This is the only place (with the BSP .c files) that includes TI headers. */
#ifndef BSP_INTERNAL_H
#define BSP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <ti/devices/msp/msp.h>
#include <ti/driverlib/driverlib.h>

#include "hal.h"

/* ---- Clocks ------------------------------------------------------------- */
#define BSP_SYSOSC_HZ   32000000u
#define BSP_PLL_HZ      80000000u
extern volatile uint32_t g_mclk_hz;   /* current MCLK (CPU, PD1 BUSCLK) */
extern volatile uint32_t g_ulpclk_hz; /* current ULPCLK (PD0 BUSCLK: I2C0) */

/* ---- Interrupt priorities (Cortex-M0+: 0 = highest .. 3) ---------------- */
#define BSP_PRIO_GPIO    0u  /* CHG_PG safety path, buttons */
#define BSP_PRIO_ADC0    0u  /* ECG DMA lap: re-arm DMAEN within 250 us */
#define BSP_PRIO_ADC1    1u  /* PPG frame collection */
#define BSP_PRIO_SYSTICK 2u
#define BSP_PRIO_RTC     2u

/* ---- Event fabric channels (1..11 are 1:1, 12..15 are 1:2 splitters) ---- */
#define BSP_EVT_CH_ECG   1u  /* TIMG6 CCD0 -> ADC0 trigger */
#define BSP_EVT_CH_PPG   2u  /* TIMA0 CCD0|CCD1 -> ADC1 trigger */

/* ---- DMA channels (0..2 FULL, 3..6 BASIC on MSPM0G3507) ------------------ */
#define BSP_DMA_CH_ECG   0u  /* FULL channel: repeat-single circular ring */
#define BSP_DMA_CH_OLED  3u  /* BASIC channel: SPI1 TX, single transfer */

/* ---- Pin helpers -------------------------------------------------------- */
typedef struct {
    GPIO_Regs *port;
    uint32_t mask;
    uint8_t pincm; /* IOMUX_PINCMx enum value (0-based index) */
} bsp_pin_t;
extern const bsp_pin_t g_bsp_pins[PIN_COUNT];

/* LED_BLANK = PA12 = PINCM34, TIMA0_CCP3 (PF 6). */
#define BSP_LED_BLANK_PORT  GPIOA
#define BSP_LED_BLANK_PIN   DL_GPIO_PIN_12
#define BSP_LED_BLANK_IOMUX IOMUX_PINCM34
#define BSP_LED_BLANK_FUNC  IOMUX_PINCM34_PF_TIMA0_CCP3
void bsp_led_blank_gpio_high(void); /* LED_BLANK as GPIO, driven high (LEDs off) */

/* ---- Events ------------------------------------------------------------- */
extern volatile uint32_t g_events;      /* HAL_EV_* latch */
extern volatile uint32_t g_wake_events; /* bits latched since hal_standby() entry */
static inline void bsp_latch(uint32_t ev) /* call from ISR (or with IRQs locked) */
{
    g_events |= ev;
    g_wake_events |= ev;
}

/* ---- Cross-module hooks ------------------------------------------------- */
void bsp_systick_config(void);
void bsp_i2c_init(void);
void bsp_i2c_retime(void);   /* recompute SCL divider for current ULPCLK */
void bsp_spi_init(void);
void bsp_spi_retime(void);   /* recompute SCK divider for current MCLK */
void bsp_acq_init(void);     /* power-on configuration of the acquisition blocks */
void bsp_rtt_init(void);
void bsp_safe_state(void);   /* fault path: rails/switches off, LEDs blanked */
/* ECG ring reads where DMASZ and DMADA disagreed (bring-up: shows whether
 * DMADA reads back live in repeat-single mode; DMASZ is the one used). */
uint32_t bsp_ecg_pos_mismatches(void);

/* Bounded busy-wait helpers (no SysTick dependence). */
void bsp_delay_cycles(uint32_t cycles);

#endif
