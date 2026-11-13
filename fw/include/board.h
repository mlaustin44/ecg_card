/* ECG Card board constants: signals, electrical scaling and timing.
 * Source: ecg-fw.md (final schematic, 2 Nov 2026). Portable; no MCU headers. */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/* ---- Named signals -------------------------------------------------------
 * GPIO-controlled nets only. Pins owned by a peripheral (I2C, SPI, UART,
 * LED_BLANK, analog) are handled inside the BSP.
 *
 * MSPM0G3507 LQFP-48 mapping (PINCM index in brackets):
 *   FAST_RESTORE PA2 [7]    MEASURE_REQ PA3 [8]    BUCK_PS_SYNC PA4 [9]
 *   OLED_EN PB2 [15]        VLED_EN PB3 [16]       ANA_EN PA8 [19]
 *   GAIN_HI PA9 [20]        OLED_DC PB6 [23]       OLED_CS PB7 [24]
 *   OLED_RESET PB14 [31]    RADIO_EN PB18 [44]     RLD_L_EN PB19 [45]
 *   RLD_R_EN PB20 [48]
 *   CHG_PG PA28 [3]         CHG_STAT1 PA5 [10]     BUTTON1 PA31 [6]
 *   BUTTON2 PB15 [32]       RADIO_IRQ PB16 [33]    CW_ALRT PA13 [35]
 * Peripheral pins:
 *   I2C0 SDA PA0 [1], SCL PA1 [2]      UART0 TX PA10 [21], RX PA11 [22]
 *   SPI1 PICO PB8 [25], SCK PB9 [26]   TIMA0_C3 LED_BLANK PA12 [34]
 *   ECG_ADC PA14 [36] ADC0 ch12        PPG_HI PA16 [38] OPA1_OUT / ADC1 ch1
 *   PPG_DCC PA17 [39] OPA1_IN1-        VMID_MON PB17 [43] ADC1 ch4
 *   TIA_OUT PA22 [47] OPA0_OUT / ADC1 ch8
 *   PD_BIAS PA26 [59] analog, unused   PD_SUM PA27 [60] OPA0_IN0-
 *   BSL_INVOKE PA18 [40] input, never driven
 *   SWDIO PA19 [41], SWCLK PA20 [42]   DAC12 out PA15 [37] analog, unconnected
 * Unused, output low: PA6 [11], PA7 [14], PA21 [46], PA23 [53], PA24 [54],
 *   PA25 [55], PB24 [52].
 * PINCM indices verified against mspm0g350x.h. ADC1 channel 8 is the A0_7
 * input on PA22 (datasheet ADC channel mapping table). */
typedef enum {
    /* outputs, all with board pull-downs: reset state is low */
    PIN_FAST_RESTORE, /* high = short ECG high-pass node to VMID */
    PIN_MEASURE_REQ,  /* high = request electrode connection (AND CHG_PG) */
    PIN_BUCK_PS_SYNC, /* high = TPS63001 forced PWM (quiet) */
    PIN_OLED_EN,      /* high = OLED_VCC on */
    PIN_VLED_EN,      /* high = 4.99 V LED boost on */
    PIN_ANA_EN,       /* high = +3V3_SW / +3V3A on */
    PIN_GAIN_HI,      /* high = ECG gain 1098x, low = 558x */
    PIN_OLED_DC,
    PIN_OLED_CS,      /* active low */
    PIN_OLED_RESET,   /* active low */
    PIN_RADIO_EN,     /* high = RADIO_VCC on (held low in this build) */
    PIN_RLD_L_EN,
    PIN_RLD_R_EN,
    /* inputs */
    PIN_CHG_PG,       /* low = USB power present */
    PIN_CHG_STAT1,    /* low = charging */
    PIN_BUTTON1,      /* active low */
    PIN_BUTTON2,      /* active low */
    PIN_RADIO_IRQ,    /* active high, internal pull-down */
    PIN_CW_ALRT,      /* active low */
    PIN_COUNT
} board_pin_t;

#define PIN_FIRST_INPUT PIN_CHG_PG

/* ---- I2C ---------------------------------------------------------------- */
#define I2C_ADDR_MCP4728 0x60u
#define I2C_ADDR_CW2015  0x62u

/* ---- ADC ---------------------------------------------------------------- */
#define ADC_FULL_SCALE      4096      /* 12-bit, VDDA reference */
#define ADC_UV_PER_CODE_X10 8057      /* 805.7 uV per code at the pin */
#define VDDA_MV             3300

/* ---- ECG ---------------------------------------------------------------- */
#define ECG_RAW_HZ       4000u /* ADC0 trigger rate, HW average x4 per trigger */
#define ECG_DECIM        8u
#define ECG_HZ           500u  /* stream / processing rate */
#define ECG_GAIN_LO      558
#define ECG_GAIN_HI      1098
#define ECG_VMID_SETTLE_MS       300u
/* VMID is half of +3V3A (about 20 mV below +3V3 after R43) while the ADC
 * reference is +3V3, so a healthy board reads about 10 mV under 1650 mV.
 * The brief's +-10 mV leaves no margin for that; tighten after bring-up. */
#define ECG_VMID_TOL_MV          30
#define ECG_FAST_RESTORE_MS      30u
#define ECG_SETTLE_MS            2500u

/* ---- PPG ---------------------------------------------------------------- */
#define PPG_HZ               100u
#define PPG_FRAME_US         10000u
#define PPG_T_DARK_US        9700u  /* ADC1 dark trigger */
#define PPG_T_LED_ON_US      9800u  /* LED_BLANK low */
#define PPG_T_ON_SAMPLE_US   9950u  /* ADC1 on trigger */
#define PPG_PD_BIAS_DAC8     19u    /* COMP0 DAC8 code, ~0.245 V */
#define PPG_TIA_OHMS         1000000
#define PPG_VLED_START_MS    5u

/* LED current: I_led = V(GREEN_SET) / 8.2 ohm, GREEN_SET = Vdac * 10/164.
 * MCP4728 on internal 2.048 V reference, gain x1: full scale 15.2 mA.
 * uA per DAC code (12-bit) = 2048 mV / 4096 * (10/164) / 8.2 ohm = 3.718 uA */
#define LED_UA_PER_CODE_X1000 3718
/* Hard ceiling per LED: the internal-reference full scale is 15.2 mA, so the
 * brief's 20 mA cap cannot be reached; settings offer 5 / 10 / 15 mA. */
#define LED_MAX_MA            15
#define LED_START_MA          5

/* ---- Power policy ------------------------------------------------------- */
#define UI_DISPLAY_TIMEOUT_S 60u
#define UI_STANDBY_TIMEOUT_S 300u
#define BATT_WARN_PCT        5u
#define BATT_CUTOFF_PCT      3u
#define BATT_CUTOFF_MV       3300u

/* ---- Buttons ------------------------------------------------------------ */
#define BTN_DEBOUNCE_MS 20u
#define BTN_LONG_MS     2000u

/* ---- Display ------------------------------------------------------------ */
#define OLED_W 128
#define OLED_H 64
#define OLED_FB_BYTES (OLED_W * OLED_H / 8)
#define UI_MAX_FPS 25u

#endif
