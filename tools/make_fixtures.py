#!/usr/bin/env python3
"""Generate the deterministic synthetic fixture set for firmware tests.

    python3 tools/make_fixtures.py --out build/fixtures [--only NAME ...] [--list]

See tools/README.md for the file formats.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ecgcard import siggen  # noqa: E402
from ecgcard.siggen import EcgParams, PpgParams  # noqa: E402

D = 30.0


def _set() -> dict[str, tuple[EcgParams, PpgParams]]:
    E, P = EcgParams, PpgParams
    return {
        "clean_60bpm": (E(duration_s=D, hr_bpm=60, seed=11), P()),
        "fast_180bpm": (E(duration_s=D, hr_bpm=180, seed=12), P(pat_ms=150)),
        "slow_40bpm": (E(duration_s=D, hr_bpm=40, seed=13), P()),
        "low_amp_noisy": (E(duration_s=D, hr_bpm=75, r_mv=0.25, emg_uv_rms=40, wander_mv=0.1,
                            gain=1098, seed=14), P(noise_na=2.0, pi_percent=1.5)),
        "mains50": (E(duration_s=D, hr_bpm=70, mains_hz=50, mains_uv=300, seed=15), P(flicker_hz=100, flicker_depth=0.1)),
        "mains60": (E(duration_s=D, hr_bpm=70, mains_hz=60, mains_uv=300, seed=16), P(flicker_hz=120, flicker_depth=0.1)),
        "wander": (E(duration_s=D, hr_bpm=65, wander_mv=1.0, seed=17), P()),
        "tall_t": (E(duration_s=D, hr_bpm=80, t_ratio=0.9, r_mv=0.8, seed=18), P()),
        "hrv": (E(duration_s=60.0, hr_bpm=62, rr_sd_ms=25, rsa_ms=40, seed=19), P(pat_sd_ms=8, seed=20)),
        "both_pat200": (E(duration_s=D, hr_bpm=72, rr_sd_ms=10, seed=21), P(pat_ms=200, seed=22)),
        "ectopic": (E(duration_s=D, hr_bpm=70, rr_sd_ms=10, ectopic_every=7, seed=23), P(seed=24)),
        "gain1098": (E(duration_s=D, hr_bpm=60, r_mv=0.6, gain=1098, seed=25), P(led_ma=8, pi_percent=4.0, seed=26)),
        "led_step": (E(duration_s=D, hr_bpm=66, seed=27), P(led_steps=((1500, 2000),), seed=28)),
    }


def write_fixture(name: str, ep: EcgParams, pp: PpgParams, out: Path) -> dict:
    sim = siggen.simulate(ep, pp)
    ecg_path = out / f"{name}.ecg.u16"
    ppg_path = out / f"{name}.ppg.u16"
    sim.ecg_codes.astype("<u2").tofile(ecg_path)
    sim.ppg.frames.astype("<u2").tofile(ppg_path)
    meta = {
        "name": name,
        "format": {
            "ecg_file": ecg_path.name, "ecg_rate_hz": siggen.ECG_FS, "ecg_dtype": "uint16 little-endian",
            "ppg_file": ppg_path.name, "ppg_rate_hz": siggen.PPG_FS,
            "ppg_dtype": "uint16 little-endian x4 per frame: tia_dark, hi_dark, tia_on, hi_on",
            "ppg_on_sample_offset_s": siggen.T_ON_S, "ppg_dark_sample_offset_s": siggen.T_DARK_S,
        },
        "ecg_samples": int(len(sim.ecg_codes)),
        "ppg_frames": int(len(sim.ppg.frames)),
        "ecg_mid_code": ep.mid_code, "ecg_gain": ep.gain, "uv_per_code": siggen.UV_PER_CODE,
        "ppg_led_code_schedule": siggen.schedule_changes(sim.ppg.led_code),
        "ppg_dac12_schedule": siggen.schedule_changes(sim.ppg.dac12),
        "params": siggen.params_dict(sim),
        "truth": sim.truth(),
    }
    (out / f"{name}.json").write_text(json.dumps(meta, indent=1))
    return meta


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="build/fixtures", help="output directory (default build/fixtures)")
    ap.add_argument("--only", nargs="*", help="generate only these names")
    ap.add_argument("--list", action="store_true", help="list fixture names and exit")
    a = ap.parse_args()
    fixtures = _set()
    if a.list:
        print("\n".join(fixtures))
        return 0
    names = a.only or list(fixtures)
    unknown = [n for n in names if n not in fixtures]
    if unknown:
        ap.error(f"unknown fixture(s): {', '.join(unknown)}")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    for n in names:
        m = write_fixture(n, *fixtures[n], out)
        size = sum(p.stat().st_size for p in out.glob(f"{n}.*"))
        total += size
        t = m["truth"]
        print(f"{n:16s} {t['hr_bpm']:6.1f} bpm  {len(t['r_peak_times_s']):3d} beats  {size / 1e6:5.2f} MB")
    print(f"total {total / 1e6:.1f} MB in {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
