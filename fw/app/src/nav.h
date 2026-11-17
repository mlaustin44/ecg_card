/* Navigation table: (screen, button event) -> action. Pure; the app
 * executes the action (it owns sessions, settings and power).
 *
 *   carousel  HOME -> ECG_CARD -> PPG_CARD -> BOTH_CARD -> SETTINGS -> ABOUT -> HOME
 *   BUTTON1 short  card: start that mode; Settings: open it, then change the
 *                  selected value, on "Exit" close it again;
 *                  live screen: end the measurement; Summary: back to the card;
 *                  Diagnostics: leave (About); on the RADIO page the app turns
 *                  NAV_DIAG_LEAVE into "toggle program mode" (app.c)
 *   BUTTON2 short  next page; Settings (open): next item, round through
 *                  "Exit"; ECG / Pulse live screen (also while waiting and
 *                  settling): Live view toggle; Summary: back to the card;
 *                  Diagnostics: next page
 *   BUTTON2 long   Home from anywhere (a measurement ends without summary)
 *   BUTTON1 long   power off (standby) from anywhere
 *   both long      on About: Diagnostics */
#ifndef APP_NAV_H
#define APP_NAV_H

#include <stdint.h>

#include "buttons.h"
#include "ui_model.h"

typedef enum {
    NAV_NONE,
    NAV_GOTO,          /* show .screen */
    NAV_START,         /* start .mode (from its launcher card) */
    NAV_SET_CHANGE,    /* cycle the selected setting */
    NAV_SET_NEXT,      /* next settings item, round through "Exit" */
    NAV_SET_OPEN,      /* Settings page: open (cursor on the first item) */
    NAV_SET_CLOSE,     /* "Exit": close (the page as the carousel shows it) */
    NAV_MEAS_END,      /* end the measurement: summary or card */
    NAV_VIEW_TOGGLE,   /* ECG / Pulse live screen: trace <-> trace + HR graph */
    NAV_HOME_ABORT,    /* to Home; a measurement ends without summary */
    NAV_POWER_OFF,
    NAV_DIAG_ENTER,
    NAV_DIAG_NEXT,
    NAV_DIAG_LEAVE     /* back to About */
} nav_act_t;

typedef struct {
    nav_act_t act;
    ui_screen_t screen; /* NAV_GOTO */
    ui_mode_t mode;     /* NAV_START */
} nav_result_t;

nav_result_t nav_on_button(ui_screen_t screen, uint8_t settings_cursor, ui_mode_t live_mode,
                           btn_event_t ev);

/* Carousel successor (HOME .. ABOUT); other screens -> HOME. */
ui_screen_t nav_next_page(ui_screen_t s);
/* Launcher card of a mode, and live screen of a mode. */
ui_screen_t nav_card_of(ui_mode_t m);
ui_screen_t nav_live_of(ui_mode_t m);
bool nav_is_live(ui_screen_t s);

#endif
