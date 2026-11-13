#!/usr/bin/env python3
"""Capture the ECG Card stream, decode it and save the raw bytes and a Recording.

Examples (see tools/README.md):
  JLinkRTTLogger -Device MSPM0G3507 -If SWD -Speed 4000 -RTTChannel 1 build/rtt.bin   # terminal 1
  python3 tools/stream_capture.py --follow build/rtt.bin --npz build/rec.npz         # terminal 2
  python3 tools/stream_capture.py --file build/rtt.bin --npz build/rec.npz --csv build/rec
"""
from __future__ import annotations

import argparse
import signal
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ecgcard.capture import CaptureSession, run_capture  # noqa: E402
from ecgcard.sources import add_source_args, open_source  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_source_args(ap)
    ap.add_argument("--raw", metavar="PATH", help="also write the raw received bytes here")
    ap.add_argument("--npz", metavar="PATH", help="write the decoded Recording (.npz) at the end")
    ap.add_argument("--csv", metavar="BASE", help="also export BASE.ecg.csv, BASE.ppg.csv, BASE.events.csv")
    ap.add_argument("--duration", type=float, metavar="S", help="stop after S seconds")
    ap.add_argument("--stats-every", type=float, default=1.0, metavar="S", help="statistics interval (default 1)")
    ap.add_argument("-q", "--quiet", action="store_true", help="no live statistics")
    a = ap.parse_args(argv)

    stopping = {"flag": False}
    signal.signal(signal.SIGINT, lambda *_: stopping.__setitem__("flag", True))
    stop = lambda: stopping["flag"]  # noqa: E731
    session = CaptureSession(a.raw)
    printer = None if a.quiet else (lambda s: print(s, file=sys.stderr, flush=True))
    try:
        run_capture(open_source(a, stop), session, a.stats_every, a.duration, printer, stop)
    finally:
        session.close()
    rec = session.snapshot()
    if printer:
        printer(session.stats_line())
    if a.npz:
        rec.save(a.npz)
        print(f"wrote {a.npz}: {rec.n_ecg} ECG samples, {rec.n_ppg} PPG frames, "
              f"{len(rec.r_peaks)} R-peaks", file=sys.stderr)
    if a.csv:
        for p in rec.export_csv(a.csv):
            print(f"wrote {p}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
