"""Protocol test vectors for the app's Dart parsers (test/protocol).

Payloads come from the reference encoder in tools/ecgcard/codec.py (ECG, PPG and
event kinds 1..5); LINK_STATUS, CMD_RESULT and the Control commands are packed
here with struct, following fw/include/link_fmt.h. Deterministic (fixed seed).

Run from the repo root (numpy needed by codec.py; the repo .venv has it):
    .venv/bin/python -I app/tool/make_vectors.py
Writes app/test/fixtures/protocol_vectors.json.
"""
from __future__ import annotations

import json
import pathlib
import random
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import numpy as np  # noqa: E402

from ecgcard import codec  # noqa: E402

OUT = ROOT / "app" / "test" / "fixtures" / "protocol_vectors.json"
rng = random.Random(20261117)


def i16() -> int:
    return rng.randint(-32768, 32767)


def ecg_vectors() -> list[dict]:
    out = []
    starts = [0, 10, 123450, 0xFFFFFFF6]
    for n in (1, 2, 3, 8):
        blocks, payload = [], b""
        for k in range(n):
            start = starts[k % len(starts)] if n < 4 else 5000 + 10 * k
            flags = rng.choice([0, 1, 2, 3, 4, 5, 7])
            s = [i16() for _ in range(10)]
            if k == 0:
                s[0], s[1] = -32768, 32767
            ftype, p = codec.message_payload(codec.EcgBlock(start, flags, np.array(s, dtype=np.int64)))
            assert ftype == codec.STREAM_ECG_BLOCK and len(p) == 25
            # cross-check with the reference parser
            back = codec.parse_payload(ftype, 0, p)
            assert back.start == start and list(back.samples) == s
            blocks.append({"start": start, "flags": flags, "samples": s,
                           "gain_hi": back.gain_hi, "settling": back.settling, "lead_off": back.lead_off})
            payload += p
        out.append({"hex": payload.hex(), "blocks": blocks})
    # trailing partial block: whole blocks only
    full = bytes.fromhex(out[1]["hex"])
    out.append({"hex": (full + full[:7]).hex(), "blocks": out[1]["blocks"], "note": "trailing partial ignored"})
    return out


def ppg_vectors() -> list[dict]:
    out = []
    for n in (1, 2, 5):
        blocks, payload = [], b""
        for k in range(n):
            start = rng.randint(0, 0xFFFFFFFF) if k == 0 else blocks[-1]["start"] + 2
            led = rng.randint(0, 4095)
            flags = rng.choice([0, 1, 2, 3, 4, 7])
            coarse = (i16(), i16())
            fine = (i16(), i16())
            ftype, p = codec.message_payload(codec.PpgBlock(start, led, flags, coarse, fine))
            assert ftype == codec.STREAM_PPG_BLOCK and len(p) == 15
            back = codec.parse_payload(ftype, 0, p)
            assert back.coarse == coarse and back.fine == fine
            blocks.append({"start": start, "led_code": led, "flags": flags,
                           "coarse": list(coarse), "fine": list(fine), "no_finger": back.no_finger})
            payload += p
        out.append({"hex": payload.hex(), "blocks": blocks})
    return out


def event_vectors() -> list[dict]:
    out = []

    def add(msg, fields):
        ftype, p = codec.message_payload(msg)
        assert ftype == codec.STREAM_EVENT
        out.append({"hex": p.hex(), **fields})

    for idx, hr in [(0, 0), (6170, 64), (0xFFFFFFFF, 65535), (123456, 182)]:
        add(codec.RPeakEvent(idx, hr), {"type": "r_peak", "sample_index": idx, "hr_bpm": hr})
    for foot, bpm, pat, pi in [(6290, 64, 215, 11), (1, 300, 0, 0), (0xFFFFFFFF, 65535, 65535, 65535)]:
        add(codec.PulseEvent(foot, bpm, pat, pi),
            {"type": "pulse", "foot_index": foot, "pulse_bpm": bpm, "pat_ms": pat, "pi_permille": pi})
    for c in (0, 1):
        add(codec.LeadEvent(c), {"type": "lead", "contact": bool(c)})
    for mode, notch, mid, batt, flags, glo, ghi in [(1, 50, 2048, 82, 0, 558, 1098), (3, 60, 2011, 255, 3, 300, 1200),
                                                     (4, 50, 65535, 0, 1, 1, 65535), (0, 50, 2048, 100, 2, 558, 1098)]:
        add(codec.StateEvent(mode, notch, mid, batt, flags, 0, glo, ghi),
            {"type": "state", "mode": mode, "notch_hz": notch, "mid_code": mid, "batt_pct": batt,
             "flags": flags, "gain_lo": glo, "gain_hi": ghi})
    # 7-byte STATE of older firmware: gains default to 558 / 1098
    short = struct.pack("<BBBHBB", codec.EV_STATE, 2, 60, 2050, 47, 1)
    parsed = codec.parse_payload(codec.STREAM_EVENT, 0, short)
    assert (parsed.gain_lo, parsed.gain_hi) == (558, 1098)
    out.append({"hex": short.hex(), "type": "state", "mode": 2, "notch_hz": 60, "mid_code": 2050, "batt_pct": 47,
                "flags": 1, "gain_lo": 558, "gain_hi": 1098, "note": "short form"})
    for b, k in [(1, 0), (2, 1), (2, 0)]:
        add(codec.ButtonEvent(b, k), {"type": "button", "button": b, "long": k == 1})
    # STREAM_EV_CMD_RESULT (6, link_fmt.h): u8 cmd, u8 result
    for cmd, res in [(1, 0), (1, 3), (2, 0), (3, 5), (4, 2), (5, 0), (9, 1)]:
        out.append({"hex": bytes([6, cmd, res]).hex(), "type": "cmd_result", "cmd": cmd, "result": res})
    # unknown kind, and known kinds that are too short
    out.append({"hex": bytes([0x7E, 1, 2, 3]).hex(), "type": "unknown", "kind": 0x7E})
    out.append({"hex": bytes([1, 0, 0]).hex(), "type": "unknown", "kind": 1, "note": "R_PEAK too short"})
    out.append({"hex": bytes([4, 1, 50]).hex(), "type": "unknown", "kind": 4, "note": "STATE too short"})
    return out


def status_bytes(s: dict) -> bytes:
    return struct.pack("<BBBBBBBBBBBBHBBBBH", s["proto"], s["mode"], s["meas"], s["batt_pct"], s["flags"],
                       s["gain"], s["notch"], s["rld"], s["led_limit"], s["sweep"], s["notch_hz"], s["hr_bpm"],
                       s["batt_mv"], s["fw_major"], s["fw_minor"], s["fw_patch"], 0, s["uptime_min"])


def status_vectors() -> list[dict]:
    rows = [
        dict(proto=1, mode=0, meas=255, batt_pct=82, flags=0x40, gain=0, notch=0, rld=0, led_limit=2, sweep=1,
             notch_hz=50, hr_bpm=0, batt_mv=3930, fw_major=0, fw_minor=3, fw_patch=1, uptime_min=17),
        dict(proto=1, mode=1, meas=2, batt_pct=55, flags=0x20, gain=2, notch=1, rld=3, led_limit=0, sweep=2,
             notch_hz=50, hr_bpm=64, batt_mv=3800, fw_major=1, fw_minor=2, fw_patch=3, uptime_min=65535),
        dict(proto=1, mode=3, meas=1, batt_pct=255, flags=0x7F, gain=1, notch=2, rld=1, led_limit=1, sweep=0,
             notch_hz=60, hr_bpm=255, batt_mv=0, fw_major=255, fw_minor=0, fw_patch=9, uptime_min=0),
        dict(proto=1, mode=4, meas=0, batt_pct=0, flags=0x09, gain=0, notch=0, rld=2, led_limit=2, sweep=1,
             notch_hz=50, hr_bpm=0, batt_mv=3300, fw_major=0, fw_minor=4, fw_patch=0, uptime_min=3),
        dict(proto=1, mode=1, meas=3, batt_pct=99, flags=0x03, gain=0, notch=0, rld=0, led_limit=2, sweep=1,
             notch_hz=50, hr_bpm=0, batt_mv=4150, fw_major=0, fw_minor=4, fw_patch=0, uptime_min=3),
        dict(proto=1, mode=2, meas=4, batt_pct=10, flags=0x14, gain=0, notch=0, rld=0, led_limit=2, sweep=1,
             notch_hz=50, hr_bpm=0, batt_mv=3400, fw_major=0, fw_minor=4, fw_patch=0, uptime_min=3),
    ]
    return [{"hex": status_bytes(r).hex(), **r} for r in rows]


def command_vectors() -> list[dict]:
    out = []
    for mode in (1, 2, 3, 4):
        out.append({"cmd": "start", "mode": mode, "hex": bytes([1, mode]).hex()})
    out.append({"cmd": "stop", "hex": "02"})
    out.append({"cmd": "get_status", "hex": "05"})
    for item, value in [(0, 2), (5, 0), (7, 1)]:
        out.append({"cmd": "set", "item": item, "value": value, "hex": bytes([3, item, value]).hex()})
    for secs, off in [(1704067200, 0), (1795000000, 60), (1795000000, -300), (0xFFFFFFFF, 840), (1800000000, -720)]:
        out.append({"cmd": "set_time", "unix_seconds": secs, "utc_offset_minutes": off,
                    "hex": struct.pack("<BIh", 4, secs, off).hex()})
    return out


def main() -> None:
    data = {
        "source": "app/tool/make_vectors.py (tools/ecgcard/codec.py encoders)",
        "ecg": ecg_vectors(),
        "ppg": ppg_vectors(),
        "events": event_vectors(),
        "status": status_vectors(),
        "commands": command_vectors(),
    }
    OUT.write_text(json.dumps(data, indent=1) + "\n")
    print(f"wrote {OUT.relative_to(ROOT)}: " + ", ".join(f"{k} {len(v)}" for k, v in data.items() if isinstance(v, list)))


if __name__ == "__main__":
    main()
