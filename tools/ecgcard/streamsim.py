"""Build a firmware-like byte stream from a synthetic Simulation (for tests and replay)."""
from __future__ import annotations

import numpy as np
from scipy import signal

from . import refdsp
from .codec import (
    ECG_FLAG_GAIN_HI, ECG_FLAG_SETTLING, PPG_FLAG_DIST0, PPG_FLAG_DIST1,
    EcgBlock, Log, Message, PpgBlock, PulseEvent, RPeakEvent, StateEvent, StreamEncoder,
)
from .record import ECG_HZ, ECG_T0_S, ECG_T0_SAMPLES, ecg_index, ppg_time, r_true_time
from .siggen import Simulation


def stream_ecg(decimated: np.ndarray) -> np.ndarray:
    """refdsp.decimate_8 output (sample i at raw time 8 i) -> firmware stream timeline.

    The firmware's linear-phase decimator leaves its delay in: stream sample m is the
    signal at raw time 8 m - 40, i.e. t = m / 500 + ECG_T0_S. So stream[m] = dec[m - 5]
    (the first 5 samples repeat dec[0]); the length is unchanged.
    """
    d = np.asarray(decimated)
    return np.concatenate((np.full(ECG_T0_SAMPLES, d[0]), d[:len(d) - ECG_T0_SAMPLES]))


def refine_peaks(x: np.ndarray, peaks: np.ndarray, half: int = 4) -> np.ndarray:
    """Sub-sample R-peak positions: maximum of x within +-half samples of each detection,
    parabolic interpolation (like the firmware's index + frac). Simulated R waves are positive.

    x is first low-passed at 30 Hz with zero phase (no timing bias), so that mains and EMG
    do not move the maximum (the firmware also refines on a mains-free "clean" signal)."""
    x = signal.sosfiltfilt(signal.butter(4, 30.0, fs=ECG_HZ, output="sos"), np.asarray(x, float))
    out = np.empty(len(peaks))
    for j, pk in enumerate(peaks):
        lo, hi = max(pk - half, 1), min(pk + half + 1, len(x) - 1)
        k = lo + int(np.argmax(x[lo:hi]))
        ym, y0, yp = x[k - 1], x[k], x[k + 1]
        den = ym - 2 * y0 + yp
        out[j] = k + (0.5 * (ym - yp) / den if den < 0 else 0.0)
    return out


def sim_messages(sim: Simulation, settle_s: float = 2.5) -> list[Message]:
    """Messages the firmware would emit for a simulation: blocks every 20 ms plus events."""
    ep, pp = sim.ecg_params, sim.ppg
    dec = stream_ecg(refdsp.decimate_8(sim.ecg_codes))   # on the firmware stream timeline
    s16 = refdsp.to_stream_units(dec, ep.mid_code)
    # stream positions: the R appears at (t_R + analog delay + 0.010) * 500
    pos = refine_peaks(dec, refdsp.pan_tompkins(dec))
    peaks = np.round(pos).astype(int)  # EV_R_PEAK index (the firmware drops the fraction too)
    r_t = r_true_time(pos)             # true R time incl. the fraction, as the firmware's PAT uses
    t_ppg = ppg_time(np.arange(len(pp.fine)))
    feet = refdsp.ppg_feet(t_ppg, pp.fine.astype(float), r_t)  # fine is systole-up as is
    pats = refdsp.pat_from_feet(r_t, feet)

    ev: list[tuple[int, Message]] = []  # (emit-at ECG sample, message)
    for i, pk in enumerate(peaks):
        hr = int(round(60.0 * ECG_HZ / (pk - peaks[i - 1]))) if i else 0
        ev.append((pk + 40, RPeakEvent(int(pk), hr)))
        if np.isfinite(feet[i]):
            foot_idx = int(round(float(ecg_index(feet[i]))))  # EV_PULSE: foot in ECG stream samples
            ev.append((foot_idx + 80, PulseEvent(foot_idx, hr, int(round(pats[i] * 1000)), 20)))
    ev.sort(key=lambda e: e[0])

    gain_flag = ECG_FLAG_GAIN_HI if ep.gain == 1098 else 0
    msgs: list[Message] = [Log("sim start"),
                           StateEvent(3, 50, ep.mid_code, 80, 0)]
    n_blocks = min(len(s16) // 10, len(pp.fine) // 2)
    k = 0
    for b in range(n_blocks):
        start = b * 10
        fl = gain_flag | (ECG_FLAG_SETTLING if start / ECG_HZ + ECG_T0_S < settle_s else 0)
        msgs.append(EcgBlock(start, fl, s16[start:start + 10]))
        f0 = 2 * b
        dist = (PPG_FLAG_DIST0 if pp.disturbed[f0] else 0) | (PPG_FLAG_DIST1 if pp.disturbed[f0 + 1] else 0)
        frames = pp.frames[f0:f0 + 2].astype(np.int64)
        coarse = frames[:, 2] - frames[:, 0]
        fine = pp.fine[f0:f0 + 2]
        msgs.append(PpgBlock(f0, int(pp.led_code[f0]), dist, (int(coarse[0]), int(coarse[1])),
                             (int(fine[0]), int(fine[1]))))
        while k < len(ev) and ev[k][0] <= start + 10:
            msgs.append(ev[k][1])
            k += 1
    return msgs


def encode_messages(msgs: list[Message], drop: set[int] | None = None, seq0: int = 0) -> bytes:
    """Encode with a running sequence number. Indices in `drop` are numbered but not sent."""
    enc = StreamEncoder(seq0)
    out = bytearray()
    for i, m in enumerate(msgs):
        data = enc.encode(m)
        if not drop or i not in drop:
            out += data
    return bytes(out)


def sim_stream(sim: Simulation, **kw) -> bytes:
    return encode_messages(sim_messages(sim), **kw)
