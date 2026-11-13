/* BQ24073 charger status decoder (reporting only).
 *
 *   CHG_PG  CHG_STAT1   state
 *   high    high        NO_USB
 *   low     low         CHARGING
 *   low     high        COMPLETE
 *   low     ~2 Hz       FAULT (safety timer), after toggling for > 2 s
 *
 * charger_tick() samples the pins; call it at least every 20 ms (e.g. every
 * main-loop pass; typical 1-10 ms). Non-blocking, well under 10 us.
 * Debounce: PG 30 ms, STAT 20 ms. A STAT change is only reported 600 ms
 * after the last STAT edge (it may be the start of a fault blink), so
 * CHARGING <-> COMPLETE lags by up to 0.6 s; during a blink the state is held. A fault latches until USB is removed
 * (PG high, debounced).
 */
#ifndef CHARGER_H
#define CHARGER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CHARGER_NO_USB = 0,
    CHARGER_CHARGING,
    CHARGER_COMPLETE,
    CHARGER_FAULT
} charger_state_t;

#define CHARGER_PG_DEBOUNCE_MS   30u
#define CHARGER_STAT_DEBOUNCE_MS 20u
#define CHARGER_FAULT_MS         2000u

/* Forget everything; the next tick adopts the pins without debounce. */
void charger_init(void);
/* Sample pins. Returns true if the reported state changed. */
bool charger_tick(uint32_t now_ms);
charger_state_t charger_state(void);
bool charger_usb_present(void); /* debounced PG low */
const char *charger_state_name(charger_state_t s);

#endif
