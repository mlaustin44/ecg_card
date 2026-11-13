/* Analog world (ECG chain, PPG chain, VMID) and the acquisition engine. */
#include <math.h>
#include <string.h>

#include "sim_priv.h"

#define PI 3.14159265358979323846
#define TAU1 0.5                    /* s: first-stage AC coupling, 0.32 Hz */
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
    sim_ppg_fn ppg_fn;
    void *ppg_ctx;
    double contact_off_uv, contact_tr_uv, mains_hz, mains_uv, noise_uv;
    int vmid_err_mv;
    bool float_high;
    double inj_gain_mv, inj_rld_mv, adc_noise;
    double amb_dc, amb_fl, amb_hz;
    /* ECG model state */
    uint64_t t_model;
    double s1, s2, l1, l2;
    double vmid_v0;
    uint64_t vmid_t0;
    /* acquisition */
    unsigned running;
    uint64_t t_start, ecg_n, ppg_k;
    int ppg_phase;
    uint16_t ecg_ring[SIM_ECG_RING_SAMPLES];
    size_t ecg_head, ecg_count;
    uint32_t ecg_overruns;
    hal_ppg_frame_t ppg_ring[SIM_PPG_RING_FRAMES];
    size_t ppg_head, ppg_count;
    uint32_t ppg_overruns;
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

static void model_zero(void)
{
    A.s1 = A.s2 = A.l1 = A.l2 = 0.0;
    A.t_model = sim_now_us();
}

static double rld_factor(void)
{
    int n = (sim_pin(PIN_RLD_L_EN) ? 1 : 0) + (sim_pin(PIN_RLD_R_EN) ? 1 : 0);
    return n == 2 ? 1.0 : n == 1 ? 3.0 : 10.0;
}

static void ecg_step(uint64_t t_end, uint64_t dt, bool coarse)
{
    double sig = 0.0;
    if (!coarse) {
        if (A.ecg_fn) sig += (double)A.ecg_fn(t_end, A.ecg_ctx);
        if (A.mains_uv != 0.0)
            sig += A.mains_uv * rld_factor() * sin(2.0 * PI * A.mains_hz * (double)t_end * 1e-6);
        if (A.noise_uv != 0.0) sig += A.noise_uv * sim_gauss();
    }
    double v1 = 21e-6 * (sig + A.contact_tr_uv); /* stage-1 input: signal + coupling transient */
    double d = (double)dt * 1e-6;
    A.s1 += (v1 - A.s1) * -expm1(-d / TAU1);
    double y1 = v1 - A.s1 + A.contact_off_uv * 1e-6; /* + DC offset at the HP node (unity DC gain) */
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

static double float_v(uint64_t t)
{
    double w = 0.06 * (0.5 + 0.5 * sin(2.0 * PI * 0.3 * (double)t * 1e-6));
    return A.float_high ? RAIL_HI - w : RAIL_LO + w;
}

static double ecg_out_v_at(uint64_t t)
{
    if (!ana()) return 0.0;
    double vm = vmid_at(t);
    if (!contact()) return sim_pin(PIN_FAST_RESTORE) ? vm : float_v(t);
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
void sim_set_ppg_source(sim_ppg_fn fn, void *ctx) { A.ppg_fn = fn; A.ppg_ctx = ctx; }
void sim_ecg_set_contact_offset_uv(int32_t uv) { an_sync(); A.contact_off_uv = (double)uv; }
void sim_ecg_set_contact_transient_uv(int32_t uv) { an_sync(); A.contact_tr_uv = (double)uv; }
void sim_ecg_set_mains(double hz, double amp) { an_sync(); A.mains_hz = hz; A.mains_uv = amp; }
void sim_ecg_set_noise_uv(double rms) { an_sync(); A.noise_uv = rms; }
void sim_ecg_set_vmid_error_mv(int mv)
{
    an_sync();
    vmid_anchor(ana());
    A.vmid_err_mv = mv;
}
void sim_ecg_set_float_high(bool high) { A.float_high = high; }
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
    A.inj_gain_mv = 15.0;
    A.inj_rld_mv = 8.0;
}

void an_mcu_reset(void)
{
    A.running = 0;
    A.ecg_head = A.ecg_count = A.ppg_head = A.ppg_count = 0;
    A.ecg_overruns = A.ppg_overruns = 0;
    A.ppg_analog = false;
    A.dac12 = 0;
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
    if (A.ecg_count == SIM_ECG_RING_SAMPLES) {
        A.ecg_head = (A.ecg_head + 1) % SIM_ECG_RING_SAMPLES;
        A.ecg_count--;
        A.ecg_overruns++;
    }
    A.ecg_ring[(A.ecg_head + A.ecg_count) % SIM_ECG_RING_SAMPLES] = code;
    A.ecg_count++;
    A.ecg_n++;
    ((sim_facts_t *)sim_facts())->ecg_samples++;
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
        if (A.ppg_count == SIM_PPG_RING_FRAMES) {
            A.ppg_head = (A.ppg_head + 1) % SIM_PPG_RING_FRAMES;
            A.ppg_count--;
            A.ppg_overruns++;
        }
        A.ppg_ring[(A.ppg_head + A.ppg_count) % SIM_PPG_RING_FRAMES] = f;
        A.ppg_count++;
        ((sim_facts_t *)sim_facts())->ppg_frames++;
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
            ((sim_facts_t *)sim_facts())->ppg_vled_early_starts++;
    }
    A.ecg_head = A.ecg_count = A.ppg_head = A.ppg_count = 0;
    A.t_start = sim_now_us();
    A.ecg_n = A.ppg_k = 0;
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

size_t hal_ecg_read(uint16_t *dst, size_t max)
{
    sim_service();
    size_t n = A.ecg_count < max ? A.ecg_count : max;
    for (size_t i = 0; i < n; i++) dst[i] = A.ecg_ring[(A.ecg_head + i) % SIM_ECG_RING_SAMPLES];
    A.ecg_head = (A.ecg_head + n) % SIM_ECG_RING_SAMPLES;
    A.ecg_count -= n;
    return n;
}
uint32_t hal_ecg_overruns(void) { return A.ecg_overruns; }

size_t hal_ppg_read(hal_ppg_frame_t *dst, size_t max)
{
    sim_service();
    size_t n = A.ppg_count < max ? A.ppg_count : max;
    for (size_t i = 0; i < n; i++) dst[i] = A.ppg_ring[(A.ppg_head + i) % SIM_PPG_RING_FRAMES];
    A.ppg_head = (A.ppg_head + n) % SIM_PPG_RING_FRAMES;
    A.ppg_count -= n;
    return n;
}
uint32_t hal_ppg_overruns(void) { return A.ppg_overruns; }

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
    sim_facts_t *f = (sim_facts_t *)sim_facts();
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
