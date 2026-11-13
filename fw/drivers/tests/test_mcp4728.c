#include <string.h>

#include "i2c_models.h"
#include "mcp4728.h"
#include "test.h"

static mcp4728_model_t m;

static void setup(bool good_eeprom)
{
    mock_reset();
    mcp4728_model_attach(&m);
    if (good_eeprom) mcp4728_model_set_good_eeprom(&m);
}

static bool all_in_pd(void)
{
    for (int i = 0; i < 4; i++) {
        if (m.in[i].pd != 1 || m.in[i].val != 0 || m.in[i].vref != 0) return false;
    }
    return true;
}

TEST(init_blank_writes_eeprom_once)
{
    setup(false);
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK_EQ(m.eeprom_writes, 1);
    CHECK(mcp4728_eeprom_was_written());
    CHECK_EQ(m.write_while_busy, 0);
    CHECK_EQ(m.unknown_cmds, 0);
    for (int i = 0; i < 4; i++) {
        /* VDD reference on every channel: internal reference off in standby */
        CHECK(m.ee[i].vref == 0 && m.ee[i].pd == 1 && m.ee[i].gx == 0 && m.ee[i].val == 0);
    }
    CHECK(all_in_pd());
    CHECK(!mcp4728_model_int_ref_on(&m));
    /* Second init: EEPROM now good, no new write. */
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK_EQ(m.eeprom_writes, 1);
    CHECK(!mcp4728_eeprom_was_written());
}

TEST(init_sequential_write_bytes)
{
    setup(false);
    mcp4728_init();
    static const uint8_t seq[] = {0x50, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00};
    CHECK_EQ(m.seq_wn, sizeof seq);
    CHECK(memcmp(m.seq_w, seq, sizeof seq) == 0);
    /* last_w is the final multi-write (12 bytes): all PD=01 blocks, A..D */
    static const uint8_t pd[] = {0x40, 0x20, 0x00, 0x42, 0x20, 0x00,
                                 0x44, 0x20, 0x00, 0x46, 0x20, 0x00};
    CHECK_EQ(m.last_wn, sizeof pd);
    CHECK(memcmp(m.last_w, pd, sizeof pd) == 0);
}

TEST(init_good_eeprom_no_write)
{
    setup(true);
    m.in[0] = (dac_reg_t){1, 0, 0, 1000}; /* LED still on after a warm reset */
    CHECK(mcp4728_model_int_ref_on(&m));
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK_EQ(m.eeprom_writes, 0);
    CHECK(all_in_pd()); /* but forced off */
    CHECK(!mcp4728_led_is_on());
    CHECK(!mcp4728_model_int_ref_on(&m));
}

/* A part programmed by the earlier build (channel A EEPROM on the internal
 * reference) keeps the reference running in standby: re-programmed once. */
TEST(init_old_defaults_reprogrammed_once)
{
    setup(true);
    m.ee[0] = (dac_reg_t){1, 1, 0, 0};
    for (int i = 0; i < 4; i++) m.in[i] = m.ee[i]; /* as loaded at power-up */
    CHECK(mcp4728_model_int_ref_on(&m));
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK_EQ(m.eeprom_writes, 1);
    CHECK(mcp4728_eeprom_was_written());
    for (int i = 0; i < 4; i++) CHECK(m.ee[i].vref == 0 && m.ee[i].pd == 1);
    CHECK(!mcp4728_model_int_ref_on(&m));
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK_EQ(m.eeprom_writes, 1);
}

TEST(init_each_difference_triggers_write)
{
    for (int f = 0; f < 7; f++) {
        setup(true);
        switch (f) {
        case 0: m.ee[0].vref = 1; break;
        case 1: m.ee[1].pd = 0; break;
        case 2: m.ee[2].gx = 1; break;
        case 3: m.ee[3].val = 5; break;
        case 4: m.ee[0].pd = 3; break;
        case 5: m.ee[3].pd = 2; break;
        case 6: m.ee[2].vref = 1; break;
        }
        CHECK_EQ(mcp4728_init(), HAL_OK);
        CHECK_EQ(m.eeprom_writes, 1);
    }
}

TEST(init_bsy_timeout_and_verify)
{
    setup(false);
    m.never_busy = 1;
    uint32_t t0 = mock_now;
    CHECK_EQ(mcp4728_init(), HAL_ERR_TIMEOUT);
    CHECK(mock_now - t0 >= 100 && mock_now - t0 < 110);

    setup(false);
    m.corrupt_eeprom = 1;
    CHECK_EQ(mcp4728_init(), HAL_ERR_BUS);
}

TEST(init_waits_for_bsy)
{
    setup(false);
    uint32_t t0 = mock_now;
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK(mock_now - t0 >= 50 && mock_now - t0 <= 52);
    CHECK_EQ(m.write_while_busy, 0);
}

TEST(nack_and_retry)
{
    setup(true);
    mock_i2c_inject(1, HAL_ERR_NACK);
    CHECK_EQ(mcp4728_init(), HAL_ERR_NACK);
    CHECK_EQ(mock_i2c_recover_count, 0);
    CHECK_EQ(m.writes, 0);

    setup(true);
    mock_i2c_inject(1, HAL_ERR_BUS); /* one failure: retried */
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK_EQ(mock_i2c_recover_count, 1);

    setup(true);
    CHECK_EQ(mcp4728_init(), HAL_OK);
    CHECK(!mcp4728_led_is_on());
    mock_i2c_inject(2, HAL_ERR_TIMEOUT); /* both fail */
    CHECK_EQ(mcp4728_set_led_code(10), HAL_ERR_TIMEOUT);
    CHECK_EQ(mock_i2c_recover_count, 1);
    /* the write may have reached the DAC: pessimistically still on */
    CHECK(mcp4728_led_is_on());
    CHECK_EQ(mcp4728_led_off(), HAL_OK);
    CHECK(!mcp4728_led_is_on());

    setup(false);
    CHECK_EQ(mcp4728_init(), HAL_OK);
    mock_i2c_inject(1, HAL_ERR_NACK);
    CHECK_EQ(mcp4728_set_led_code(10), HAL_ERR_NACK);
    CHECK(mcp4728_led_is_on());
    mock_i2c_inject(1, HAL_ERR_NACK); /* a failed power-down keeps it "on" */
    CHECK_EQ(mcp4728_led_off(), HAL_ERR_NACK);
    CHECK(mcp4728_led_is_on());
    CHECK_EQ(mcp4728_power_down_all(), HAL_OK);
    CHECK(!mcp4728_led_is_on());

    /* a failed init leaves the state pessimistic too (warm reset: channel A
     * may still be active) */
    setup(true);
    mock_i2c_inject(1, HAL_ERR_NACK);
    CHECK_EQ(mcp4728_init(), HAL_ERR_NACK);
    CHECK(mcp4728_led_is_on());
}

TEST(set_led_and_off_bytes)
{
    setup(true);
    mcp4728_init();
    int ee = m.eeprom_writes;
    CHECK_EQ(mcp4728_set_led_code(0x123), HAL_OK);
    static const uint8_t on[] = {0x40, 0x81, 0x23};
    CHECK_EQ(m.last_wn, 3);
    CHECK(memcmp(m.last_w, on, 3) == 0);
    CHECK(m.in[0].pd == 0 && m.in[0].vref == 1 && m.in[0].gx == 0 && m.in[0].val == 0x123);
    CHECK(mcp4728_led_is_on());
    CHECK(mcp4728_model_int_ref_on(&m)); /* only while channel A is on */
    for (int i = 1; i < 4; i++) CHECK(m.in[i].vref == 0);

    CHECK_EQ(mcp4728_set_led_code(0xFFFF), HAL_OK); /* clamp */
    CHECK(m.in[0].val == 4095);
    static const uint8_t full[] = {0x40, 0x8F, 0xFF};
    CHECK(memcmp(m.last_w, full, 3) == 0);

    CHECK_EQ(mcp4728_led_off(), HAL_OK);
    static const uint8_t off[] = {0x40, 0x20, 0x00};
    CHECK(memcmp(m.last_w, off, 3) == 0);
    CHECK(!mcp4728_led_is_on());
    CHECK(m.in[0].pd == 1);
    CHECK(!mcp4728_model_int_ref_on(&m));
    for (int i = 1; i < 4; i++) CHECK(m.in[i].pd == 1);
    CHECK_EQ(m.eeprom_writes, ee);
    CHECK_EQ(m.unknown_cmds, 0);
}

TEST(conversions)
{
    CHECK_EQ(mcp4728_code_from_ma100(0), 0);
    /* 5 mA -> 5000 uA / 3.718 = 1344.8 */
    CHECK_EQ(mcp4728_code_from_ma100(500), 1345);
    CHECK_EQ(mcp4728_code_from_ma100(2000), 5379 > 4095 ? 4095 : 5379);
    CHECK_EQ(mcp4728_code_from_ma100(1500), 4034);
    CHECK_EQ(mcp4728_code_from_ma100(1520), 4088);
    CHECK_EQ(mcp4728_code_from_ma100(1530), 4095);
    CHECK_EQ(mcp4728_code_from_ma100(0xFFFFFFFFu), 4095);
    CHECK_EQ(mcp4728_ma100_from_code(0), 0);
    CHECK_EQ(mcp4728_ma100_from_code(1345), 500);
    CHECK_EQ(mcp4728_ma100_from_code(4095), 1523);
    CHECK_EQ(mcp4728_ma100_from_code(60000), 1523);
    for (uint32_t c = 0; c <= 4095; c += 7) {
        uint32_t ma = mcp4728_ma100_from_code((uint16_t)c);
        int back = mcp4728_code_from_ma100(ma);
        CHECK(back - (int)c <= 3 && (int)c - back <= 3);
    }
}

int main(void)
{
    RUN(init_blank_writes_eeprom_once);
    RUN(init_sequential_write_bytes);
    RUN(init_good_eeprom_no_write);
    RUN(init_old_defaults_reprogrammed_once);
    RUN(init_each_difference_triggers_write);
    RUN(init_bsy_timeout_and_verify);
    RUN(init_waits_for_bsy);
    RUN(nack_and_retry);
    RUN(set_led_and_off_bytes);
    RUN(conversions);
    return TEST_RESULT();
}
