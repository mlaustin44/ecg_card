/* Replay of a recorded session through the whole application.
 *
 * ECG_REPLAY=<ble_client "Save session" file> app_scen_replay
 *
 * The file's "## ECG index,uV,flags" section (500 Hz, input-referred uV at
 * the gain in its "session: start ... gain=N" line) drives ECG_ADC directly
 * (sim_set_ecg_output: the analog chain model is bypassed, the recording
 * already went through the real one). The thumbs go on when the acquisition
 * starts and off at the end of the recording. Prints every ECG state change,
 * the blips, the auto-gain and the OLED scale, and saves screenshots to
 * build/app_shots/replay_<s>.png every 5 s. Without ECG_REPLAY the test is
 * skipped: recordings are personal data and stay out of the repository. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acq.h"
#include "app.h"
#include "scen.h"
#include "sim.h"
#include "test.h"
#include "trace.h"

static struct {
    int32_t *uv;      /* ADC-pin microvolts relative to VMID, at ECG_GAIN_LO */
    uint32_t n;
    uint64_t t0_us;   /* acquisition start */
} R;

static bool load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[512];
    bool in = false;
    int gain = ECG_GAIN_LO;
    uint32_t cap = 0, first = 0;
    bool have_first = false;
    while (fgets(line, sizeof line, f)) {
        const char *g = strstr(line, "session: start");
        if (g && (g = strstr(g, "gain=")) != NULL)
            gain = atoi(g + 5);
        if (!strncmp(line, "## ECG index", 12)) {
            in = true;
            continue;
        }
        if (in && !strncmp(line, "## ", 3))
            break;
        if (!in)
            continue;
        unsigned idx;
        int uv;
        if (sscanf(line, "%u,%d", &idx, &uv) != 2)
            continue;
        if (!have_first) {
            first = idx;
            have_first = true;
        }
        uint32_t k = idx - first;
        if (k >= 1000000u)
            break;
        if (k >= cap) {
            uint32_t nc = cap ? cap * 2u : 4096u;
            while (nc <= k)
                nc *= 2u;
            R.uv = realloc(R.uv, nc * sizeof *R.uv);
            for (uint32_t i = cap; i < nc; i++)
                R.uv[i] = INT32_MIN; /* missing */
            cap = nc;
        }
        R.uv[k] = (int32_t)uv * gain; /* input uV -> ADC-pin uV at that gain */
        if (k + 1u > R.n)
            R.n = k + 1u;
    }
    fclose(f);
    /* fill gaps with the previous sample; scale to ECG_GAIN_LO (the sim
     * applies ECG_GAIN_HI / ECG_GAIN_LO when GAIN_HI is high) */
    int32_t prev = 0;
    for (uint32_t i = 0; i < R.n; i++) {
        if (R.uv[i] == INT32_MIN)
            R.uv[i] = prev;
        prev = R.uv[i];
        R.uv[i] = (int32_t)((int64_t)R.uv[i] * ECG_GAIN_LO / gain);
    }
    printf("  %s: %u samples (%.1f s) recorded at %dx\n", path, (unsigned)R.n, R.n / 500.0, gain);
    return R.n > 0;
}

static int32_t out_uv(uint64_t t_us, void *ctx)
{
    (void)ctx;
    if (t_us < R.t0_us)
        return R.uv[0];
    uint64_t dt = t_us - R.t0_us;
    uint32_t k = (uint32_t)(dt / 2000u);
    if (k + 1u >= R.n)
        return R.uv[R.n - 1u];
    int64_t a = R.uv[k], b = R.uv[k + 1u];
    return (int32_t)(a + (b - a) * (int64_t)(dt % 2000u) / 2000);
}

static const char *const st_name[] = {"wait", "settle", "run", "hold", "end"};

TEST(replay)
{
    const char *path = getenv("ECG_REPLAY");
    if (!path || !*path) {
        printf("  ECG_REPLAY not set: skipped\n");
        return;
    }
    CHECK(load(path));
    if (!R.n)
        return;
    scen_boot(false);
    sim_set_ecg_output(out_uv, NULL);
    scen_run_ms(300);
    scen_tap(2); /* ECG card */
    scen_tap(1); /* start */
    while (acq_status()->phase != ACQ_ON)
        scen_run_ms(1);
    R.t0_us = sim_now_us();
    uint64_t t0 = sim_now_ms();
    sim_set_thumbs(true);
    uint8_t last = 255, gain_sw = 0;
    uint16_t blips = 0;
    uint32_t holds = 0, next_shot = 5000;
    uint64_t end_ms = t0 + (uint64_t)R.n * 2u;
    while (sim_now_ms() < end_ms) {
        scen_run_ms(10);
        const acq_status_t *st = acq_status();
        uint32_t t = (uint32_t)(sim_now_ms() - t0);
        if (st->phase != ACQ_ON)
            break;
        if (st->ecg_state != last) {
            printf("  %6.2f s  %s -> %s\n", t / 1000.0, last < 5 ? st_name[last] : "-",
                   st_name[st->ecg_state]);
            if (st->ecg_state == ACQ_ECG_HOLD)
                holds++;
            last = st->ecg_state;
        }
        if (st->blips != blips) {
            printf("  %6.2f s  blip %u\n", t / 1000.0, st->blips);
            blips = st->blips;
        }
        if (st->gain_switches != gain_sw) {
            printf("  %6.2f s  auto-gain -> %s\n", t / 1000.0, st->gain_hi ? "hi" : "lo");
            gain_sw = st->gain_switches;
        }
        if (t >= next_shot) {
            char name[32];
            snprintf(name, sizeof name, "replay_%02u", (unsigned)(t / 1000u));
            scen_shot(name);
            printf("  %6.2f s  shot %s: OLED %u uV per height, QRS %u uV, HR %u\n", t / 1000.0,
                   name, trace_ecg_steps_uv[trace_state(UI_TRACE_ECG)->step], st->qrs_uv,
                   st->hr_bpm);
            next_shot += 5000;
        }
    }
    const acq_status_t *st = acq_status();
    printf("  end of recording: state %s, running %.1f s of %.1f s, %u blips, %u holds, "
           "%u beats\n", st_name[st->ecg_state < 5 ? st->ecg_state : 0], st->running_ms / 1000.0,
           R.n / 500.0, st->blips, (unsigned)holds, st->ecg_beats);
    sim_set_thumbs(false);
    scen_tap(1);
    scen_run_ms(200);
    sim_set_ecg_output(NULL, NULL);
    free(R.uv);
}

int main(void)
{
    RUN(replay);
    return TEST_RESULT();
}
