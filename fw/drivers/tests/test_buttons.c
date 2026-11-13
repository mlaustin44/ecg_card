#include "buttons.h"
#include "mock_hal.h"
#include "test.h"

static uint32_t T;

static void setpin(int b, bool down) { mock_pin[b ? PIN_BUTTON2 : PIN_BUTTON1] = !down; }

static void run(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) buttons_tick(++T);
}

static void setup(void)
{
    mock_reset();
    T = 10000;
    buttons_init(T);
}

static btn_event_t one(void)
{
    btn_event_t e = buttons_get_event();
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE); /* exactly one */
    return e;
}

static void click(int b, uint32_t hold)
{
    setpin(b, true);
    run(hold);
    setpin(b, false);
    run(50);
}

TEST(short_press)
{
    setup();
    click(0, 100);
    CHECK_EQ(one(), BTN_EV_BTN1_SHORT);
    click(1, 500);
    CHECK_EQ(one(), BTN_EV_BTN2_SHORT);
    click(0, 1950);
    CHECK_EQ(one(), BTN_EV_BTN1_SHORT);
    CHECK(!buttons_any_down());
}

TEST(glitch_ignored)
{
    setup();
    for (int k = 0; k < 10; k++) { /* 10 ms blips */
        setpin(0, true);
        run(10);
        setpin(0, false);
        run(10);
    }
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    CHECK(!buttons_any_down());
}

TEST(chatter_on_press_and_release)
{
    setup();
    /* bouncing press: 5 ms on/off for 15 ms, then stable */
    for (int k = 0; k < 3; k++) {
        setpin(0, true); run(5);
        setpin(0, false); run(5);
    }
    setpin(0, true);
    run(200);
    CHECK(buttons_any_down());
    /* bouncing release */
    for (int k = 0; k < 3; k++) {
        setpin(0, false); run(5);
        setpin(0, true); run(5);
    }
    setpin(0, false);
    run(100);
    CHECK_EQ(one(), BTN_EV_BTN1_SHORT); /* exactly one event */
    CHECK(!buttons_any_down());
}

TEST(long_press_once)
{
    setup();
    setpin(0, true);
    run(1900);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    CHECK_NEAR(buttons_hold_ms(BTN_1), 1900, 25);
    run(150);
    CHECK_EQ(buttons_get_event(), BTN_EV_BTN1_LONG);
    run(3000);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    setpin(0, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE); /* no short afterwards */
    CHECK_EQ(buttons_hold_ms(BTN_1), 0);

    setpin(1, true);
    run(2100);
    setpin(1, false);
    run(100);
    CHECK_EQ(one(), BTN_EV_BTN2_LONG);
}

TEST(hold_progress)
{
    setup();
    CHECK_EQ(buttons_hold_ms(BTN_1), 0);
    setpin(1, true);
    run(1000);
    CHECK_NEAR(buttons_hold_ms(BTN_2), 1000, 2);
    CHECK_EQ(buttons_hold_ms(BTN_1), 0);
    CHECK(buttons_any_down());
}

TEST(both_long)
{
    setup();
    setpin(0, true);
    run(100);
    setpin(1, true);
    run(1900);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    /* BTN1 has now been down 2 s, but the pair is what counts */
    CHECK_NEAR(buttons_hold_ms(BTN_1), 1900, 2);
    CHECK_NEAR(buttons_hold_ms(BTN_2), 1900, 2);
    run(150);
    CHECK_EQ(buttons_get_event(), BTN_EV_BOTH_LONG);
    run(3000);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    setpin(0, false);
    run(100);
    setpin(1, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    /* afterwards buttons work normally */
    click(1, 100);
    CHECK_EQ(one(), BTN_EV_BTN2_SHORT);
}

TEST(both_released_early_gives_nothing)
{
    setup();
    setpin(0, true);
    run(100);
    setpin(1, true);
    run(500);
    setpin(1, false);
    run(100);
    setpin(0, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    /* remaining button held on long does not fire either */
    setpin(0, true); run(100);
    setpin(1, true); run(300);
    setpin(1, false); run(100);
    run(3000);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    setpin(0, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
}

TEST(second_press_after_long)
{
    setup();
    setpin(0, true);
    run(2100);
    CHECK_EQ(buttons_get_event(), BTN_EV_BTN1_LONG);
    setpin(1, true);
    run(3000);
    setpin(1, false);
    run(100);
    setpin(0, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
}

TEST(stuck_from_boot)
{
    mock_reset();
    T = 10000;
    setpin(1, true);
    buttons_init(T);
    run(2500); /* would have been a long press */
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    CHECK(!buttons_fault());
    CHECK_EQ(buttons_hold_ms(BTN_2), 0);
    run(2400);
    CHECK(!buttons_fault());
    run(200);
    CHECK(buttons_fault());
    CHECK(!buttons_any_down());
    /* other button still works */
    click(0, 100);
    CHECK_EQ(one(), BTN_EV_BTN1_SHORT);
    CHECK(buttons_fault());
    /* release clears the fault, no event */
    setpin(1, false);
    run(100);
    CHECK(!buttons_fault());
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    click(1, 100);
    CHECK_EQ(one(), BTN_EV_BTN2_SHORT);
}

/* Review finding 10c: a button stuck after boot kept "down" forever, which
 * refreshed the app's inactivity timer forever. */
TEST(stuck_after_boot)
{
    setup();
    run(100);
    setpin(1, true);
    run(2500);
    CHECK_EQ(one(), BTN_EV_BTN2_LONG);
    CHECK(buttons_any_down());
    CHECK(!buttons_fault());
    run(BTN_HELD_MAX_MS - 2500 - 100);
    CHECK(buttons_any_down()); /* a long deliberate hold is still a hold */
    run(200);
    CHECK(!buttons_any_down());
    CHECK(buttons_fault());
    CHECK_EQ(buttons_hold_ms(BTN_2), 0);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    /* other button unaffected */
    click(0, 100);
    CHECK_EQ(one(), BTN_EV_BTN1_SHORT);
    /* release clears it, no event; then it works normally */
    setpin(1, false);
    run(100);
    CHECK(!buttons_fault());
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    click(1, 100);
    CHECK_EQ(one(), BTN_EV_BTN2_SHORT);
    /* the same for a wake press that never ends */
    setpin(0, true);
    buttons_ignore_until_release();
    run(BTN_HELD_MAX_MS + 100);
    CHECK(!buttons_any_down());
    CHECK(buttons_fault());
}

TEST(boot_press_released_before_5s)
{
    mock_reset();
    T = 10000;
    setpin(0, true);
    buttons_init(T);
    run(1000);
    setpin(0, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    setpin(0, true);
    run(6000); /* a later real hold is a long press, not a fault */
    CHECK(!buttons_fault());
    CHECK_EQ(one(), BTN_EV_BTN1_LONG);
}

TEST(wake_press_suppressed)
{
    setup();
    run(100);
    /* woken by a press: pin is low when we come back, before tick sees it */
    setpin(0, true);
    buttons_ignore_until_release();
    run(300);
    CHECK_EQ(buttons_hold_ms(BTN_1), 0);
    setpin(0, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    /* long wake press: no long event either */
    setpin(0, true);
    buttons_ignore_until_release();
    run(3000);
    setpin(0, false);
    run(100);
    CHECK_EQ(buttons_get_event(), BTN_EV_NONE);
    CHECK(!buttons_fault());
    /* next press is normal */
    click(0, 100);
    CHECK_EQ(one(), BTN_EV_BTN1_SHORT);
    /* ignore with nothing pressed does nothing */
    buttons_ignore_until_release();
    click(1, 100);
    CHECK_EQ(one(), BTN_EV_BTN2_SHORT);
}

TEST(queue_overflow_drops_newest)
{
    setup();
    for (int i = 0; i < 12; i++) click(0, 50);
    int n = 0;
    while (buttons_get_event() != BTN_EV_NONE) n++;
    CHECK_EQ(n, 8);
}

int main(void)
{
    RUN(short_press);
    RUN(glitch_ignored);
    RUN(chatter_on_press_and_release);
    RUN(long_press_once);
    RUN(hold_progress);
    RUN(both_long);
    RUN(both_released_early_gives_nothing);
    RUN(second_press_after_long);
    RUN(stuck_from_boot);
    RUN(stuck_after_boot);
    RUN(boot_press_released_before_5s);
    RUN(wake_press_suppressed);
    RUN(queue_overflow_drops_newest);
    return TEST_RESULT();
}
