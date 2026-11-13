/* PPG control laws (pure functions plus tiny state): DC cancellation, LED AGC
 * and finger detection. All voltages are 12-bit ADC / DAC12 codes on the VDDA
 * (3.3 V) reference: 1.65 V = 2048 codes. The application applies the
 * results between frames and flags the next frame (DSP_PPG_IN_*_STEP). */
#ifndef DSP_PPG_CTRL_H
#define DSP_PPG_CTRL_H

#include <stdbool.h>
#include <stdint.h>

/* ---- DC cancellation (about once per second) ------------------------------
 * V_DAC12 = (1.65 V + 10 TIA_OUT(on)) / 11 puts PPG_HI(on) at mid-scale:
 * code = round((2048 + 10 tia_on) / 11), clamped to 0..4095. */
uint16_t dsp_ppg_dcc_code(uint16_t tia_on);

/* Closed-loop wrapper with a dead band: if hi_on (mean) is within
 * +-DSP_DCC_DEADBAND of 2048 nothing changes; otherwise *dac12 is set from
 * dsp_ppg_dcc_code(tia_on). Returns true if *dac12 changed. */
#define DSP_DCC_DEADBAND 1024u
bool dsp_ppg_dcc_update(uint16_t tia_on, uint16_t hi_on, uint16_t *dac12);

/* DAC12 code for the light level expected after an LED change, so the
 * offset can be re-cancelled together with the AGC step instead of PPG_HI
 * railing until the next DCC: TIA_OUT(on) scales as
 * tia_dark + (tia_on - tia_dark) * led_new / led_old. */
uint16_t dsp_ppg_dcc_predict(uint16_t tia_dark, uint16_t tia_on, uint16_t led_old, uint16_t led_new);

/* ---- LED AGC ---------------------------------------------------------------
 * Trigger:  TIA_OUT(on) > 2.8 V (3475)  -> step down 20 %
 *           TIA_OUT(on) < 0.6 V (745)   -> step up 25 %
 * Once triggered, keep stepping the same way (one step per call) until the
 * target window 1.5..2.0 V (1862..2482) is reached, then stop: the trigger and
 * target thresholds are the hysteresis. A 25 % step spans less than the
 * window (ratio 1.40 on the photocurrent), so the loop cannot hunt.
 * Call at most once per settled measurement (e.g. every 100-250 ms with the
 * mean tia_on of undisturbed frames). */
#define DSP_AGC_HIGH    3475u
#define DSP_AGC_LOW     745u
#define DSP_AGC_TGT_LO  1862u
#define DSP_AGC_TGT_HI  2482u
#define DSP_AGC_MIN_CODE 8u

typedef struct {
    int8_t dir; /* 0 idle, +1 stepping up, -1 stepping down */
} dsp_agc_t;

/* Largest MCP4728 code for a per-LED current limit, from
 * LED_UA_PER_CODE_X1000 (board.h), clamped to 4095. */
uint16_t dsp_led_max_code(uint8_t limit_ma);

void dsp_agc_init(dsp_agc_t *a);

/* One AGC decision. Updates *led_code (clamped to DSP_AGC_MIN_CODE..max_code)
 * and returns true if it changed. */
bool dsp_agc_step(dsp_agc_t *a, uint16_t tia_on, uint16_t *led_code, uint16_t max_code);

/* ---- Finger detection -------------------------------------------------------
 * Present when coarse (tia_on - tia_dark, codes) > DSP_FINGER_ON for 300 ms;
 * absent when coarse < DSP_FINGER_OFF, or TIA_OUT(on) is on the top rail
 * (ambient overload), for 300 ms. Feed every frame. */
#define DSP_FINGER_ON   250
#define DSP_FINGER_OFF  150
#define DSP_FINGER_HOLD 30u   /* frames */
#define DSP_FINGER_RAIL 4040u

typedef struct {
    uint8_t present;
    uint8_t count;
} dsp_finger_t;

void dsp_finger_init(dsp_finger_t *f);
/* Returns the debounced state. */
bool dsp_finger_update(dsp_finger_t *f, int16_t coarse, uint16_t tia_on);

#endif
