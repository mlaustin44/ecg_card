/* ECG Card application: state machine, navigation, power policy, settings,
 * stream output, diagnostics.
 *
 * app_main() = app_init() + for (;;) app_step(). Host tests drive
 * app_init() / app_step() against the simulator until sim_should_stop().
 * One app_step() is one main-loop pass and ends in hal_idle() (or, in the
 * off state, one hal_standby()). */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

#include "ui_model.h"

#define APP_FW_VERSION "0.1.0"

void app_init(void);
void app_step(void);
void app_main(void); /* never returns on target */

/* ---- observation (diagnostics page, host tests) ---- */
typedef enum { APP_PWR_AWAKE, APP_PWR_DISPLAY_OFF, APP_PWR_STANDBY } app_pwr_t;

typedef struct {
    app_pwr_t pwr;
    uint32_t loops;
    uint32_t loop_max_ms;      /* longest gap between main-loop passes (awake) */
    uint32_t loop_acq_max_ms;  /* ... while acquisition was active */
    uint32_t frames_drawn;
    uint32_t standby_entries, timed_wakes, wakes;
    uint32_t settings_writes;
    uint32_t i2c_errors;       /* gauge / probe failures */
    uint32_t sessions;
    bool mcp_ok, gauge_ok;
    bool batt_valid;
    uint8_t batt_pct;
    uint16_t batt_mv;
    uint8_t settings_status;   /* settings_load_t at boot */
} app_debug_t;

const app_debug_t *app_debug(void);
/* The model of the last frame rendered (or built). */
const ui_model_t *app_model(void);
/* Settings in RAM (may not be saved yet). */
const settings_t *app_settings(void);

#endif
