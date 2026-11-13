/* MCP4728 quad 12-bit DAC (I2C 0x60). LDAC tied low (writes apply at once);
 * RDY not wired (BSY is polled through the read-back byte).
 *
 * Only channel A is used (LED current set-point). Channels B-D are always
 * powered down (PD=01, 1 kOhm to GND). Channel A runs on the internal 2.048 V
 * reference, gain x1, while it is ON. EEPROM is written only by
 * mcp4728_init(), and only when the stored defaults differ from the required
 * ones.
 *
 * Reference and standby current (DS22187E 4.10): the internal reference stays
 * on (IPD_INT ~45 uA) as long as ANY channel selects it, even powered down;
 * with every channel on VDD and powered down the part draws ~40 nA. The chip
 * is on the always-on +3V3 rail, so every powered-down state written by this
 * driver (EEPROM defaults, led_off, power_down_all) selects VREF=VDD; only
 * set_led_code() selects the internal reference.
 *
 * Blocking times (400 kHz I2C): every call below is under 1 ms, except
 * mcp4728_init(): about 1 ms normally; when the EEPROM must be programmed,
 * up to ~100 ms (typical 50 ms) while BSY is polled. A failing transfer is
 * retried once after hal_i2c_recover() (adds up to ~10 ms + recovery).
 *
 * Note: this driver does not limit current. The caller caps the code
 * (LED_MAX_MA) before calling mcp4728_set_led_code().
 */
#ifndef MCP4728_H
#define MCP4728_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

#define MCP4728_MAX_CODE 4095u

/* Read back; if the EEPROM is not {VREF=VDD, gain x1, PD=01, code 0} on all
 * four channels, program it once (Sequential Write; a part programmed by an
 * earlier build with channel A on the internal reference is re-programmed
 * once) and wait for BSY (timeout 100 ms -> HAL_ERR_TIMEOUT), then verify
 * (mismatch -> HAL_ERR_BUS). Finally always powers down all four input
 * registers on VDD (a DAC may still be active after a warm MCU reset). */
hal_status_t mcp4728_init(void);
/* True if the last init() had to program the EEPROM. */
bool mcp4728_eeprom_was_written(void);

/* Channel A active (PD=00), internal ref, gain x1, Multi-Write (no EEPROM).
 * code is clamped to 4095. */
hal_status_t mcp4728_set_led_code(uint16_t code);
/* Channel A back to PD=01 (code 0, VREF=VDD: internal reference off). */
hal_status_t mcp4728_led_off(void);
/* Power down all four channels on VDD (Multi-Write). */
hal_status_t mcp4728_power_down_all(void);
/* Pessimistic: true from any set_led_code() attempt (a write that reported an
 * error may still have reached the DAC) and from init() until a power-down
 * (led_off / power_down_all) has been acknowledged. */
bool mcp4728_led_is_on(void);

/* Current in mA x100 <-> DAC code, using LED_UA_PER_CODE_X1000. Rounded to
 * nearest; to-code clamps at 4095. */
uint16_t mcp4728_code_from_ma100(uint32_t ma100);
uint32_t mcp4728_ma100_from_code(uint16_t code);

#endif
