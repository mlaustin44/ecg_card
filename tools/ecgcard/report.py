"""Offline PNG report for a Recording (no pyplot: works headless)."""
from __future__ import annotations

from pathlib import Path

import numpy as np
from matplotlib.backends.backend_agg import FigureCanvasAgg
from matplotlib.figure import Figure

from . import refdsp
from .hrv import hrv_stats
from .record import ECG_HZ, Recording, ecg_index, ecg_time

ECG_C, PPG_C, MARK_C, FOOT_C = "#1f6fb2", "#b5481f", "#c0272d", "#2a8a4a"


def longest_valid_run(valid: np.ndarray) -> tuple[int, int]:
    """(start, stop) of the longest run of True."""
    if not valid.any():
        return 0, 0
    d = np.diff(np.concatenate(([0], valid.astype(np.int8), [0])))
    s, e = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    k = int(np.argmax(e - s))
    return int(s[k]), int(e[k])


def amplitude_spectrum(x_uv: np.ndarray, fs: float = ECG_HZ) -> tuple[np.ndarray, np.ndarray]:
    """Single-sided amplitude spectrum (peak amplitude in the unit of x) with a Hann window."""
    x = x_uv - np.mean(x_uv)
    w = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * w)) * 2.0 / np.sum(w)
    return np.fft.rfftfreq(len(x), 1 / fs), spec


def r_peaks_of(rec: Recording) -> tuple[np.ndarray, str]:
    """R-peak times on the stream timeline (where the R appears in rec.ecg_t): device events
    when present, otherwise the reference detector."""
    t = rec.r_peak_times()
    if len(t) >= 2:
        return np.sort(t), "device events"
    uv = rec.ecg_uv
    a, b = longest_valid_run(rec.ecg_valid)
    if b - a < 3 * ECG_HZ:
        return np.array([]), "none"
    pk = refdsp.pan_tompkins(uv[a:b])
    return ecg_time(pk + a + rec.ecg_offset).astype(float), "reference detector"


def summary_rows(rec: Recording, r_t: np.ndarray, source: str) -> list[tuple[str, str]]:
    rr = np.diff(r_t)
    rr = rr[(rr > 0.25) & (rr < 2.5)]
    h = hrv_stats(rr)
    d = rec.meta.get("decoder", {})
    ecg_missing = int((~rec.ecg_valid).sum())
    ppg_missing = int((~rec.ppg_valid).sum())
    rows = [
        ("Duration", f"{rec.duration_s:.1f} s"),
        ("R-peaks (source)", f"{len(r_t)} ({source})"),
        ("HR mean / min / max", f"{h['hr_bpm']:.1f} / {h['hr_min']:.1f} / {h['hr_max']:.1f} bpm"),
        ("SDNN / RMSSD", f"{h['sdnn_ms']:.1f} / {h['rmssd_ms']:.1f} ms"),
        ("ECG samples missing", f"{ecg_missing} ({100 * ecg_missing / max(rec.n_ecg, 1):.2f} %)"),
        ("PPG frames missing", f"{ppg_missing} ({100 * ppg_missing / max(rec.n_ppg, 1):.2f} %)"),
        ("Frames lost (seq) / gaps", f"{d.get('seq_lost', 0)} / {d.get('seq_gaps', 0)}"),
        ("Bad CRC / length / COBS", f"{d.get('bad_crc', 0)} / {d.get('bad_length', 0)} / {d.get('bad_cobs', 0)}"),
        ("Duplicate blocks (ECG/PPG)", f"{rec.meta.get('dup_ecg_blocks', 0)} / {rec.meta.get('dup_ppg_blocks', 0)}"),
    ]
    pats = [e["pat_ms"] for e in rec.pulses if e.get("pat_ms")]
    if pats:
        rows.append(("PAT (device) mean +- sd", f"{np.mean(pats):.0f} +- {np.std(pats):.0f} ms (n={len(pats)})"))
    return rows


def make_report(rec: Recording, png_path: str | Path, title: str = "ECG Card recording",
                zoom_s: float = 5.0) -> dict:
    """Write the PNG report; returns the summary as a dict (strings)."""
    r_t, source = r_peaks_of(rec)
    rows = summary_rows(rec, r_t, source)
    has_ppg = bool(rec.ppg_valid.any())
    uv, t = rec.ecg_uv, rec.ecg_t

    fig = Figure(figsize=(14, 14 if has_ppg else 12), dpi=100, layout="constrained")
    FigureCanvasAgg(fig)
    gs = fig.add_gridspec(5 if has_ppg else 4, 2, height_ratios=[1, 1, 1.2, 1.1, 0.9] if has_ppg else [1, 1.2, 1.1, 0.9])
    row = 0
    fig.suptitle(title, fontsize=14)

    # full ECG
    ax = fig.add_subplot(gs[row, :]); row += 1
    ax.plot(t, uv, lw=0.5, color=ECG_C)
    if len(r_t):
        idx = np.clip(np.round(ecg_index(r_t)).astype(int) - rec.ecg_offset, 0, rec.n_ecg - 1)
        ax.plot(r_t, uv[idx], "v", ms=4, color=MARK_C, label="R-peak")
        ax.legend(loc="upper right", fontsize=8)
    ax.set_ylabel("ECG (uV)"); ax.set_xlabel("time (s)"); ax.set_title("ECG, full recording", fontsize=10, loc="left")
    ax.grid(alpha=0.3)

    # full PPG
    if has_ppg:
        ax = fig.add_subplot(gs[row, :]); row += 1
        p = rec.ppg_systole_up
        ax.plot(rec.ppg_t, p - np.nanmedian(p), lw=0.7, color=PPG_C)
        ax.set_ylabel("PPG fine, systole up\n(80.6 pA codes)"); ax.set_xlabel("time (s)")
        ax.set_title("PPG fine channel (-photocurrent, offset removed)", fontsize=10, loc="left")
        ax.grid(alpha=0.3)

    # zoom
    ax = fig.add_subplot(gs[row, :]); row += 1
    a, b = longest_valid_run(rec.ecg_valid)
    z0 = float(ecg_time(a + rec.ecg_offset)) + 0.4 * ((b - a) / ECG_HZ - zoom_s) if b > a else 0.0
    z0 = max(z0, 0.0); z1 = z0 + zoom_s
    m = (t >= z0) & (t < z1)
    ax.plot(t[m], uv[m], lw=1.0, color=ECG_C, label="ECG")
    ax.set_xlim(z0, z1); ax.set_ylabel("ECG (uV)"); ax.set_xlabel("time (s)")
    for x in r_t[(r_t >= z0) & (r_t < z1)]:
        ax.axvline(x, color=MARK_C, lw=0.6, alpha=0.5)
    ax.set_title(f"Zoom {zoom_s:g} s", fontsize=10, loc="left"); ax.grid(alpha=0.3)
    if has_ppg:
        ax2 = ax.twinx()
        pt, p = rec.ppg_t, rec.ppg_systole_up
        mp = (pt >= z0) & (pt < z1)
        if mp.any():
            ax2.plot(pt[mp], p[mp] - np.nanmedian(p[mp]), lw=1.0, color=PPG_C, label="PPG")
        ft = rec.pulse_foot_times()
        for x in ft[(ft >= z0) & (ft < z1)]:
            ax2.axvline(x, color=FOOT_C, lw=1.0, ls="--")
        ax2.set_ylabel("PPG (codes)", color=PPG_C)

    # spectrum
    ax = fig.add_subplot(gs[row, 0])
    if b - a >= 256:
        f, s = amplitude_spectrum(uv[a:b])
        k = f <= 250
        ax.semilogy(f[k], np.maximum(s[k], 1e-3), lw=0.8, color=ECG_C)
        for mf in (50, 100, 150, 200, 60, 120, 180, 240):
            ax.axvline(mf, color="grey", lw=0.5, ls=":")
        ax.set_xlim(0, 250)
    ax.set_xlabel("frequency (Hz)"); ax.set_ylabel("amplitude (uV)")
    ax.set_title("ECG amplitude spectrum (dotted: 50/60 Hz and harmonics)", fontsize=10, loc="left")
    ax.grid(alpha=0.3, which="both")

    # tachogram
    ax = fig.add_subplot(gs[row, 1]); row += 1
    if len(r_t) > 2:
        rr = np.diff(r_t) * 1000
        ax.plot(r_t[1:], rr, "o-", ms=3, lw=0.8, color=ECG_C)
    ax.set_xlabel("time (s)"); ax.set_ylabel("RR (ms)")
    ax.set_title("RR tachogram", fontsize=10, loc="left"); ax.grid(alpha=0.3)

    # table
    ax = fig.add_subplot(gs[row, :])
    ax.axis("off")
    tbl = ax.table(cellText=[[k, v] for k, v in rows], colLabels=["Quantity", "Value"],
                   loc="center", cellLoc="left", colWidths=[0.3, 0.5])
    tbl.auto_set_font_size(False); tbl.set_fontsize(10); tbl.scale(1, 1.25)
    fig.savefig(png_path)
    return dict(rows)
