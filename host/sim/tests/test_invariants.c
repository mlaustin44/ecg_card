#include "common.h"

static void dump(void) { for (size_t i = 0; i < sim_violation_count(); i++) printf("   V: %s\n", sim_violation_text(i)); }
#define EXPECT_ONE(id) CHECK_EQ(sim_violation_count_of(id), 1)

static void i2c_write(uint8_t a, uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t w[3] = {b0, b1, b2};
    hal_i2c_xfer(a, w, 3, NULL, 0);
}

TEST(guarded_pins_need_ana_en)
{
    fresh();
    high(PIN_MEASURE_REQ);
    EXPECT_ONE(SIM_V_GUARDED_PIN_ANA_LOW);
    low(PIN_MEASURE_REQ);
    high(PIN_FAST_RESTORE);
    high(PIN_GAIN_HI);
    high(PIN_RLD_L_EN);
    high(PIN_RLD_R_EN);
    CHECK_EQ(sim_violation_count_of(SIM_V_GUARDED_PIN_ANA_LOW), 5);
    CHECK(sim_violation_count_containing("RLD_R_EN") == 1);
    CHECK(sim_violation_count_containing("[t=0.000 ms]") == 5);
}

TEST(ana_falling_with_guarded_pin_high)
{
    fresh();
    high(PIN_ANA_EN);
    high(PIN_GAIN_HI);
    high(PIN_RLD_L_EN);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
    low(PIN_ANA_EN);
    CHECK_EQ(sim_violation_count_of(SIM_V_GUARDED_PIN_ANA_LOW), 2);
    /* correct order is quiet */
    fresh();
    ecg_rails_up();
    high(PIN_GAIN_HI);
    high(PIN_RLD_L_EN);
    high(PIN_RLD_R_EN);
    high(PIN_MEASURE_REQ);
    high(PIN_FAST_RESTORE);
    hal_delay_ms(5);
    ecg_rails_down();
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
}

TEST(usb_plug_makes_measure_req_safe)
{
    fresh();
    ecg_rails_up();
    high(PIN_MEASURE_REQ);
    sim_set_usb(true);
    CHECK(!hal_pin_read(PIN_MEASURE_REQ));
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
    /* BODY_EN also low with USB present even if firmware raises MEASURE_REQ */
    high(PIN_MEASURE_REQ);
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
}

TEST(rails_follow_pins)
{
    fresh();
    CHECK(!sim_rail(SIM_RAIL_3V3A) && !sim_rail(SIM_RAIL_OLED_VCC) && !sim_rail(SIM_RAIL_VLED));
    high(PIN_ANA_EN);
    CHECK(sim_rail(SIM_RAIL_3V3A) && sim_rail(SIM_RAIL_3V3_SW));
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
    high(PIN_MEASURE_REQ);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    high(PIN_VLED_EN);
    CHECK(sim_rail(SIM_RAIL_VLED));
    low(PIN_VLED_EN);
    low(PIN_MEASURE_REQ);
    low(PIN_ANA_EN);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
}

TEST(ana_fall_during_acquisition_or_led)
{
    fresh();
    ecg_rails_up();
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_ECG);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
    low(PIN_ANA_EN);
    EXPECT_ONE(SIM_V_ANA_FALL_ACQ_RUNNING);
    hal_acq_stop();
    high(PIN_ANA_EN);
    /* LED_BLANK low: run PPG into the LED window */
    sim_violations_clear();
    hal_ppg_analog(true);
    high(PIN_VLED_EN);
    hal_delay_ms(6);
    hal_acq_start(HAL_ACQ_PPG);
    hal_delay_us(9850); /* LED on since 9800 */
    CHECK(!sim_led_blank());
    low(PIN_ANA_EN);
    EXPECT_ONE(SIM_V_ANA_FALL_LED_BLANK_LOW);
    high(PIN_ANA_EN);
    high(PIN_VLED_EN); /* already high */
    low(PIN_VLED_EN);
    EXPECT_ONE(SIM_V_VLED_CHANGE_NOT_BLANKED);
}

TEST(ana_fall_with_dac_a_driving)
{
    fresh();
    high(PIN_ANA_EN); /* factory-default DAC output is not the firmware's doing */
    low(PIN_ANA_EN);
    CHECK_EQ(sim_violation_count(), 0);
    high(PIN_ANA_EN);
    i2c_write(0x60, 0x40, 0x00, 0x00); /* multi-write ch A VDD ref code 0 -> 0 mV: quiet */
    low(PIN_ANA_EN);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
    high(PIN_ANA_EN);
    i2c_write(0x60, 0x40, 0x01, 0x00); /* code 256 */
    low(PIN_ANA_EN);
    EXPECT_ONE(SIM_V_ANA_FALL_DAC_ACTIVE);
    high(PIN_ANA_EN);
    i2c_write(0x60, 0x40, 0x21, 0x00); /* PD=01 */
    low(PIN_ANA_EN);
    CHECK_EQ(sim_violation_count_of(SIM_V_ANA_FALL_DAC_ACTIVE), 1);
}

TEST(oled_rules)
{
    fresh();
    /* correct power-up: RESET low (already), EN high, wait, release, attach */
    high(PIN_OLED_EN);
    hal_delay_ms(5);
    high(PIN_OLED_RESET);
    hal_oled_bus_attach(true);
    high(PIN_OLED_CS);
    low(PIN_OLED_CS);
    uint8_t b = 0xAE;
    hal_oled_write(&b, 1);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
    /* correct power-down */
    high(PIN_OLED_CS);
    hal_oled_bus_attach(false);
    low(PIN_OLED_DC);
    low(PIN_OLED_CS);
    low(PIN_OLED_RESET);
    low(PIN_OLED_EN);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();

    high(PIN_OLED_DC);
    EXPECT_ONE(SIM_V_OLED_SIGNAL_UNPOWERED);
    high(PIN_OLED_CS);
    high(PIN_OLED_RESET);
    CHECK_EQ(sim_violation_count_of(SIM_V_OLED_SIGNAL_UNPOWERED), 3);
    low(PIN_OLED_DC);
    low(PIN_OLED_CS);
    low(PIN_OLED_RESET);
    hal_oled_bus_attach(true);
    CHECK_EQ(sim_violation_count_of(SIM_V_OLED_SIGNAL_UNPOWERED), 4);
    hal_oled_write(&b, 1);
    EXPECT_ONE(SIM_V_OLED_SPI_UNPOWERED);
    hal_oled_bus_attach(false);

    /* EN falls with signals high / bus attached */
    sim_violations_clear();
    high(PIN_OLED_EN);
    high(PIN_OLED_RESET);
    hal_oled_bus_attach(true);
    high(PIN_OLED_CS);
    low(PIN_OLED_EN);
    CHECK_EQ(sim_violation_count_of(SIM_V_OLED_SIGNAL_UNPOWERED), 3); /* bus, CS, RESET */
    /* write with bus detached */
    fresh();
    high(PIN_OLED_EN);
    hal_oled_write(&b, 1);
    EXPECT_ONE(SIM_V_OLED_SPI_DETACHED);
}

TEST(oled_dma_misuse)
{
    fresh();
    high(PIN_OLED_EN);
    hal_delay_ms(5);
    high(PIN_OLED_RESET);
    hal_oled_bus_attach(true);
    static uint8_t buf[100];
    CHECK(hal_oled_write_async(buf, sizeof buf));
    CHECK(hal_oled_busy());
    CHECK(!hal_oled_write_async(buf, sizeof buf)); /* busy: false, not a violation */
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
    buf[3] = 1; /* modified during transfer */
    hal_delay_us(200);
    CHECK(!hal_oled_busy());
    EXPECT_ONE(SIM_V_OLED_DMA_MISUSE);
    /* completes after exactly n bytes at 8 MHz = 100 us */
    sim_violations_clear();
    CHECK(hal_oled_write_async(buf, sizeof buf));
    hal_delay_us(99);
    CHECK(hal_oled_busy());
    hal_delay_us(1);
    CHECK(!hal_oled_busy());
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
}

/* RADIO_EN alone is legal since the radio link exists; the rule is that no
 * pin is high into the unpowered CH585 (UART0 idles PA10 high). */
TEST(radio_backfeed)
{
    fresh();
    high(PIN_RADIO_EN);
    CHECK_EQ(sim_violation_count(), 0);
    low(PIN_RADIO_EN);
    hal_radio_power(true);
    hal_radio_power(false);
    CHECK_EQ(sim_violation_count(), 0);
    hal_radio_power(true);
    low(PIN_RADIO_EN); /* rail cut under a running UART */
    EXPECT_ONE(SIM_V_RADIO_BACKFEED);
    fresh();
    uint8_t b = 0;
    CHECK_EQ(hal_uart_write(&b, 1), 0);
    EXPECT_ONE(SIM_V_UART_RADIO_OFF);
}

TEST(standby_preconditions)
{
    fresh();
    sim_at_button(10, 1, true);
    hal_standby(0);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();

    board_pin_t rails[] = {PIN_ANA_EN, PIN_OLED_EN, PIN_VLED_EN, PIN_BUCK_PS_SYNC};
    for (size_t i = 0; i < 4; i++) {
        fresh();
        high(rails[i]);
        hal_standby(1);
        EXPECT_ONE(SIM_V_STANDBY_PRECOND);
    }
    fresh();
    hal_clock_fast(true);
    hal_standby(1);
    EXPECT_ONE(SIM_V_STANDBY_PRECOND);
    fresh();
    hal_oled_bus_attach(true);
    CHECK_EQ(sim_violation_count_of(SIM_V_OLED_SIGNAL_UNPOWERED), 1);
    hal_standby(1);
    EXPECT_ONE(SIM_V_STANDBY_PRECOND);
    fresh();
    high(PIN_ANA_EN);
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_ECG);
    hal_standby(1);
    CHECK_EQ(sim_violation_count_of(SIM_V_STANDBY_PRECOND), 3);
}

TEST(acquisition_rules)
{
    fresh();
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_ECG); /* ANA_EN low */
    EXPECT_ONE(SIM_V_ACQ_START);
    CHECK_EQ(hal_acq_running(), HAL_ACQ_ECG);
    hal_acq_start(HAL_ACQ_ECG); /* twice */
    CHECK_EQ(sim_violation_count_of(SIM_V_ACQ_START), 2);
    hal_clock_fast(false);
    EXPECT_ONE(SIM_V_CLOCK_WHILE_ACQ);
    (void)hal_adc_read_once(HAL_ADC_VMID);
    EXPECT_ONE(SIM_V_ADC_WHILE_ACQ);
    hal_acq_stop();

    fresh();
    high(PIN_ANA_EN);
    hal_acq_start(HAL_ACQ_ECG); /* slow clock */
    EXPECT_ONE(SIM_V_ACQ_START);
    hal_acq_stop();

    fresh();
    high(PIN_ANA_EN);
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_PPG); /* no analog, no VLED */
    CHECK_EQ(sim_violation_count_of(SIM_V_PPG_START), 2);
    hal_acq_stop();

    /* correct PPG start/stop is quiet; analog off while running is not */
    fresh();
    high(PIN_ANA_EN);
    high(PIN_BUCK_PS_SYNC);
    hal_ppg_analog(true);
    high(PIN_VLED_EN);
    hal_delay_ms(PPG_VLED_START_MS);
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_PPG);
    hal_delay_ms(50);
    hal_ppg_analog(false);
    EXPECT_ONE(SIM_V_PPG_ANALOG_OFF);
    hal_ppg_analog(true);
    sim_violations_clear();
    hal_acq_stop();
    CHECK(sim_led_blank());
    low(PIN_VLED_EN);
    hal_ppg_analog(false);
    hal_clock_fast(false);
    low(PIN_BUCK_PS_SYNC);
    low(PIN_ANA_EN);
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
}

TEST(vled_early_start_is_a_fact)
{
    fresh();
    high(PIN_ANA_EN);
    hal_ppg_analog(true);
    high(PIN_VLED_EN);
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_PPG);
    CHECK_EQ(sim_facts()->ppg_vled_early_starts, 1);
}

TEST(led_overcurrent)
{
    fresh();
    high(PIN_ANA_EN);
    hal_ppg_analog(true);
    high(PIN_VLED_EN);
    hal_delay_ms(6);
    /* internal 2.048 V ref, code 4095 => 15.2 mA: fine. VDD ref => 24.5 mA */
    i2c_write(0x60, 0x58, 0x0F, 0xFF); /* single write A: VDD ref, code 4095 */
    hal_delay_ms(60);
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_PPG);
    hal_delay_ms(15);
    EXPECT_ONE(SIM_V_LED_OVERCURRENT);
}

TEST(i2c_protocol_violations)
{
    fresh();
    i2c_write(0x60, 0x58, 0x0F, 0xFF);   /* EEPROM write */
    i2c_write(0x60, 0x58, 0x00, 0x10);   /* while BSY: ignored */
    EXPECT_ONE(SIM_V_I2C_PROTOCOL);
    CHECK_EQ(sim_facts()->mcp_writes_ignored_busy, 1);
    hal_delay_ms(60);
    i2c_write(0x60, 0x58, 0x00, 0x10);
    CHECK_EQ(sim_violation_count_of(SIM_V_I2C_PROTOCOL), 1);
}

TEST(violation_abort_mode_aborts)
{
    /* Verified by the death test in ctest (see CMakeLists); here: mode switch only. */
    fresh();
    sim_set_violation_mode(SIM_VIOL_COLLECT);
    high(PIN_GAIN_HI);
    CHECK_EQ(sim_violation_count(), 1);
    CHECK(sim_violation_text(0)[0] == '[');
    sim_violations_clear();
    CHECK_EQ(sim_violation_count(), 0); if (sim_violation_count()) dump();
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) { /* death test: default mode aborts with a clear message */
        sim_reset();
        sim_set_violation_mode(SIM_VIOL_ABORT);
        hal_init();
        hal_pin_write(PIN_GAIN_HI, true);
        return 0;
    }
    RUN(guarded_pins_need_ana_en);
    RUN(ana_falling_with_guarded_pin_high);
    RUN(usb_plug_makes_measure_req_safe);
    RUN(rails_follow_pins);
    RUN(ana_fall_during_acquisition_or_led);
    RUN(ana_fall_with_dac_a_driving);
    RUN(oled_rules);
    RUN(oled_dma_misuse);
    RUN(radio_backfeed);
    RUN(standby_preconditions);
    RUN(acquisition_rules);
    RUN(vled_early_start_is_a_fact);
    RUN(led_overcurrent);
    RUN(i2c_protocol_violations);
    RUN(violation_abort_mode_aborts);
    return TEST_RESULT();
}
