/* Virtual time, scheduler, pins/rails, invariant checker, core HAL functions,
 * flash blob, debug channels. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_priv.h"

#define VIOL_MAX 256
#define VIOL_TEXT 240
#define WDT_US 2000000ull
#define NEVER UINT64_MAX

typedef struct {
    uint64_t t_us;
    uint64_t seq;
    sim_cb_t cb;
    void *ctx;
    bool owned; /* ctx is malloc'd by us */
} sched_t;

static struct {
    uint64_t now_us;
    uint64_t standby_total_us;
    int rtc_ppm;
    uint64_t rtc_k_next;
    bool in_standby, in_advance;
    bool pin[PIN_COUNT];
    bool led_blank, oled_attached, clock_fast;
    uint32_t irq_depth;
    uint32_t events;
    bool thumbs, finger, usb;
    sim_charge_t charge_mode;
    uint64_t chg_next_us;
    uint16_t battery_mv;
    int soc_override;
    uint64_t vled_rise_us;
    sched_t *q;
    size_t qn, qcap;
    uint64_t seq;
    bool stop;
    uint64_t stop_at_us;
    sim_exit_hook_t hook;
    void *hook_ctx;
    bool wdt_on;
    uint64_t wdt_deadline_us;
    bool reset_requested;
    int pending_cause; /* -1 none */
    bool cause_explicit;
    hal_reset_cause_t boot_cause, latched_cause;
    sim_facts_t facts;
    uint64_t rng;
} S;

static struct {
    sim_viol_mode_t mode;
    bool mode_init;
    size_t n;
    size_t per_id[SIM_V_COUNT];
    sim_viol_t id[VIOL_MAX];
    char text[VIOL_MAX][VIOL_TEXT];
} V;

static const board_pin_t guarded_pins[] = {PIN_MEASURE_REQ, PIN_FAST_RESTORE, PIN_GAIN_HI,
                                           PIN_RLD_L_EN, PIN_RLD_R_EN};
static const char *const pin_names[PIN_COUNT] = {
    "FAST_RESTORE", "MEASURE_REQ", "BUCK_PS_SYNC", "OLED_EN",  "VLED_EN",
    "ANA_EN",       "GAIN_HI",     "OLED_DC",      "OLED_CS",  "OLED_RESET",
    "RADIO_EN",     "RLD_L_EN",    "RLD_R_EN",     "CHG_PG",   "CHG_STAT1",
    "BUTTON1",      "BUTTON2",     "RADIO_IRQ",    "CW_ALRT"};

/* ------------------------------------------------------------------------ */
/* violations                                                                */

static void viol_mode_init(void)
{
    if (!V.mode_init) {
        const char *e = getenv("SIM_VIOLATIONS");
        V.mode = (e && strcmp(e, "collect") == 0) ? SIM_VIOL_COLLECT : SIM_VIOL_ABORT;
        V.mode_init = true;
    }
}

void sim_viol(sim_viol_t id, const char *fmt, ...)
{
    char msg[VIOL_TEXT - 40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    viol_mode_init();
    char full[VIOL_TEXT];
    snprintf(full, sizeof full, "[t=%llu.%03llu ms] %s", (unsigned long long)(S.now_us / 1000),
             (unsigned long long)(S.now_us % 1000), msg);
    V.per_id[id]++;
    if (V.n < VIOL_MAX) {
        V.id[V.n] = id;
        memcpy(V.text[V.n], full, sizeof full);
    }
    V.n++;
    if (V.mode == SIM_VIOL_ABORT) {
        fprintf(stderr, "SIM VIOLATION #%d: %s\n", (int)id, full);
        fflush(stdout);
        abort();
    }
}

void sim_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "SIM FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stdout);
    abort();
}

void sim_set_violation_mode(sim_viol_mode_t mode)
{
    viol_mode_init();
    V.mode = mode;
}
size_t sim_violation_count(void) { return V.n; }
const char *sim_violation_text(size_t i) { return i < V.n && i < VIOL_MAX ? V.text[i] : ""; }
sim_viol_t sim_violation_id(size_t i) { return i < V.n && i < VIOL_MAX ? V.id[i] : SIM_V_NONE; }
size_t sim_violation_count_of(sim_viol_t id) { return (unsigned)id < SIM_V_COUNT ? V.per_id[id] : 0; }
size_t sim_violation_count_containing(const char *sub)
{
    size_t c = 0;
    for (size_t i = 0; i < V.n && i < VIOL_MAX; i++)
        if (strstr(V.text[i], sub)) c++;
    return c;
}
void sim_violations_clear(void)
{
    V.n = 0;
    memset(V.per_id, 0, sizeof V.per_id);
}

/* ------------------------------------------------------------------------ */
/* random numbers                                                            */

uint64_t sim_rng_next(void)
{
    uint64_t x = S.rng ? S.rng : 0x9E3779B97F4A7C15ull;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    S.rng = x;
    return x;
}
double sim_gauss(void)
{
    double u1 = ((double)(sim_rng_next() >> 11) + 1.0) / 9007199254740994.0;
    double u2 = (double)(sim_rng_next() >> 11) / 9007199254740992.0;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* ------------------------------------------------------------------------ */
/* accessors                                                                 */

uint64_t sim_now_us(void) { return S.now_us; }
uint64_t sim_now_ms(void) { return S.now_us / 1000; }
bool sim_pin(board_pin_t p) { return (unsigned)p < PIN_COUNT ? S.pin[p] : false; }
bool sim_led_blank(void) { return S.led_blank; }
bool sim_oled_bus_attached(void) { return S.oled_attached; }
bool sim_clock_fast(void) { return S.clock_fast; }
bool sim_in_standby(void) { return S.in_standby; }
bool sim_usb_present(void) { return S.usb; }
bool sim_reset_requested(void) { return S.reset_requested; }
sim_facts_t *sim_facts_raw(void) { return &S.facts; }
const sim_facts_t *sim_facts(void)
{
    mcp_vref_track();
    return &S.facts;
}
uint16_t sim_battery_mv_get(void) { return S.battery_mv; }
int sim_soc_override_get(void) { return S.soc_override; }
bool sim_thumbs(void) { return S.thumbs; }
bool sim_finger(void) { return S.finger; }
bool sim_acq_running_any(void) { return an_running() != 0; }

bool sim_body_en(void)
{
    return S.pin[PIN_MEASURE_REQ] && S.pin[PIN_CHG_PG] && S.pin[PIN_ANA_EN];
}

bool sim_rail(sim_rail_t r)
{
    switch (r) {
    case SIM_RAIL_3V3A:
    case SIM_RAIL_3V3_SW: return S.pin[PIN_ANA_EN];
    case SIM_RAIL_OLED_VCC: return S.pin[PIN_OLED_EN];
    case SIM_RAIL_VLED: return S.pin[PIN_VLED_EN];
    case SIM_RAIL_RADIO_VCC: return S.pin[PIN_RADIO_EN];
    case SIM_RAIL_BODY_EN: return sim_body_en();
    }
    return false;
}

void sim_set_rtc_error_ppm(int ppm) { S.rtc_ppm = ppm; S.rtc_k_next = 0; }

/* ------------------------------------------------------------------------ */
/* RTC                                                                       */

static double rtc_scale(void) { return 1.0 + (double)S.rtc_ppm * 1e-6; }
static uint64_t rtc_seconds_now(void)
{
    return (uint64_t)floor((double)S.now_us * rtc_scale() / 1e6);
}
static uint64_t rtc_time_of(uint64_t k)
{
    return (uint64_t)ceil((double)k * 1e6 / rtc_scale());
}
static void rtc_resync(void) { S.rtc_k_next = rtc_seconds_now() + 1; }

/* ------------------------------------------------------------------------ */
/* scheduler                                                                 */

static void sched_insert(uint64_t t_us, sim_cb_t cb, void *ctx, bool owned)
{
    if (!cb) {
        if (owned) free(ctx);
        sim_viol(SIM_V_API, "sim_at with NULL callback");
        return;
    }
    if (S.qn == S.qcap) {
        S.qcap = S.qcap ? S.qcap * 2 : 64;
        S.q = realloc(S.q, S.qcap * sizeof *S.q);
        if (!S.q) sim_fatal("out of memory");
    }
    size_t i = S.qn;
    while (i > 0 && S.q[i - 1].t_us > t_us) {
        S.q[i] = S.q[i - 1];
        i--;
    }
    S.q[i] = (sched_t){t_us, S.seq++, cb, ctx, owned};
    S.qn++;
}

void sim_at_us(uint64_t t_us, sim_cb_t cb, void *ctx) { sched_insert(t_us, cb, ctx, false); }
void sim_at(uint64_t ms, sim_cb_t cb, void *ctx) { sched_insert(ms * 1000, cb, ctx, false); }
void sim_after_ms(uint64_t d, sim_cb_t cb, void *ctx)
{
    sched_insert(S.now_us + d * 1000, cb, ctx, false);
}
size_t sim_scheduled_count(void) { return S.qn; }
void sim_clear_scheduled(void)
{
    for (size_t i = 0; i < S.qn; i++)
        if (S.q[i].owned) free(S.q[i].ctx);
    S.qn = 0;
}

typedef struct {
    int a;
    bool b;
    uint32_t v;
} canned_t;
static void canned_button(void *c) { sim_set_button(((canned_t *)c)->a, ((canned_t *)c)->b); }
static void canned_usb(void *c) { sim_set_usb(((canned_t *)c)->b); }
static void canned_thumbs(void *c) { sim_set_thumbs(((canned_t *)c)->b); }
static void canned_finger(void *c) { sim_set_finger(((canned_t *)c)->b); }
static void canned_batt(void *c) { sim_set_battery_mv((uint16_t)((canned_t *)c)->v); }
static void canned_adjust(void *c)
{
    sim_ecg_thumb_adjust((int32_t)((canned_t *)c)->v, (unsigned)((canned_t *)c)->a);
}
static void canned(uint64_t ms, sim_cb_t cb, int a, bool b, uint32_t v)
{
    canned_t *c = malloc(sizeof *c);
    if (!c) sim_fatal("out of memory");
    *c = (canned_t){a, b, v};
    sched_insert(ms * 1000, cb, c, true);
}
void sim_at_button(uint64_t ms, int btn, bool pressed) { canned(ms, canned_button, btn, pressed, 0); }
void sim_at_usb(uint64_t ms, bool present) { canned(ms, canned_usb, 0, present, 0); }
void sim_at_thumbs(uint64_t ms, bool on) { canned(ms, canned_thumbs, 0, on, 0); }
void sim_at_finger(uint64_t ms, bool on) { canned(ms, canned_finger, 0, on, 0); }
void sim_at_battery_mv(uint64_t ms, uint16_t mv) { canned(ms, canned_batt, 0, false, mv); }
void sim_at_thumb_adjust(uint64_t ms, int32_t step_uv, unsigned ramp_ms)
{
    canned(ms, canned_adjust, (int)ramp_ms, false, (uint32_t)step_uv);
}

/* ------------------------------------------------------------------------ */
/* run control                                                               */

void sim_stop_at(uint64_t ms) { S.stop_at_us = ms * 1000; }
void sim_stop(void) { S.stop = true; }
void sim_clear_stop(void) { S.stop = false; }
bool sim_should_stop(void) { return S.stop; }
void sim_set_exit_hook(sim_exit_hook_t h, void *ctx) { S.hook = h; S.hook_ctx = ctx; }

static void call_hook(sim_exit_reason_t r)
{
    S.stop = true;
    if (S.hook) S.hook(r, S.hook_ctx);
}

/* ------------------------------------------------------------------------ */
/* pins                                                                      */

static void pin_set(board_pin_t pin, bool high);

static void update_stat1(void)
{
    if (!S.usb) {
        S.chg_next_us = NEVER;
        sim_input_set(PIN_CHG_STAT1, true);
        return;
    }
    switch (S.charge_mode) {
    case SIM_CHG_CHARGING: S.chg_next_us = NEVER; sim_input_set(PIN_CHG_STAT1, false); break;
    case SIM_CHG_DONE: S.chg_next_us = NEVER; sim_input_set(PIN_CHG_STAT1, true); break;
    case SIM_CHG_FAULT:
        if (S.chg_next_us == NEVER) S.chg_next_us = S.now_us + 250000;
        break;
    }
}

void sim_input_set(board_pin_t pin, bool level)
{
    bool old = S.pin[pin];
    if (old == level) return;
    oled_progress();
    an_sync();
    S.pin[pin] = level;
    an_pin_changed(pin, old, level);
    switch (pin) {
    case PIN_BUTTON1:
    case PIN_BUTTON2: S.events |= HAL_EV_BUTTON; break;
    case PIN_CHG_PG:
        if (!level) {
            pin_set(PIN_MEASURE_REQ, false); /* safety ISR */
            S.events |= HAL_EV_USB_PLUGGED;
        } else {
            S.events |= HAL_EV_USB_REMOVED;
        }
        break;
    case PIN_CHG_STAT1: S.events |= HAL_EV_CHG_STAT; break;
    case PIN_CW_ALRT:
        if (!level) S.events |= HAL_EV_CW_ALRT;
        break;
    case PIN_RADIO_IRQ:
        if (level) S.events |= HAL_EV_RADIO_IRQ;
        break;
    default: break;
    }
}

void sim_set_button(int b, bool pressed)
{
    if (b != 1 && b != 2) {
        sim_viol(SIM_V_API, "sim_set_button: bad button %d", b);
        return;
    }
    sim_input_set(b == 1 ? PIN_BUTTON1 : PIN_BUTTON2, !pressed);
}
void sim_set_usb(bool present)
{
    S.usb = present;
    sim_input_set(PIN_CHG_PG, !present);
    update_stat1();
}
void sim_set_charge_mode(sim_charge_t m)
{
    S.charge_mode = m;
    S.chg_next_us = NEVER;
    update_stat1();
}
void sim_set_radio_irq(bool level) { sim_input_set(PIN_RADIO_IRQ, level); }
void sim_set_battery_mv(uint16_t mv) { S.battery_mv = mv; cw_battery_changed(); }
void sim_set_battery_soc(int pct) { S.soc_override = pct > 100 ? 100 : pct; cw_battery_changed(); }
void sim_set_thumbs(bool on)
{
    an_sync();
    S.thumbs = on;
    an_world_changed();
}
void sim_set_finger(bool on)
{
    an_sync();
    S.finger = on;
    an_world_changed();
}

void sim_core_set_led_blank(bool level) { S.led_blank = level; }
uint64_t sim_vled_rise_us(void) { return S.vled_rise_us; }
void sim_core_set_oled_attached(bool a) { S.oled_attached = a; }

static void pin_set(board_pin_t pin, bool high)
{
    if (S.pin[pin] == high) return;
    oled_progress();
    an_sync();
    /* checks that must see the state before the change */
    if (pin == PIN_ANA_EN && !high) {
        for (size_t i = 0; i < sizeof guarded_pins / sizeof guarded_pins[0]; i++)
            if (S.pin[guarded_pins[i]])
                sim_viol(SIM_V_GUARDED_PIN_ANA_LOW, "%s still high when ANA_EN fell",
                         pin_names[guarded_pins[i]]);
        if (sim_acq_running_any())
            sim_viol(SIM_V_ANA_FALL_ACQ_RUNNING, "ANA_EN fell while acquisition runs (0x%x)",
                     an_running());
        if (!S.led_blank)
            sim_viol(SIM_V_ANA_FALL_LED_BLANK_LOW, "ANA_EN fell while LED_BLANK is low");
        bool act;
        double mv;
        mcp_a_state(&act, &mv);
        if (act && mv > 0.0)
            sim_viol(SIM_V_ANA_FALL_DAC_ACTIVE,
                     "ANA_EN fell with MCP4728 channel A still driving GREEN_SET (%.0f mV)", mv);
    }
    if (pin == PIN_VLED_EN && !S.led_blank)
        sim_viol(SIM_V_VLED_CHANGE_NOT_BLANKED, "VLED_EN changed while LED_BLANK is low");
    if (pin == PIN_OLED_EN && !high) {
        if (S.oled_attached)
            sim_viol(SIM_V_OLED_SIGNAL_UNPOWERED, "OLED_EN fell with the SPI bus attached");
        const board_pin_t sig[] = {PIN_OLED_DC, PIN_OLED_CS, PIN_OLED_RESET};
        for (size_t i = 0; i < 3; i++)
            if (S.pin[sig[i]])
                sim_viol(SIM_V_OLED_SIGNAL_UNPOWERED, "%s high when OLED_EN fell", pin_names[sig[i]]);
        if (oled_dma_active())
            sim_viol(SIM_V_OLED_DMA_MISUSE, "OLED_EN fell during a DMA transfer");
    }

    bool old = S.pin[pin];
    S.pin[pin] = high;
    if (pin == PIN_VLED_EN && high) S.vled_rise_us = S.now_us;

    /* checks on the new state */
    if (high) {
        for (size_t i = 0; i < sizeof guarded_pins / sizeof guarded_pins[0]; i++)
            if (guarded_pins[i] == pin && !S.pin[PIN_ANA_EN])
                sim_viol(SIM_V_GUARDED_PIN_ANA_LOW, "%s raised while ANA_EN is low", pin_names[pin]);
        if ((pin == PIN_OLED_DC || pin == PIN_OLED_CS || pin == PIN_OLED_RESET) &&
            !S.pin[PIN_OLED_EN])
            sim_viol(SIM_V_OLED_SIGNAL_UNPOWERED, "%s raised while OLED_EN is low", pin_names[pin]);
        if (pin == PIN_RADIO_EN)
            sim_viol(SIM_V_RADIO_EN, "RADIO_EN driven high (radio must stay off)");
    }
    an_pin_changed(pin, old, high);
    oled_pin_changed(pin, old, high);
    if (pin == PIN_VLED_EN) mcp_vref_track();
}

void hal_pin_write(board_pin_t pin, bool high)
{
    if ((unsigned)pin >= PIN_FIRST_INPUT) return;
    sim_service();
    pin_set(pin, high);
}

bool hal_pin_read(board_pin_t pin)
{
    if ((unsigned)pin >= PIN_COUNT) return false;
    sim_service();
    return S.pin[pin];
}

/* ------------------------------------------------------------------------ */
/* time advance                                                              */

static uint64_t next_internal(void)
{
    uint64_t t = NEVER;
    if (S.qn) t = S.q[0].t_us;
    if (S.chg_next_us < t) t = S.chg_next_us;
    if (S.stop_at_us < t) t = S.stop_at_us;
    if (!S.in_standby) {
        if (S.rtc_k_next == 0) rtc_resync();
        uint64_t r = rtc_time_of(S.rtc_k_next);
        if (r < t) t = r;
        if (S.wdt_on && S.wdt_deadline_us < t) t = S.wdt_deadline_us;
        uint64_t a = an_next_event_us();
        if (a < t) t = a;
    }
    return t;
}

static void run_events_at(uint64_t tn)
{
    while (S.qn && S.q[0].t_us <= tn) {
        sched_t e = S.q[0];
        memmove(&S.q[0], &S.q[1], (S.qn - 1) * sizeof S.q[0]);
        S.qn--;
        e.cb(e.ctx);
        if (e.owned) free(e.ctx);
    }
    if (S.chg_next_us <= tn) {
        S.chg_next_us = tn + 250000;
        sim_input_set(PIN_CHG_STAT1, !S.pin[PIN_CHG_STAT1]);
    }
    if (!S.in_standby) {
        if (rtc_time_of(S.rtc_k_next) <= tn) {
            S.rtc_k_next++;
            S.events |= HAL_EV_RTC;
        }
        if (an_next_event_us() <= tn) an_run_events(tn);
        if (S.wdt_on && S.wdt_deadline_us <= tn) {
            sim_viol(SIM_V_WATCHDOG, "watchdog expired: no hal_wdt_kick() for 2 s");
            S.facts.wdt_resets++;
            S.pending_cause = HAL_RESET_WATCHDOG;
            S.reset_requested = true;
            S.wdt_deadline_us = tn + WDT_US;
            call_hook(SIM_EXIT_WATCHDOG);
        }
    }
    if (S.stop_at_us <= tn) {
        S.stop_at_us = NEVER;
        call_hook(SIM_EXIT_STOP_AT);
    }
}

static void advance_to(uint64_t target)
{
    if (S.in_advance) sim_fatal("time-advancing HAL call made from a scripted callback");
    S.in_advance = true;
    for (;;) {
        uint64_t tn = next_internal();
        if (tn > target) break;
        if (tn < S.now_us) tn = S.now_us;
        S.now_us = tn;
        run_events_at(tn);
    }
    if (target > S.now_us) S.now_us = target;
    oled_progress();
    S.in_advance = false;
}

void sim_advance_us(uint64_t dt) { advance_to(S.now_us + dt); }
void sim_service(void)
{
    if (!S.in_advance) advance_to(S.now_us);
}

/* ------------------------------------------------------------------------ */
/* HAL core                                                                  */

static void mcu_pins_reset(void)
{
    S.led_blank = true;
    for (int p = 0; p < PIN_FIRST_INPUT; p++) {
        if (S.pin[p]) {
            oled_progress();
            an_sync();
            S.pin[p] = false;
            an_pin_changed((board_pin_t)p, true, false);
            oled_pin_changed((board_pin_t)p, true, false);
        }
    }
    S.oled_attached = false;
    S.clock_fast = false;
    S.irq_depth = 0;
    S.events = 0;
    S.in_standby = false;
    S.wdt_on = false;
    an_mcu_reset();
    oled_mcu_reset();
    mcp_vref_track(); /* VLED_EN went low; the MCP4728 keeps its registers */
}

void sim_reboot(void)
{
    if (S.pending_cause >= 0 && !S.cause_explicit) S.boot_cause = (hal_reset_cause_t)S.pending_cause;
    else if (!S.cause_explicit) S.boot_cause = HAL_RESET_PIN;
    S.pending_cause = -1;
    S.cause_explicit = false;
    S.reset_requested = false;
    S.stop = false;
    S.in_advance = false;
    mcu_pins_reset();
}

void sim_set_reset_cause(hal_reset_cause_t c)
{
    S.boot_cause = c;
    S.cause_explicit = true;
}

void hal_init(void)
{
    mcu_pins_reset();
    S.latched_cause = S.boot_cause;
    S.wdt_on = true;
    S.wdt_deadline_us = S.now_us + WDT_US;
    sim_service();
}

hal_reset_cause_t hal_reset_cause(void) { return S.latched_cause; }

void hal_wdt_kick(void)
{
    S.facts.wdt_kicks++;
    S.wdt_deadline_us = S.now_us + WDT_US;
}

void hal_system_reset(void)
{
    S.facts.system_resets++;
    S.pending_cause = HAL_RESET_SOFTWARE;
    S.reset_requested = true;
    call_hook(SIM_EXIT_SYSTEM_RESET);
}

uint32_t hal_irq_lock(void) { return S.irq_depth++; }
void hal_irq_unlock(uint32_t state)
{
    S.irq_depth = state;
}

uint32_t hal_millis(void)
{
    sim_service();
    return (uint32_t)((S.now_us - S.standby_total_us) / 1000);
}
uint32_t hal_rtc_seconds(void)
{
    sim_service();
    return (uint32_t)rtc_seconds_now();
}
void hal_delay_ms(uint32_t ms) { advance_to(S.now_us + (uint64_t)ms * 1000); }
void hal_delay_us(uint32_t us) { advance_to(S.now_us + us); }

void hal_clock_fast(bool fast)
{
    if (an_running())
        sim_viol(SIM_V_CLOCK_WHILE_ACQ, "hal_clock_fast(%d) while acquisition runs", fast);
    S.clock_fast = fast;
}

uint32_t hal_events_take(void)
{
    sim_service();
    uint32_t e = S.events;
    S.events = 0;
    return e;
}

void hal_idle(void)
{
    sim_service();
    uint64_t ms = (S.now_us - S.standby_total_us) / 1000;
    uint64_t target = S.standby_total_us + (ms + 1) * 1000;
    if (S.qn && S.q[0].t_us < target) target = S.q[0].t_us;
    if (target < S.now_us) target = S.now_us;
    advance_to(target);
}

uint32_t hal_standby(uint32_t wake_after_s)
{
    sim_service();
    S.facts.standby_entries++;
    if (S.pin[PIN_ANA_EN]) sim_viol(SIM_V_STANDBY_PRECOND, "standby with ANA_EN on");
    if (S.pin[PIN_OLED_EN]) sim_viol(SIM_V_STANDBY_PRECOND, "standby with OLED_EN on");
    if (S.pin[PIN_VLED_EN]) sim_viol(SIM_V_STANDBY_PRECOND, "standby with VLED_EN on");
    if (S.pin[PIN_RADIO_EN]) sim_viol(SIM_V_STANDBY_PRECOND, "standby with RADIO_EN on");
    if (S.pin[PIN_BUCK_PS_SYNC])
        sim_viol(SIM_V_STANDBY_PRECOND, "standby with BUCK_PS_SYNC high (forced PWM)");
    if (an_running()) sim_viol(SIM_V_STANDBY_PRECOND, "standby with acquisition running");
    if (an_ppg_analog_on()) sim_viol(SIM_V_STANDBY_PRECOND, "standby with PPG analog blocks on");
    if (S.oled_attached) sim_viol(SIM_V_STANDBY_PRECOND, "standby with OLED bus attached");
    if (S.clock_fast) sim_viol(SIM_V_STANDBY_PRECOND, "standby with fast MCLK");
    /* Not a violation (a dead I2C bus can leave it on), a fact for tests:
     * ~45 uA on the always-on rail (DS22187E IPD_INT). */
    if (sim_mcp_int_ref_on()) S.facts.mcp_vref_int_standby++;

    S.events &= ~(uint32_t)HAL_EV_RTC; /* periodic awake tick is not a standby wake source */
    /* As in the BSP: CHG_STAT1 is masked in STANDBY (a 2 Hz charger-fault
     * toggle would keep the MCU awake); an edge stays latched and is
     * reported with the next real wake. */
    const uint32_t wake_mask = ~(uint32_t)(HAL_EV_RTC | HAL_EV_CHG_STAT);
    if (S.events & wake_mask) return S.events;

    uint64_t t_enter = S.now_us;
    uint64_t rtc_deadline = NEVER;
    if (wake_after_s)
        rtc_deadline = S.now_us + (uint64_t)((double)wake_after_s * 1e6 / rtc_scale());
    S.in_standby = true;
    uint32_t cause = 0;
    for (;;) {
        uint64_t tn = next_internal();
        if (rtc_deadline < tn) tn = rtc_deadline;
        if (tn == NEVER) {
            S.in_standby = false; /* leave consistent for the hook */
            if (S.hook) {
                call_hook(SIM_EXIT_STANDBY_DEADLOCK);
            } else {
                S.stop = true;
                sim_viol(SIM_V_STANDBY_DEADLOCK,
                         "hal_standby(%u): nothing can ever wake the MCU (no scripted event, "
                         "no RTC timeout, no stop time)",
                         (unsigned)wake_after_s);
            }
            break;
        }
        advance_to(tn < S.now_us ? S.now_us : tn);
        if (S.events & wake_mask) {
            cause = S.events;
            break;
        }
        if (tn >= rtc_deadline) {
            S.events |= HAL_EV_RTC;
            cause = HAL_EV_RTC;
            break;
        }
        if (S.stop) break;
    }
    S.in_standby = false;
    uint64_t slept = S.now_us - t_enter;
    S.standby_total_us += slept;
    S.facts.standby_total_us += slept;
    if (S.wdt_on) S.wdt_deadline_us += slept;
    rtc_resync();
    return cause;
}

/* ------------------------------------------------------------------------ */
/* non-volatile blob                                                         */

static struct {
    uint8_t data[HAL_NV_MAX];
    bool fail;
    hal_status_t fail_status;
} NV;

void hal_nv_read(void *dst, size_t n)
{
    size_t m = n < HAL_NV_MAX ? n : HAL_NV_MAX;
    memcpy(dst, NV.data, m);
    if (n > m) memset((uint8_t *)dst + m, 0xFF, n - m);
}

hal_status_t hal_nv_write(const void *src, size_t n)
{
    if (n > HAL_NV_MAX || (!src && n)) return HAL_ERR_PARAM;
    if (NV.fail) {
        NV.fail = false;
        advance_to(S.now_us + 1000);
        return NV.fail_status;
    }
    S.facts.nv_writes++;
    S.facts.nv_erase_cycles++;
    memset(NV.data, 0xFF, sizeof NV.data);
    if (n) memcpy(NV.data, src, n);
    advance_to(S.now_us + 15000); /* sector erase + program; CPU stalls */
    return HAL_OK;
}

void sim_nv_set(const void *d, size_t n)
{
    memset(NV.data, 0xFF, sizeof NV.data);
    memcpy(NV.data, d, n < HAL_NV_MAX ? n : HAL_NV_MAX);
}
void sim_nv_get(void *dst, size_t n) { hal_nv_read(dst, n); }
void sim_nv_corrupt(size_t off, uint8_t m)
{
    if (off < HAL_NV_MAX) NV.data[off] ^= m;
}
void sim_nv_erase(void) { memset(NV.data, 0xFF, sizeof NV.data); }
uint32_t sim_nv_erase_count(void) { return S.facts.nv_erase_cycles; }
void sim_nv_fail_next_write(hal_status_t st) { NV.fail = true; NV.fail_status = st; }

/* ------------------------------------------------------------------------ */
/* debug channels                                                            */

static struct {
    char *log;
    size_t log_n, log_cap;
    bool echo, echo_init;
    uint8_t *st;
    size_t st_n, st_cap;
    FILE *fp;
    bool fp_tried;
} D;

size_t hal_log_write(const char *s, size_t n)
{
    if (!D.echo_init) {
        const char *e = getenv("SIM_LOG_ECHO");
        if (e && *e && *e != '0') D.echo = true;
        D.echo_init = true;
    }
    if (D.log_n + n + 1 > D.log_cap) {
        D.log_cap = (D.log_n + n + 1) * 2;
        D.log = realloc(D.log, D.log_cap);
        if (!D.log) sim_fatal("out of memory");
    }
    memcpy(D.log + D.log_n, s, n);
    D.log_n += n;
    D.log[D.log_n] = 0;
    if (D.echo) fwrite(s, 1, n, stdout);
    return n;
}
const char *sim_log_text(void) { return D.log ? D.log : ""; }
size_t sim_log_len(void) { return D.log_n; }
void sim_log_clear(void)
{
    D.log_n = 0;
    if (D.log) D.log[0] = 0;
}
void sim_set_log_echo(bool on) { D.echo = on; D.echo_init = true; }

size_t hal_stream_write(const uint8_t *buf, size_t n)
{
    if (D.st_n + n > D.st_cap) {
        D.st_cap = (D.st_n + n) * 2 + 4096;
        D.st = realloc(D.st, D.st_cap);
        if (!D.st) sim_fatal("out of memory");
    }
    memcpy(D.st + D.st_n, buf, n);
    D.st_n += n;
    if (!D.fp_tried) {
        D.fp_tried = true;
        const char *path = getenv("SIM_STREAM_FILE");
        if (path && *path) D.fp = fopen(path, "ab");
    }
    if (D.fp) {
        fwrite(buf, 1, n, D.fp);
        fflush(D.fp);
    }
    return n;
}
const uint8_t *sim_stream_data(size_t *len)
{
    if (len) *len = D.st_n;
    return D.st;
}
void sim_stream_clear(void) { D.st_n = 0; }
bool sim_stream_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = D.st_n ? fwrite(D.st, 1, D.st_n, f) : 0;
    fclose(f);
    return w == D.st_n;
}

/* ------------------------------------------------------------------------ */
/* full reset                                                                */

void sim_reset(void)
{
    sim_clear_scheduled();
    sched_t *q = S.q;
    size_t qcap = S.qcap;
    uint64_t seq = S.seq;
    memset(&S, 0, sizeof S);
    S.q = q;
    S.qcap = qcap;
    S.seq = seq;
    S.rng = 0x1234567887654321ull;
    S.pin[PIN_CHG_PG] = true;
    S.pin[PIN_CHG_STAT1] = true;
    S.pin[PIN_BUTTON1] = true;
    S.pin[PIN_BUTTON2] = true;
    S.pin[PIN_CW_ALRT] = true;
    S.led_blank = true;
    S.charge_mode = SIM_CHG_CHARGING;
    S.chg_next_us = NEVER;
    S.stop_at_us = NEVER;
    S.battery_mv = 3900;
    S.soc_override = -1;
    S.pending_cause = -1;
    S.boot_cause = HAL_RESET_POWER_ON;
    S.latched_cause = HAL_RESET_POWER_ON;
    viol_mode_init();
    V.n = 0;
    memset(V.per_id, 0, sizeof V.per_id);
    memset(NV.data, 0xFF, sizeof NV.data);
    NV.fail = false;
    sim_log_clear();
    sim_stream_clear();
    if (D.fp) {
        fclose(D.fp);
        D.fp = NULL;
    }
    D.fp_tried = false;
    i2c_reset();
    oled_reset();
    an_reset();
    cw_power_on();
}
