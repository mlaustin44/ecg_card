"""Assemble decoded stream messages into a Recording (aligned arrays on a common time axis)."""
from __future__ import annotations

import csv
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import numpy as np

from .codec import (
    ECG_FLAG_GAIN_HI, ECG_FLAG_LEAD_OFF, ECG_FLAG_SETTLING,
    PPG_FLAG_DIST0, PPG_FLAG_DIST1, PPG_FLAG_NO_FINGER,
    ButtonEvent, EcgBlock, LeadEvent, Message, PpgBlock, PulseEvent, RPeakEvent, StateEvent,
)

ECG_HZ = 500.0
PPG_HZ = 100.0
PPG_ON_SAMPLE_S = 0.00995        # LED-on ADC sample within a PPG frame
# Common timeline (seconds from the acquisition start event, fw/dsp/include/dsp_delays.h):
# ECG stream sample m (ECG_BLOCK start + i, EV_R_PEAK index, EV_PULSE foot index) is the
# signal at t = m / 500 + ECG_T0_S: the decimator FIR (95 taps) delays it by 47 raw samples,
# so m = raw time 8 m - 40 (DSP_ECG_STREAM_T0_US = -10000).
ECG_T0_S = -0.010
ECG_T0_SAMPLES = 5               # the same offset in stream samples (-ECG_T0_S * ECG_HZ)
# A true R-peak is this much earlier than its stream-sample time (analog front end delay,
# DSP_ECG_ANALOG_DELAY_US = 974). The firmware's PAT already removes both offsets.
ECG_ANALOG_DELAY_S = 0.000974
UV_PER_CODE = 805.7
GAIN_LO, GAIN_HI = 558, 1098
MAX_INDEX = 500 * 3600 * 24      # reject absurd start indices (corrupt but CRC-valid)
NA_PER_COARSE_CODE = 0.8057      # 3.3 V / 4096 / 1 Mohm
NA_PER_FINE_CODE = 0.08057


def ecg_time(index):
    """Time in seconds of ECG stream sample index (scalar or array): index / 500 + ECG_T0_S.

    Also applies to EV_R_PEAK and EV_PULSE indices (where the R-peak / foot is on the
    stream timeline). For the true R-peak time use r_true_time()."""
    return np.asarray(index) / ECG_HZ + ECG_T0_S


def ecg_index(t):
    """Inverse of ecg_time (float index)."""
    return (np.asarray(t) - ECG_T0_S) * ECG_HZ


def r_true_time(index):
    """True R-peak time (s) of an R-peak at stream index: ecg_time(index) - ECG_ANALOG_DELAY_S.

    This is the time PAT is measured from (as the firmware's dsp_pat_r_time_us)."""
    return ecg_time(index) - ECG_ANALOG_DELAY_S


def ppg_time(index):
    """Time in seconds of the LED-on sample of PPG frame index."""
    return np.asarray(index) / PPG_HZ + PPG_ON_SAMPLE_S


def unwrap_int16(raw: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Unwrap a modulo-2^16 int16 series over its valid samples; invalid -> NaN.

    Each step is taken as the smallest signed difference, so the series may change by up
    to +-32767 between consecutive valid samples (also across gaps).
    """
    out = np.full(raw.shape, np.nan)
    idx = np.flatnonzero(valid)
    if idx.size == 0:
        return out
    v = raw[idx].astype(np.int64)
    d = ((np.diff(v) + 32768) & 0xFFFF) - 32768
    out[idx] = v[0] + np.concatenate(([0], np.cumsum(d)))
    return out


@dataclass
class Recording:
    """Everything captured, on index-based arrays.

    ECG arrays have one entry per 500 Hz sample index, PPG arrays one per 100 Hz frame.
    Missing data is NaN (floats) or False in the *_valid masks.
    """
    ecg_raw: np.ndarray            # int16 stream units (code x 8), 0 where missing
    ecg_valid: np.ndarray          # bool
    ecg_flags: np.ndarray          # uint8 block flags per sample
    ppg_coarse: np.ndarray         # float, ADC codes (0.81 nA), NaN missing
    ppg_fine_raw: np.ndarray       # int16 as received (wrapped)
    ppg_valid: np.ndarray          # bool
    ppg_disturbed: np.ndarray      # bool
    ppg_no_finger: np.ndarray      # bool
    ppg_led_code: np.ndarray       # uint16 per frame
    # events: lists of dicts with at least "time_s"
    r_peaks: list[dict] = field(default_factory=list)   # sample, hr_bpm
    pulses: list[dict] = field(default_factory=list)    # foot_sample, pulse_bpm, pat_ms, pi_permille
    leads: list[dict] = field(default_factory=list)     # contact
    states: list[dict] = field(default_factory=list)    # mode, notch_hz, mid_code, batt_pct, flags
    buttons: list[dict] = field(default_factory=list)   # button, kind
    meta: dict = field(default_factory=dict)            # decoder stats, duplicate counts, t0_index...
    ecg_offset: int = 0            # absolute ECG index of element 0 (for windows)
    ppg_offset: int = 0

    # ---- derived ECG
    @property
    def n_ecg(self) -> int:
        return len(self.ecg_raw)

    @property
    def n_ppg(self) -> int:
        return len(self.ppg_coarse)

    @property
    def ecg_gain(self) -> np.ndarray:
        return np.where(self.ecg_flags & ECG_FLAG_GAIN_HI, GAIN_HI, GAIN_LO)

    @property
    def ecg_uv(self) -> np.ndarray:
        """ECG in microvolts, input-referred; NaN where samples are missing."""
        uv = self.ecg_raw.astype(float) * UV_PER_CODE / (8.0 * self.ecg_gain)
        uv[~self.ecg_valid] = np.nan
        return uv

    @property
    def ecg_settling(self) -> np.ndarray:
        return (self.ecg_flags & ECG_FLAG_SETTLING) != 0

    @property
    def ecg_lead_off(self) -> np.ndarray:
        return (self.ecg_flags & ECG_FLAG_LEAD_OFF) != 0

    @property
    def ecg_t(self) -> np.ndarray:
        """Time of each ECG sample on the common timeline (index / 500 + ECG_T0_S)."""
        return ecg_time(np.arange(self.n_ecg) + self.ecg_offset)

    # ---- derived PPG
    @property
    def ppg_fine(self) -> np.ndarray:
        """Fine channel unwrapped modulo 2^16 (80.6 pA per code), NaN where missing.

        Firmware convention (dsp_ppg_fine): fine = -(LED-induced photocurrent), so more
        blood -> less light -> fine goes up: the systolic upstroke points UP as is."""
        return unwrap_int16(self.ppg_fine_raw, self.ppg_valid)

    @property
    def ppg_systole_up(self) -> np.ndarray:
        """PPG trace with systole pointing up: the unwrapped fine channel unchanged
        (the firmware's sign convention already does this; no negation)."""
        return self.ppg_fine

    @property
    def ppg_t(self) -> np.ndarray:
        return (np.arange(self.n_ppg) + self.ppg_offset) * (1.0 / PPG_HZ) + PPG_ON_SAMPLE_S

    @property
    def ppg_dc_na(self) -> np.ndarray:
        return self.ppg_coarse * NA_PER_COARSE_CODE

    # ---- events
    def r_peak_samples(self) -> np.ndarray:
        """Stream sample indices of the R-peak events."""
        return np.array([e["sample"] for e in self.r_peaks], dtype=np.int64)

    def r_peak_times(self) -> np.ndarray:
        """R-peak event times on the stream timeline (where the R appears in ecg_t)."""
        return ecg_time(self.r_peak_samples()).astype(float)

    def r_peak_true_times(self) -> np.ndarray:
        """R-peak event times with the analog delay removed (the PAT reference)."""
        return r_true_time(self.r_peak_samples()).astype(float)

    def pulse_foot_times(self) -> np.ndarray:
        """Pulse foot times (EV_PULSE index is in ECG stream sample units)."""
        return ecg_time(np.array([e["foot_sample"] for e in self.pulses], dtype=np.int64)).astype(float)

    @property
    def duration_s(self) -> float:
        return max(self.n_ecg / ECG_HZ, self.n_ppg / PPG_HZ)

    # ---- windows
    def window(self, t0: float, t1: float) -> "Recording":
        """Slice [t0, t1) seconds (absolute); events are filtered by time."""
        e0 = max(int(np.floor(ecg_index(t0) + 1e-9)) - self.ecg_offset, 0)
        e1 = max(int(np.ceil(ecg_index(t1) - 1e-9)) - self.ecg_offset, 0)
        p0 = max(int(np.floor(t0 * PPG_HZ)) - self.ppg_offset, 0)
        p1 = max(int(np.ceil(t1 * PPG_HZ)) - self.ppg_offset, 0)
        keep = lambda ev: [e for e in ev if t0 <= e["time_s"] < t1]
        return Recording(
            self.ecg_raw[e0:e1], self.ecg_valid[e0:e1], self.ecg_flags[e0:e1],
            self.ppg_coarse[p0:p1], self.ppg_fine_raw[p0:p1], self.ppg_valid[p0:p1],
            self.ppg_disturbed[p0:p1], self.ppg_no_finger[p0:p1], self.ppg_led_code[p0:p1],
            keep(self.r_peaks), keep(self.pulses), keep(self.leads), keep(self.states),
            keep(self.buttons), dict(self.meta), self.ecg_offset + e0, self.ppg_offset + p0)

    # ---- persistence
    def save(self, path: str | Path) -> None:
        events = {k: getattr(self, k) for k in ("r_peaks", "pulses", "leads", "states", "buttons")}
        np.savez_compressed(
            path, ecg_raw=self.ecg_raw, ecg_valid=self.ecg_valid, ecg_flags=self.ecg_flags,
            ppg_coarse=self.ppg_coarse, ppg_fine_raw=self.ppg_fine_raw, ppg_valid=self.ppg_valid,
            ppg_disturbed=self.ppg_disturbed, ppg_no_finger=self.ppg_no_finger,
            ppg_led_code=self.ppg_led_code,
            offsets=np.array([self.ecg_offset, self.ppg_offset]),
            events_json=np.array(json.dumps(events)), meta_json=np.array(json.dumps(self.meta)))

    @staticmethod
    def load(path: str | Path) -> "Recording":
        with np.load(path, allow_pickle=False) as z:
            ev = json.loads(str(z["events_json"]))
            off = z["offsets"]
            return Recording(
                z["ecg_raw"], z["ecg_valid"], z["ecg_flags"], z["ppg_coarse"], z["ppg_fine_raw"],
                z["ppg_valid"], z["ppg_disturbed"], z["ppg_no_finger"], z["ppg_led_code"],
                ev["r_peaks"], ev["pulses"], ev["leads"], ev["states"], ev["buttons"],
                json.loads(str(z["meta_json"])), int(off[0]), int(off[1]))

    def export_csv(self, base: str | Path) -> list[Path]:
        """Write <base>.ecg.csv, <base>.ppg.csv and <base>.events.csv."""
        base = Path(base)
        paths = [Path(f"{base}.ecg.csv"), Path(f"{base}.ppg.csv"), Path(f"{base}.events.csv")]
        uv = self.ecg_uv
        with open(paths[0], "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["index", "time_s", "uv", "gain_hi", "settling", "lead_off"])
            for i in range(self.n_ecg):
                if self.ecg_valid[i]:
                    fl = int(self.ecg_flags[i])
                    w.writerow([i + self.ecg_offset, f"{ecg_time(i + self.ecg_offset):.4f}",
                                f"{uv[i]:.2f}", fl & 1, (fl >> 1) & 1, (fl >> 2) & 1])
        fine = self.ppg_fine
        with open(paths[1], "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["frame", "time_s", "coarse", "fine", "fine_raw", "disturbed", "no_finger", "led_code"])
            for i in range(self.n_ppg):
                if self.ppg_valid[i]:
                    k = i + self.ppg_offset
                    w.writerow([k, f"{k / PPG_HZ + PPG_ON_SAMPLE_S:.5f}", f"{self.ppg_coarse[i]:.0f}",
                                f"{fine[i]:.0f}", int(self.ppg_fine_raw[i]), int(self.ppg_disturbed[i]),
                                int(self.ppg_no_finger[i]), int(self.ppg_led_code[i])])
        with open(paths[2], "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["time_s", "kind", "fields"])
            rows = []
            for kind in ("r_peaks", "pulses", "leads", "states", "buttons"):
                for e in getattr(self, kind):
                    rows.append((e["time_s"], kind, json.dumps({k: v for k, v in e.items() if k != "time_s"})))
            for r in sorted(rows):
                w.writerow([f"{r[0]:.4f}", r[1], r[2]])
        return paths


class _Growable:
    """Array that grows by doubling; `fill` marks unwritten entries."""

    def __init__(self, dtype, fill=0) -> None:
        self.a = np.full(4096, fill, dtype=dtype)
        self.fill = fill
        self.n = 0  # logical length = highest written index + 1

    def ensure(self, n: int) -> None:
        if n > len(self.a):
            new = np.full(max(n, 2 * len(self.a)), self.fill, dtype=self.a.dtype)
            new[:len(self.a)] = self.a
            self.a = new

    def view(self, lo: int = 0) -> np.ndarray:
        return self.a[lo:self.n]


class RecordingBuilder:
    """Incrementally builds a Recording from messages (suitable for live use)."""

    def __init__(self) -> None:
        self.ecg_raw = _Growable(np.int16)
        self.ecg_valid = _Growable(bool, False)
        self.ecg_flags = _Growable(np.uint8)
        self.ppg_coarse = _Growable(float, np.nan)
        self.ppg_fine = _Growable(np.int16)
        self.ppg_valid = _Growable(bool, False)
        self.ppg_dist = _Growable(bool, False)
        self.ppg_nofing = _Growable(bool, False)
        self.ppg_led = _Growable(np.uint16)
        self.events: dict[str, list[dict]] = {k: [] for k in ("r_peaks", "pulses", "leads", "states", "buttons")}
        self.logs: list[str] = []
        self.dup_ecg_blocks = 0
        self.dup_ppg_blocks = 0
        self.bad_start = 0
        self.ecg_blocks = 0
        self.ppg_blocks = 0
        self._now = 0.0  # time of the newest block, used to stamp untimed events

    def add_all(self, msgs: Iterable[Message]) -> None:
        for m in msgs:
            self.add(m)

    def add(self, m: Message) -> None:
        if isinstance(m, EcgBlock):
            self._add_ecg(m)
        elif isinstance(m, PpgBlock):
            self._add_ppg(m)
        elif isinstance(m, RPeakEvent):
            self.events["r_peaks"].append({"time_s": float(ecg_time(m.sample)), "sample": m.sample,
                                           "hr_bpm": m.hr_bpm})
        elif isinstance(m, PulseEvent):
            self.events["pulses"].append({"time_s": float(ecg_time(m.foot_sample)), "foot_sample": m.foot_sample,
                                          "pulse_bpm": m.pulse_bpm, "pat_ms": m.pat_ms,
                                          "pi_permille": m.pi_permille})
        elif isinstance(m, LeadEvent):
            self.events["leads"].append({"time_s": self._now, "contact": m.contact})
        elif isinstance(m, StateEvent):
            self.events["states"].append({"time_s": self._now, "mode": m.mode, "notch_hz": m.notch_hz,
                                          "mid_code": m.mid_code, "batt_pct": m.batt_pct, "flags": m.flags})
        elif isinstance(m, ButtonEvent):
            self.events["buttons"].append({"time_s": self._now, "button": m.button, "kind": m.kind})
        else:  # Log
            self.logs.append(m.text)

    def _add_ecg(self, b: EcgBlock) -> None:
        if b.start > MAX_INDEX:
            self.bad_start += 1
            return
        n = len(b.samples)
        end = b.start + n
        self.ecg_raw.ensure(end)
        self.ecg_valid.ensure(end)
        self.ecg_flags.ensure(end)
        if self.ecg_valid.a[b.start:end].any():
            self.dup_ecg_blocks += 1
            return  # keep the first copy
        self.ecg_raw.a[b.start:end] = b.samples
        self.ecg_valid.a[b.start:end] = True
        self.ecg_flags.a[b.start:end] = b.flags
        for g in (self.ecg_raw, self.ecg_valid, self.ecg_flags):
            g.n = max(g.n, end)
        self.ecg_blocks += 1
        self._now = max(self._now, float(ecg_time(end)))

    def _add_ppg(self, b: PpgBlock) -> None:
        if b.start > MAX_INDEX // 5:
            self.bad_start += 1
            return
        end = b.start + 2
        for g in (self.ppg_coarse, self.ppg_fine, self.ppg_valid, self.ppg_dist, self.ppg_nofing, self.ppg_led):
            g.ensure(end)
        if self.ppg_valid.a[b.start:end].any():
            self.dup_ppg_blocks += 1
            return
        d = b.disturbed
        for j in range(2):
            k = b.start + j
            self.ppg_coarse.a[k] = b.coarse[j]
            self.ppg_fine.a[k] = b.fine[j]
            self.ppg_valid.a[k] = True
            self.ppg_dist.a[k] = d[j]
            self.ppg_nofing.a[k] = b.no_finger
            self.ppg_led.a[k] = b.led_code
        for g in (self.ppg_coarse, self.ppg_fine, self.ppg_valid, self.ppg_dist, self.ppg_nofing, self.ppg_led):
            g.n = max(g.n, end)
        self.ppg_blocks += 1
        self._now = max(self._now, end / PPG_HZ)

    def build(self, decoder_stats: dict | None = None, last_s: float | None = None) -> Recording:
        """Snapshot as a Recording (copies). `last_s` keeps only the most recent seconds."""
        lo_e = lo_p = 0
        if last_s is not None:
            lo_e = max(self.ecg_raw.n - int(last_s * ECG_HZ), 0)
            lo_p = max(self.ppg_coarse.n - int(last_s * PPG_HZ), 0)
        rec = Recording(
            self.ecg_raw.view(lo_e).copy(), self.ecg_valid.view(lo_e).copy(), self.ecg_flags.view(lo_e).copy(),
            self.ppg_coarse.view(lo_p).copy(), self.ppg_fine.view(lo_p).copy(), self.ppg_valid.view(lo_p).copy(),
            self.ppg_dist.view(lo_p).copy(), self.ppg_nofing.view(lo_p).copy(), self.ppg_led.view(lo_p).copy(),
            ecg_offset=lo_e, ppg_offset=lo_p)
        t0 = min(float(ecg_time(lo_e)), lo_p / PPG_HZ) if last_s is not None else -1.0
        for k, ev in self.events.items():
            setattr(rec, k, [dict(e) for e in ev if e["time_s"] >= t0] if last_s is not None else [dict(e) for e in ev])
        rec.meta = {
            "dup_ecg_blocks": self.dup_ecg_blocks, "dup_ppg_blocks": self.dup_ppg_blocks,
            "bad_start": self.bad_start, "ecg_blocks": self.ecg_blocks, "ppg_blocks": self.ppg_blocks,
            "decoder": decoder_stats or {}, "log": self.logs[-200:],
        }
        return rec


def assemble(msgs: Iterable[Message], decoder_stats: dict | None = None) -> Recording:
    b = RecordingBuilder()
    b.add_all(msgs)
    return b.build(decoder_stats)
