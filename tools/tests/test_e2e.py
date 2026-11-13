import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np
import pytest

from ecgcard import codec, refdsp, streamsim
from ecgcard.capture import CaptureSession, run_capture
from ecgcard.record import ECG_ANALOG_DELAY_S, ECG_T0_S, ECG_T0_SAMPLES, Recording, assemble
from ecgcard.sources import follow_source, tcp_source

TOOLS = Path(__file__).resolve().parents[1]


def test_siggen_to_report_end_to_end(sim, tmp_path):
    s = sim("both_pat200")
    wire = streamsim.sim_stream(s)
    raw = tmp_path / "rtt.bin"
    raw.write_bytes(wire)
    npz, png = tmp_path / "rec.npz", tmp_path / "report.png"
    run = lambda *a: subprocess.run([sys.executable, str(TOOLS / a[0]), *a[1:]], check=True,
                                    capture_output=True, text=True)
    r1 = run("stream_capture.py", "--file", str(raw), "--npz", str(npz), "-q")
    rec = Recording.load(npz)
    assert rec.n_ecg == 15000 and rec.meta["decoder"]["bad_crc"] == 0 and rec.meta["decoder"]["seq_lost"] == 0
    # the decoded ECG equals the reference decimation of the generated codes (in uV), delayed
    # by the firmware's stream offset (stream sample m = raw time 8 m - 40)
    dec = refdsp.decimate_8(s.ecg_codes)
    ref_uv = (dec - s.ecg_params.mid_code) * 805.7 / s.ecg_params.gain
    k = ECG_T0_SAMPLES
    assert np.abs(rec.ecg_uv[k:] - ref_uv[:rec.n_ecg - k]).max() < 805.7 / s.ecg_params.gain / 8
    truth = np.array(s.truth()["r_peak_times_s"])
    tp, fp, fn, err = refdsp.match_events(rec.r_peak_true_times(), truth, 0.05)
    assert tp >= len(truth) - 1 and fp == 0
    assert abs(err.mean()) < 0.0015 and np.abs(err).max() <= 0.0035  # timeline offsets removed
    feet_truth = np.array(s.truth()["pulse_foot_times_s"])
    tp, fp, fn, err = refdsp.match_events(rec.pulse_foot_times(), feet_truth, 0.05)
    assert tp >= len(feet_truth) - 2 and abs(err.mean()) < 0.004
    pats = [e["pat_ms"] for e in rec.pulses]
    assert abs(np.mean(pats) - 200) < 8
    run("stream_plot.py", "--report", str(npz), "--png", str(png))
    assert png.stat().st_size > 50_000
    assert png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    # report straight from the raw stream too
    run("stream_plot.py", "--report", str(raw), "--png", str(tmp_path / "r2.png"))
    assert (tmp_path / "r2.png").exists()


@pytest.mark.parametrize("name", ["clean_60bpm", "mains50", "both_pat200", "fast_180bpm", "low_amp_noisy"])
def test_sim_stream_on_firmware_timeline(sim, name):
    """Event indices and fine values as the firmware would send them for the simulation."""
    s = sim(name)
    msgs, st = codec.decode_stream(streamsim.sim_stream(s))
    rec = assemble(msgs)
    t_r = np.array(s.truth()["r_peak_times_s"])
    # R-peak at stream index round((t_R + analog 0.974 ms + 10 ms) * 500), within one sample
    # (the generator's analog band shifts the R by ~1.07 ms, not exactly 0.974 ms)
    want = np.round((t_r + ECG_ANALOG_DELAY_S - ECG_T0_S) * 500)
    got = rec.r_peak_samples()
    assert len(got) == len(want) and np.abs(got - want).max() <= 1
    assert np.mean(got == want) >= 0.6
    pats = np.array([e["pat_ms"] for e in rec.pulses])
    assert len(pats) >= len(t_r) - 2 and abs(pats.mean() - s.ppg_params.pat_ms) < 3.5
    # fine goes over the wire exactly as computed by the firmware formula (modulo 2^16)
    n = rec.n_ppg
    diff = rec.ppg_fine - s.ppg.fine[:n]
    assert np.all(diff == diff[0]) and diff[0] % 65536 == 0


def test_report_with_losses_and_no_ppg(sim, tmp_path):
    msgs = [m for m in streamsim.sim_messages(sim("clean_60bpm")) if not isinstance(m, (codec.PpgBlock, codec.PulseEvent))]
    wire = streamsim.encode_messages(msgs, drop=set(range(200, 230, 3)) | {400})
    p = tmp_path / "x.bin"
    p.write_bytes(wire)
    run = subprocess.run([sys.executable, str(TOOLS / "stream_plot.py"), "--report", str(p), "--png",
                          str(tmp_path / "x.png")], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert "Frames lost" in run.stdout and (tmp_path / "x.png").exists()


def test_tail_follow_growing_file(sim, tmp_path):
    wire = streamsim.sim_stream(sim("clean_60bpm"))[:20000]
    f = tmp_path / "grow.bin"

    def writer():
        time.sleep(0.2)  # file appears late
        with open(f, "wb") as fh:
            for i in range(0, len(wire), 997):
                fh.write(wire[i:i + 997]); fh.flush(); time.sleep(0.002)

    th = threading.Thread(target=writer); th.start()
    sess = CaptureSession(tmp_path / "copy.bin")
    run_capture(follow_source(f, idle_timeout_s=0.5), sess)
    sess.close(); th.join()
    assert (tmp_path / "copy.bin").read_bytes() == wire
    assert sess.decoder.stats.frames_ok > 50 and sess.decoder.stats.seq_gaps == 0
    assert "fr/s" in sess.stats_line()


def test_tcp_source(sim):
    wire = streamsim.sim_stream(sim("clean_60bpm"))[:8000]
    srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(1)

    def serve():
        conn, _ = srv.accept()
        for i in range(0, len(wire), 500):
            conn.sendall(wire[i:i + 500])
        conn.close()

    th = threading.Thread(target=serve); th.start()
    sess = CaptureSession()
    run_capture(tcp_source(f"127.0.0.1:{srv.getsockname()[1]}"), sess)
    th.join(); srv.close()
    assert sess.snapshot().n_ecg > 100 and sess.decoder.stats.bytes == len(wire)


def test_make_fixtures_cli(tmp_path):
    out = tmp_path / "fx"
    subprocess.run([sys.executable, str(TOOLS / "make_fixtures.py"), "--out", str(out), "--only", "clean_60bpm"],
                   check=True, capture_output=True)
    import json
    m = json.loads((out / "clean_60bpm.json").read_text())
    ecg = np.fromfile(out / "clean_60bpm.ecg.u16", dtype="<u2")
    ppg = np.fromfile(out / "clean_60bpm.ppg.u16", dtype="<u2").reshape(-1, 4)
    assert len(ecg) == m["ecg_samples"] == 120000 and len(ppg) == m["ppg_frames"] == 3000
    assert m["truth"]["hr_bpm"] == 60.0 and len(m["truth"]["r_peak_times_s"]) == 29
    # deterministic
    out2 = tmp_path / "fx2"
    subprocess.run([sys.executable, str(TOOLS / "make_fixtures.py"), "--out", str(out2), "--only", "clean_60bpm"],
                   check=True, capture_output=True)
    assert (out / "clean_60bpm.ecg.u16").read_bytes() == (out2 / "clean_60bpm.ecg.u16").read_bytes()
