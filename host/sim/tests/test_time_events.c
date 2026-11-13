#include <string.h>

#include "common.h"

static uint64_t fired_at[8];
static int fired_n;
static void rec(void *ctx)
{
    (void)ctx;
    if (fired_n < 8) fired_at[fired_n++] = sim_now_us();
}
static int order[4], order_n;
static void ord(void *ctx) { order[order_n++] = *(int *)ctx; }

TEST(virtual_time)
{
    fresh();
    CHECK_EQ(hal_millis(), 0);
    hal_delay_ms(10);
    CHECK_EQ(hal_millis(), 10);
    hal_delay_us(500);
    CHECK_EQ(sim_now_us(), 10500);
    CHECK_EQ(hal_millis(), 10);
    hal_delay_ms(3500);
    CHECK_EQ(hal_rtc_seconds(), 3);
    hal_idle(); /* to the next ms tick */
    CHECK_EQ(sim_now_us(), 3511000);
}

TEST(events_fire_in_order_inside_delays)
{
    fresh();
    fired_n = 0;
    order_n = 0;
    int a = 1, b = 2, c = 3;
    sim_at(5, rec, NULL);
    sim_at(20, rec, NULL);
    sim_at(7, ord, &a);
    sim_at(7, ord, &b);
    sim_at(7, ord, &c);
    hal_delay_ms(10);
    CHECK_EQ(fired_n, 1);
    CHECK_EQ(fired_at[0], 5000);
    CHECK_EQ(order_n, 3);
    CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3);
    hal_delay_ms(20);
    CHECK_EQ(fired_n, 2);
    CHECK_EQ(fired_at[1], 20000);
    sim_after_ms(5, rec, NULL);
    CHECK_EQ(sim_scheduled_count(), 1);
    sim_at(0, rec, NULL); /* in the past: next advance */
    hal_idle();
    CHECK_EQ(fired_n, 3);
}

TEST(idle_stops_at_tick_or_event)
{
    fresh();
    fired_n = 0;
    hal_idle();
    CHECK_EQ(sim_now_us(), 1000);
    hal_idle();
    CHECK_EQ(sim_now_us(), 2000);
    sim_at_us(2500, rec, NULL);
    hal_idle();
    CHECK_EQ(sim_now_us(), 2500);
    CHECK_EQ(fired_n, 1);
    hal_idle();
    CHECK_EQ(sim_now_us(), 3000);
}

TEST(stop_at_ends_main_loop)
{
    fresh();
    sim_stop_at(100);
    int n = 0;
    while (!sim_should_stop() && n < 10000) {
        hal_idle();
        n++;
    }
    CHECK(sim_should_stop());
    CHECK_EQ(sim_now_us(), 100000);
    CHECK_EQ(n, 100);
}

TEST(button_usb_events)
{
    fresh();
    sim_at_button(10, 1, true);
    sim_at_button(30, 1, false);
    hal_delay_ms(15);
    CHECK(!hal_pin_read(PIN_BUTTON1));
    CHECK_EQ(hal_events_take(), HAL_EV_BUTTON);
    CHECK_EQ(hal_events_take(), 0);
    hal_delay_ms(20);
    CHECK(hal_pin_read(PIN_BUTTON1));
    CHECK_EQ(hal_events_take(), HAL_EV_BUTTON);
    sim_set_button(2, true);
    CHECK(!hal_pin_read(PIN_BUTTON2));
    hal_events_take();

    /* plug: CHG_PG falls, MEASURE_REQ forced low by the "ISR" */
    high(PIN_ANA_EN);
    high(PIN_MEASURE_REQ);
    sim_set_usb(true);
    CHECK(!hal_pin_read(PIN_CHG_PG));
    CHECK(!hal_pin_read(PIN_MEASURE_REQ));
    CHECK(!hal_pin_read(PIN_CHG_STAT1)); /* charging */
    uint32_t e = hal_events_take();
    CHECK(e & HAL_EV_USB_PLUGGED);
    CHECK(e & HAL_EV_CHG_STAT);
    CHECK(!(e & HAL_EV_USB_REMOVED));
    sim_set_usb(false);
    e = hal_events_take();
    CHECK(e & HAL_EV_USB_REMOVED);
    CHECK(hal_pin_read(PIN_CHG_STAT1));
    CHECK_EQ(sim_violation_count(), 0);
    /* writing an input is ignored */
    hal_pin_write(PIN_BUTTON1, false);
    CHECK(hal_pin_read(PIN_BUTTON1));
    hal_pin_write((board_pin_t)99, true);
    CHECK(!hal_pin_read((board_pin_t)99));
}

TEST(charger_fault_toggles_at_2hz)
{
    fresh();
    sim_set_usb(true);
    sim_set_charge_mode(SIM_CHG_FAULT);
    hal_events_take();
    int edges = 0;
    bool last = hal_pin_read(PIN_CHG_STAT1);
    for (int i = 0; i < 2000; i++) {
        hal_delay_ms(1);
        bool now = hal_pin_read(PIN_CHG_STAT1);
        if (now != last) edges++;
        last = now;
    }
    CHECK_EQ(edges, 8); /* 2 s of 2 Hz = 4 periods = 8 edges */
    CHECK(hal_events_take() & HAL_EV_CHG_STAT);
    sim_set_charge_mode(SIM_CHG_DONE);
    CHECK(hal_pin_read(PIN_CHG_STAT1));
}

TEST(rtc_event_and_ppm)
{
    fresh();
    hal_delay_ms(999);
    CHECK(!(hal_events_take() & HAL_EV_RTC));
    hal_delay_ms(2);
    CHECK(hal_events_take() & HAL_EV_RTC);
    sim_set_rtc_error_ppm(100000); /* 10 % fast */
    uint32_t r0 = hal_rtc_seconds();
    hal_delay_ms(10000);
    CHECK_EQ(hal_rtc_seconds() - r0, 11);
}

TEST(standby_wake_causes)
{
    fresh();
    sim_at_button(5000, 2, true);
    uint32_t ms0 = hal_millis();
    uint32_t cause = hal_standby(0);
    CHECK_EQ(cause, HAL_EV_BUTTON);
    CHECK_EQ(sim_now_us(), 5000000);
    CHECK_EQ(hal_millis() - ms0, 0); /* millis stopped */
    CHECK_EQ(hal_rtc_seconds(), 5);  /* rtc kept counting */
    CHECK(hal_events_take() & HAL_EV_BUTTON);

    /* timed wake, earlier than the next scripted event */
    sim_set_button(2, false);
    hal_events_take();
    sim_at_usb(100000, true);
    cause = hal_standby(10);
    CHECK_EQ(cause, HAL_EV_RTC);
    CHECK_EQ(sim_now_us(), 15000000);

    /* USB plug wakes before the timeout */
    sim_set_usb(true);
    hal_events_take();
    sim_at_usb(20000, false);
    sim_at_usb(30000, true);
    cause = hal_standby(100);
    CHECK(cause & HAL_EV_USB_REMOVED);
    CHECK_EQ(sim_now_us(), 20000000);
    hal_events_take();
    cause = hal_standby(100);
    CHECK(cause & HAL_EV_USB_PLUGGED);
    CHECK_EQ(sim_now_us(), 30000000);
    CHECK_EQ(sim_violation_count(), 0);

    /* battery change does not wake; the alert does */
    hal_events_take();
    sim_set_usb(false);
    hal_events_take();
    sim_at_battery_mv(40000, 3500);
    CHECK_EQ(hal_standby(60), HAL_EV_RTC);
    CHECK_EQ(sim_now_us(), 90000000);
    CHECK_EQ(sim_facts()->standby_entries, 5);
}

static int hook_calls;
static sim_exit_reason_t hook_reason;
static void hook(sim_exit_reason_t r, void *c)
{
    (void)c;
    hook_calls++;
    hook_reason = r;
}

TEST(standby_ignores_chg_stat_and_clear_stop)
{
    fresh();
    sim_set_usb(true);
    (void)hal_events_take();
    sim_set_charge_mode(SIM_CHG_FAULT); /* CHG_STAT1 toggles every 250 ms */
    uint64_t t0 = sim_now_ms();
    uint32_t cause = hal_standby(3);
    CHECK_EQ(cause & HAL_EV_RTC, HAL_EV_RTC); /* not woken by the blink */
    CHECK(sim_now_ms() - t0 >= 2900);
    CHECK(hal_events_take() & HAL_EV_CHG_STAT); /* still reported afterwards */
    /* a pending CHG_STAT alone does not end standby at once either */
    sim_set_charge_mode(SIM_CHG_CHARGING);
    (void)hal_events_take();
    sim_set_charge_mode(SIM_CHG_DONE);
    t0 = sim_now_ms();
    CHECK_EQ(hal_standby(2) & HAL_EV_RTC, HAL_EV_RTC);
    CHECK(sim_now_ms() - t0 >= 1900);
    /* sim_clear_stop() re-arms the main loop after a stop time */
    sim_stop_at(sim_now_ms() + 5);
    while (!sim_should_stop())
        hal_idle();
    sim_clear_stop();
    CHECK(!sim_should_stop());
    hal_delay_ms(10);
    CHECK(!sim_should_stop());
}

TEST(standby_deadlock_fails_loudly)
{
    fresh();
    CHECK_EQ(hal_standby(0), 0);
    CHECK_EQ(sim_violation_count_of(SIM_V_STANDBY_DEADLOCK), 1);
    CHECK(sim_should_stop());

    fresh();
    hook_calls = 0;
    sim_set_exit_hook(hook, NULL);
    CHECK_EQ(hal_standby(0), 0);
    CHECK_EQ(hook_calls, 1);
    CHECK_EQ(hook_reason, SIM_EXIT_STANDBY_DEADLOCK);
    CHECK_EQ(sim_violation_count(), 0);
    CHECK(sim_should_stop());

    fresh();
    sim_stop_at(500); /* stop time is a wake source of last resort */
    CHECK_EQ(hal_standby(0), 0);
    CHECK_EQ(sim_now_us(), 500000);
    CHECK(sim_should_stop());
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(system_reset_and_reboot)
{
    fresh();
    CHECK_EQ(hal_reset_cause(), HAL_RESET_POWER_ON);
    high(PIN_ANA_EN);
    high(PIN_GAIN_HI);
    hal_system_reset();
    CHECK(sim_should_stop());
    CHECK(sim_reset_requested());
    sim_reboot();
    CHECK(!sim_should_stop());
    CHECK(!hal_pin_read(PIN_ANA_EN) && !hal_pin_read(PIN_GAIN_HI));
    hal_init();
    CHECK_EQ(hal_reset_cause(), HAL_RESET_SOFTWARE);
    sim_set_reset_cause(HAL_RESET_BROWNOUT);
    sim_reboot();
    hal_init();
    CHECK_EQ(hal_reset_cause(), HAL_RESET_BROWNOUT);
    sim_reboot();
    hal_init();
    CHECK_EQ(hal_reset_cause(), HAL_RESET_PIN);
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(watchdog)
{
    fresh();
    for (int i = 0; i < 50; i++) {
        hal_delay_ms(100);
        hal_wdt_kick();
    }
    CHECK_EQ(sim_violation_count(), 0);
    hal_delay_ms(1900);
    CHECK_EQ(sim_violation_count(), 0);
    hal_delay_ms(200);
    CHECK_EQ(sim_violation_count_of(SIM_V_WATCHDOG), 1);
    CHECK(sim_should_stop());
    sim_reboot();
    hal_init();
    CHECK_EQ(hal_reset_cause(), HAL_RESET_WATCHDOG);
    /* standby does not trip the watchdog */
    sim_violations_clear();
    sim_at_button(60000, 1, true);
    hal_standby(0);
    CHECK_EQ(sim_violation_count_of(SIM_V_WATCHDOG), 0);
}

TEST(cw_alert_pin_event)
{
    fresh();
    hal_events_take();
    sim_set_radio_irq(true);
    CHECK(hal_events_take() & HAL_EV_RADIO_IRQ);
    CHECK_EQ(sim_violation_count(), 0);
}

int main(void)
{
    RUN(virtual_time);
    RUN(events_fire_in_order_inside_delays);
    RUN(idle_stops_at_tick_or_event);
    RUN(stop_at_ends_main_loop);
    RUN(button_usb_events);
    RUN(charger_fault_toggles_at_2hz);
    RUN(rtc_event_and_ppm);
    RUN(standby_wake_causes);
    RUN(standby_ignores_chg_stat_and_clear_stop);
    RUN(standby_deadlock_fails_loudly);
    RUN(system_reset_and_reboot);
    RUN(watchdog);
    RUN(cw_alert_pin_event);
    return TEST_RESULT();
}
