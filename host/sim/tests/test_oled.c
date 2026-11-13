#include <stdio.h>
#include <string.h>

#include "common.h"

static const uint8_t init_seq[] = {
    0xAE, 0x00, 0x10, 0x40, 0x81, 0xCF, 0xA1, 0xC8, 0xA6, 0xA8, 0x3F, 0xD3, 0x00,
    0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x30, 0x20, 0x00, 0x8D, 0x14, 0xAF};

static void cmds(const uint8_t *c, size_t n)
{
    low(PIN_OLED_DC);
    low(PIN_OLED_CS);
    hal_oled_write(c, n);
    high(PIN_OLED_CS);
}
static void power_up_and_init(void)
{
    low(PIN_OLED_RESET);
    high(PIN_OLED_EN);
    hal_delay_ms(5);
    high(PIN_OLED_RESET);
    hal_oled_bus_attach(true);
    high(PIN_OLED_CS);
    uint8_t pre[] = {0xAE};
    cmds(pre, 1);
    cmds(init_seq + 1, sizeof init_seq - 1);
}
static uint8_t fb[OLED_FB_BYTES];
static void send_frame(void)
{
    static const uint8_t win[] = {0x21, 0, 127, 0x22, 0, 7};
    cmds(win, sizeof win);
    high(PIN_OLED_DC);
    low(PIN_OLED_CS);
    hal_oled_write(fb, sizeof fb);
    high(PIN_OLED_CS);
}
static void pattern(void)
{
    memset(fb, 0, sizeof fb);
    for (int c = 0; c < 128; c++) {
        fb[0 * 128 + c] = 0x01;          /* y = 0 line */
        fb[7 * 128 + c] = 0x80;          /* y = 63 line */
    }
    fb[3 * 128 + 10] = 0x18;             /* two pixels at x=10, y=27,28 */
}
static void power_down(void)
{
    uint8_t off[] = {0xAE, 0x8D, 0x10};
    cmds(off, sizeof off);
    hal_delay_ms(100);
    hal_oled_bus_attach(false);
    low(PIN_OLED_DC);
    low(PIN_OLED_CS);
    low(PIN_OLED_RESET);
    low(PIN_OLED_EN);
}

TEST(renders_known_pattern)
{
    fresh();
    power_up_and_init();
    pattern();
    send_frame();
    uint8_t img[64][128];
    sim_oled_image(img);
    int lit = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) lit += img[y][x];
    CHECK_EQ(lit, 0); /* not settled yet: charge pump just started */
    CHECK(!sim_oled_info().image_settled);
    hal_delay_ms(100);
    sim_oled_image(img);
    lit = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) lit += img[y][x];
    CHECK_EQ(lit, 128 * 2 + 2);
    CHECK_EQ(img[0][0] && img[0][127] && img[63][0] && img[63][127], 1);
    CHECK(img[27][10] && img[28][10] && !img[26][10] && !img[29][10]);
    CHECK_EQ(sim_violation_count(), 0);
    sim_oled_info_t i = sim_oled_info();
    CHECK(i.powered && i.reset_pulse_seen && i.init_complete && i.image_settled);
    CHECK_EQ(i.early_reset_releases, 0);
    CHECK_EQ(i.data_bytes, 1024);
    CHECK_EQ(i.contrast, 0xCF);
    CHECK(sim_oled_save_pgm("/tmp/claude-1000/-home-mlaustin-repos-ecg-card/7837beaa-f700-4f20-990f-e116cf2b9488/scratchpad/oled.pgm", 2));
    CHECK(sim_oled_save_png("/tmp/claude-1000/-home-mlaustin-repos-ecg-card/7837beaa-f700-4f20-990f-e116cf2b9488/scratchpad/oled.png", 2));
}

TEST(orientation_and_modes)
{
    fresh();
    power_up_and_init();
    pattern();
    send_frame();
    hal_delay_ms(100);
    uint8_t img[64][128];
    /* flip both axes: A0 + C0 */
    uint8_t flip[] = {0xA0, 0xC0};
    cmds(flip, 2);
    sim_oled_image(img);
    CHECK(img[63][127] && img[0][0] && img[36][117] && img[35][117]);
    /* inverse and entire-on */
    uint8_t inv[] = {0xA1, 0xC8, 0xA7};
    cmds(inv, 3);
    sim_oled_image(img);
    CHECK(!img[0][0] && img[10][50]);
    uint8_t on[] = {0xA6, 0xA5};
    cmds(on, 2);
    sim_oled_image(img);
    CHECK(img[10][50] && img[30][70]);
    uint8_t norm[] = {0xA4};
    cmds(norm, 1);
    /* start line 1 scrolls the image up by one row: row 0 shows RAM row 1 (dark) */
    uint8_t sl[] = {0x41};
    cmds(sl, 1);
    sim_oled_image(img);
    CHECK(!img[0][0] && img[62][0] && img[63][0] && !img[61][0]);
    uint8_t sl0[] = {0x40};
    cmds(sl0, 1);
    /* page addressing: B3, column 20, write 0xFF */
    uint8_t pg[] = {0x20, 0x02, 0xB3, 0x04, 0x11};
    cmds(pg, sizeof pg);
    high(PIN_OLED_DC);
    low(PIN_OLED_CS);
    uint8_t ff = 0xFF;
    hal_oled_write(&ff, 1);
    high(PIN_OLED_CS);
    CHECK_EQ(sim_oled_ram_byte(3, 20 + 4 - 4 + 0 * 0 + 0), 0xFF);
    /* column pointer was set to (0x1<<4)|0x4 = 20 */
    /* horizontal wrap: window cols 5..6, pages 1..2 */
    uint8_t hw[] = {0x20, 0x00, 0x21, 5, 6, 0x22, 1, 2};
    cmds(hw, sizeof hw);
    uint8_t d[5] = {1, 2, 3, 4, 5};
    high(PIN_OLED_DC);
    low(PIN_OLED_CS);
    hal_oled_write(d, 5);
    high(PIN_OLED_CS);
    CHECK_EQ(sim_oled_ram_byte(1, 6), 2);
    CHECK_EQ(sim_oled_ram_byte(2, 5), 3);
    CHECK_EQ(sim_oled_ram_byte(2, 6), 4);
    CHECK_EQ(sim_oled_ram_byte(1, 5), 5); /* wrapped to page 1 start: overwritten */
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(blank_when_off_or_unpowered)
{
    fresh();
    uint8_t img[64][128];
    int lit;
#define COUNT_LIT() do { lit = 0; sim_oled_image(img); for (int y_ = 0; y_ < 64; y_++) for (int x_ = 0; x_ < 128; x_++) lit += img[y_][x_]; } while (0)
    COUNT_LIT();
    CHECK_EQ(lit, 0);
    power_up_and_init();
    pattern();
    send_frame();
    hal_delay_ms(100);
    COUNT_LIT();
    CHECK(lit > 0);
    uint8_t aeoff[] = {0xAE};
    cmds(aeoff, 1);
    COUNT_LIT();
    CHECK_EQ(lit, 0);
    uint8_t aeon[] = {0xAF};
    cmds(aeon, 1);
    hal_delay_ms(100);
    COUNT_LIT();
    CHECK(lit > 0);
    uint8_t pump_off[] = {0x8D, 0x10};
    cmds(pump_off, 2);
    COUNT_LIT();
    CHECK_EQ(lit, 0);
    uint8_t pump_on[] = {0x8D, 0x14};
    cmds(pump_on, 2);
    hal_delay_ms(100);
    COUNT_LIT();
    CHECK(lit > 0);
    /* module in reset shows nothing */
    low(PIN_OLED_RESET);
    COUNT_LIT();
    CHECK_EQ(lit, 0);
    high(PIN_OLED_RESET);
    /* RAM survives reset, but display is off after it */
    CHECK_EQ(sim_oled_ram_byte(0, 0), 0x01);
    COUNT_LIT();
    CHECK_EQ(lit, 0);

    /* proper power down: unpowered, RAM lost, and stays blank after power-up until redrawn */
    fresh();
    power_up_and_init();
    send_frame();
    hal_delay_ms(100);
    power_down();
    COUNT_LIT();
    CHECK_EQ(lit, 0);
    CHECK_EQ(sim_oled_ram_byte(0, 0), 0);
    CHECK_EQ(sim_violation_count(), 0);
    CHECK_EQ(sim_oled_info().dirty_power_offs, 0);
    CHECK(!sim_oled_info().powered);
    /* dirty power off is recorded */
    fresh();
    power_up_and_init();
    hal_oled_bus_attach(false);
    low(PIN_OLED_DC);
    low(PIN_OLED_CS);
    low(PIN_OLED_RESET);
    low(PIN_OLED_EN);
    CHECK_EQ(sim_oled_info().dirty_power_offs, 1);
}

TEST(power_sequence_facts)
{
    fresh();
    /* no reset pulse: RESET high before power */
    high(PIN_OLED_EN); /* EN high first: RESET is low (initial), fine */
    CHECK(sim_oled_info().in_reset);
    hal_delay_ms(1);
    high(PIN_OLED_RESET); /* released after 1 ms: too early */
    CHECK_EQ(sim_oled_info().early_reset_releases, 1);
    CHECK(sim_oled_info().reset_pulse_seen);
    CHECK_EQ(sim_violation_count(), 0);
    /* commands before attach are not seen */
    fresh();
    high(PIN_OLED_EN);
    hal_delay_ms(5);
    high(PIN_OLED_RESET);
    hal_oled_bus_attach(true);
    high(PIN_OLED_CS);
    uint8_t ae = 0xAF;
    low(PIN_OLED_DC);
    hal_oled_write(&ae, 1); /* CS high: ignored */
    CHECK_EQ(sim_oled_info().ignored_bytes, 1);
    CHECK(!sim_oled_info().display_on);
    low(PIN_OLED_CS);
    hal_oled_write(&ae, 1);
    CHECK(sim_oled_info().display_on);
}

TEST(async_frame_timing)
{
    fresh();
    power_up_and_init();
    pattern();
    static const uint8_t win[] = {0x21, 0, 127, 0x22, 0, 7};
    cmds(win, sizeof win);
    high(PIN_OLED_DC);
    low(PIN_OLED_CS);
    uint64_t t0 = sim_now_us();
    CHECK(hal_oled_write_async(fb, sizeof fb));
    CHECK(hal_oled_busy());
    hal_delay_us(1023);
    CHECK(hal_oled_busy());
    CHECK_EQ(sim_oled_ram_byte(7, 126), 0x80); /* all but the last byte have arrived */
    CHECK_EQ(sim_oled_ram_byte(7, 127) == 0x80, 0);
    hal_delay_us(1);
    CHECK(!hal_oled_busy());
    CHECK_EQ(sim_now_us() - t0, 1024);
    CHECK_EQ(sim_oled_ram_byte(7, 127), 0x80);
    high(PIN_OLED_CS);
    /* raising CS mid-transfer corrupts the tail: bytes sent with CS high are ignored */
    low(PIN_OLED_CS);
    memset(fb, 0xFF, sizeof fb);
    CHECK(hal_oled_write_async(fb, sizeof fb));
    hal_delay_us(100);
    high(PIN_OLED_CS);
    hal_delay_us(2000);
    CHECK(sim_oled_info().ignored_bytes >= 900);
    CHECK_EQ(sim_violation_count(), 0);
}

int main(void)
{
    RUN(renders_known_pattern);
    RUN(orientation_and_modes);
    RUN(blank_when_off_or_unpowered);
    RUN(power_sequence_facts);
    RUN(async_frame_timing);
    return TEST_RESULT();
}
