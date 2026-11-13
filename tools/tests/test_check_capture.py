import subprocess
import sys
from pathlib import Path

import numpy as np

from ecgcard import codec as c

TOOLS = Path(__file__).resolve().parents[1]


def messages(n_blocks=50, ecg0=0, ppg0=0, skip_ecg=()):
    """ECG and PPG blocks every 20 ms plus a few events, as the firmware sends them."""
    msgs = [c.StateEvent(3, 50, 2048, 80, 0), c.LeadEvent(1)]
    for b in range(n_blocks):
        if b not in skip_ecg:
            msgs.append(c.EcgBlock(ecg0 + 10 * b, 0, np.arange(10, dtype=np.int16)))
        msgs.append(c.PpgBlock(ppg0 + 2 * b, 1345, 0, (1800, 1801), (-18000, -17990)))
        if b % 25 == 10:
            msgs.append(c.RPeakEvent(ecg0 + 10 * b, 60 if b > 10 else 0))
            msgs.append(c.PulseEvent(ecg0 + 10 * b + 130, 60, 250, 20))
    return msgs


def wire(msgs):
    enc = c.StreamEncoder()
    return b"".join(enc.encode(m) for m in msgs)


def run(path, *args, cwd=None):
    return subprocess.run([sys.executable, str(TOOLS / "check_capture.py"), str(path), *args],
                          capture_output=True, text=True, cwd=cwd)


def test_good_capture_passes_from_any_cwd(tmp_path):
    p = tmp_path / "good.bin"
    p.write_bytes(wire(messages()))
    r = run(p, "--min-ecg-blocks", "50", "--min-ppg-blocks", "50", "--require-events", "r_peak,pulse,state",
            cwd=tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "ECG blocks 50 in 1 run(s), index gaps 0" in r.stdout
    assert "PPG blocks 50 in 1 run(s), index gaps 0" in r.stdout
    assert "r_peak 2" in r.stdout and "pulse 2" in r.stdout and "mean PAT 250.0 ms" in r.stdout
    assert "mean HR 60.0 bpm (n=1)" in r.stdout and r.stdout.strip().endswith("OK")
    q = run(p, "-q", cwd=tmp_path)
    assert q.returncode == 0 and q.stdout == "" and q.stderr == ""


def test_new_run_restarting_at_zero_is_not_a_gap(tmp_path):
    p = tmp_path / "two_runs.bin"
    p.write_bytes(wire(messages(30) + messages(20)))
    r = run(p)
    assert r.returncode == 0, r.stderr
    assert "ECG blocks 50 in 2 run(s), index gaps 0" in r.stdout


def test_corrupted_byte_fails(tmp_path):
    data = bytearray(wire(messages()))
    data[len(data) // 2] ^= 0x40  # inside some frame (never turns a byte into 0x00 here: checked)
    assert data[len(data) // 2] != 0
    p = tmp_path / "bad.bin"
    p.write_bytes(bytes(data))
    r = run(p, "-q")
    assert r.returncode == 1
    assert "FAIL" in r.stderr and ("bad_crc" in r.stderr or "bad_cobs" in r.stderr or "bad_length" in r.stderr)


def test_index_gap_fails(tmp_path):
    p = tmp_path / "gap.bin"
    p.write_bytes(wire(messages(skip_ecg={20})))  # one ECG block never sent (indices 190 -> 210)
    r = run(p)
    assert r.returncode == 1
    assert "ECG block index not contiguous: 1 gap(s)" in r.stderr and "190->210" in r.stderr


def test_minimums_and_required_events(tmp_path):
    msgs = [m for m in messages(10) if not isinstance(m, c.PulseEvent)]
    p = tmp_path / "short.bin"
    p.write_bytes(wire(msgs))
    assert run(p).returncode == 0
    r = run(p, "--min-ecg-blocks", "11")
    assert r.returncode == 1 and "ECG blocks 10 < 11" in r.stderr
    r = run(p, "--require-events", "pulse")
    assert r.returncode == 1 and "no pulse event" in r.stderr
    assert run(p, "--require-events", "bogus").returncode == 2
    assert run(tmp_path / "missing.bin").returncode == 2
