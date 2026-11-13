#include <stdio.h>
#include <string.h>

#include "common.h"

TEST(nv_persists_across_reboot_and_counts_erases)
{
    fresh();
    uint8_t b[16];
    hal_nv_read(b, sizeof b);
    for (size_t i = 0; i < sizeof b; i++) CHECK_EQ(b[i], 0xFF);
    uint8_t blob[100];
    for (int i = 0; i < 100; i++) blob[i] = (uint8_t)i;
    uint64_t t0 = sim_now_us();
    CHECK_EQ(hal_nv_write(blob, sizeof blob), HAL_OK);
    CHECK(sim_now_us() - t0 >= 5000); /* flash is slow */
    CHECK_EQ(sim_nv_erase_count(), 1);
    sim_reboot();
    hal_init();
    uint8_t rd[256];
    hal_nv_read(rd, sizeof rd);
    CHECK(memcmp(rd, blob, 100) == 0);
    CHECK_EQ(rd[100], 0xFF);
    CHECK_EQ(rd[255], 0xFF);
    CHECK_EQ(sim_nv_erase_count(), 1);
    hal_nv_write(blob, 4);
    CHECK_EQ(sim_nv_erase_count(), 2);
    hal_nv_read(rd, 8);
    CHECK_EQ(rd[3], 3);
    CHECK_EQ(rd[4], 0xFF);
    CHECK_EQ(hal_nv_write(blob, HAL_NV_MAX + 1), HAL_ERR_PARAM);
    CHECK_EQ(sim_nv_erase_count(), 2);
    /* corrupt / preload / failure injection */
    sim_nv_corrupt(2, 0x01);
    hal_nv_read(rd, 4);
    CHECK_EQ(rd[2], 3);
    sim_nv_fail_next_write(HAL_ERR_TIMEOUT);
    CHECK_EQ(hal_nv_write(blob, 10), HAL_ERR_TIMEOUT);
    hal_nv_read(rd, 4);
    CHECK_EQ(rd[2], 3);
    CHECK_EQ(hal_nv_write(blob, 10), HAL_OK);
    const uint8_t pre[3] = {9, 8, 7};
    sim_nv_set(pre, 3);
    hal_nv_read(rd, 4);
    CHECK(rd[0] == 9 && rd[2] == 7 && rd[3] == 0xFF);
    sim_reset();
    hal_nv_read(rd, 1);
    CHECK_EQ(rd[0], 0xFF);
    CHECK_EQ(sim_nv_erase_count(), 0);
}

TEST(log_and_stream_capture)
{
    fresh();
    CHECK_EQ(hal_log_write("hello ", 6), 6);
    CHECK_EQ(hal_log_write("world", 5), 5);
    CHECK(strcmp(sim_log_text(), "hello world") == 0);
    CHECK_EQ(sim_log_len(), 11);
    sim_log_clear();
    CHECK_EQ(sim_log_len(), 0);
    uint8_t a[3] = {1, 2, 3}, b[2] = {0, 4};
    CHECK_EQ(hal_stream_write(a, 3), 3);
    hal_stream_write(b, 2);
    size_t n = 0;
    const uint8_t *d = sim_stream_data(&n);
    CHECK_EQ(n, 5);
    CHECK(d[0] == 1 && d[4] == 4);
    const char *path = "/tmp/claude-1000/-home-mlaustin-repos-ecg-card/7837beaa-f700-4f20-990f-e116cf2b9488/scratchpad/stream.bin";
    CHECK(sim_stream_save(path));
    FILE *f = fopen(path, "rb");
    uint8_t rd[8];
    CHECK(f != NULL);
    if (f) {
        CHECK_EQ(fread(rd, 1, 8, f), 5);
        fclose(f);
        CHECK(memcmp(rd, d, 5) == 0);
    }
    /* big streams grow */
    uint8_t blk[1000] = {0};
    for (int i = 0; i < 100; i++) hal_stream_write(blk, sizeof blk);
    sim_stream_data(&n);
    CHECK_EQ(n, 100005);
    sim_stream_clear();
    sim_stream_data(&n);
    CHECK_EQ(n, 0);
}

static void count_cb_forward(void *c) { (void)c; }
TEST(reset_restores_power_on_state)
{
    fresh();
    high(PIN_ANA_EN);
    sim_set_usb(true);
    sim_set_battery_mv(3300);
    sim_at(100, count_cb_forward, NULL);
    hal_delay_ms(500);
    sim_violations_clear();
    sim_reset();
    CHECK_EQ(sim_now_us(), 0);
    CHECK(!sim_pin(PIN_ANA_EN));
    CHECK(sim_pin(PIN_CHG_PG));
    CHECK(sim_pin(PIN_BUTTON1) && sim_pin(PIN_BUTTON2) && sim_pin(PIN_CW_ALRT));
    CHECK(sim_led_blank());
    CHECK_EQ(sim_scheduled_count(), 0);
    CHECK(!sim_should_stop());
    CHECK_EQ(sim_violation_count(), 0);
    CHECK_EQ(sim_mcp_code(0), 0x800);
    CHECK(sim_cw_asleep());
    CHECK(!sim_oled_info().powered);
    CHECK_EQ(hal_acq_running(), 0);
    CHECK_EQ(hal_reset_cause(), HAL_RESET_POWER_ON);
}

TEST(irq_lock_nests)
{
    fresh();
    uint32_t a = hal_irq_lock();
    uint32_t b = hal_irq_lock();
    hal_irq_unlock(b);
    hal_irq_unlock(a);
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 1);
}

static int calls;
static void count_cb(void *c) { (void)c; calls++; }
TEST(canned_events_and_clear)
{
    fresh();
    calls = 0;
    sim_at_thumbs(10, true);
    sim_at_finger(20, true);
    sim_at_battery_mv(30, 3600);
    sim_at(40, count_cb, NULL);
    sim_clear_scheduled();
    hal_delay_ms(50);
    CHECK_EQ(calls, 0);
    sim_at_battery_mv(60, 3600);
    sim_at_usb(70, true);
    sim_at_usb(80, false);
    hal_delay_ms(100);
    CHECK(!sim_usb_present());
    CHECK_EQ(sim_scheduled_count(), 0);
}

int main(void)
{
    RUN(nv_persists_across_reboot_and_counts_erases);
    RUN(log_and_stream_capture);
    RUN(reset_restores_power_on_state);
    RUN(irq_lock_nests);
    RUN(canned_events_and_clear);
    return TEST_RESULT();
}
