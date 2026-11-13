/* CW2015 fuel gauge (I2C 0x62) plus battery-percent helpers.
 *
 * NO BATTERY PROFILE IS LOADED in this build: the driver never writes
 * registers 0x10-0x4F and never sets CONFIG.UFG. Gauge accuracy is therefore
 * limited; cw2015_combined_pct() falls back to a voltage estimate when the
 * gauge looks implausible.
 *
 * Blocking times (400 kHz I2C): init() ~1.5 ms (4 short transfers), read()
 * ~0.4 ms, clear_alert() / sleep() ~0.3 ms. A BUS/TIMEOUT error is retried
 * once after hal_i2c_recover().
 */
#ifndef CW2015_H
#define CW2015_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

#define CW2015_VERSION_EXPECTED 0x73u

/* Reads VERSION (a mismatch is recorded, not fatal), wakes the gauge (MODE =
 * 0x00) and sets ATHD (CONFIG bits 7:3) to BATT_WARN_PCT, preserving the other
 * CONFIG bits. Returns the first I2C error. */
hal_status_t cw2015_init(void);
uint8_t cw2015_version(void); /* as read by the last init() */
bool cw2015_version_ok(void); /* version == 0x73 */

/* VCELL -> millivolts (14-bit, 305 uV/LSB), SOC -> whole percent (0..100).
 * Returns HAL_ERR_PARAM if the reading is not (yet) a measurement: VCELL = 0
 * or SOC > 100, as the gauge reports for a while after it is woken (mv and
 * soc are still set). The caller retries; see app.c battery_read(). */
hal_status_t cw2015_read(uint16_t *mv, uint8_t *soc_pct);
/* Clear RRT_ALERT (bit 15 of RRT_ALRT, reg 0x06). */
hal_status_t cw2015_clear_alert(void);
/* Put the gauge to sleep (MODE = 0xC0). */
hal_status_t cw2015_sleep(void);

/* Pure helpers. Open-circuit single-cell LiPo curve, piecewise linear:
 * <= 3300 mV -> 0 %, >= 4200 mV -> 100 %. */
uint8_t batt_pct_from_mv(uint16_t mv);
/* Plausibility rule: use the gauge SOC unless
 *   - gauge_soc > 100, or
 *   - gauge_soc == 0 while mv > 3700 (gauge not yet converged), or
 *   - |gauge_soc - voltage estimate| > 30 points;
 * in those cases use the voltage estimate. */
uint8_t batt_pct_combine(uint16_t mv, uint8_t gauge_soc);

#endif
