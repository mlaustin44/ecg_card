"""Capture session: bytes in -> raw file, decoder, RecordingBuilder, live statistics."""
from __future__ import annotations

import threading
import time
from pathlib import Path
from typing import Callable, Iterable

from .codec import StreamDecoder
from .record import Recording, RecordingBuilder


class CaptureSession:
    """Feed it byte chunks (from any thread, serialised by a lock)."""

    def __init__(self, raw_path: str | Path | None = None) -> None:
        self.decoder = StreamDecoder()
        self.builder = RecordingBuilder()
        self.lock = threading.Lock()
        self._raw = open(raw_path, "wb") if raw_path else None
        self.t_start = time.monotonic()
        self._last = (self.t_start, 0, 0)  # time, frames, bytes
        self.last_hr: int | None = None
        self.last_pat_ms: int | None = None
        self.last_pi: int | None = None

    def feed(self, data: bytes) -> None:
        if not data:
            return
        if self._raw:
            self._raw.write(data)
        with self.lock:
            msgs = self.decoder.feed(data)
            self.builder.add_all(msgs)
            for m in msgs:
                if m.__class__.__name__ == "RPeakEvent" and m.hr_bpm:
                    self.last_hr = m.hr_bpm
                elif m.__class__.__name__ == "PulseEvent":
                    self.last_pat_ms = m.pat_ms or None
                    self.last_pi = m.pi_permille

    def close(self) -> None:
        if self._raw:
            self._raw.close()
            self._raw = None

    def snapshot(self, last_s: float | None = None) -> Recording:
        with self.lock:
            return self.builder.build(self.decoder.stats.as_dict(), last_s)

    def stats_line(self) -> str:
        """One line of live statistics; frames/s and B/s are measured since the previous call."""
        now = time.monotonic()
        with self.lock:
            st = self.decoder.stats
            frames = st.frames_ok
            nbytes = st.bytes
            line = (f"t={now - self.t_start:6.1f}s  frames={frames}  ecg={self.builder.ecg_blocks}  "
                    f"ppg={self.builder.ppg_blocks}  "
                    f"bad_crc={st.bad_crc} bad_len={st.bad_length} bad_cobs={st.bad_cobs} "
                    f"unknown={st.unknown_type}  seq_lost={st.seq_lost}  "
                    f"dup={self.builder.dup_ecg_blocks + self.builder.dup_ppg_blocks}")
        t0, f0, b0 = self._last
        dt = max(now - t0, 1e-6)
        hr = f"{self.last_hr} bpm" if self.last_hr else "--"
        pat = f"  PAT {self.last_pat_ms} ms" if self.last_pat_ms else ""
        self._last = (now, frames, nbytes)
        return f"{line}  {(frames - f0) / dt:5.1f} fr/s  {(nbytes - b0) / dt:6.0f} B/s  HR {hr}{pat}"


def run_capture(source: Iterable[bytes], session: CaptureSession, stats_every_s: float = 1.0,
                duration_s: float | None = None, printer: Callable[[str], None] | None = None,
                stop: Callable[[], bool] | None = None) -> None:
    """Pump a source into a session, printing statistics periodically."""
    t_end = time.monotonic() + duration_s if duration_s else None
    next_stat = time.monotonic() + stats_every_s
    for chunk in source:
        session.feed(chunk)
        now = time.monotonic()
        if printer and now >= next_stat:
            printer(session.stats_line())
            next_stat = now + stats_every_s
        if (t_end and now > t_end) or (stop and stop()):
            break
