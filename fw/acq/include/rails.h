/* Rail and analog-switch sequencing: THE ONLY CODE that drives ANA_EN,
 * VLED_EN, BUCK_PS_SYNC, MEASURE_REQ, FAST_RESTORE, GAIN_HI, RLD_L_EN and
 * RLD_R_EN. It also owns hal_acq_start/stop, hal_ppg_analog, the PPG offset
 * DAC (DAC12), MCP4728 channel A (LED current) and the MCLK speed, because
 * the rules below tie them together (ecg-fw.md "Rail sequencing", "ECG
 * acquisition / Stop sequence", "PPG acquisition / Stop"):
 *
 *   - no guarded pin (MEASURE_REQ, FAST_RESTORE, GAIN_HI, RLD_x) high, no
 *     LED DAC output and no PPG analog block while ANA_EN is low;
 *   - before ANA_EN falls: guarded pins low, acquisition stopped (TIMA0
 *     stopped, LED_BLANK high), DAC channel A powered down, VLED off;
 *   - VLED_EN only changes while acquisition is stopped (LED_BLANK high);
 *   - MEASURE_REQ is never raised while USB power is present;
 *   - MCLK fast only around acquisition.
 *
 * Every way a measurement or the diagnostics analog page ends (button,
 * lead-off timeout, USB, battery, fault, power-off, standby) calls
 * rails_off(), which runs the one stop sequence. All functions are
 * non-blocking (no waits); timing between steps belongs to the caller
 * (fw/acq/acq.c sequencer). */
#ifndef RAILS_H
#define RAILS_H

#include <stdbool.h>
#include <stdint.h>

/* Boot: run the stop sequence unconditionally so the module state matches
 * the pins (all low after hal_init()). */
void rails_init(void);

/* ANA_EN and BUCK_PS_SYNC high (stop sequence step 4 reversed). Records the
 * time for the VMID settle check. Idempotent. */
void rails_analog_on(void);
bool rails_analog_is_on(void);
uint32_t rails_analog_on_ms(void);

/* ECG front-end switches (start sequence step 4). Requires the analog rail;
 * returns false (and changes nothing) otherwise. rld = rld_mode_t. */
bool rails_ecg_frontend(bool gain_hi, uint8_t rld);
bool rails_fast_restore(bool on);
/* MEASURE_REQ. Raising it is refused (false) without the analog rail or
 * while CHG_PG reads low (USB power): "ECG only runs on battery". */
bool rails_measure_req(bool on);

/* PPG on-chip analog (DAC8 bias, OPA0, DAC12, OPA1) and its offset code. */
bool rails_ppg_analog_on(uint16_t dac12);
void rails_ppg_offset(uint16_t dac12);
uint16_t rails_ppg_offset_code(void);

/* LED boost. Only while no acquisition stream runs (LED_BLANK high). */
bool rails_vled(bool on);
uint32_t rails_vled_on_ms(void);

/* MCP4728 channel A: LED current code, clamped to the board ceiling
 * (LED_MAX_MA). Requires the analog rail (the DAC output feeds the current
 * sinks on +3V3_SW). Returns false on an I2C error. */
bool rails_led_set(uint16_t code);
uint16_t rails_led_code(void); /* last code written, 0 when off */

/* MCLK fast + hal_acq_start(). Requires the analog rail (and for PPG the
 * PPG analog, VLED and channel A); returns false if a precondition fails. */
bool rails_acq_start(unsigned streams);

/* THE stop sequence (idempotent, never waits):
 *   1. MEASURE_REQ low
 *   2. FAST_RESTORE, GAIN_HI, RLD_L_EN, RLD_R_EN low
 *   3. hal_acq_stop() (timers, ADCs; LED_BLANK high)
 *   4. MCP4728 channel A powered down (retried; all channels as fallback);
 *      always sent when the analog rail was on, whatever the last LED write
 *      reported (a failed write may still have reached the DAC)
 *   5. VLED_EN low
 *   6. PPG analog off (OPAs, DACs)
 *   7. BUCK_PS_SYNC low, ANA_EN low
 *   8. MCLK slow */
void rails_off(void);

/* Diagnostics. */
uint32_t rails_i2c_errors(void);    /* LED DAC writes that failed */
bool rails_led_dac_stuck(void);     /* last rails_off() could not power it down */

#endif
