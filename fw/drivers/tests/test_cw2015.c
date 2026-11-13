#include "cw2015.h"
#include "i2c_models.h"
#include "test.h"

static cw2015_model_t m;

static void setup(void)
{
    mock_reset();
    cw2015_model_attach(&m);
}

TEST(init_sequence)
{
    setup();
    CHECK_EQ(cw2015_init(), HAL_OK);
    CHECK(cw2015_version_ok());
    CHECK_EQ(cw2015_version(), 0x73);
    CHECK_EQ(m.reg[0x0A], 0x00); /* awake */
    /* ATHD = 5 in bits 7:3, other bits (UFG=bit1) preserved */
    CHECK_EQ(m.reg[0x08], (5 << 3) | 0x02);
    CHECK_EQ(m.bad_writes, 0);
    CHECK_EQ(m.wlog_n, 2);
    CHECK(m.wlog[0].reg == 0x0A && m.wlog[0].val == 0x00);
    CHECK(m.wlog[1].reg == 0x08 && m.wlog[1].val == ((5 << 3) | 0x02));
}

TEST(init_does_not_set_ufg)
{
    setup();
    m.reg[0x08] = 0x00; /* UFG clear: we must not set it */
    CHECK_EQ(cw2015_init(), HAL_OK);
    CHECK_EQ(m.reg[0x08], 5 << 3);
    CHECK_EQ(m.bad_writes, 0);
}

TEST(init_skips_redundant_config_write)
{
    setup();
    m.reg[0x08] = (5 << 3) | 0x01;
    cw2015_init();
    CHECK_EQ(m.wlog_n, 1); /* only MODE */
    CHECK_EQ(m.reg[0x08], (5 << 3) | 0x01);
}

TEST(version_mismatch_continues)
{
    setup();
    m.reg[0x00] = 0x55;
    CHECK_EQ(cw2015_init(), HAL_OK);
    CHECK(!cw2015_version_ok());
    CHECK_EQ(cw2015_version(), 0x55);
    CHECK_EQ(m.reg[0x0A], 0x00);
}

TEST(read_conversion)
{
    setup();
    /* 4.2 V = 13770 codes (305 uV) */
    cw2015_model_set_vcell(&m, 13770, 97);
    uint16_t mv = 0;
    uint8_t soc = 0;
    CHECK_EQ(cw2015_read(&mv, &soc), HAL_OK);
    CHECK_EQ(mv, 4199);
    CHECK_EQ(soc, 97);
    /* upper two bits ignored */
    cw2015_model_set_vcell(&m, 0xC000 | 12000, 50);
    CHECK_EQ(cw2015_read(&mv, &soc), HAL_OK);
    CHECK_EQ(mv, 3660);
    cw2015_model_set_vcell(&m, 0x3FFF, 255);
    CHECK_EQ(cw2015_read(&mv, &soc), HAL_ERR_PARAM);
}

TEST(alert_and_sleep)
{
    setup();
    m.reg[0x06] = 0x80 | 0x15;
    CHECK_EQ(cw2015_clear_alert(), HAL_OK);
    CHECK_EQ(m.reg[0x06], 0x15);
    CHECK_EQ(cw2015_sleep(), HAL_OK);
    CHECK_EQ(m.reg[0x0A], 0xC0);
    CHECK_EQ(m.bad_writes, 0);
}

TEST(errors_and_retry)
{
    setup();
    mock_i2c_inject(1, HAL_ERR_BUS);
    CHECK_EQ(cw2015_init(), HAL_OK);
    CHECK_EQ(mock_i2c_recover_count, 1);
    setup();
    mock_i2c_inject(1, HAL_ERR_NACK);
    CHECK_EQ(cw2015_init(), HAL_ERR_NACK);
    CHECK_EQ(m.wlog_n, 0);
    setup();
    uint16_t mv;
    uint8_t soc;
    mock_i2c_inject(2, HAL_ERR_TIMEOUT);
    CHECK_EQ(cw2015_read(&mv, &soc), HAL_ERR_TIMEOUT);
}

TEST(pct_curve)
{
    CHECK_EQ(batt_pct_from_mv(0), 0);
    CHECK_EQ(batt_pct_from_mv(3300), 0);
    CHECK_EQ(batt_pct_from_mv(4200), 100);
    CHECK_EQ(batt_pct_from_mv(5000), 100);
    uint8_t prev = 0;
    for (unsigned v = 3200; v <= 4300; v++) {
        uint8_t p = batt_pct_from_mv((uint16_t)v);
        CHECK(p >= prev);
        prev = p;
    }
    CHECK_EQ(batt_pct_from_mv(3650), 13);
    CHECK_EQ(batt_pct_from_mv(3800), 40);
}

TEST(combine)
{
    CHECK_EQ(batt_pct_combine(3800, 42), 42);  /* plausible */
    CHECK_EQ(batt_pct_combine(3800, 0), 40);   /* 0 % but 3.8 V */
    CHECK_EQ(batt_pct_combine(3400, 0), 0);    /* 0 % at low voltage: agree */
    CHECK_EQ(batt_pct_combine(3800, 80), 40);  /* disagreement > 30 */
    CHECK_EQ(batt_pct_combine(3800, 70), 70);  /* exactly 30 */
    CHECK_EQ(batt_pct_combine(3800, 255), 40); /* invalid */
    CHECK_EQ(batt_pct_combine(3500, 100), 3);
}

int main(void)
{
    RUN(init_sequence);
    RUN(init_does_not_set_ufg);
    RUN(init_skips_redundant_config_write);
    RUN(version_mismatch_continues);
    RUN(read_conversion);
    RUN(alert_and_sleep);
    RUN(errors_and_retry);
    RUN(pct_curve);
    RUN(combine);
    return TEST_RESULT();
}
