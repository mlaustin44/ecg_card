"""Floating-point reference models of the firmware DSP (clarity over speed).

These are the numbers to compare the fixed-point C implementation against.
"""
from __future__ import annotations

import numpy as np
from scipy import interpolate, signal

from .hrv import hrv_stats  # re-exported

__all__ = ["decimation_filter", "decimate_8", "to_stream_units", "display_filter",
           "pan_tompkins", "hrv_stats", "ppg_feet", "pat_from_feet", "match_events"]

RAW_FS = 4000
FS = 500
DECIM = 8


def decimation_filter(numtaps: int = 127) -> np.ndarray:
    """Equiripple low-pass FIR for 4 kHz: pass 0-150 Hz, stop from 250 Hz (>= 40 dB)."""
    return signal.remez(numtaps, [0, 150, 250, RAW_FS / 2], [1, 0], weight=[1, 10], fs=RAW_FS)


def decimate_8(codes: np.ndarray, taps: np.ndarray | None = None) -> np.ndarray:
    """4 kHz ADC codes -> 500 Hz, in ADC-code units (float).

    Linear-phase FIR with its group delay removed, so output sample i is aligned with
    input sample 8 i (time i / 500 s). The edges are zero-padded with the first/last value.
    Note: the firmware stream keeps its decimator delay (stream sample m is at
    m / 500 - 0.010 s); streamsim.stream_ecg converts this output to that timeline.
    """
    h = decimation_filter() if taps is None else taps
    x = np.asarray(codes, dtype=float)
    d = (len(h) - 1) // 2
    xp = np.concatenate([np.full(d, x[0]), x, np.full(d, x[-1])])
    y = np.convolve(xp, h, mode="valid")  # len == len(x)
    return y[::DECIM]


def to_stream_units(decimated_codes: np.ndarray, mid_code: float) -> np.ndarray:
    """What the firmware sends: int16 of (code - mid) x 8."""
    return np.clip(np.round((decimated_codes - mid_code) * 8), -32768, 32767).astype(np.int16)


def display_filter(x: np.ndarray, notch_hz: float = 50.0, fs: float = FS, causal: bool = True) -> np.ndarray:
    """0.5 Hz high-pass (2nd order), notch at 50/60 Hz (Q = 30), 40 Hz low-pass (4th order)."""
    sos = np.vstack([
        signal.butter(2, 0.5, "highpass", fs=fs, output="sos"),
        signal.tf2sos(*signal.iirnotch(notch_hz, 30.0, fs=fs)),
        signal.butter(4, 40, "lowpass", fs=fs, output="sos"),
    ])
    x = np.asarray(x, dtype=float)
    if causal:
        return signal.sosfilt(sos, x, zi=signal.sosfilt_zi(sos) * x[0])[0]
    return signal.sosfiltfilt(sos, x)


def pan_tompkins(ecg: np.ndarray, fs: float = FS) -> np.ndarray:
    """Pan-Tompkins R-peak detector. Returns peak sample indices (at `fs`).

    Band-pass 5-15 Hz, derivative, squaring, 150 ms moving-window integration, then
    adaptive signal/noise thresholds with a 200 ms refractory period and T-wave rejection
    (slope test inside 360 ms). Each detection is refined to the maximum of |band-passed
    signal| around the integrator peak.
    """
    x = np.asarray(ecg, dtype=float)
    x = x - np.nanmean(x)
    bp = signal.sosfiltfilt(signal.butter(2, [5, 15], "bandpass", fs=fs, output="sos"), x)
    der = np.gradient(bp) * fs
    win = int(round(0.150 * fs))
    integ = np.convolve(der ** 2, np.ones(win) / win, mode="same")

    cand, _ = signal.find_peaks(integ, distance=int(0.2 * fs))
    if cand.size == 0:
        return np.array([], dtype=int)
    n_init = min(int(2 * fs), len(integ))
    spki = 0.5 * np.max(integ[:n_init])
    npki = 0.1 * np.mean(integ[:n_init])
    thr = npki + 0.25 * (spki - npki)

    half = int(0.12 * fs)
    peaks: list[int] = []
    last_slope = 0.0
    for c in cand:
        pk = integ[c]
        lo, hi = max(c - half * 2, 0), min(c + half, len(bp))
        r = lo + int(np.argmax(np.abs(bp[lo:hi])))
        slope = np.max(np.abs(der[max(r - int(0.05 * fs), 0):r + int(0.05 * fs) + 1]))
        if pk > thr:
            if peaks and (r - peaks[-1]) < 0.36 * fs and slope < 0.5 * last_slope:
                npki = 0.125 * pk + 0.875 * npki  # T wave
            elif peaks and r - peaks[-1] < 0.2 * fs:
                continue
            else:
                peaks.append(r)
                last_slope = slope
                spki = 0.125 * pk + 0.875 * spki
        else:
            npki = 0.125 * pk + 0.875 * npki
        thr = npki + 0.25 * (spki - npki)
    # search-back for a missed beat when an interval exceeds 1.66 x the running mean RR
    out = sorted(set(peaks))
    return np.array(out, dtype=int)


def ppg_feet(t: np.ndarray, y: np.ndarray, r_times: np.ndarray, lowpass_hz: float = 10.0) -> np.ndarray:
    """Pulse foot by intersecting tangents, one search per R-peak.

    `y` must have systole pointing UP: the stream fine channel as is (firmware convention,
    fine = -photocurrent). `t` and `r_times` are on the common timeline; pass true R-peak
    times (record.r_true_time of stream indices) for PAT. For each R-peak the steepest rising
    point is searched in [R + 0.05 s, R + min(0.6 s, RR - 0.03 s)]; the foot is where
    the tangent there meets the horizontal through the preceding minimum. NaN if absent.
    """
    t = np.asarray(t, float)
    y = np.asarray(y, float)
    ok = np.isfinite(y)
    t, y = t[ok], y[ok]
    fs = 1.0 / np.median(np.diff(t))
    ys = signal.sosfiltfilt(signal.butter(2, lowpass_hz, "lowpass", fs=fs, output="sos"), y)
    spl = interpolate.CubicSpline(t, ys)
    dspl = spl.derivative()
    r_times = np.asarray(r_times, float)
    feet = np.full(len(r_times), np.nan)
    for i, r in enumerate(r_times):
        rr = (r_times[i + 1] - r) if i + 1 < len(r_times) else 1.0
        lo, hi = r + 0.05, r + min(0.6, rr - 0.03)
        if lo < t[0] or hi > t[-1] or hi <= lo:
            continue
        tf = np.arange(lo, hi, 0.001)
        s = dspl(tf)
        k = int(np.argmax(s))
        if s[k] <= 0:
            continue
        tm = tf[k]
        before = np.arange(max(tm - 0.35, t[0]), tm, 0.001)
        ymin = spl(before).min()
        feet[i] = tm - (spl(tm) - ymin) / s[k]
    return feet


def pat_from_feet(r_times: np.ndarray, feet: np.ndarray) -> np.ndarray:
    """PAT (s) = foot - R for each beat; NaN where no foot."""
    return np.asarray(feet, float) - np.asarray(r_times, float)


def match_events(detected: np.ndarray, truth: np.ndarray, tol: float) -> tuple[int, int, int, np.ndarray]:
    """Greedy one-to-one matching within +-tol. Returns (TP, FP, FN, signed errors det-truth)."""
    det = np.sort(np.asarray(detected, float))
    tru = np.sort(np.asarray(truth, float))
    used = np.zeros(len(det), bool)
    errs = []
    tp = 0
    for tt in tru:
        if det.size == 0:
            break
        j = int(np.argmin(np.abs(det - tt)))
        if not used[j] and abs(det[j] - tt) <= tol:
            used[j] = True
            tp += 1
            errs.append(det[j] - tt)
    return tp, int((~used).sum()), len(tru) - tp, np.array(errs)
