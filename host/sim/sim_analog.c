/* Analog world (ECG chain, PPG chain, VMID) and the acquisition engine. */
#include <math.h>
#include <string.h>

#include "sim_priv.h"

#define PI 3.14159265358979323846
#define TAU1 0.5                    /* s: first-stage AC coupling, 0.32 Hz */
#define STAGE1_GAIN 21.0            /* U5 for changes; 1 at DC */
#define STAGE1_SWING 1.55           /* V: U5 output clips at VMID +- this */
#define TAU2 (1.0 / (2.0 * PI * 0.48)) /* s: C34/R36 high-pass node */
#define TAU_L1 (1.0 / (2.0 * PI * 159.0))
#define TAU_L2 (1.0 / (2.0 * PI * 339.0))
#define VMID_TAU 0.05
#define RAIL_LO 0.05
#define RAIL_HI 3.25
#define VDDA 3.3
#define PD_BIAS_V ((double)PPG_PD_BIAS_DAC8 / 256.0 * VDDA)
#define NEVER UINT64_MAX

static const uint32_t phase_off_us[4] = {PPG_T_DARK_US, PPG_T_LED_ON_US, PPG_T_ON_SAMPLE_US,
                                         PPG_FRAME_US};

static struct {
    /* configuration */
    sim_ecg_fn ecg_fn;
    void *ecg_ctx;
    sim_ecg_fn out_fn;              /* replay: ECG_ADC directly (bypasses the chain) */
    void *out_ctx;
    sim_ppg_fn ppg_fn;
    void *ppg_ctx;
    double contact_off_uv, contact_tr_uv, mains_hz, mains_uv, noise_uv;
    int vmid_err_mv;
    bool float_high;
    sim_float_model_t float_model;
    double ex_rate;                 /* floating excursions: starts per second */
    unsigned ex_min_ms, ex_max_ms;
    uint64_t ex_rng;
    uint64_t ex_start, ex_end;      /* current / next excursion, us */
    double ex_level;                /* V */
    double inj_gain_mv, inj_rld_mv, adc_noise;
    double amb_dc, amb_fl, amb_hz;
    /* ECG model state */
    uint64_t t_model;
    double s1, s2, l1, l2;
    double e_off_uv;                /* electrode offset now (contact + adjustments) */
    double ramp_uv_per_us;          /* thumb adjustment in progress */
    uint64_t ramp_end;
    double vmid_v0;
    uint64_t vmid_t0;
    /* acquisition */
    unsigned running;
    uint64_t t_start, ecg_n, ppg_k;
    int ppg_phase;
    /* Rings indexed by timeline position (hal.h): slot = index % size. The
     * capacity actually usable depends on the ring model. */
    uint16_t ecg_ring[SIM_BSP_ECG_RING_SAMPLES];
    bool ecg_valid[SIM_BSP_ECG_RING_SAMPLES];  /* false: conversion missed */
    uint64_t ecg_rd;                           /* next index to hand out */
    uint32_t ecg_delivered, ecg_seen_end;
    hal_ppg_frame_t ppg_ring[SIM_PPG_RING_FRAMES];
    bool ppg_valid[SIM_PPG_RING_FRAMES];
    uint64_t ppg_done, ppg_rd;                 /* frames completed / next to hand out */
    uint32_t ppg_delivered, ppg_seen_end;
    sim_ring_model_t ring_model;               /* kept across sim_reboot */
    uint32_t miss_ecg, miss_ppg;               /* kept across sim_reboot */
    bool ppg_analog;
    uint16_t dac12;
    uint16_t tia_dark, hi_dark;
} A;

uint64_t sim_vled_rise_us(void);

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

static bool ana(void) { return sim_pin(PIN_ANA_EN); }
static bool contact(void) { return sim_thumbs() && sim_body_en(); }
static double gain(void) { return sim_pin(PIN_GAIN_HI) ? (double)ECG_GAIN_HI : (double)ECG_GAIN_LO; }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint16_t to_code(double v, double noise_lsb)
{
    double c = floor(v / VDDA * (double)ADC_FULL_SCALE + 0.5 + (noise_lsb > 0 ? noise_lsb * sim_gauss() : 0.0));
    return (uint16_t)clampd(c, 0.0, 4095.0);
}

static double vmid_with(uint64_t t, bool ana_flag)
{
    double tgt = ana_flag ? 1.65 + (double)A.vmid_err_mv / 1000.0 : 0.0;
    double dt = (double)(t - A.vmid_t0) * 1e-6;
    return tgt + (A.vmid_v0 - tgt) * exp(-dt / VMID_TAU);
}
static double vmid_at(uint64_t t) { return vmid_with(t, ana()); }
static void vmid_anchor(bool ana_flag_before)
{
    uint64_t now = sim_now_us();
    A.vmid_v0 = vmid_with(now, ana_flag_before);
    A.vmid_t0 = now;
}

double sim_vmid_mv(void) { return vmid_at(sim_now_us()) * 1000.0; }

/* No contact: the chain rests at VMID and the next contact steps the
 * electrode offset from 0 to contact_tr_uv. */
static void model_zero(void)
{
    A.s1 = A.s2 = A.l1 = A.l2 = 0.0;
    A.e_off_uv = A.contact_tr_uv;
    A.ramp_uv_per_us = 0.0;
    A.ramp_end = 0;
    A.t_model = sim_now_us();
}

static double rld_factor(void)
{
    int n = (sim_pin(PIN_RLD_L_EN) ? 1 : 0) + (sim_pin(PIN_RLD_R_EN) ? 1 : 0);
    return n == 2 ? 1.0 : n == 1 ? 3.0 : 10.0;
}

static void ecg_step(uint64_t t_end, uint64_t dt, bool coarse)
{
    if (A.ramp_end) {
        uint64_t t0 = t_end - dt;
        uint64_t te = t_end < A.ramp_end ? t_end : A.ramp_end;
        if (te > t0) A.e_off_uv += A.ramp_uv_per_us * (double)(te - t0);
        if (t_end >= A.ramp_end) A.ramp_end = 0;
    }
    double sig = 0.0;
    if (!coarse) {
        if (A.ecg_fn) sig += (double)A.ecg_fn(t_end, A.ecg_ctx);
        if (A.mains_uv != 0.0)
            sig += A.mains_uv * rld_factor() * sin(2.0 * PI * A.mains_hz * (double)t_end * 1e-6);
        if (A.noise_uv != 0.0) sig += A.noise_uv * sim_gauss();
    }
    double v = 1e-6 * (sig + A.e_off_uv); /* electrodes: signal + electrode offset */
    double d = (double)dt * 1e-6;
    A.s1 += (v - A.s1) * -expm1(-d / TAU1);
    /* U5: 1 + 20 * HP(v) (the gain network's AC coupling), clipped */
    double y1 = clampd(v + (STAGE1_GAIN - 1.0) * (v - A.s1), -STAGE1_SWING, STAGE1_SWING);
    y1 += A.contact_off_uv * 1e-6; /* + DC offset at the HP node (unity DC gain) */
    if (sim_pin(PIN_FAST_RESTORE)) A.s2 = y1;
    else A.s2 += (y1 - A.s2) * -expm1(-d / TAU2);
    double y2 = y1 - A.s2;
    double vm = vmid_at(t_end);
    double u = clampd(gain() / 21.0 * y2, RAIL_LO - vm, RAIL_HI - vm);
    A.l1 += (u - A.l1) * -expm1(-d / TAU_L1);
    A.l2 += (A.l1 - A.l2) * -expm1(-d / TAU_L2);
}

static void model_to(uint64_t t)
{
    if (t <= A.t_model) return;
    if (!ana() || !contact()) {
        model_zero();
        A.t_model = t;
        return;
    }
    while (A.t_model < t) {
        uint64_t rem = t - A.t_model;
        bool coarse = rem > 500000;
        uint64_t dt = coarse ? 20000 : (rem > 250 ? 250 : rem);
        ecg_step(A.t_model + dt, dt, coarse);
        A.t_model += dt;
    }
}

/* splitmix64: any seed (also 0 or 1) gives a well-mixed sequence */
static double ex_uniform(void)
{
    uint64_t z = (A.ex_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return ((double)(z >> 11) + 0.5) / 9007199254740992.0; /* (0, 1) */
}

/* Floating-input excursions: a renewal process on the absolute time axis,
 * drawn in time order, so it does not depend on who samples when. */
static void ex_advance(uint64_t t)
{
    while (t >= A.ex_end) {
        double gap_s = A.ex_rate > 0.0 ? -log(ex_uniform()) / A.ex_rate : 1e9;
        double u = ex_uniform();
        double dur_ms = A.ex_min_ms + (A.ex_max_ms - A.ex_min_ms) * u * u * u; /* mostly short */
        A.ex_start = A.ex_end + (uint64_t)(gap_s * 1e6);
        A.ex_end = A.ex_start + (uint64_t)(dur_ms * 1000.0) + 1u;
        A.ex_level = VDDA * (0.25 + 0.5 * ex_uniform());
    }
}

static double float_v(uint64_t t)
{
    double ts = (double)t * 1e-6;
    double w = 0.06 * (0.5 + 0.5 * sin(2.0 * PI * 0.3 * ts));
    double rail = A.float_high ? RAIL_HI - w : RAIL_LO + w;
    switch (A.float_model) {
    case SIM_FLOAT_MID: return vmid_at(t) + 0.004 * sin(2.0 * PI * 0.3 * ts);
    case SIM_FLOAT_RAIL: return rail;
    default:
        ex_advance(t);
        if (t >= A.ex_start && t < A.ex_end) return A.ex_level + 0.01 * sin(2.0 * PI * 50.0 * ts);
        return rail;
    }
}

static double ecg_out_v_at(uint64_t t)
{
    if (!ana()) return 0.0;
    double vm = vmid_at(t);
    if (!contact()) return sim_pin(PIN_FAST_RESTORE) ? vm : float_v(t);
    if (A.out_fn) {
        double gain_ratio = sim_pin(PIN_GAIN_HI) ? (double)ECG_GAIN_HI / ECG_GAIN_LO : 1.0;
        return clampd(vm + 1e-6 * gain_ratio * A.out_fn(t, A.out_ctx), RAIL_LO, RAIL_HI);
    }
    model_to(t);
    return clampd(vm + A.l2, RAIL_LO, RAIL_HI);
}

double sim_ecg_out_v(void) { return ecg_out_v_at(sim_now_us()); }

static double ambient_pa(uint64_t t)
{
    return A.amb_dc + A.amb_fl * sin(2.0 * PI * A.amb_hz * (double)t * 1e-6);
}

static double tia_v_at(uint64_t t, double led_ma)
{
    if (!A.ppg_analog) return 0.0;
    double i_pa = ambient_pa(t);
    if (led_ma > 0.0 && sim_finger() && A.ppg_fn) i_pa += (double)A.ppg_fn(t, A.ppg_ctx) * led_ma;
    return clampd(PD_BIAS_V + i_pa * 1e-6, 0.0, VDDA);
}
static double hi_v_from(double tia)
{
    if (!A.ppg_analog) return 0.0;
    return clampd(11.0 * ((double)A.dac12 / 4096.0 * VDDA) - 10.0 * tia, 0.0, VDDA);
}
double sim_ppg_tia_v(void) { return tia_v_at(sim_now_us(), sim_led_now_ma()); }
double sim_ppg_hi_v(void) { return hi_v_from(sim_ppg_tia_v()); }

/* ------------------------------------------------------------------------ */
/* configuration                                                             */

void sim_set_ecg_source(sim_ecg_fn fn, void *ctx) { A.ecg_fn = fn; A.ecg_ctx = ctx; }
void sim_set_ecg_output(sim_ecg_fn fn, void *ctx) { A.out_fn = fn; A.out_ctx = ctx; }
void sim_set_ppg_source(sim_ppg_fn fn, void *ctx) { A.ppg_fn = fn; A.ppg_ctx = ctx; }
void sim_ecg_set_contact_offset_uv(int32_t uv) { an_sync(); A.contact_off_uv = (double)uv; }
void sim_ecg_set_contact_transient_uv(int32_t uv)
{
    an_sync();
    if (ana() && contact()) A.e_off_uv += (double)uv - A.contact_tr_uv; /* takes effect now */
    A.contact_tr_uv = (double)uv;
}
void sim_ecg_set_mains(double hz, double amp) { an_sync(); A.mains_hz = hz; A.mains_uv = amp; }
void sim_ecg_set_noise_uv(double rms) { an_sync(); A.noise_uv = rms; }
void sim_ecg_set_vmid_error_mv(int mv)
{
    an_sync();
    vmid_anchor(ana());
    A.vmid_err_mv = mv;
}
void sim_ecg_set_float_high(bool high) { A.float_high = high; }
void sim_ecg_set_float_model(sim_float_model_t m) { A.float_model = m; }
void sim_ecg_set_float_excursions(double rate_hz, unsigned min_ms, unsigned max_ms, uint64_t seed)
{
    A.ex_rate = rate_hz;
    A.ex_min_ms = min_ms;
    A.ex_max_ms = max_ms < min_ms ? min_ms : max_ms;
    A.ex_rng = seed;
    A.ex_start = A.ex_end = sim_now_us(); /* restart the process now */
}
void sim_ecg_thumb_adjust(int32_t step_uv, unsigned ramp_ms)
{
    an_sync();
    if (!ana() || !contact()) return;
    if (A.ramp_end) { /* finish the previous ramp at once */
        A.e_off_uv += A.ramp_uv_per_us * (double)(A.ramp_end - sim_now_us());
        A.ramp_end = 0;
    }
    if (ramp_ms == 0) {
        A.e_off_uv += (double)step_uv;
        return;
    }
    A.ramp_uv_per_us = (double)step_uv / ((double)ramp_ms * 1000.0);
    A.ramp_end = sim_now_us() + (uint64_t)ramp_ms * 1000u;
}
void sim_ecg_set_injection_mv(double g, double r) { A.inj_gain_mv = g; A.inj_rld_mv = r; }
void sim_ecg_set_adc_noise_lsb(double rms) { A.adc_noise = rms; }
void sim_ppg_set_ambient(double dc, double amp, double hz)
{
    A.amb_dc = dc;
    A.amb_fl = amp;
    A.amb_hz = hz;
}

/* ------------------------------------------------------------------------ */
/* resets, change notifications                                              */

void an_reset(void)
{
    memset(&A, 0, sizeof A);
    A.contact_off_uv = 200000.0;
    A.contact_tr_uv = 1000.0;
    A.mains_hz = 50.0;
    A.float_high = true;
    A.float_model = SIM_FLOAT_EXCURSIONS;
    A.ex_rate = 1.0;
    A.ex_min_ms = 20;
    A.ex_max_ms = 300;
    A.ex_rng = 1;
    A.inj_gain_mv = 15.0;
    A.inj_rld_mv = 8.0;
}

static void rings_reset(void)
{
    A.ecg_n = A.ecg_rd = 0;
    A.ecg_delivered = A.ecg_seen_end = 0;
    A.ppg_k = A.ppg_done = A.ppg_rd = 0;
    A.ppg_delivered = A.ppg_seen_end = 0;
    memset(A.ecg_valid, 0, sizeof A.ecg_valid);
    memset(A.ppg_valid, 0, sizeof A.ppg_valid);
}

void an_mcu_reset(void)
{
    A.running = 0;
    rings_reset();
    A.ppg_analog = false;
    A.dac12 = 0;
}

void sim_acq_set_ring_model(sim_ring_model_t m) { A.ring_model = m; }
void sim_acq_set_missed(uint32_t ecg_every, uint32_t ppg_every)
{
    A.miss_ecg = ecg_every;
    A.miss_ppg = ppg_every;
}

void an_sync(void) { model_to(sim_now_us()); }

static void after_change(void)
{
    if (!ana() || !contact()) model_zero();
}

void an_pin_changed(board_pin_t p, bool old_level, bool new_level)
{
    switch (p) {
    case PIN_ANA_EN:
        vmid_anchor(old_level);
        after_change();
        break;
    case PIN_MEASURE_REQ:
    case PIN_CHG_PG: after_change(); break;
    case PIN_GAIN_HI:
        if (ana() && contact()) A.s2 -= A.inj_gain_mv / 1000.0 / (gain() / 21.0);
        break;
    case PIN_RLD_L_EN:
    case PIN_RLD_R_EN:
        if (ana() && contact()) A.s2 -= A.inj_rld_mv / 1000.0 / (gain() / 21.0);
        break;
    default: break;
    }
    (void)new_level;
}

void an_world_changed(void) { after_change(); }

/* ------------------------------------------------------------------------ */
/* acquisition engine                                                        */

unsigned an_running(void) { return A.running; }
bool an_ppg_running(void) { return (A.running & HAL_ACQ_PPG) != 0; }
bool an_ppg_analog_on(void) { return A.ppg_analog; }

uint64_t an_next_event_us(void)
{
    uint64_t t = NEVER;
    if (A.running & HAL_ACQ_ECG) t = A.t_start + A.ecg_n * 250u;
    if (A.running & HAL_ACQ_PPG) {
        uint64_t p = A.t_start + A.ppg_k * PPG_FRAME_US + phase_off_us[A.ppg_phase];
        if (p < t) t = p;
    }
    return t;
}

static void ecg_sample(uint64_t t)
{
    uint16_t code = to_code(ecg_out_v_at(t), A.adc_noise);
    size_t slot = (size_t)(A.ecg_n % SIM_BSP_ECG_RING_SAMPLES);
    A.ecg_ring[slot] = code;
    /* a missed conversion: the index advances, no data */
    A.ecg_valid[slot] = !(A.miss_ecg && (A.ecg_n + 1) % A.miss_ecg == 0);
    A.ecg_n++;
    (sim_facts_raw())->ecg_samples++;
}

static void ppg_event(uint64_t t)
{
    switch (A.ppg_phase) {
    case 0: {
        double tia = tia_v_at(t, 0.0);
        A.tia_dark = to_code(tia, A.adc_noise);
        A.hi_dark = to_code(hi_v_from(tia), A.adc_noise);
        A.ppg_phase = 1;
        break;
    }
    case 1:
        sim_core_set_led_blank(false);
        if (sim_led_now_ma() > (double)LED_MAX_MA + 0.05)
            sim_viol(SIM_V_LED_OVERCURRENT, "LED lit at %.2f mA (limit %d mA)", sim_led_now_ma(),
                     LED_MAX_MA);
        A.ppg_phase = 2;
        break;
    case 2: {
        double tia = tia_v_at(t, sim_led_now_ma());
        hal_ppg_frame_t f = {A.tia_dark, A.hi_dark, to_code(tia, A.adc_noise),
                             to_code(hi_v_from(tia), A.adc_noise)};
        size_t slot = (size_t)(A.ppg_k % SIM_PPG_RING_FRAMES);
        A.ppg_ring[slot] = f;
        A.ppg_valid[slot] = !(A.miss_ppg && (A.ppg_k + 1) % A.miss_ppg == 0);
        A.ppg_done = A.ppg_k + 1;
        (sim_facts_raw())->ppg_frames++;
        A.ppg_phase = 3;
        break;
    }
    default:
        sim_core_set_led_blank(true);
        A.ppg_phase = 0;
        A.ppg_k++;
        break;
    }
}

void an_run_events(uint64_t t)
{
    for (;;) {
        uint64_t te = (A.running & HAL_ACQ_ECG) ? A.t_start + A.ecg_n * 250u : NEVER;
        uint64_t tp = (A.running & HAL_ACQ_PPG)
                          ? A.t_start + A.ppg_k * PPG_FRAME_US + phase_off_us[A.ppg_phase]
                          : NEVER;
        if (te <= t && te <= tp) ecg_sample(te);
        else if (tp <= t) ppg_event(tp);
        else break;
    }
}

void hal_acq_start(unsigned streams)
{
    sim_service();
    if (!streams || (streams & ~(unsigned)(HAL_ACQ_ECG | HAL_ACQ_PPG))) {
        sim_viol(SIM_V_ACQ_START, "hal_acq_start(0x%x): bad stream mask", streams);
        return;
    }
    if (A.running) {
        sim_viol(SIM_V_ACQ_START, "hal_acq_start(0x%x) while streams 0x%x already run", streams,
                 A.running);
        return;
    }
    if (!ana()) sim_viol(SIM_V_ACQ_START, "acquisition started with ANA_EN low");
    if (!sim_clock_fast()) sim_viol(SIM_V_ACQ_START, "acquisition started with slow MCLK");
    if (streams & HAL_ACQ_PPG) {
        if (!A.ppg_analog)
            sim_viol(SIM_V_PPG_START, "PPG started without hal_ppg_analog(true)");
        if (!sim_pin(PIN_VLED_EN)) sim_viol(SIM_V_PPG_START, "PPG started with VLED_EN low");
        else if (sim_now_us() - sim_vled_rise_us() < (uint64_t)PPG_VLED_START_MS * 1000)
            (sim_facts_raw())->ppg_vled_early_starts++;
    }
    rings_reset();
    A.t_start = sim_now_us();
    A.ppg_phase = 0;
    A.running = streams;
    sim_service(); /* sample 0 is taken at t = 0 */
}

void hal_acq_stop(void)
{
    sim_service();
    A.running = 0;
    sim_core_set_led_blank(true);
}

unsigned hal_acq_running(void) { return A.running; }

/* Both ring models hand out contiguous runs with their timeline index:
 *   DEFAULT: SIM_ECG_RING_SAMPLES / SIM_PPG_RING_FRAMES deep;
 *   BSP:     the fw/bsp ECG ring: 512 deep, 16 kept away from the write
 *            position, a read never crosses a 512-sample lap boundary.
 * Overwritten or missed data shows as a jump of the index. */
size_t hal_ecg_read(uint16_t *dst, size_t max, uint32_t *first_index)
{
    sim_service();
    if (!dst || !first_index) {
        sim_viol(SIM_V_API, "hal_ecg_read with NULL %s", dst ? "first_index" : "dst");
        return 0;
    }
    bool bsp = A.ring_model == SIM_RING_BSP;
    uint64_t keep = bsp ? SIM_BSP_ECG_RING_SAMPLES - 16u : SIM_ECG_RING_SAMPLES;
    if (A.ecg_n - A.ecg_rd > keep) A.ecg_rd = A.ecg_n - keep;
    size_t n = 0;
    uint64_t start = A.ecg_rd;
    while (n < max && A.ecg_rd < A.ecg_n) {
        if (bsp && n > 0 && A.ecg_rd % SIM_BSP_ECG_RING_SAMPLES == 0) break;
        size_t slot = (size_t)(A.ecg_rd % SIM_BSP_ECG_RING_SAMPLES);
        if (!A.ecg_valid[slot]) {
            if (n) break;
            A.ecg_rd++;
            continue;
        }
        if (n == 0) start = A.ecg_rd;
        dst[n++] = A.ecg_ring[slot];
        A.ecg_rd++;
    }
    if (n) {
        *first_index = (uint32_t)start;
        A.ecg_delivered += (uint32_t)n;
        A.ecg_seen_end = (uint32_t)(start + n);
    }
    return n;
}
uint32_t hal_ecg_overruns(void) { return A.ecg_seen_end - A.ecg_delivered; }

size_t hal_ppg_read(hal_ppg_frame_t *dst, size_t max, uint32_t *first_index)
{
    sim_service();
    if (!dst || !first_index) {
        sim_viol(SIM_V_API, "hal_ppg_read with NULL %s", dst ? "first_index" : "dst");
        return 0;
    }
    if (A.ppg_done - A.ppg_rd > SIM_PPG_RING_FRAMES) A.ppg_rd = A.ppg_done - SIM_PPG_RING_FRAMES;
    size_t n = 0;
    uint64_t start = A.ppg_rd;
    while (n < max && A.ppg_rd < A.ppg_done) {
        size_t slot = (size_t)(A.ppg_rd % SIM_PPG_RING_FRAMES);
        if (!A.ppg_valid[slot]) {
            if (n) break;
            A.ppg_rd++;
            continue;
        }
        if (n == 0) start = A.ppg_rd;
        dst[n++] = A.ppg_ring[slot];
        A.ppg_rd++;
    }
    if (n) {
        *first_index = (uint32_t)start;
        A.ppg_delivered += (uint32_t)n;
        A.ppg_seen_end = (uint32_t)(start + n);
    }
    return n;
}
uint32_t hal_ppg_overruns(void) { return A.ppg_seen_end - A.ppg_delivered; }
uint32_t hal_acq_resyncs(void) { return 0; }

void hal_ppg_analog(bool on)
{
    sim_service();
    if (!on && (A.running & HAL_ACQ_PPG))
        sim_viol(SIM_V_PPG_ANALOG_OFF, "hal_ppg_analog(false) while PPG runs");
    if (on && !A.ppg_analog) A.dac12 = 0;
    A.ppg_analog = on;
}

void hal_ppg_set_offset(uint16_t code)
{
    sim_service();
    if (code > 4095) code = 4095;
    if (code == A.dac12) return;
    sim_facts_t *f = sim_facts_raw();
    f->ppg_offset_changes++;
    if ((A.running & HAL_ACQ_PPG) && (A.ppg_phase == 1 || A.ppg_phase == 2))
        f->ppg_offset_in_sample_window++;
    A.dac12 = code;
}

uint16_t hal_adc_read_once(hal_adc_ch_t ch)
{
    sim_service();
    if (A.running)
        sim_viol(SIM_V_ADC_WHILE_ACQ, "hal_adc_read_once(%d) while streams 0x%x run", (int)ch,
                 A.running);
    uint64_t t = sim_now_us();
    double v = 0.0;
    switch (ch) {
    case HAL_ADC_ECG: v = ecg_out_v_at(t); break;
    case HAL_ADC_TIA: v = tia_v_at(t, sim_led_now_ma()); break;
    case HAL_ADC_PPG_HI: v = hi_v_from(tia_v_at(t, sim_led_now_ma())); break;
    case HAL_ADC_VMID: v = vmid_at(t); break;
    default: sim_viol(SIM_V_API, "hal_adc_read_once: bad channel"); break;
    }
    uint16_t code = to_code(v, A.adc_noise);
    sim_advance_us(60); /* 16 conversions */
    return code;
}
