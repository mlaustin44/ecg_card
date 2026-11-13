#include <string.h>

#include "common.h"

static hal_status_t wr(uint8_t a, const uint8_t *w, size_t n) { return hal_i2c_xfer(a, w, n, NULL, 0); }
static uint8_t cw_rd(uint8_t reg)
{
    uint8_t v = 0xEE;
    hal_status_t st = hal_i2c_xfer(0x62, &reg, 1, &v, 1);
    return st == HAL_OK ? v : 0xEE;
}
static void cw_wr(uint8_t reg, uint8_t v)
{
    uint8_t w[2] = {reg, v};
    wr(0x62, w, 2);
}

TEST(transport_basics)
{
    fresh();
    uint8_t b = 0;
    CHECK_EQ(hal_i2c_xfer(0x50, &b, 1, NULL, 0), HAL_ERR_NACK);
    CHECK_EQ(hal_i2c_xfer(0x60, NULL, 0, NULL, 0), HAL_OK); /* probe */
    CHECK_EQ(hal_i2c_xfer(0x00, &b, 1, NULL, 0), HAL_OK);   /* general call ignored */
    uint64_t t0 = sim_now_us();
    uint8_t reg = 0, v;
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, &v, 1), HAL_OK);
    /* start + 2 addr/data bytes + restart + 2 bytes at 400 kHz: tens of us */
    CHECK(sim_now_us() - t0 > 40 && sim_now_us() - t0 < 100);
    CHECK_EQ(v, 0x73);
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, NULL, 0), HAL_OK);
    CHECK_EQ(hal_i2c_xfer(0x62, NULL, 0, NULL, 4), HAL_ERR_PARAM);
}

TEST(fault_injection_and_stuck_bus)
{
    fresh();
    uint8_t reg = 0, v = 0;
    sim_i2c_fail_next(2, HAL_ERR_TIMEOUT);
    uint64_t t0 = sim_now_us();
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, &v, 1), HAL_ERR_TIMEOUT);
    CHECK(sim_now_us() - t0 >= 10000);
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, &v, 1), HAL_ERR_TIMEOUT);
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, &v, 1), HAL_OK);
    CHECK_EQ(v, 0x73);
    sim_i2c_fail_next(1, HAL_ERR_NACK);
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, &v, 1), HAL_ERR_NACK);

    sim_i2c_stick_bus(true);
    for (int i = 0; i < 3; i++) CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, &v, 1), HAL_ERR_BUS);
    CHECK(sim_i2c_bus_stuck());
    hal_i2c_recover();
    CHECK(!sim_i2c_bus_stuck());
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, &v, 1), HAL_OK);
    CHECK_EQ(sim_facts()->i2c_recoveries, 1);
    CHECK(sim_i2c_log_count() >= 8);
    CHECK_EQ(sim_i2c_log_get(0)->addr, 0x62);
    CHECK_EQ(sim_i2c_log_get(0)->status, HAL_ERR_TIMEOUT);
}

TEST(mcp4728_writes_and_led_math)
{
    fresh();
    /* power-on: factory EEPROM, VDD ref, code 0x800 */
    CHECK_EQ(sim_mcp_code(0), 0x800);
    /* Fast write A,B: PD=0, A=0x123 B=0xABC */
    uint8_t fw[4] = {0x01, 0x23, 0x0A, 0xBC};
    CHECK_EQ(wr(0x60, fw, 4), HAL_OK);
    CHECK_EQ(sim_mcp_code(0), 0x123);
    CHECK_EQ(sim_mcp_code(1), 0xABC);
    CHECK_EQ(sim_mcp_code(2), 0x800);
    CHECK_EQ(sim_facts()->mcp_eeprom_writes, 0);
    /* Multi-write channel A: internal ref, gain x1, code 4095 => 2.048 V => 15.2 mA */
    uint8_t mw[3] = {0x40, 0x80 | 0x0F, 0xFF};
    CHECK_EQ(wr(0x60, mw, 3), HAL_OK);
    CHECK(sim_mcp_vref_internal(0));
    CHECK_NEAR(sim_mcp_vout_mv(0), 2047.5, 0.1);
    CHECK_NEAR(sim_led_set_ma(), 15.2, 0.05);
    /* gain x2 clips at VDD */
    mw[1] = 0x80 | 0x10 | 0x0F;
    wr(0x60, mw, 3);
    CHECK_NEAR(sim_mcp_vout_mv(0), 3300.0, 0.1);
    /* LED uA per code: 3.718 */
    mw[1] = 0x80;
    mw[2] = 0x64; /* code 100 */
    wr(0x60, mw, 3);
    CHECK_NEAR(sim_led_set_ma() * 1000.0, 100 * 3.718, 1.0);
    CHECK_NEAR(sim_led_set_ma(), 100.0 * LED_UA_PER_CODE_X1000 / 1e6, 0.005);
    /* PD bits in the data byte */
    mw[1] = 0x80 | 0x20;
    wr(0x60, mw, 3);
    CHECK_EQ(sim_mcp_pd(0), 1);
    CHECK_NEAR(sim_mcp_vout_mv(0), 0.0, 0.01);
    CHECK_NEAR(sim_led_set_ma(), 0.0, 0.001);
    /* VDD ref, code 4095 -> 24.5 mA */
    uint8_t vr = 0x80; /* all channels VDD */
    wr(0x60, &vr, 1);
    CHECK(!sim_mcp_vref_internal(0));
    uint8_t fa[2] = {0x0F, 0xFF};
    wr(0x60, fa, 2);
    CHECK_NEAR(sim_led_set_ma(), 24.5, 0.1);
    /* write VREF command: 0b100 + A,B,C,D bits */
    vr = 0x88;
    wr(0x60, &vr, 1);
    CHECK(sim_mcp_vref_internal(0) && !sim_mcp_vref_internal(1));
    uint8_t gn = 0xC8;
    wr(0x60, &gn, 1);
    CHECK(sim_mcp_gain2(0) && !sim_mcp_gain2(1));
    /* PD command: A=01 B=10 C=11 D=00 */
    uint8_t pd[2] = {0xA0 | (1 << 2) | 2, (3 << 6)};
    wr(0x60, pd, 2);
    CHECK_EQ(sim_mcp_pd(0), 1);
    CHECK_EQ(sim_mcp_pd(1), 2);
    CHECK_EQ(sim_mcp_pd(2), 3);
    CHECK_EQ(sim_mcp_pd(3), 0);
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(mcp4728_eeprom_busy_readback_poweron)
{
    fresh();
    /* Single write ch A with EEPROM: PD=01 (powered down), internal ref, code 0 */
    uint8_t sw[3] = {0x58, 0x80 | 0x20, 0x00};
    CHECK_EQ(wr(0x60, sw, 3), HAL_OK);
    CHECK_EQ(sim_facts()->mcp_eeprom_writes, 1);
    CHECK(sim_mcp_busy());
    uint8_t rb[24];
    CHECK_EQ(hal_i2c_xfer(0x60, NULL, 0, rb, 24), HAL_OK);
    CHECK_EQ(rb[0] & 0x80, 0); /* BSY */
    hal_delay_ms(49);
    CHECK(sim_mcp_busy());
    hal_delay_ms(2);
    CHECK(!sim_mcp_busy());
    CHECK_EQ(hal_i2c_xfer(0x60, NULL, 0, rb, 24), HAL_OK);
    CHECK_EQ(rb[0] & 0x80, 0x80); /* RDY */
    /* byte-exact channel A: input reg then EEPROM image */
    CHECK_EQ(rb[1], 0xA0);
    CHECK_EQ(rb[2], 0x00);
    CHECK_EQ(rb[4], 0xA0);
    CHECK_EQ(rb[5], 0x00);
    /* channel B untouched: VDD ref, code 0x800 */
    CHECK_EQ(rb[6 + 1], 0x08);
    CHECK_EQ(rb[6 + 2], 0x00);
    CHECK_EQ((rb[6] >> 4) & 3, 1); /* channel index in header */
    /* sequential write from B: B,C,D with EEPROM => 3 writes */
    uint8_t sq[7] = {0x50 | (1 << 1), 0x00 | 0x01, 0x11, 0x02, 0x22, 0x03, 0x33};
    CHECK_EQ(wr(0x60, sq, 7), HAL_OK);
    CHECK_EQ(sim_mcp_code(1), 0x111);
    CHECK_EQ(sim_mcp_code(3), 0x333);
    CHECK_EQ(sim_facts()->mcp_eeprom_writes, 4);
    hal_delay_ms(60);
    /* RAM write, then power cycle returns EEPROM contents */
    uint8_t fa[2] = {0x0A, 0xAA};
    wr(0x60, fa, 2);
    CHECK_EQ(sim_mcp_code(0), 0xAAA);
    sim_mcp_power_cycle();
    CHECK_EQ(sim_mcp_code(0), 0);
    CHECK_EQ(sim_mcp_pd(0), 1);
    CHECK_EQ(sim_mcp_code(2), 0x222);
    unsigned code, pd;
    bool v, g;
    sim_mcp_eeprom_get(3, &code, &v, &pd, &g);
    CHECK_EQ(code, 0x333);
    /* survives an MCU reboot, as the chip is on the always-on rail */
    wr(0x60, fa, 2);
    sim_reboot();
    CHECK_EQ(sim_mcp_code(0), 0xAAA);
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(cw2015_registers)
{
    fresh();
    CHECK_EQ(cw_rd(0x00), 0x73);
    CHECK_EQ(cw_rd(0x0A), 0xC0); /* asleep */
    CHECK(sim_cw_asleep());
    CHECK(!sim_cw_ufg());
    cw_wr(0x0A, 0x00);
    CHECK(!sim_cw_asleep());
    sim_set_battery_mv(3800);
    uint8_t reg = 0x02, v[4];
    CHECK_EQ(hal_i2c_xfer(0x62, &reg, 1, v, 4), HAL_OK);
    unsigned vcell = (unsigned)(v[0] << 8 | v[1]);
    CHECK_EQ(vcell, 3800000 / 305); /* 12459, 305 uV/LSB */
    CHECK_EQ(v[2], 50);              /* whole percent from the table */
    CHECK_EQ(v[3], 0);
    sim_set_battery_mv(4200);
    reg = 0x02;
    hal_i2c_xfer(0x62, &reg, 1, v, 4);
    CHECK_EQ(v[2], 100);
    sim_set_battery_soc(37);
    CHECK_EQ(cw_rd(0x04), 37);
    sim_set_battery_soc(-1);
    CHECK_EQ(cw_rd(0x04), 100);
    /* version is read-only */
    cw_wr(0x00, 0x00);
    CHECK_EQ(cw_rd(0x00), 0x73);
    CHECK_EQ(sim_violation_count_of(SIM_V_I2C_PROTOCOL), 1);
}

TEST(cw2015_profile_config_alert)
{
    fresh();
    uint8_t prof[65];
    prof[0] = 0x10;
    for (int i = 0; i < 64; i++) prof[1 + i] = (uint8_t)(0xA0 + i);
    CHECK_EQ(wr(0x62, prof, 65), HAL_OK);
    CHECK_EQ(sim_cw_profile_writes(), 1);
    CHECK_EQ(sim_facts()->cw_profile_bytes, 64);
    uint8_t out[64];
    sim_cw_profile(out);
    CHECK(memcmp(out, prof + 1, 64) == 0);
    CHECK_EQ(cw_rd(0x4F), 0xA0 + 63);
    /* CONFIG: UFG + ATHD = 5 */
    cw_wr(0x08, (uint8_t)((5 << 3) | 0x02));
    CHECK(sim_cw_ufg());
    CHECK_EQ(sim_cw_athd(), 5);
    CHECK_EQ(cw_rd(0x08), (5 << 3) | 2);
    CHECK_EQ(sim_facts()->cw_profile_writes, 1); /* config write is not a profile write */

    cw_wr(0x0A, 0x00);
    sim_set_battery_mv(3800);
    CHECK(hal_pin_read(PIN_CW_ALRT));
    hal_events_take();
    sim_set_battery_mv(3480); /* below 5 % */
    CHECK(!hal_pin_read(PIN_CW_ALRT));
    CHECK(sim_cw_alert_flag());
    CHECK(hal_events_take() & HAL_EV_CW_ALRT);
    CHECK_EQ(cw_rd(0x06) & 0x80, 0x80);
    /* clear by writing 0 to bit 15 */
    cw_wr(0x06, 0x00);
    CHECK(hal_pin_read(PIN_CW_ALRT));
    CHECK(!sim_cw_alert_flag());
    sim_set_battery_mv(3470); /* still below: no re-trigger */
    CHECK(hal_pin_read(PIN_CW_ALRT));
    sim_set_battery_mv(3800);
    sim_set_battery_mv(3400);
    CHECK(!hal_pin_read(PIN_CW_ALRT));
    /* gauge state survives MCU reboot */
    sim_reboot();
    CHECK(sim_cw_ufg());
    CHECK_EQ(sim_violation_count(), 0);
}

int main(void)
{
    RUN(transport_basics);
    RUN(fault_injection_and_stuck_bus);
    RUN(mcp4728_writes_and_led_math);
    RUN(mcp4728_eeprom_busy_readback_poweron);
    RUN(cw2015_registers);
    RUN(cw2015_profile_config_alert);
    return TEST_RESULT();
}
