import numpy as np
import pytest
from scipy import signal

from ecgcard import refdsp, siggen

ECG_NAMES = ["clean_60bpm", "fast_180bpm", "slow_40bpm", "low_amp_noisy", "mains50", "mains60",
             "wander", "tall_t", "hrv", "both_pat200", "ectopic", "gain1098"]


def test_decimation_filter_response():
    h = refdsp.decimation_filter()
    w, H = signal.freqz(h, worN=8192, fs=4000)
    a = 20 * np.log10(np.abs(H) + 1e-12)
    assert np.abs(a[w <= 150]).max() < 0.5          # passband flat to 150 Hz
    assert a[w >= 250].max() < -40                  # >= 40 dB at 250 Hz and beyond


def test_decimate_alignment_and_length():
    n = 8 * 500
    t = np.arange(n) / 4000
    x = 100 * np.sin(2 * np.pi * 10 * t)
    y = refdsp.decimate_8(x)
    assert len(y) == n // 8
    assert np.max(np.abs(y[50:-50] - x[::8][50:-50])) < 1.0  # zero delay, unity gain


def test_decimate_rejects_alias_band():
    t = np.arange(4000 * 4) / 4000
    y = refdsp.decimate_8(100 * np.sin(2 * np.pi * 300 * t))  # would alias to 200 Hz
    assert np.abs(y[100:-100]).max() < 1.5
    assert refdsp.to_stream_units(np.array([2048.4, 2049.0, 2047.0]), 2048).tolist() == [3, 8, -8]


def test_display_filter_notch_and_hp():
    t = np.arange(500 * 20) / 500
    x = 500 * np.sin(2 * np.pi * 50 * t) + 1000 + 200 * np.sin(2 * np.pi * 10 * t)
    y = refdsp.display_filter(x, 50.0)
    seg = slice(2000, None)
    spec = np.abs(np.fft.rfft(y[seg] * np.hanning(len(y[seg]))))
    f = np.fft.rfftfreq(len(y[seg]), 1 / 500)
    assert spec[np.argmin(abs(f - 50))] < 0.01 * spec[np.argmin(abs(f - 10))]
    assert abs(y[seg].mean()) < 5  # DC removed


@pytest.mark.parametrize("name", ECG_NAMES)
def test_r_peak_detection(sim, name):
    s = sim(name)
    x = refdsp.decimate_8(s.ecg_codes)
    pk = refdsp.pan_tompkins(x) / 500.0
    truth = np.array(s.truth()["r_peak_times_s"])
    tp, fp, fn, err = refdsp.match_events(pk, truth, 0.05)
    se, ppv = tp / (tp + fn), tp / (tp + fp)
    assert se >= 0.97 and ppv >= 0.97, (name, tp, fp, fn)
    assert np.abs(err).max() <= 0.010           # within 5 samples at 500 Hz
    assert abs(err.mean()) <= 0.004


def test_hrv_on_hrv_fixture(sim):
    s = sim("hrv")
    pk = refdsp.pan_tompkins(refdsp.decimate_8(s.ecg_codes)) / 500.0
    h = refdsp.hrv_stats(np.diff(pk))
    t = s.truth()
    assert abs(h["hr_bpm"] - t["hr_bpm"]) < 0.5
    assert abs(h["sdnn_ms"] - t["sdnn_ms"]) < 3
    assert abs(h["rmssd_ms"] - t["rmssd_ms"]) < 4


def test_hrv_known_values():
    h = refdsp.hrv_stats(np.array([1.0, 1.1, 0.9, 1.0]))
    assert np.isclose(h["sdnn_ms"], np.std([1000, 1100, 900, 1000], ddof=1))
    assert np.isclose(h["rmssd_ms"], np.sqrt(np.mean(np.array([100, -200, 100]) ** 2)))


@pytest.mark.parametrize("name", ["clean_60bpm", "slow_40bpm", "mains50", "mains60", "wander", "tall_t",
                                  "hrv", "both_pat200", "ectopic", "gain1098", "fast_180bpm", "low_amp_noisy"])
def test_ppg_foot_and_pat(sim, name):
    s = sim(name)
    r = np.array(s.truth()["r_peak_times_s"])
    t = np.arange(len(s.ppg.fine)) / 100 + 0.00995
    feet = refdsp.ppg_feet(t, -s.ppg.fine.astype(float), r)
    ok = np.isfinite(feet)
    truth_pat = np.array(s.truth()["pat_s"])
    n = len(truth_pat)  # beats whose pulse falls inside the recording
    pat = refdsp.pat_from_feet(r, feet)[:n]
    ok = ok[:n]
    assert ok.mean() > 0.9
    err = pat[ok] - truth_pat[ok]
    assert np.median(np.abs(err)) < 0.008
    assert np.percentile(np.abs(err), 90) < 0.025
    assert abs(np.mean(pat[ok]) - s.ppg_params.pat_ms / 1000) < 0.008


def test_ppg_feet_nan_without_pulse():
    t = np.arange(1000) / 100
    feet = refdsp.ppg_feet(t, np.zeros(1000), np.array([2.0, 3.0]))
    assert np.isnan(feet).all()
