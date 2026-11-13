import numpy as np
import pytest

from ecgcard import siggen
from ecgcard.hrv import hrv_stats

NAMES = ["clean_60bpm", "fast_180bpm", "slow_40bpm", "low_amp_noisy", "mains50", "mains60", "wander",
         "tall_t", "hrv", "both_pat200", "ectopic", "gain1098", "led_step"]


def test_deterministic():
    a = siggen.simulate(siggen.EcgParams(duration_s=5, seed=3), siggen.PpgParams(seed=4))
    b = siggen.simulate(siggen.EcgParams(duration_s=5, seed=3), siggen.PpgParams(seed=4))
    assert np.array_equal(a.ecg_codes, b.ecg_codes) and np.array_equal(a.ppg.frames, b.ppg.frames)


@pytest.mark.parametrize("name", NAMES)
def test_truth_self_consistent(sim, name):
    s = sim(name)
    tr = s.truth()
    r = np.array(tr["r_peak_times_s"])
    assert len(s.ecg_codes) == s.ecg_params.duration_s * 4000
    assert np.allclose(np.diff(r), tr["rr_s"], atol=1e-5)
    assert np.all(r > 0) and np.all(r < s.ecg_params.duration_s)
    nn = np.array(tr["rr_s"])
    if not any(tr["ectopic"]):
        h = hrv_stats(nn)
        assert np.isclose(tr["hr_bpm"], h["hr_bpm"]) and np.isclose(tr["sdnn_ms"], h["sdnn_ms"], equal_nan=True)
    pat = np.array(tr["pat_s"])
    feet = np.array(tr["pulse_foot_times_s"])
    assert len(feet) == len(pat) and len(feet) >= len(r) - 2
    assert np.allclose(feet - pat, r[:len(feet)], atol=1e-5)
    assert abs(pat.mean() - s.ppg_params.pat_ms / 1000) < 0.01
    # ADC codes in range, no clipping, and the beat is where the truth says
    assert s.ecg_codes.min() > 0 and s.ecg_codes.max() < 4095
    assert s.ppg.frames.max() <= 4095


def test_hr_range_and_variability():
    for hr in (40, 120, 200):
        s = siggen.simulate(siggen.EcgParams(duration_s=20, hr_bpm=hr), siggen.PpgParams())
        assert abs(s.truth()["hr_bpm"] - hr) < 0.5
    s = siggen.simulate(siggen.EcgParams(duration_s=60, hr_bpm=60, rr_sd_ms=30, seed=9))
    assert 15 < s.truth()["sdnn_ms"] < 50


def test_r_amplitude_and_scaling(sim):
    s = sim("clean_60bpm")
    r = np.array(s.truth()["r_peak_times_s"])
    mid = s.ecg_params.mid_code
    k = (r[5] * 4000).astype(int)
    peak = s.ecg_codes[k - 8:k + 9].astype(int).max() - mid
    expected = s.ecg_params.r_mv * 1000 * s.ecg_params.gain / siggen.UV_PER_CODE
    assert 0.85 * expected < peak < 1.05 * expected  # analog LP shaves the narrow R a little
    assert abs(np.median(s.ecg_codes) - mid) < 40  # beats are positive, the HP centres the mean
    s2 = sim("gain1098")
    assert s2.ecg_params.gain == 1098


def test_mains_present_at_right_frequency(sim):
    for name, f0 in (("mains50", 50), ("mains60", 60)):
        x = sim(name).ecg_codes.astype(float)
        spec = np.abs(np.fft.rfft((x - x.mean()) * np.hanning(len(x))))
        f = np.fft.rfftfreq(len(x), 1 / 4000)
        for h in (1, 2):
            band = (abs(f - f0 * h) < 1)
            ref = (abs(f - (f0 * h + 7)) < 1)
            assert spec[band].max() > 20 * spec[ref].max(), (name, h)


def test_ppg_formulas(sim):
    s = sim("clean_60bpm")
    f = s.ppg.frames.astype(float)
    tia_on_v = f[:, 2] / 4096 * 3.3
    i_ua = (tia_on_v - 0.245)  # 1 Mohm: volts == microamps
    assert 0.5 < i_ua.mean() < 3.0
    # PPG_HI(on) stays near mid-scale thanks to the DAC12 offset
    assert abs(np.median(f[:, 3]) - 2048) < 400
    # dark phase: PPG_HI rails high (expected), TIA near bias
    assert np.median(f[:, 1]) > 4000 and np.median(f[:, 0]) < 0.5 / 3.3 * 4096
    # systole DECREASES the light: pulse lowers TIA_on, i.e. coarse dips after each foot
    feet = np.array(s.truth()["pulse_foot_times_s"])
    t = np.arange(len(f)) / 100
    peak_idx = [int((x + 0.16) * 100) for x in feet[2:10]]
    base_idx = [int((x - 0.1) * 100) for x in feet[2:10]]
    assert np.mean(f[peak_idx, 2]) < np.mean(f[base_idx, 2]) - 10
    # fine channel (true) rises with light, so it is anti-correlated with the pulse
    fine = s.ppg.fine
    assert np.mean(fine[peak_idx]) < np.mean(fine[base_idx])


def test_ppg_fine_continuous_across_dac_steps():
    s = siggen.simulate(siggen.EcgParams(duration_s=20), siggen.PpgParams(dac_period_frames=10, noise_na=3.0))
    steps = np.flatnonzero(np.diff(s.ppg.dac12.astype(int)))
    assert len(steps) >= 20
    d = np.abs(np.diff(s.ppg.fine))
    assert np.all(d[steps] < 4 * np.median(d) + 40)  # no jump of order 11 x DAC LSB at the steps
    assert np.all(s.ppg.disturbed[steps + 1])


def test_led_step_schedule(sim):
    s = sim("led_step")
    assert s.ppg.led_code[1499] != s.ppg.led_code[1500] and s.ppg.disturbed[1500]
    assert siggen.schedule_changes(s.ppg.led_code)[-1][0] == 1500


def test_flicker_in_dark_and_on_samples():
    p = siggen.PpgParams(flicker_hz=100, flicker_depth=0.5, ambient_ua=0.5, noise_na=0)
    s = siggen.simulate(siggen.EcgParams(duration_s=4), p)
    f = s.ppg.frames.astype(float)
    # 100 Hz flicker sampled at 100 Hz is constant, 120 Hz is not; check the 120 Hz case varies
    s2 = siggen.simulate(siggen.EcgParams(duration_s=4), siggen.PpgParams(flicker_hz=120, flicker_depth=0.5, ambient_ua=0.5, noise_na=0))
    assert np.std(s2.ppg.frames[:, 0].astype(float)) > 3 * max(np.std(f[:, 0]), 0.3)
