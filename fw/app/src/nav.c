#include "nav.h"

ui_screen_t nav_next_page(ui_screen_t s)
{
    if (s >= UI_SCREEN_ABOUT)
        return UI_SCREEN_HOME;
    return (ui_screen_t)(s + 1);
}

ui_screen_t nav_card_of(ui_mode_t m)
{
    return m == UI_MODE_PPG ? UI_SCREEN_PPG_CARD : m == UI_MODE_BOTH ? UI_SCREEN_BOTH_CARD
                                                                     : UI_SCREEN_ECG_CARD;
}

ui_screen_t nav_live_of(ui_mode_t m)
{
    return m == UI_MODE_PPG ? UI_SCREEN_PPG : m == UI_MODE_BOTH ? UI_SCREEN_BOTH : UI_SCREEN_ECG;
}

bool nav_is_live(ui_screen_t s)
{
    return s == UI_SCREEN_ECG || s == UI_SCREEN_PPG || s == UI_SCREEN_BOTH;
}

static nav_result_t r_act(nav_act_t a)
{
    nav_result_t r = {a, UI_SCREEN_HOME, UI_MODE_ECG};
    return r;
}
static nav_result_t r_goto(ui_screen_t s)
{
    nav_result_t r = {NAV_GOTO, s, UI_MODE_ECG};
    return r;
}
static nav_result_t r_start(ui_mode_t m)
{
    nav_result_t r = {NAV_START, nav_live_of(m), m};
    return r;
}

nav_result_t nav_on_button(ui_screen_t screen, uint8_t cursor, ui_mode_t live_mode,
                           btn_event_t ev)
{
    /* global rules first */
    if (ev == BTN_EV_BTN1_LONG)
        return r_act(NAV_POWER_OFF);
    if (ev == BTN_EV_BTN2_LONG)
        return nav_is_live(screen) ? r_act(NAV_HOME_ABORT) : r_goto(UI_SCREEN_HOME);
    if (ev == BTN_EV_BOTH_LONG)
        return screen == UI_SCREEN_ABOUT ? r_act(NAV_DIAG_ENTER) : r_act(NAV_NONE);

    bool b1 = ev == BTN_EV_BTN1_SHORT, b2 = ev == BTN_EV_BTN2_SHORT;
    if (!b1 && !b2)
        return r_act(NAV_NONE);

    switch (screen) {
    case UI_SCREEN_HOME:
    case UI_SCREEN_ABOUT:
        return b2 ? r_goto(nav_next_page(screen)) : r_act(NAV_NONE);
    case UI_SCREEN_ECG_CARD:
        return b1 ? r_start(UI_MODE_ECG) : r_goto(nav_next_page(screen));
    case UI_SCREEN_PPG_CARD:
        return b1 ? r_start(UI_MODE_PPG) : r_goto(nav_next_page(screen));
    case UI_SCREEN_BOTH_CARD:
        return b1 ? r_start(UI_MODE_BOTH) : r_goto(nav_next_page(screen));
    case UI_SCREEN_SETTINGS:
        if (b1)
            return r_act(NAV_SET_CHANGE);
        return cursor + 1u >= SET_ITEM_COUNT ? r_goto(nav_next_page(screen)) : r_act(NAV_SET_NEXT);
    case UI_SCREEN_ECG:
    case UI_SCREEN_PPG:
    case UI_SCREEN_BOTH:
        return b1 ? r_act(NAV_MEAS_END) : r_act(NAV_NONE);
    case UI_SCREEN_SUMMARY:
        return r_goto(nav_card_of(live_mode));
    case UI_SCREEN_DIAG:
        return b1 ? r_act(NAV_DIAG_LEAVE) : r_act(NAV_DIAG_NEXT);
    default:
        return r_goto(UI_SCREEN_HOME);
    }
}
