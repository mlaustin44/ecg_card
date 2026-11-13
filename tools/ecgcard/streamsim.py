"""Build a firmware-like byte stream from a synthetic Simulation (for tests and replay)."""
from __future__ import annotations

import numpy as np

from . import refdsp
from .codec import (
    ECG_FLAG_GAIN_HI, ECG_FLAG_SETTLING, PPG_FLAG_DIST0, PPG_FLAG_DIST1,
    EcgBlock, Log, Message, PpgBlock, PulseEvent, RPeakEvent, StateEvent, StreamEncoder,
)
from .siggen import Simulation


def sim_messages(sim: Simulation, settle_s: float = 2.5) -> list[Message]:
    """Messages the firmware would emit for a simulation: blocks every 20 ms plus events."""
    ep, pp = sim.ecg_params, sim.ppg
    dec = refdsp.decimate_8(sim.ecg_codes)
    s16 = refdsp.to_stream_units(dec, ep.mid_code)
    peaks = refdsp.pan_tompkins(dec)
    t_ppg = np.arange(len(pp.fine)) / 100.0 + 0.00995
    feet = refdsp.ppg_feet(t_ppg, -pp.fine.astype(float), peaks / 500.0)
    pats = refdsp.pat_from_feet(peaks / 500.0, feet)

    ev: list[tuple[int, Message]] = []  # (emit-at ECG sample, message)
    for i, pk in enumerate(peaks):
        hr = int(round(60.0 * 500.0 / (pk - peaks[i - 1]))) if i else 0
        ev.append((pk + 40, RPeakEvent(int(pk), hr)))
        if np.isfinite(feet[i]):
            ev.append((int(feet[i] * 500) + 80, PulseEvent(int(round(feet[i] * 500)), hr,
                                                           int(round(pats[i] * 1000)), 20)))
    ev.sort(key=lambda e: e[0])

    gain_flag = ECG_FLAG_GAIN_HI if ep.gain == 1098 else 0
    msgs: list[Message] = [Log("sim start"),
                           StateEvent(3, 50, ep.mid_code, 80, 0)]
    n_blocks = min(len(s16) // 10, len(pp.fine) // 2)
    k = 0
    for b in range(n_blocks):
        start = b * 10
        fl = gain_flag | (ECG_FLAG_SETTLING if start / 500.0 < settle_s else 0)
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
