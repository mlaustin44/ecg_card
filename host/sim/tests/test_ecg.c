#include <math.h>
#include <string.h>

#include "common.h"

typedef struct { double amp_uv, hz; } sine_t;
static int32_t sine(uint64_t t_us, void *ctx)
{
    const sine_t *s = ctx;
    return (int32_t)lrint(s->amp_uv * sin(6.283185307179586 * s->hz * (double)t_us * 1e-6));
}

#define MAXS 20000
static uint16_t buf[MAXS];
static size_t nbuf;

/* Run for ms, reading every ms so the ring never overruns. */
static void collect(unsigned ms)
{
    nbuf = 0;
    for (unsigned i = 0; i < ms; i++) {
        hal_wdt_kick();
        hal_delay_ms(1);
        nbuf += ecg_read(buf + nbuf, MAXS - nbuf);
    }
}
static void run_ms(unsigned ms)
{
    while (ms--) {
        hal_wdt_kick();
        hal_delay_ms(1);
    }
}
static void stats(size_t from, double *mean, double *half_pp, double *sd)
{
    double sum = 0, sq = 0;
    uint16_t mn = 65535, mx = 0;
    size_t n = nbuf - from;
    for (size_t i = from; i < nbuf; i++) {
        sum += buf[i];
        sq += (double)buf[i] * buf[i];
        if (buf[i] < mn) mn = buf[i];
        if (buf[i] > mx) mx = buf[i];
    }
    *mean = sum / (double)n;
    *half_pp = (mx - mn) / 2.0;
    *sd = sqrt(sq / (double)n - *mean * *mean);
}

static void bring_up(bool gain_hi, bool thumbs)
{
    ecg_rails_up();
    if (gain_hi) high(PIN_GAIN_HI);
    high(PIN_RLD_L_EN);
    high(PIN_RLD_R_EN);
    high(PIN_MEASURE_REQ);
    sim_set_thumbs(thumbs);
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_ECG);
}

TEST(vmid_settles_with_50ms_tau)
{
    fresh();
    CHECK_EQ(hal_adc_read_once(HAL_ADC_VMID), 0);
    high(PIN_ANA_EN);
    hal_delay_ms(50);
    unsigned c = hal_adc_read_once(HAL_ADC_VMID);
    CHECK_NEAR(c, 2048.0 * (1.0 - exp(-1.0)), 8); /* ~1 tau */
    hal_delay_ms(250);
    c = hal_adc_read_once(HAL_ADC_VMID);
    CHECK_NEAR(c, 2048, 12); /* within 10 mV after 300 ms */
    CHECK_NEAR(sim_vmid_mv(), 1650, 5);
    /* configurable error for fault tests */
    sim_ecg_set_vmid_error_mv(100);
    hal_delay_ms(500);
    c = hal_adc_read_once(HAL_ADC_VMID);
    CHECK_NEAR(c, (1650 + 100) / 3300.0 * 4096.0, 2);
    /* rail off: decays */
    sim_ecg_set_vmid_error_mv(0);
    low(PIN_ANA_EN);
    hal_delay_ms(500);
    CHECK(hal_adc_read_once(HAL_ADC_VMID) < 5);
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(sine_amplitude_both_gains)
{
    const double gains[2] = {ECG_GAIN_LO, ECG_GAIN_HI};
    for (int g = 0; g < 2; g++) {
        fresh();
        sine_t s = {1000.0, 10.0};
        sim_set_ecg_source(sine, &s);
        sim_ecg_set_contact_offset_uv(0);
        sim_ecg_set_contact_transient_uv(0);
        bring_up(g == 1, true);
        collect(1500);
        double mean, hpp, sd;
        stats(nbuf - 2000, &mean, &hpp, &sd); /* last 0.5 s = 5 cycles */
        double expect = gains[g] * 1e-3 / 3.3 * 4096.0;
        CHECK_NEAR(mean, 2048, 3);
        CHECK_NEAR(hpp, expect * 0.996, 3.0);
        CHECK_EQ(hal_ecg_overruns(), 0);
        CHECK_EQ(sim_violation_count(), 0);
        /* uV conversion the firmware will use: (code - mid) * 805.7 / gain */
        CHECK_NEAR(hpp * 805.7 / gains[g], 1000.0, 8.0);
    }
}

TEST(sample_timing_exactly_4khz)
{
    fresh();
    ecg_rails_up();
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_ECG);
    uint16_t tmp[300];
    size_t n = ecg_read(tmp, 300);
    CHECK_EQ(n, 1); /* sample 0 at t = 0 */
    hal_delay_us(249);
    CHECK_EQ(ecg_read(tmp, 300), 0);
    hal_delay_us(1);
    CHECK_EQ(ecg_read(tmp, 300), 1);
    hal_delay_ms(10);
    CHECK_EQ(ecg_read(tmp, 300), 40);
    CHECK_EQ(sim_facts()->ecg_samples, 42);
    hal_acq_stop();
    hal_delay_ms(10);
    CHECK_EQ(ecg_read(tmp, 300), 0);
    CHECK_EQ(hal_acq_running(), 0);
    /* restart: indices restart, sample 0 immediately */
    hal_acq_start(HAL_ACQ_ECG);
    CHECK_EQ(ecg_read(tmp, 300), 1);
}

TEST(overrun_counting)
{
    fresh();
    ecg_rails_up();
    hal_clock_fast(true);
    hal_acq_start(HAL_ACQ_ECG);
    hal_delay_ms(100); /* samples 0..400 = 401 */
    CHECK_EQ(sim_facts()->ecg_samples, 401);
    CHECK_EQ(hal_ecg_overruns(), 0); /* counted when a read passes the loss */
    uint16_t tmp[512];
    uint32_t first = 12345;
    CHECK_EQ(hal_ecg_read(tmp, 512, &first), SIM_ECG_RING_SAMPLES);
    CHECK_EQ(first, 401 - SIM_ECG_RING_SAMPLES); /* the oldest were overwritten */
    CHECK_EQ(hal_ecg_overruns(), 401 - SIM_ECG_RING_SAMPLES);
    first = 777;
    CHECK_EQ(hal_ecg_read(tmp, 512, &first), 0);
    CHECK_EQ(first, 777); /* untouched when nothing is returned */
    /* partial reads keep order and nothing is lost */
    hal_delay_ms(10);
    CHECK_EQ(hal_ecg_read(tmp, 10, &first), 10);
    CHECK_EQ(first, 401);
    CHECK_EQ(hal_ecg_read(tmp, 512, &first), 40 - 10);
    CHECK_EQ(first, 411);
    CHECK_EQ(hal_ecg_overruns(), 401 - SIM_ECG_RING_SAMPLES);
    /* restart: indices and the counter restart */
    hal_acq_stop();
    hal_acq_start(HAL_ACQ_ECG);
    CHECK_EQ(hal_ecg_read(tmp, 512, &first), 1);
    CHECK_EQ(first, 0);
    CHECK_EQ(hal_ecg_overruns(), 0);
}

/* The fw/bsp failure pattern: 512-sample ring, 16 kept from the writer,
 * reads end at lap boundaries; missed conversions skip an index. Whatever
 * happens, index + count stays exact: first index of each read = delivered +
 * overruns, and the sample values (a ramp source) match their index. */
static int32_t ramp_uv(uint64_t t_us, void *ctx)
{
    (void)ctx;
    /* 1 code per sample at 558x: 805.7/558 uV; period 256 samples */
    uint64_t n = t_us / 250;
    return (int32_t)lrint(((double)(n % 256) - 128.0) * 805.7 / 558.0 * 4.0);
}

TEST(bsp_ring_model_timeline)
{
    for (int model = 0; model < 2; model++) {
        fresh();
        sim_acq_set_ring_model(model ? SIM_RING_BSP : SIM_RING_DEFAULT);
        sim_acq_set_missed(1000, 0);
        sim_ecg_set_contact_offset_uv(0);
        sim_ecg_set_contact_transient_uv(0);
        sim_set_ecg_source(ramp_uv, NULL);
        ecg_rails_up();
        high(PIN_MEASURE_REQ);
        sim_set_thumbs(true);
        hal_clock_fast(true);
        hal_acq_start(HAL_ACQ_ECG);
        uint32_t next = 0, delivered = 0, lost = 0, stalls = 0;
        bool crossed_lap = false;
        for (int ms = 0; ms < 3000; ms++) {
            hal_wdt_kick();
            hal_delay_ms(ms % 700 == 699 ? 150 : 1); /* 150 ms reader stalls */
            stalls += ms % 700 == 699;
            uint16_t c[64];
            uint32_t first;
            size_t n;
            while ((n = hal_ecg_read(c, 64, &first)) != 0) {
                CHECK((int32_t)(first - next) >= 0);
                lost += first - next;
                delivered += (uint32_t)n;
                if (model && first / 512 != (first + (uint32_t)n - 1) / 512) crossed_lap = true;
                next = first + (uint32_t)n;
                CHECK_EQ(next, delivered + hal_ecg_overruns());
            }
        }
        CHECK(!crossed_lap);
        CHECK(stalls >= 4);
        CHECK_EQ(lost, hal_ecg_overruns());
        CHECK(lost > stalls * (model ? 600u - 496u : 600u - 256u));
        CHECK_EQ(hal_acq_resyncs(), 0);
        CHECK_EQ(sim_violation_count(), 0);
        sim_acq_set_missed(0, 0);
    }
}

TEST(lead_off_railing)
{
    fresh();
    /* the fixed-rail floating model (the default excursion model is tested
     * in floating_excursions) */
    sim_ecg_set_float_model(SIM_FLOAT_RAIL);
    bring_up(false, false); /* thumbs off */
    collect(300);
    double mean, hpp, sd;
    stats(0, &mean, &hpp, &sd);
    CHECK(mean > 4096 * 0.95 || mean < 4096 * 0.05);
    CHECK(hpp < 100);
    /* thumbs on but BODY_EN low (MEASURE_REQ low) is just as railed */
    low(PIN_MEASURE_REQ);
    sim_set_thumbs(true);
    collect(100);
    stats(0, &mean, &hpp, &sd);
    CHECK(mean > 4096 * 0.95);
    /* USB present also disconnects the electrodes */
    high(PIN_MEASURE_REQ);
    sim_ecg_set_contact_offset_uv(0);
        sim_ecg_set_contact_transient_uv(0);
    collect(200);
    stats(nbuf - 100, &mean, &hpp, &sd);
    CHECK_NEAR(mean, 2048, 5);
    sim_set_usb(true);
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
    collect(100);
    stats(nbuf - 50, &mean, &hpp, &sd);
    CHECK(mean > 4096 * 0.95);
    CHECK_EQ(sim_violation_count_of(SIM_V_GUARDED_PIN_ANA_LOW), 0);
    /* low rail variant */
    sim_ecg_set_float_high(false);
    collect(50);
    stats(nbuf - 20, &mean, &hpp, &sd);
    CHECK(mean < 4096 * 0.05);
}

/* Time (ms) after contact after which |code - mid| stays below 25 % of full scale. */
static int settle_ms(bool pulse_fr, double *first_code)
{
    fresh();
    bring_up(false, false);
    collect(50);
    sim_set_thumbs(true); /* contact: large offset step */
    int last_bad = -1;
    for (int ms = 0; ms < 6000; ms++) {
        hal_wdt_kick();
        if (pulse_fr && ms == 40) high(PIN_FAST_RESTORE);
        if (pulse_fr && ms == 70) low(PIN_FAST_RESTORE);
        hal_delay_ms(1);
        uint16_t c[8];
        size_t n = ecg_read(c, 8);
        if (ms == 5) *first_code = c[n - 1];
        if (fabs((double)c[n - 1] - 2048.0) >= 1024.0) last_bad = ms;
    }
    return last_bad + 1;
}

TEST(contact_step_and_fast_restore)
{
    double first;
    int t_plain = settle_ms(false, &first);
    CHECK(first > 3891 || first < 205); /* railed right after contact */
    (void)0;
    CHECK(t_plain > 300 && t_plain < 1500);
    double first2;
    int t_fr = settle_ms(true, &first2);

    CHECK(t_fr > 0);
    CHECK(t_fr < t_plain);
    CHECK(t_plain - t_fr > 200);
    CHECK(t_fr < 150); /* FR at 40..70 ms removes the offset; only the small first-stage tail is left */
    /* during the pulse the output sits at mid */
    fresh();
    bring_up(false, true);
    hal_delay_ms(40);
    high(PIN_FAST_RESTORE);
    hal_delay_ms(30);
    uint16_t c[300];
    size_t n = ecg_read(c, 300);
    CHECK_NEAR(c[n - 1], 2048, 6);
    CHECK_EQ(sim_violation_count(), 0);
    /* with a small offset (no saturation) the HP node settling is observable at the output:
       an offset of 0.5 mV settles from <1 V swing; after FR + 3 s residual < 25 codes */
    fresh();
    sim_ecg_set_contact_offset_uv(500);
    bring_up(false, false);
    hal_delay_ms(20);
    sim_set_thumbs(true);
    hal_delay_ms(10);
    high(PIN_FAST_RESTORE);
    hal_delay_ms(30);
    low(PIN_FAST_RESTORE);
    run_ms(3000);
    n = ecg_read(c, 300);
    CHECK(fabs((double)c[n - 1] - 2048.0) < 25);
}

TEST(gain_and_rld_changes_inject_steps)
{
    fresh();
    sim_ecg_set_contact_offset_uv(0);
        sim_ecg_set_contact_transient_uv(0);
    ecg_rails_up();
    high(PIN_MEASURE_REQ);
    sim_set_thumbs(true);
    hal_delay_ms(100);
    double before = hal_adc_read_once(HAL_ADC_ECG);
    CHECK_NEAR(before, 2048, 2);
    high(PIN_GAIN_HI);
    run_ms(5);
    double after = hal_adc_read_once(HAL_ADC_ECG);
    double expect = 15.0 / 3300.0 * 4096.0;
    CHECK_NEAR(after - before, expect, 3);
    CHECK_NEAR(hal_adc_read_once(HAL_ADC_ECG) - before, 0.0, 30.0); /* before the 1 ms LP moves */
    run_ms(3000);
    CHECK_NEAR(hal_adc_read_once(HAL_ADC_ECG), 2048, 2); /* decays away */
    high(PIN_RLD_L_EN);
    run_ms(5);
    CHECK_NEAR(hal_adc_read_once(HAL_ADC_ECG) - 2048.0, 8.0 / 3300.0 * 4096.0, 3);
    CHECK_EQ(sim_violation_count(), 0);
}

TEST(mains_and_noise)
{
    double amp[2];
    for (int r = 0; r < 2; r++) {
        fresh();
        sim_ecg_set_contact_offset_uv(0);
        sim_ecg_set_contact_transient_uv(0);
        sim_ecg_set_mains(50.0, 20.0);
        ecg_rails_up();
        if (r == 0) {
            high(PIN_RLD_L_EN);
            high(PIN_RLD_R_EN);
        }
        high(PIN_MEASURE_REQ);
        sim_set_thumbs(true);
        hal_clock_fast(true);
        hal_acq_start(HAL_ACQ_ECG);
        collect(1500);
        double mean, hpp, sd;
        stats(nbuf - 2000, &mean, &hpp, &sd);
        amp[r] = hpp;
    }
    CHECK_NEAR(amp[0], 558.0 * 20e-6 / 3.3 * 4096.0, 2.0);
    CHECK_NEAR(amp[1] / amp[0], 10.0, 0.5);

    fresh();
    sim_ecg_set_contact_offset_uv(0);
        sim_ecg_set_contact_transient_uv(0);
    sim_ecg_set_noise_uv(100.0);
    bring_up(false, true);
    collect(1000);
    double mean, hpp, sd;
    stats(nbuf - 2000, &mean, &hpp, &sd);
    CHECK(sd > 5.0 && sd < 100.0);
}

/* Floating inputs, default model: mostly on the rail, brief in-range
 * excursions about once a second, none as long as 500 ms; FAST_RESTORE holds
 * mid-scale; deterministic for a seed. The quiet model sits at mid. */
static void float_stats(unsigned ms, double *rail_frac, unsigned *n_ex, unsigned *max_ex_ms,
                        uint64_t *hash)
{
    unsigned rail = 0, run = 0, total = 0;
    *n_ex = 0;
    *max_ex_ms = 0;
    *hash = 1469598103934665603ull;
    for (unsigned i = 0; i < ms; i++) {
        hal_wdt_kick();
        hal_delay_ms(1);
        uint16_t c[16];
        size_t n = ecg_read(c, 16);
        for (size_t k = 0; k < n; k++) {
            bool r = c[k] < 205 || c[k] >= 3891;
            *hash = (*hash ^ c[k]) * 1099511628211ull;
            total++;
            if (r) {
                rail++;
                if (run) {
                    (*n_ex)++;
                    if (run / 4 > *max_ex_ms) *max_ex_ms = run / 4;
                }
                run = 0;
            } else {
                run++;
            }
        }
    }
    *rail_frac = (double)rail / total;
}

TEST(floating_excursions)
{
    double frac;
    unsigned n, mx;
    uint64_t h1, h2;
    fresh();
    bring_up(false, false); /* thumbs off: inputs float */
    float_stats(60000, &frac, &n, &mx, &h1);
    printf("  floating 60 s: %.1f %% on the rail, %u excursions, longest %u ms\n", frac * 100, n, mx);
    CHECK(frac > 0.8 && frac < 0.95);
    CHECK(n > 40 && n < 80);
    CHECK(mx >= 200 && mx <= 300);
    /* same seed, same samples */
    fresh();
    bring_up(false, false);
    float_stats(60000, &frac, &n, &mx, &h2);
    CHECK_EQ(h1, h2);
    /* other seed / rate */
    fresh();
    sim_ecg_set_float_excursions(3.0, 20, 60, 7);
    bring_up(false, false);
    float_stats(20000, &frac, &n, &mx, &h2);
    CHECK(h1 != h2);
    CHECK(n > 40 && n < 80 && mx <= 60);
    /* FAST_RESTORE holds mid-scale */
    high(PIN_FAST_RESTORE);
    collect(30);
    double mean, hpp, sd;
    stats(nbuf - 40, &mean, &hpp, &sd);
    CHECK_NEAR(mean, 2048, 3);
    low(PIN_FAST_RESTORE);
    /* quiet model: never on a rail */
    fresh();
    sim_ecg_set_float_model(SIM_FLOAT_MID);
    bring_up(false, false);
    float_stats(5000, &frac, &n, &mx, &h2);
    CHECK_EQ(frac, 0.0);
    collect(2000);
    stats(0, &mean, &hpp, &sd);
    CHECK_NEAR(mean, 2048, 3);
    CHECK(hpp < 8);
    CHECK_EQ(sim_violation_count(), 0);
}

/* U5: an electrode offset step appears x21 (clipped at +-1.55 V) and decays
 * to x1 with tau 0.5 s; FAST_RESTORE cannot remove it: right after the pulse
 * the output runs back to a rail while the slope is large. */
static unsigned rail_after_fr_ms(unsigned at_ms)
{
    fresh();
    sim_ecg_set_contact_offset_uv(0);
    sim_ecg_set_contact_transient_uv(150000); /* 150 mV: dry thumbs */
    bring_up(false, false);
    run_ms(10);
    sim_set_thumbs(true);
    run_ms(at_ms);
    high(PIN_FAST_RESTORE);
    run_ms(30);
    low(PIN_FAST_RESTORE);
    (void)ecg_read(buf, MAXS);
    for (unsigned ms = 0; ms < 3000; ms++) {
        hal_wdt_kick();
        hal_delay_ms(1);
        uint16_t c[8];
        size_t n = ecg_read(c, 8);
        for (size_t k = 0; k < n; k++)
            if (c[k] < 205 || c[k] >= 3891) return ms;
    }
    return 9999;
}

TEST(electrode_offset_step)
{
    /* The first-stage tail is not touched by FAST_RESTORE. While U5 is
     * clipped (the first ~0.4 s of a 150 mV step) its output is flat and the
     * pulse holds; once it decays, the slope rails the output again within
     * tens of ms of the release, until ~2 s (4 tau) after the step. */
    unsigned t02 = rail_after_fr_ms(200), t05 = rail_after_fr_ms(500), t10 = rail_after_fr_ms(1000);
    unsigned t15 = rail_after_fr_ms(1500), t20 = rail_after_fr_ms(2000);
    printf("  150 mV step: after a FAST_RESTORE pulse at 0.2/0.5/1/1.5/2 s the output rails "
           "again after %u/%u/%u/%u/%u ms (9999 = stays in range)\n", t02, t05, t10, t15, t20);
    CHECK(t02 > 100 && t02 < 400); /* clipped until ~0.38 s */
    CHECK(t05 < 50 && t10 < 150);
    CHECK(t20 == 9999);
    /* small steps: x21 at once, then back to mid as both high-passes settle */
    fresh();
    sim_ecg_set_contact_offset_uv(0);
    sim_ecg_set_contact_transient_uv(0);
    bring_up(false, true);
    collect(100);
    sim_ecg_thumb_adjust(1000, 0); /* 1 mV: 21 mV at U5 -> 0.56 V at the output */
    collect(10);
    double c0 = buf[nbuf - 1] - 2048.0;
    CHECK_NEAR(c0, 1e-3 * 558.0 / 3.3 * 4096.0, 30);
    collect(5000);
    CHECK_NEAR(buf[nbuf - 1], 2048, 3);
    /* clipped at +-1.55 V: 200 mV would be 4.2 V at U5. While clipped, U5's
     * output is flat, so after a FAST_RESTORE pulse the output stays at mid
     * (~0.5 s); then the decay out of the clip rails it low */
    sim_ecg_thumb_adjust(200000, 0);
    high(PIN_FAST_RESTORE);
    collect(30);
    low(PIN_FAST_RESTORE);
    collect(150);
    CHECK_NEAR(buf[nbuf - 1], 2048, 40); /* flat while clipped */
    collect(1500);
    CHECK(buf[nbuf - 1] < 205); /* decaying out of the clip: rails low */
    /* a ramp: the same offset change spread over 200 ms */
    fresh();
    sim_ecg_set_contact_offset_uv(0);
    sim_ecg_set_contact_transient_uv(0);
    bring_up(false, true);
    collect(100);
    sim_ecg_thumb_adjust(-1000, 200);
    collect(100);
    double half = buf[nbuf - 1] - 2048.0;
    collect(100);
    double full = buf[nbuf - 1] - 2048.0;
    printf("  -1 mV ramp over 200 ms: %.0f codes at 100 ms, %.0f at 200 ms\n", half, full);
    CHECK(half < -150 && full < half - 100);
    CHECK_EQ(sim_violation_count(), 0);
}

/* The detector the first hardware run used (dsp_lead_push before this
 * change): CONTACT = 20 ms continuously off the rail, LEAD OFF = on the rail
 * for > 100 ms (gaps < 20 ms ignored); FAST_RESTORE pulsed for 30 ms on each
 * contact edge. Fed by this model, it must flap like the logged session:
 * contact / lost pairs 130-270 ms apart, many at 130-140 ms. */
typedef struct {
    uint16_t rail_run, ok_run;
    bool off;
} legacy_t;

static int legacy_push(legacy_t *l, uint16_t c)
{
    if (c < 205 || c >= 3891) {
        l->ok_run = 0;
        if (l->rail_run < 65535u) l->rail_run++;
        if (!l->off && l->rail_run > 400u) {
            l->off = true;
            return -1;
        }
    } else {
        if (l->ok_run < 65535u) l->ok_run++;
        if (l->ok_run >= 80u) {
            l->rail_run = 0;
            if (l->off) {
                l->off = false;
                return 1;
            }
        }
    }
    return 0;
}

#define MAXP 400
static struct {
    uint64_t on[MAXP], off[MAXP];
    unsigned n;
} P;

static void legacy_run(uint64_t until_ms, legacy_t *l, uint64_t *fr_off, bool print)
{
    while (sim_now_ms() < until_ms) {
        hal_wdt_kick();
        hal_delay_ms(1);
        uint64_t now = sim_now_ms();
        if (*fr_off && now >= *fr_off) {
            low(PIN_FAST_RESTORE);
            *fr_off = 0;
        }
        uint16_t c[16];
        size_t n = ecg_read(c, 16);
        int ev = 0;
        for (size_t k = 0; k < n; k++) {
            int e = legacy_push(l, c[k]);
            if (e) ev = e;
        }
        if (ev > 0) {
            if (print) printf("  [%llu] acq: contact\n", (unsigned long long)now);
            if (P.n < MAXP) P.on[P.n] = now;
            if (!*fr_off) {
                high(PIN_FAST_RESTORE);
                *fr_off = now + 30;
            }
        } else if (ev < 0) {
            if (print) printf("  [%llu] acq: contact lost\n", (unsigned long long)now);
            if (P.n < MAXP) P.off[P.n++] = now;
        }
    }
}

static uint32_t lcg(uint32_t *x)
{
    *x = *x * 1664525u + 1013904223u;
    return *x >> 8;
}

TEST(legacy_detector_flaps)
{
    fresh();
    memset(&P, 0, sizeof P);
    sim_ecg_set_contact_transient_uv(150000); /* dry thumbs */
    sim_ecg_set_mains(50.0, 30.0);
    /* thumbs hovering / brushing the pads: excursions twice a second */
    sim_ecg_set_float_excursions(2.0, 20, 300, 2);
    bring_up(false, false);
    legacy_t l = {0, 0, true};
    uint64_t fr_off = 0;
    /* 2..19 s: the user struggles: light, intermittent touches (each new
     * touch is a new 150 mV step; in between the inputs float) and offset
     * changes of 30..200 mV while pressing; then holds still; lifts the thumbs
     * at 30 s. */
    uint32_t r = 12345;
    uint64_t t = 2000;
    bool on = false;
    while (t < 19000) {
        on = !on;
        sim_at_thumbs(t, on);
        uint64_t len = on ? 150 + lcg(&r) % 900 : 40 + lcg(&r) % 400;
        if (on)
            for (uint64_t ta = t + 100 + lcg(&r) % 300; ta < t + len; ta += 200 + lcg(&r) % 400) {
                int32_t uv = (int32_t)(30000 + lcg(&r) % 170000);
                sim_at_thumb_adjust(ta, lcg(&r) & 1 ? uv : -uv, lcg(&r) % 3 ? 0 : 20);
            }
        t += len;
    }
    if (!on) sim_at_thumbs(t, true);
    sim_at_thumbs(30000, false);
    printf("  simulated old logic, start of the session:\n");
    legacy_run(7000, &l, &fr_off, true);
    printf("  ...\n");
    legacy_run(36000, &l, &fr_off, false);
    unsigned during = 0, after_lift = 0, held = 0;
    unsigned hist[6] = {0}; /* <125, 125-140, 140-170, 170-270, 270-500, >500 */
    for (unsigned i = 0; i < P.n; i++) {
        uint64_t d = P.off[i] - P.on[i];
        hist[d < 125 ? 0 : d <= 140 ? 1 : d <= 170 ? 2 : d <= 270 ? 3 : d <= 500 ? 4 : 5]++;
        if (P.on[i] < 19000) during++;
        else if (P.on[i] < 30000) held++;
        else after_lift++;
    }
    printf("  %u contact/lost pairs (%u while struggling, %u after holding still, %u after the "
           "lift); contact->lost: <125 ms %u, 125-140 %u, 140-170 %u, 170-270 %u, 270-500 %u, "
           ">500 %u\n", P.n, during, held, after_lift, hist[0], hist[1], hist[2], hist[3], hist[4],
           hist[5]);
    CHECK(during >= 15);
    CHECK(after_lift >= 3);
    CHECK(held <= 3);                /* holding still: the last touch's step decays, then it lasts */
    /* never under the 30 ms pulse + 100 ms; a third right at 130-140 ms
     * (re-railed at once after the pulse), most of the rest 140-270 ms */
    CHECK_EQ(hist[0], 0);
    CHECK(hist[1] * 10 >= P.n * 3);
    CHECK((hist[1] + hist[2] + hist[3]) * 10 >= P.n * 8);
    CHECK_EQ(sim_violation_count(), 0);
}

int main(void)
{
    RUN(vmid_settles_with_50ms_tau);
    RUN(sine_amplitude_both_gains);
    RUN(sample_timing_exactly_4khz);
    RUN(overrun_counting);
    RUN(bsp_ring_model_timeline);
    RUN(lead_off_railing);
    RUN(contact_step_and_fast_restore);
    RUN(gain_and_rld_changes_inject_steps);
    RUN(mains_and_noise);
    RUN(floating_excursions);
    RUN(electrode_offset_step);
    RUN(legacy_detector_flaps);
    return TEST_RESULT();
}
