/* Two active-low buttons with debounce, short/long/both events.
 *
 * buttons_tick(now_ms) must be called every 1..10 ms (non-blocking, a few us).
 *
 * Events:
 *   BTN_EV_BTN1_SHORT / BTN_EV_BTN2_SHORT  on (debounced) release, if the hold
 *                                          was shorter than BTN_LONG_MS
 *   BTN_EV_BTN1_LONG / BTN_EV_BTN2_LONG    once, when the hold reaches
 *                                          BTN_LONG_MS; no short on release
 *   BTN_EV_BOTH_LONG                       both held together for BTN_LONG_MS
 *                                          (measured from the later press);
 *                                          no individual events for either
 *                                          button in that press
 * If the second button goes down while the first is still down and has not
 * fired its long event, the pair becomes a "combo": the individual events of
 * both are suppressed until each is released. If the first already fired its
 * long event, the second press is just ignored.
 * Hold times are measured from the first raw edge (debounce lag compensated).
 *
 * Stuck check: a button that is already down at buttons_init() is ignored
 * (like a wake press) and, if still held continuously BTN_STUCK_MS (5 s) after
 * init, reported by buttons_fault(). It stays ignored until released.
 */
#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_EV_NONE = 0,
    BTN_EV_BTN1_SHORT,
    BTN_EV_BTN2_SHORT,
    BTN_EV_BTN1_LONG,
    BTN_EV_BTN2_LONG,
    BTN_EV_BOTH_LONG
} btn_event_t;

typedef enum { BTN_1 = 0, BTN_2 = 1 } btn_id_t;

#define BTN_STUCK_MS 5000u

/* Reads the pins now; buttons already down are ignored until released. */
void buttons_init(uint32_t now_ms);
void buttons_tick(uint32_t now_ms);

/* Pop the oldest event (BTN_EV_NONE when empty). Queue holds 8; when full
 * new events are dropped. */
btn_event_t buttons_get_event(void);

/* How long the button has been held (ms, as of the last tick), for a progress
 * bar. In a combo both buttons report the time since the later press. 0 if
 * not down, ignored or stuck. Not capped at BTN_LONG_MS. */
uint32_t buttons_hold_ms(btn_id_t b);
/* Any debounced button down (ignored buttons count; stuck ones do not). */
bool buttons_any_down(void);
/* True while a stuck-from-boot button is held. */
bool buttons_fault(void);

/* Call after waking from standby (or any time): every button that is down now
 * produces no events (and no hold time) until it is released. */
void buttons_ignore_until_release(void);

#endif
