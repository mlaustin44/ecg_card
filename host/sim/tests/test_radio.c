/* UART0 model and fake CH585, driven through hal.h only (no app code). */
#include <string.h>

#include "common.h"

/* ---- a minimal MCU-side codec for these tests ---- */
static uint16_t crc(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}
static size_t frame(uint8_t type, uint8_t seq, const uint8_t *p, size_t n, uint8_t *w)
{
    uint8_t f[80];
    f[0] = type; f[1] = seq; f[2] = (uint8_t)n; f[3] = 0;
    memcpy(f + 4, p, n);
    uint16_t c = crc(f, n + 4);
    f[n + 4] = (uint8_t)c; f[n + 5] = (uint8_t)(c >> 8);
    size_t o = 1, at = 0; uint8_t code = 1;
    for (size_t i = 0; i < n + 6; i++) {
        if (f[i]) { w[o++] = f[i]; code++; }
        else { w[at] = code; at = o++; code = 1; }
    }
    w[at] = code;
    w[o++] = 0;
    return o;
}

typedef struct { uint8_t type, seq; uint8_t p[64]; size_t n; } rx_t;
static uint8_t g_acc[256];
static size_t g_acc_n;
/* Read everything the radio sent; decode frames into out[]. */
static size_t rx_frames(rx_t *out, size_t max)
{
    uint8_t b[256];
    size_t got = hal_uart_read(b, sizeof b), k = 0;
    for (size_t i = 0; i < got; i++) {
        if (b[i]) { if (g_acc_n < sizeof g_acc) g_acc[g_acc_n++] = b[i]; continue; }
        uint8_t f[256]; size_t o = 0, j = 0;
        while (j < g_acc_n) {
            uint8_t c = g_acc[j++];
            for (int q = 1; q < c && j < g_acc_n; q++) f[o++] = g_acc[j++];
            if (c != 0xFF && j < g_acc_n) f[o++] = 0;
        }
        g_acc_n = 0;
        if (o < 6 || crc(f, o - 2) != (uint16_t)(f[o - 2] | f[o - 1] << 8) || k >= max) continue;
        out[k].type = f[0]; out[k].seq = f[1]; out[k].n = o - 6;
        memcpy(out[k].p, f + 4, out[k].n < 64 ? out[k].n : 64);
        k++;
    }
    return k;
}

static void send(uint8_t type, uint8_t seq, const uint8_t *p, size_t n)
{
    uint8_t w[100];
    size_t m = frame(type, seq, p, n, w);
    CHECK_EQ(hal_uart_write(w, m), m);
}

TEST(power_order_and_trace)
{
    fresh();
    hal_radio_power(true);
    CHECK(sim_pin(PIN_RADIO_EN) && sim_uart_enabled() && sim_radio_tx_pin());
    CHECK(hal_radio_powered());
    hal_radio_power(false);
    CHECK(!sim_pin(PIN_RADIO_EN) && !sim_uart_enabled() && !sim_radio_tx_pin());
    CHECK_EQ(sim_radio_trace_count(), 4);
    CHECK_EQ(sim_radio_trace_get(0, NULL), SIM_RT_RADIO_EN_HIGH);
    CHECK_EQ(sim_radio_trace_get(1, NULL), SIM_RT_UART_ON);
    CHECK_EQ(sim_radio_trace_get(2, NULL), SIM_RT_UART_OFF);
    CHECK_EQ(sim_radio_trace_get(3, NULL), SIM_RT_RADIO_EN_LOW);
    CHECK_EQ(sim_violation_count(), 0);
    /* standby with the radio on is a precondition violation */
    hal_radio_power(true);
    hal_standby(1);
    CHECK_EQ(sim_violation_count_of(SIM_V_STANDBY_PRECOND), 2); /* RADIO_EN, UART0 */
}

TEST(hello_after_boot_and_byte_timing)
{
    fresh();
    hal_radio_power(true);
    rx_t f[8];
    hal_delay_ms(19);
    CHECK_EQ(rx_frames(f, 8), 0);
    hal_delay_us(1100); /* HELLO at 20 ms + 19 bytes x 21.7 us */
    size_t n = rx_frames(f, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(f[0].type, LINK_HELLO);
    CHECK_EQ(f[0].n, LINK_HELLO_LEN);
    CHECK_EQ(f[0].p[0], LINK_PROTO_VERSION);
    CHECK_EQ(f[0].p[1], 1);
    CHECK_EQ(f[0].p[4], 0xF6);
    hal_delay_ms(5);
    n = rx_frames(f, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(f[0].type, STREAM_LOG);
    CHECK(sim_radio_info()->hello_sent);

    /* MCU -> radio: 460 bytes take 10 ms on the wire */
    uint8_t junk[460];
    memset(junk, 0x55, sizeof junk);
    junk[sizeof junk - 1] = 0;
    uint64_t t0 = sim_now_us();
    CHECK_EQ(hal_uart_write(junk, sizeof junk), sizeof junk);
    CHECK_EQ(hal_uart_write(junk, 100), 0); /* all or nothing: does not fit */
    hal_delay_us(9900);
    CHECK_EQ(hal_uart_write(junk, 100), 100); /* drained meanwhile */
    hal_delay_ms(20);
    (void)t0;
    hal_uart_stats_t st;
    hal_uart_stats(&st);
    CHECK_EQ(st.tx_bytes, 560);
    CHECK_EQ(sim_radio_info()->crc_errors, 1); /* the junk was one bad frame */
}

TEST(ctrl_ack_adv_connect)
{
    fresh();
    hal_radio_power(true);
    hal_delay_ms(30);
    rx_t f[8];
    rx_frames(f, 8);
    uint8_t info[] = {LINK_RC_SET_INFO, LINK_INFO_MCU_FW, '0', '.', '1'};
    send(LINK_RADIO_CTRL, 0, info, sizeof info);
    uint8_t adv[] = {LINK_RC_ADV_START};
    send(LINK_RADIO_CTRL, 1, adv, 1);
    hal_delay_ms(3);
    size_t n = rx_frames(f, 8);
    CHECK_EQ(n, 3); /* CONN_STATE (adv fast) at once, the two ACKs 0.3 ms after each frame */
    CHECK_EQ(f[0].type, LINK_CONN_STATE);
    CHECK_EQ(f[0].p[0], LINK_CS_ADV_FAST);
    CHECK_EQ(f[1].type, LINK_ACK);
    CHECK_EQ(f[1].p[0], 0);
    CHECK_EQ(f[2].type, LINK_ACK);
    CHECK_EQ(f[2].p[0], 1);
    CHECK(strcmp(sim_radio_info()->mcu_info, "0.1") == 0);
    CHECK(sim_phone_connect(247, 16, LINK_SUB_ECG));
    hal_delay_ms(2);
    n = rx_frames(f, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(f[0].p[0], LINK_CS_CONNECTED);
    CHECK_EQ(f[0].p[1] | f[0].p[2] << 8, 247);
    CHECK_EQ(f[0].p[5], LINK_SUB_ECG);
    uint8_t cmd[] = {LINK_CMD_GET_STATUS};
    int seq = sim_phone_command(cmd, 1);
    CHECK(seq >= 0);
    hal_delay_ms(2);
    n = rx_frames(f, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(f[0].type, LINK_CMD);
    CHECK_EQ(f[0].seq, seq);
    uint8_t ack[] = {(uint8_t)seq, LINK_RES_OK};
    send(LINK_ACK, 2, ack, 2);
    hal_delay_ms(2);
    uint8_t r = 0xFF;
    CHECK(sim_radio_ack_of((uint8_t)seq, &r));
    CHECK_EQ(r, LINK_RES_OK);
    /* shutdown: ACK, then silence; a cut within 50 ms is clean */
    uint8_t sd[] = {LINK_RC_SHUTDOWN};
    send(LINK_RADIO_CTRL, 3, sd, 1);
    hal_delay_ms(2);
    n = rx_frames(f, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(f[0].type, LINK_ACK);
    CHECK_EQ(f[0].p[0], 3);
    sim_radio_log("late");
    hal_radio_power(false);
    CHECK_EQ(sim_radio_info()->clean_offs, 1);
    CHECK_EQ(sim_radio_info()->unclean_offs, 0);
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(silent_and_unclean)
{
    fresh();
    sim_radio_cfg_t c = sim_radio_defaults();
    c.silent_boots = 1;
    sim_radio_config(&c);
    hal_radio_power(true);
    hal_delay_ms(600);
    rx_t f[4];
    CHECK_EQ(rx_frames(f, 4), 0);
    hal_radio_power(false);
    CHECK_EQ(sim_radio_info()->unclean_offs, 0); /* never said HELLO */
    hal_radio_power(true);
    hal_delay_ms(25);
    CHECK_EQ(rx_frames(f, 4), 2); /* HELLO, boot log */
    hal_radio_power(false); /* after HELLO, no SHUTDOWN */
    CHECK_EQ(sim_radio_info()->unclean_offs, 1);
}

TEST(masked_interrupts_and_clock_switch)
{
    fresh();
    hal_radio_power(true);
    hal_delay_ms(30);
    rx_t f[8];
    rx_frames(f, 8);
    hal_uart_stats_t st0, st;
    hal_uart_stats(&st0);
    /* a 30-byte log arrives while flash is written: only 4 bytes survive */
    sim_radio_log("0123456789012345678901234567");
    uint8_t blob[8] = {1};
    hal_nv_write(blob, sizeof blob);
    hal_delay_ms(2);
    hal_uart_stats(&st);
    CHECK(st.rx_overruns - st0.rx_overruns > 20);
    CHECK_EQ(rx_frames(f, 8), 0);
    /* a byte on the wire during a clock switch is corrupted */
    sim_radio_log("abc");
    hal_delay_us(30);
    hal_uart_stats(&st0);
    hal_clock_fast(true);
    hal_delay_ms(2);
    hal_uart_stats(&st);
    CHECK_EQ(st.rx_errors - st0.rx_errors, 1);
    CHECK_EQ(rx_frames(f, 8), 0);
    hal_clock_fast(false);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    RUN(power_order_and_trace);
    RUN(hello_after_boot_and_byte_timing);
    RUN(ctrl_ack_adv_connect);
    RUN(silent_and_unclean);
    RUN(masked_interrupts_and_clock_switch);
    return TEST_RESULT();
}
