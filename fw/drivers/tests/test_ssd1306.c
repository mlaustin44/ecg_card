#include <string.h>

#include "mock_hal.h"
#include "ssd1306.h"
#include "test.h"

static const uint8_t k_expect_cmds[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xA1, 0xC8, 0xDA, 0x12,
    0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x30, 0xA4, 0xA6, 0x20, 0x00, 0x21, 0x00,
    0x7F, 0x22, 0x00, 0x07, 0x8D, 0x14, 0xAF,
};

static int count_pin_ev(board_pin_t p, int level)
{
    int n = 0;
    for (int i = 0; i < mock_ev_n; i++) {
        n += mock_ev[i].kind == MEV_PIN && mock_ev[i].a == (int)p && mock_ev[i].b == level;
    }
    return n;
}

static void fresh(void)
{
    if (ssd1306_is_powered()) ssd1306_power_off();
    mock_reset();
}

TEST(power_on_sequence)
{
    fresh();
    uint32_t t0 = mock_now;
    ssd1306_power_on();

    /* Pin/bus event order up to first SPI byte. */
    int i_rst0 = mock_find_pin_ev(0, PIN_OLED_RESET, 0);
    int i_en = mock_find_pin_ev(0, PIN_OLED_EN, 1);
    int i_rst1 = mock_find_pin_ev(0, PIN_OLED_RESET, 1);
    CHECK(i_rst0 == 0);
    CHECK(i_en == 1);
    CHECK(i_rst1 > i_en);
    /* >= 5 ms between EN high and the bus attach / reset release. */
    int i_att = -1;
    for (int i = 0; i < mock_ev_n; i++) {
        if (mock_ev[i].kind == MEV_ATTACH && mock_ev[i].a == 1) {
            i_att = i;
            break;
        }
    }
    CHECK(i_att > i_en && i_att < i_rst1);
    CHECK(mock_ev[i_att].t - mock_ev[i_en].t >= 5);
    CHECK(mock_ev[i_rst1].t - mock_ev[i_en].t >= 5);
    /* Before OLED_EN rises, no pin write other than RESET low. */
    for (int i = 0; i < i_en; i++) {
        CHECK(mock_ev[i].kind == MEV_PIN && mock_ev[i].a == PIN_OLED_RESET && mock_ev[i].b == 0);
    }
    /* CS idle high and DC low were set before reset release. */
    int i_cs = mock_find_pin_ev(0, PIN_OLED_CS, 1);
    CHECK(i_cs > i_att && i_cs < i_rst1);
    /* First SPI traffic is after reset release. */
    for (int i = 0; i < mock_ev_n; i++) {
        if (mock_ev[i].kind == MEV_SPI) {
            CHECK(i > i_rst1);
            break;
        }
    }

    /* Command bytes: all DC=0 bytes, in order, CS low. */
    uint8_t cmds[64];
    int nc = 0, nd = 0;
    bool data_all_zero = true;
    for (int i = 0; i < mock_spi_n; i++) {
        if (!mock_spi[i].dc) {
            if (nc < 64) cmds[nc] = mock_spi[i].byte;
            nc++;
            CHECK(!mock_spi[i].cs);
        } else {
            nd++;
            data_all_zero &= mock_spi[i].byte == 0 && !mock_spi[i].cs;
        }
    }
    CHECK_EQ(nc, sizeof k_expect_cmds);
    CHECK(nc == (int)sizeof k_expect_cmds && memcmp(cmds, k_expect_cmds, sizeof k_expect_cmds) == 0);
    CHECK_EQ(nd, 1024);
    CHECK(data_all_zero);
    /* 0x8D 0x14 immediately before 0xAF; AF is the last command. */
    CHECK(cmds[nc - 3] == 0x8D && cmds[nc - 2] == 0x14 && cmds[nc - 1] == 0xAF);
    /* Idle levels at the end: CS high, DC low, reset released. */
    CHECK(mock_pin[PIN_OLED_CS] && mock_pin[PIN_OLED_RESET] && mock_pin[PIN_OLED_EN]);

    /* Blocking time: ~9 ms, and image reliable only after 100 ms. */
    CHECK(mock_now - t0 <= 12);
    CHECK(ssd1306_is_on() && ssd1306_is_powered());
    CHECK(!ssd1306_ready());
    mock_advance(99);
    CHECK(!ssd1306_ready());
    mock_advance(1);
    CHECK(ssd1306_ready());
    CHECK_EQ(mock_viol_high_unpowered + mock_viol_en_fell_driven + mock_viol_spi, 0);
}

TEST(power_off_sequence)
{
    fresh();
    ssd1306_power_on();
    mock_spi_n = 0;
    mock_ev_n = 0;
    ssd1306_power_off();

    CHECK(!ssd1306_is_powered() && !ssd1306_is_on());
    CHECK_EQ(mock_spi_n, 3);
    CHECK(mock_spi[0].byte == 0xAE && mock_spi[1].byte == 0x8D && mock_spi[2].byte == 0x10);
    CHECK(!mock_spi[0].dc && !mock_spi[1].dc && !mock_spi[2].dc);

    int i_en = mock_find_pin_ev(0, PIN_OLED_EN, 0);
    CHECK(i_en >= 0);
    int i_wait = -1;
    for (int i = 0; i < mock_ev_n; i++) {
        if (mock_ev[i].kind == MEV_DELAY && mock_ev[i].a >= 100) i_wait = i;
    }
    CHECK(i_wait >= 0);
    int i_dc = mock_find_pin_ev(i_wait, PIN_OLED_DC, 0);
    int i_cs = mock_find_pin_ev(i_wait, PIN_OLED_CS, 0);
    int i_rs = mock_find_pin_ev(i_wait, PIN_OLED_RESET, 0);
    int i_at = -1;
    int i_spi_last = -1;
    for (int i = 0; i < mock_ev_n; i++) {
        if (mock_ev[i].kind == MEV_ATTACH && mock_ev[i].a == 0) i_at = i;
        if (mock_ev[i].kind == MEV_SPI) i_spi_last = i;
    }
    CHECK(i_dc >= 0 && i_cs >= 0 && i_rs >= 0 && i_at >= 0);
    CHECK(i_dc < i_en && i_cs < i_en && i_rs < i_en && i_at < i_en);
    /* 100 ms between the last command and the first pin drop. */
    int first = i_dc < i_cs ? i_dc : i_cs;
    if (i_rs < first) first = i_rs;
    CHECK(mock_ev[first].t - mock_ev[i_spi_last].t >= 100);
    CHECK(i_wait > i_spi_last);
    /* Every OLED pin low and bus detached when EN falls; nothing driven high
     * after the last SPI command. */
    for (int i = i_spi_last; i < mock_ev_n; i++) {
        CHECK(!(mock_ev[i].kind == MEV_PIN && mock_ev[i].b == 1 && i > i_spi_last &&
                mock_ev[i].a != PIN_OLED_CS));
    }
    CHECK_EQ(mock_viol_en_fell_driven, 0);
    CHECK(!mock_pin[PIN_OLED_EN] && !mock_pin[PIN_OLED_DC] &&
          !mock_pin[PIN_OLED_CS] && !mock_pin[PIN_OLED_RESET] && !mock_attached);
    CHECK_EQ(mock_viol_high_unpowered, 0);

    /* Second power cycle works. */
    ssd1306_power_on();
    CHECK(ssd1306_is_on());
    CHECK_EQ(mock_viol_high_unpowered + mock_viol_en_fell_driven + mock_viol_spi, 0);
}

TEST(power_off_during_flush)
{
    static uint8_t fb[OLED_FB_BYTES];
    fresh();
    ssd1306_power_on();
    mock_dma_ms = 5;
    CHECK(ssd1306_flush_start(fb));
    ssd1306_power_off();
    CHECK_EQ(mock_viol_en_fell_driven + mock_viol_spi + mock_viol_high_unpowered, 0);
    CHECK(!ssd1306_busy());
}

/* Review finding 8: the SPI never completes (hardware fault). power_off()
 * must still finish the pin-safe sequence in bounded time: no command sent
 * into a busy bus, pins low and the bus detached before OLED_EN falls. */
TEST(power_off_spi_stuck)
{
    static uint8_t fb[OLED_FB_BYTES];
    fresh();
    ssd1306_power_on();
    mock_dma_ms = 1000000; /* never finishes */
    CHECK(ssd1306_flush_start(fb));
    mock_ev_n = 0;
    uint32_t t0 = mock_now;
    ssd1306_power_off();
    CHECK(mock_now - t0 <= 60); /* the 50 ms wait, then on */
    CHECK(!ssd1306_is_powered());
    int i_en = mock_find_pin_ev(0, PIN_OLED_EN, 0);
    CHECK(i_en >= 0);
    int i_at = -1;
    for (int i = 0; i < mock_ev_n; i++) {
        CHECK(mock_ev[i].kind != MEV_SPI); /* nothing written into the busy bus */
        if (mock_ev[i].kind == MEV_ATTACH && mock_ev[i].a == 0) i_at = i;
    }
    CHECK(i_at >= 0 && i_at < i_en);
    CHECK(mock_find_pin_ev(0, PIN_OLED_DC, 0) < i_en);
    CHECK(mock_find_pin_ev(0, PIN_OLED_RESET, 0) < i_en);
    CHECK(!mock_pin[PIN_OLED_EN] && !mock_pin[PIN_OLED_DC] && !mock_pin[PIN_OLED_CS] &&
          !mock_pin[PIN_OLED_RESET] && !mock_attached);
    CHECK_EQ(mock_viol_en_fell_driven + mock_viol_high_unpowered, 0);
    mock_dma_ms = 2;
}

TEST(flush_framing)
{
    static uint8_t fb[OLED_FB_BYTES];
    for (unsigned i = 0; i < sizeof fb; i++) fb[i] = (uint8_t)(i * 7 + 3);
    fresh();
    ssd1306_power_on();
    mock_spi_n = 0;
    mock_ev_n = 0;

    CHECK(ssd1306_flush_start(fb));
    /* Window commands then data. */
    static const uint8_t win[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
    CHECK_EQ(mock_spi_n, 6 + 1024);
    for (int i = 0; i < 6; i++) {
        CHECK(mock_spi[i].byte == win[i] && !mock_spi[i].dc && !mock_spi[i].cs && !mock_spi[i].async);
    }
    bool ok = true;
    for (int i = 0; i < 1024; i++) {
        mock_spi_t *s = &mock_spi[6 + i];
        ok &= s->byte == fb[i] && s->dc && !s->cs && s->async;
    }
    CHECK(ok);
    CHECK(ssd1306_busy());
    CHECK(!mock_pin[PIN_OLED_CS]);
    CHECK(!ssd1306_flush_start(fb));       /* busy */
    CHECK(!ssd1306_set_contrast(1));       /* refused while busy */
    CHECK(!ssd1306_display_on(false));
    CHECK(ssd1306_busy());
    CHECK(!mock_pin[PIN_OLED_CS]);         /* CS still low: transfer running */
    mock_advance(2);
    CHECK(!ssd1306_flush_poll());
    CHECK(mock_pin[PIN_OLED_CS]);
    CHECK(!ssd1306_busy());
    CHECK_EQ(mock_viol_spi, 0);

    /* Back to back flush works. */
    CHECK(ssd1306_flush_start(fb));
    mock_advance(2);
    CHECK(!ssd1306_busy());
}

TEST(contrast_and_blank)
{
    fresh();
    CHECK(!ssd1306_set_contrast(0x10)); /* not powered */
    ssd1306_power_on();
    mock_spi_n = 0;
    CHECK(ssd1306_set_contrast(0x42));
    CHECK(ssd1306_display_on(false));
    CHECK(!ssd1306_is_on() && ssd1306_is_powered());
    CHECK(ssd1306_display_on(true));
    CHECK(ssd1306_is_on());
    CHECK_EQ(mock_spi_n, 4);
    CHECK(mock_spi[0].byte == 0x81 && mock_spi[1].byte == 0x42 && mock_spi[2].byte == 0xAE &&
          mock_spi[3].byte == 0xAF);
    CHECK(mock_pin[PIN_OLED_CS]);
    CHECK(count_pin_ev(PIN_OLED_EN, 1) == 1);
    static uint8_t fb[OLED_FB_BYTES];
    ssd1306_power_off();
    CHECK(!ssd1306_flush_start(fb)); /* not powered */
}

int main(void)
{
    RUN(power_on_sequence);
    RUN(power_off_sequence);
    RUN(power_off_during_flush);
    RUN(power_off_spi_stuck);
    RUN(flush_framing);
    RUN(contrast_and_blank);
    return TEST_RESULT();
}
