#!/usr/bin/env python3
"""Check a raw stream capture (bytes as sent by the device: COBS frames + 0x00).

    check_capture.py FILE [--min-ecg-blocks N] [--min-ppg-blocks N]
                          [--require-events KIND,...] [-q]

Prints a short summary: frames, per-type counts, decoder errors, sequence losses, ECG/PPG
block-index continuity, event counts, mean HR and PAT from the events.

Exit status: 0 = all checks passed; 1 = a check failed (any bad CRC, bad length or bad
COBS frame; a gap or repeat in ECG_BLOCK start indices (step != 10) or PPG_BLOCK start
indices (step != 2) within a run; a minimum not met; a required event kind absent);
2 = usage error or unreadable file. A block whose start index goes back to 0 begins a new
acquisition run and is not a gap. Lost frames by sequence number, unknown types and
trailing bytes without a delimiter are reported but do not fail the check on their own.
KIND is one of r_peak, pulse, lead, state, button.
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ecgcard import codec  # noqa: E402

EVENT_KINDS = {"r_peak": codec.RPeakEvent, "pulse": codec.PulseEvent, "lead": codec.LeadEvent,
               "state": codec.StateEvent, "button": codec.ButtonEvent}
TYPE_NAMES = {codec.STREAM_ECG_BLOCK: "ecg_block", codec.STREAM_PPG_BLOCK: "ppg_block",
              codec.STREAM_EVENT: "event", codec.STREAM_LOG: "log"}


@dataclass
class Continuity:
    """Start-index continuity of one block type (in arrival order)."""
    step: int
    blocks: int = 0
    runs: int = 0
    gaps: int = 0               # steps != `step` within a run (also repeats / reorders)
    first_gaps: list = field(default_factory=list)  # (previous start, start), first few
    _prev: int | None = None

    def add(self, start: int) -> None:
        self.blocks += 1
        if self._prev is None or start == 0:  # first block, or a new acquisition run
            self.runs += 1
        elif start - self._prev != self.step:
            self.gaps += 1
            if len(self.first_gaps) < 5:
                self.first_gaps.append((self._prev, start))
        self._prev = start


def check(data: bytes, min_ecg: int = 0, min_ppg: int = 0,
          require: tuple[str, ...] = ()) -> tuple[list[str], list[str]]:
    """Returns (summary lines, problems). The capture passes if problems is empty."""
    msgs, st = codec.decode_stream(data)
    ecg, ppg = Continuity(codec.ECG_SAMPLES_PER_BLOCK), Continuity(codec.PPG_FRAMES_PER_BLOCK)
    ev = {k: [] for k in EVENT_KINDS}
    for m in msgs:
        if isinstance(m, codec.EcgBlock):
            ecg.add(m.start)
        elif isinstance(m, codec.PpgBlock):
            ppg.add(m.start)
        else:
            for k, cls in EVENT_KINDS.items():
                if isinstance(m, cls):
                    ev[k].append(m)
    trailing = len(data) - (data.rfind(b"\x00") + 1)

    by_type = " ".join(f"{TYPE_NAMES.get(t, hex(t))}={n}" for t, n in sorted(st.by_type.items()))
    hr = [e.hr_bpm for e in ev["r_peak"] if e.hr_bpm]
    pat = [e.pat_ms for e in ev["pulse"] if e.pat_ms]
    pr = [e.pulse_bpm for e in ev["pulse"] if e.pulse_bpm]
    lines = [
        f"bytes {st.bytes}  frames {st.frames_ok}  ({by_type or 'none'})",
        f"bad_crc {st.bad_crc}  bad_length {st.bad_length}  bad_cobs {st.bad_cobs}  "
        f"unknown_type {st.unknown_type}  seq_lost {st.seq_lost} (gaps {st.seq_gaps}, "
        f"repeats {st.seq_repeats})  trailing_bytes {trailing}",
        f"ECG blocks {ecg.blocks} in {ecg.runs} run(s), index gaps {ecg.gaps}",
        f"PPG blocks {ppg.blocks} in {ppg.runs} run(s), index gaps {ppg.gaps}",
        "events  " + "  ".join(f"{k} {len(v)}" for k, v in ev.items()),
        "mean HR " + (f"{sum(hr) / len(hr):.1f} bpm (n={len(hr)})" if hr else "--")
        + "  mean pulse rate " + (f"{sum(pr) / len(pr):.1f} bpm" if pr else "--")
        + "  mean PAT " + (f"{sum(pat) / len(pat):.1f} ms (n={len(pat)})" if pat else "--"),
    ]

    problems = []
    for name in ("bad_crc", "bad_length", "bad_cobs"):
        if getattr(st, name):
            problems.append(f"{name} = {getattr(st, name)}")
    for label, c in (("ECG", ecg), ("PPG", ppg)):
        if c.gaps:
            ex = ", ".join(f"{a}->{b}" for a, b in c.first_gaps)
            problems.append(f"{label} block index not contiguous: {c.gaps} gap(s) (step != {c.step}; {ex})")
    if ecg.blocks < min_ecg:
        problems.append(f"ECG blocks {ecg.blocks} < {min_ecg}")
    if ppg.blocks < min_ppg:
        problems.append(f"PPG blocks {ppg.blocks} < {min_ppg}")
    for k in require:
        if not ev[k]:
            problems.append(f"no {k} event")
    return lines, problems


def _kinds(s: str) -> tuple[str, ...]:
    kinds = tuple(k.strip() for k in s.split(",") if k.strip())
    bad = [k for k in kinds if k not in EVENT_KINDS]
    if bad:
        raise argparse.ArgumentTypeError(f"unknown event kind(s) {', '.join(bad)}; "
                                         f"choose from {', '.join(EVENT_KINDS)}")
    return kinds


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", metavar="FILE", help="raw stream capture")
    ap.add_argument("--min-ecg-blocks", type=int, default=0, metavar="N", help="fail if fewer ECG blocks")
    ap.add_argument("--min-ppg-blocks", type=int, default=0, metavar="N", help="fail if fewer PPG blocks")
    ap.add_argument("--require-events", type=_kinds, default=(), metavar="KIND,...",
                    help=f"fail unless each kind occurs ({', '.join(EVENT_KINDS)})")
    ap.add_argument("-q", "--quiet", action="store_true", help="no summary; print only failures")
    a = ap.parse_args(argv)
    try:
        data = Path(a.file).read_bytes()
    except OSError as e:
        print(f"check_capture: {e}", file=sys.stderr)
        return 2
    lines, problems = check(data, a.min_ecg_blocks, a.min_ppg_blocks, a.require_events)
    if not a.quiet:
        print("\n".join(lines))
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    if not a.quiet:
        print("FAIL" if problems else "OK")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
