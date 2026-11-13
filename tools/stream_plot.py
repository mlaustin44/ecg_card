#!/usr/bin/env python3
"""Live scrolling plot of the ECG Card stream, or an offline PNG report.

Live:     python3 tools/stream_plot.py --follow build/rtt.bin [--window 8]
Offline:  python3 tools/stream_plot.py --report build/rec.npz --png build/report.png
          python3 tools/stream_plot.py --report build/rtt.bin --png build/report.png
(--report accepts a Recording .npz or a raw stream file; offline mode needs no display.)
"""
from __future__ import annotations

import argparse
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

from ecgcard.capture import CaptureSession, run_capture  # noqa: E402
from ecgcard.record import Recording, assemble  # noqa: E402
from ecgcard.codec import decode_stream  # noqa: E402
from ecgcard.report import make_report  # noqa: E402
from ecgcard.sources import add_source_args, open_source  # noqa: E402


def load_any(path: str) -> Recording:
    if path.endswith(".npz"):
        return Recording.load(path)
    msgs, st = decode_stream(Path(path).read_bytes())
    return assemble(msgs, st.as_dict())


def live(a) -> int:
    import matplotlib
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation

    if matplotlib.get_backend().lower() == "agg":
        print("no display backend available; use --report for an offline PNG", file=sys.stderr)
        return 2
    stop_evt = threading.Event()
    session = CaptureSession(a.raw)
    th = threading.Thread(target=run_capture, daemon=True,
                          args=(open_source(a, stop_evt.is_set), session, 1e9, None, None, stop_evt.is_set))
    th.start()

    fig, (ax_e, ax_p) = plt.subplots(2, 1, figsize=(12, 7), sharex=True)
    (l_e,) = ax_e.plot([], [], lw=0.9, color="#1f6fb2")
    (m_r,) = ax_e.plot([], [], "v", color="#c0272d", ms=6, label="R-peak")
    (l_p,) = ax_p.plot([], [], lw=1.0, color="#b5481f")
    (m_f,) = ax_p.plot([], [], "^", color="#2a8a4a", ms=6, label="pulse foot")
    ax_e.set_ylabel("ECG (uV)"); ax_p.set_ylabel("PPG fine, systole up"); ax_p.set_xlabel("time (s)")
    for ax in (ax_e, ax_p):
        ax.grid(alpha=0.3); ax.legend(loc="upper left", fontsize=8)
    txt = ax_e.text(0.99, 0.95, "", transform=ax_e.transAxes, ha="right", va="top", family="monospace")

    def update(_):
        rec = session.snapshot(a.window + 1)
        if rec.ecg_valid.any():
            uv, t = rec.ecg_uv, rec.ecg_t
            l_e.set_data(t, uv)
            rt = rec.r_peak_times()
            idx = np.round(rt * 500).astype(int) - rec.ecg_offset
            ok = (idx >= 0) & (idx < len(uv))
            m_r.set_data(rt[ok], uv[idx[ok]])
            lo, hi = np.nanmin(uv), np.nanmax(uv)
            pad = max(0.15 * (hi - lo), 50)
            ax_e.set_ylim(lo - pad, hi + pad)
            ax_e.set_xlim(t[-1] - a.window, t[-1] + 0.2)
        if rec.ppg_valid.any():
            p = rec.ppg_pulse
            pt = rec.ppg_t
            l_p.set_data(pt, p - np.nanmedian(p))
            ft = rec.pulse_foot_times()
            ok = (ft >= pt[0]) & (ft <= pt[-1])
            y = np.interp(ft[ok], pt[np.isfinite(p)], (p - np.nanmedian(p))[np.isfinite(p)]) if ok.any() else []
            m_f.set_data(ft[ok], y)
            q = p[np.isfinite(p)] - np.nanmedian(p)
            lo, hi = np.percentile(q, [0.5, 99.5])
            ax_p.set_ylim(lo - 0.2 * (hi - lo) - 1, hi + 0.2 * (hi - lo) + 1)
        hr = f"{session.last_hr} bpm" if session.last_hr else "--"
        pat = f"{session.last_pat_ms} ms" if session.last_pat_ms else "--"
        st = session.decoder.stats
        txt.set_text(f"HR {hr}   PAT {pat}\nCRC err {st.bad_crc}  lost {st.seq_lost}")
        return l_e, m_r, l_p, m_f, txt

    _anim = FuncAnimation(fig, update, interval=100, cache_frame_data=False)
    try:
        plt.show()
    finally:
        stop_evt.set()
        th.join(timeout=2)
        session.close()
        if a.npz:
            session.snapshot().save(a.npz)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_source_args(ap)
    ap.add_argument("--report", metavar="INPUT", help="offline: Recording .npz or raw stream file")
    ap.add_argument("--png", metavar="PATH", help="offline: output PNG (default <INPUT>.png)")
    ap.add_argument("--zoom", type=float, default=5.0, help="offline: zoom window in seconds (default 5)")
    ap.add_argument("--window", type=float, default=8.0, help="live: visible seconds (default 8)")
    ap.add_argument("--raw", metavar="PATH", help="live: also save the raw bytes")
    ap.add_argument("--npz", metavar="PATH", help="live: save the Recording on exit")
    a = ap.parse_args(argv)

    if a.report:
        import matplotlib
        matplotlib.use("Agg")
        rec = load_any(a.report)
        png = a.png or str(Path(a.report).with_suffix(".png"))
        rows = make_report(rec, png, title=Path(a.report).name, zoom_s=a.zoom)
        for k, v in rows.items():
            print(f"{k:28s} {v}")
        print(f"wrote {png}")
        return 0
    return live(a)


if __name__ == "__main__":
    raise SystemExit(main())
