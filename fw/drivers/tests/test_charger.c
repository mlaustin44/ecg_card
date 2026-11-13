#include "charger.h"
#include "mock_hal.h"
#include "test.h"

static uint32_t T;

static void pins(bool usb, bool stat_high)
{
    mock_pin[PIN_CHG_PG] = !usb;
    mock_pin[PIN_CHG_STAT1] = stat_high;
}

/* Run ticks every ms for ms milliseconds. */
static void run(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        charger_tick(++T);
    }
}

/* STAT toggling with a half period (ms) for a total (ms) starting with level. */
static void blink(uint32_t half, uint32_t total, bool *lvl)
{
    for (uint32_t el = 0; el < total; el += half) {
        *lvl = !*lvl;
        pins(true, *lvl);
        run(half);
    }
}

static void setup(void)
{
    mock_reset();
    charger_init();
    T = 5000;
}

TEST(table)
{
    setup();
    pins(false, true);
    charger_tick(T);
    CHECK_EQ(charger_state(), CHARGER_NO_USB);
    CHECK(!charger_usb_present());

    pins(true, false);
    run(100);
    CHECK_EQ(charger_state(), CHARGER_CHARGING);
    CHECK(charger_usb_present());

    pins(true, true);
    run(100);
    CHECK_EQ(charger_state(), CHARGER_CHARGING); /* held back 600 ms */
    run(600);
    CHECK_EQ(charger_state(), CHARGER_COMPLETE);

    pins(false, true);
    run(100);
    CHECK_EQ(charger_state(), CHARGER_NO_USB);

    /* PG high with STAT low (not a valid combination): still NO_USB */
    pins(false, false);
    run(100);
    CHECK_EQ(charger_state(), CHARGER_NO_USB);
}

TEST(boot_with_usb)
{
    setup();
    pins(true, false);
    CHECK(charger_tick(T)); /* adopted immediately */
    CHECK_EQ(charger_state(), CHARGER_CHARGING);
}

TEST(debounce)
{
    setup();
    pins(false, true);
    run(100);
    /* 20 ms PG glitch is ignored */
    pins(true, false);
    run(20);
    pins(false, true);
    run(100);
    CHECK_EQ(charger_state(), CHARGER_NO_USB);
    /* real plug-in takes effect after ~30 ms */
    pins(true, false);
    run(25);
    CHECK_EQ(charger_state(), CHARGER_NO_USB);
    run(10);
    CHECK_EQ(charger_state(), CHARGER_CHARGING);
}

TEST(change_flag)
{
    setup();
    pins(false, true);
    charger_tick(++T);
    pins(true, false);
    int changes = 0;
    for (int i = 0; i < 100; i++) changes += charger_tick(++T);
    CHECK_EQ(changes, 1);
}

TEST(fault_after_2s_of_2hz)
{
    setup();
    pins(true, false);
    run(200);
    CHECK_EQ(charger_state(), CHARGER_CHARGING);
    bool lvl = false;
    uint32_t start = T;
    /* first edge at T */
    uint32_t fault_at = 0;
    while (T - start < 4000) {
        lvl = !lvl;
        pins(true, lvl);
        for (int i = 0; i < 250; i++) {
            charger_tick(++T);
            if (!fault_at && charger_state() == CHARGER_FAULT) fault_at = T;
            /* never flickers to COMPLETE while the blink is being assessed */
            if (charger_state() == CHARGER_COMPLETE) CHECK(false);
        }
    }
    CHECK(fault_at != 0);
    /* first debounced edge ~20 ms after start; fault must come > 2 s later */
    CHECK(fault_at - start > 2000);
    CHECK(fault_at - start < 2400);
    CHECK_EQ(charger_state(), CHARGER_FAULT);
}

TEST(no_fault_if_blink_short)
{
    setup();
    pins(true, false);
    run(200);
    bool lvl = false;
    blink(250, 1500, &lvl); /* 1.5 s of blinking */
    pins(true, false);
    run(1000);
    CHECK_EQ(charger_state(), CHARGER_CHARGING);
    /* and a slow toggle (e.g. complete -> charging transitions) is no fault */
    pins(true, true);
    run(1000);
    pins(true, false);
    run(1000);
    pins(true, true);
    run(3000);
    CHECK_EQ(charger_state(), CHARGER_COMPLETE);
}

TEST(fault_rejects_other_rates)
{
    setup();
    pins(true, false);
    run(200);
    bool lvl = false;
    blink(1000, 8000, &lvl); /* 0.5 Hz */
    CHECK(charger_state() != CHARGER_FAULT);
    blink(30, 4000, &lvl);   /* fast glitching at ~16 Hz */
    CHECK(charger_state() != CHARGER_FAULT);
}

TEST(fault_clears_on_unplug)
{
    setup();
    pins(true, false);
    run(100);
    bool lvl = false;
    blink(250, 3000, &lvl);
    CHECK_EQ(charger_state(), CHARGER_FAULT);
    /* stat goes steady: fault stays latched */
    pins(true, true);
    run(2000);
    CHECK_EQ(charger_state(), CHARGER_FAULT);
    pins(false, true);
    run(100);
    CHECK_EQ(charger_state(), CHARGER_NO_USB);
    /* replug: healthy charge, no stale fault */
    pins(true, false);
    run(100);
    CHECK_EQ(charger_state(), CHARGER_CHARGING);
    run(5000);
    CHECK_EQ(charger_state(), CHARGER_CHARGING);
}

TEST(no_fault_without_usb)
{
    setup();
    pins(false, true);
    run(100);
    bool lvl = true;
    for (int i = 0; i < 20; i++) {
        lvl = !lvl;
        mock_pin[PIN_CHG_STAT1] = lvl;
        run(250);
    }
    CHECK_EQ(charger_state(), CHARGER_NO_USB);
}

int main(void)
{
    RUN(table);
    RUN(boot_with_usb);
    RUN(debounce);
    RUN(change_flag);
    RUN(fault_after_2s_of_2hz);
    RUN(no_fault_if_blink_short);
    RUN(fault_rejects_other_rates);
    RUN(fault_clears_on_unplug);
    RUN(no_fault_without_usb);
    return TEST_RESULT();
}
