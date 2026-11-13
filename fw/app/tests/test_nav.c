/* Navigation table. */
#include "nav.h"
#include "test.h"

static nav_result_t on(ui_screen_t s, btn_event_t e) { return nav_on_button(s, 0, UI_MODE_ECG, e); }

TEST(carousel)
{
    ui_screen_t order[] = {UI_SCREEN_HOME, UI_SCREEN_ECG_CARD, UI_SCREEN_PPG_CARD,
                           UI_SCREEN_BOTH_CARD, UI_SCREEN_SETTINGS, UI_SCREEN_ABOUT};
    for (int i = 0; i < 6; i++) {
        ui_screen_t s = order[i];
        if (s == UI_SCREEN_SETTINGS)
            continue;
        nav_result_t r = on(s, BTN_EV_BTN2_SHORT);
        CHECK_EQ(r.act, NAV_GOTO);
        CHECK_EQ(r.screen, order[(i + 1) % 6]);
    }
    /* Settings: next item until the last, then the next page */
    for (uint8_t c = 0; c + 1 < SET_ITEM_COUNT; c++)
        CHECK_EQ(nav_on_button(UI_SCREEN_SETTINGS, c, UI_MODE_ECG, BTN_EV_BTN2_SHORT).act, NAV_SET_NEXT);
    nav_result_t r = nav_on_button(UI_SCREEN_SETTINGS, SET_ITEM_COUNT - 1, UI_MODE_ECG, BTN_EV_BTN2_SHORT);
    CHECK_EQ(r.act, NAV_GOTO);
    CHECK_EQ(r.screen, UI_SCREEN_ABOUT);
    CHECK_EQ(on(UI_SCREEN_SETTINGS, BTN_EV_BTN1_SHORT).act, NAV_SET_CHANGE);
}

TEST(start_and_end)
{
    nav_result_t r = on(UI_SCREEN_ECG_CARD, BTN_EV_BTN1_SHORT);
    CHECK_EQ(r.act, NAV_START);
    CHECK_EQ(r.mode, UI_MODE_ECG);
    CHECK_EQ(on(UI_SCREEN_PPG_CARD, BTN_EV_BTN1_SHORT).mode, UI_MODE_PPG);
    CHECK_EQ(on(UI_SCREEN_BOTH_CARD, BTN_EV_BTN1_SHORT).mode, UI_MODE_BOTH);
    CHECK_EQ(on(UI_SCREEN_HOME, BTN_EV_BTN1_SHORT).act, NAV_NONE);
    CHECK_EQ(on(UI_SCREEN_ECG, BTN_EV_BTN1_SHORT).act, NAV_MEAS_END);
    CHECK_EQ(on(UI_SCREEN_BOTH, BTN_EV_BTN2_SHORT).act, NAV_NONE);
    CHECK_EQ(on(UI_SCREEN_PPG, BTN_EV_BTN2_LONG).act, NAV_HOME_ABORT);
    /* summary -> that mode's card, either button */
    r = nav_on_button(UI_SCREEN_SUMMARY, 0, UI_MODE_BOTH, BTN_EV_BTN1_SHORT);
    CHECK_EQ(r.act, NAV_GOTO);
    CHECK_EQ(r.screen, UI_SCREEN_BOTH_CARD);
    r = nav_on_button(UI_SCREEN_SUMMARY, 0, UI_MODE_PPG, BTN_EV_BTN2_SHORT);
    CHECK_EQ(r.screen, UI_SCREEN_PPG_CARD);
}

TEST(global)
{
    for (int s = 0; s < UI_SCREEN_COUNT; s++) {
        CHECK_EQ(on((ui_screen_t)s, BTN_EV_BTN1_LONG).act, NAV_POWER_OFF);
        nav_result_t r = on((ui_screen_t)s, BTN_EV_BTN2_LONG);
        CHECK(r.act == NAV_HOME_ABORT || (r.act == NAV_GOTO && r.screen == UI_SCREEN_HOME));
        CHECK_EQ(on((ui_screen_t)s, BTN_EV_BOTH_LONG).act,
                 s == UI_SCREEN_ABOUT ? NAV_DIAG_ENTER : NAV_NONE);
    }
    CHECK_EQ(on(UI_SCREEN_DIAG, BTN_EV_BTN2_SHORT).act, NAV_DIAG_NEXT);
    CHECK_EQ(on(UI_SCREEN_DIAG, BTN_EV_BTN1_SHORT).act, NAV_DIAG_LEAVE);
    CHECK_EQ(on(UI_SCREEN_DIAG, BTN_EV_BTN2_LONG).screen, UI_SCREEN_HOME);
}

int main(void)
{
    RUN(carousel);
    RUN(start_and_end);
    RUN(global);
    return TEST_RESULT();
}
