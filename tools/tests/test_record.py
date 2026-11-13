import numpy as np

from ecgcard import codec as c
from ecgcard.record import (ECG_ANALOG_DELAY_S, ECG_T0_S, Recording, RecordingBuilder, assemble, ecg_index,
                            ecg_time, r_true_time, unwrap_int16)


def ecg_blocks(n, gain_flag=0, value=lambda i: i):
    return [c.EcgBlock(10 * b, gain_flag, np.array([value(10 * b + j) for j in range(10)], np.int16)) for b in range(n)]


def test_placement_gaps_and_scaling():
    blocks = ecg_blocks(10)
    del blocks[3]
    rec = assemble(blocks)
    assert rec.n_ecg == 100
    assert rec.ecg_valid[:30].all() and not rec.ecg_valid[30:40].any() and rec.ecg_valid[40:].all()
    uv = rec.ecg_uv
    assert np.isnan(uv[30:40]).all()
    # 800 stream units at gain 558: 800 / 8 * 805.7 / 558
    assert np.isclose(uv[50] * 8 * 558 / 805.7, 50)
    assert np.isclose(rec.ecg_t[50], 0.1 + ECG_T0_S)  # stream sample m is at m/500 - 10 ms


def test_ecg_timeline_constants():
    # fw/dsp/include/dsp_delays.h: DSP_ECG_STREAM_T0_US = -10000, DSP_ECG_ANALOG_DELAY_US = 974
    assert ECG_T0_S == -0.010 and ECG_ANALOG_DELAY_S == 0.000974
    assert np.isclose(ecg_time(100), 0.19) and np.isclose(ecg_index(0.19), 100)
    assert np.isclose(r_true_time(100), 0.19 - 0.000974)


def test_gain_flag_per_block_and_other_flags():
    b0 = c.EcgBlock(0, 0, np.full(10, 80, np.int16))
    b1 = c.EcgBlock(10, c.ECG_FLAG_GAIN_HI | c.ECG_FLAG_SETTLING, np.full(10, 80, np.int16))
    b2 = c.EcgBlock(20, c.ECG_FLAG_LEAD_OFF, np.full(10, 80, np.int16))
    rec = assemble([b0, b1, b2])
    uv = rec.ecg_uv
    assert np.isclose(uv[0], 10 * 805.7 / 558) and np.isclose(uv[10], 10 * 805.7 / 1098)
    assert rec.ecg_settling[10:20].all() and not rec.ecg_settling[:10].any()
    assert rec.ecg_lead_off[20:].all() and not rec.ecg_lead_off[:20].any()


def test_duplicates_and_out_of_order():
    b = RecordingBuilder()
    blocks = ecg_blocks(5)
    for blk in [blocks[0], blocks[2], blocks[1], blocks[1], blocks[3], blocks[0], blocks[4]]:
        b.add(blk)
    rec = b.build()
    assert rec.meta["dup_ecg_blocks"] == 2
    assert rec.ecg_valid.all() and rec.n_ecg == 50
    assert np.array_equal(rec.ecg_raw, np.arange(50))


def test_duplicate_keeps_first_copy():
    a = c.EcgBlock(0, 0, np.full(10, 1, np.int16))
    d = c.EcgBlock(0, 0, np.full(10, 9, np.int16))
    rec = assemble([a, d])
    assert (rec.ecg_raw == 1).all() and rec.meta["dup_ecg_blocks"] == 1


def test_absurd_start_rejected():
    rec = assemble([c.EcgBlock(2**31, 0, np.zeros(10, np.int16)), *ecg_blocks(1)])
    assert rec.n_ecg == 10 and rec.meta["bad_start"] == 1


def test_ppg_placement_flags_and_time():
    blocks = [c.PpgBlock(2 * k, 1000 + k, [0, 1, 2, 4][k % 4] | (4 if k == 2 else 0), (10 * k, 10 * k + 1), (k, k)) for k in range(6)]
    del blocks[1]
    blocks.append(blocks[0])  # duplicate
    rec = assemble(blocks)
    assert rec.n_ppg == 12 and rec.meta["dup_ppg_blocks"] == 1
    assert not rec.ppg_valid[2:4].any() and np.isnan(rec.ppg_coarse[2:4]).all()
    assert rec.ppg_disturbed[0] is np.False_ and rec.ppg_disturbed[5]
    assert rec.ppg_no_finger[4:6].all() and not rec.ppg_no_finger[0]
    assert np.isclose(rec.ppg_t[3], 0.03 + 0.00995)
    assert rec.ppg_led_code[4] == 1002


def test_unwrap_fine_modulo_2_16():
    true = np.arange(0, 200000, 1500)
    raw = ((true + 32768) % 65536 - 32768).astype(np.int16)
    valid = np.ones(len(raw), bool)
    u = unwrap_int16(raw, valid)
    assert np.array_equal(u, true)
    valid[10:15] = False  # gap shorter than half a range: still continuous
    u = unwrap_int16(raw, valid)
    assert np.isnan(u[10:15]).all() and np.array_equal(u[valid], true[valid])


def test_events_and_save_load(tmp_path):
    msgs = [*ecg_blocks(3), c.PpgBlock(0, 1, 0, (1, 2), (-5, 6)),
            c.RPeakEvent(12, 70), c.PulseEvent(20, 70, 250, 15), c.LeadEvent(1),
            c.StateEvent(3, 50, 2040, 90, 1), c.ButtonEvent(1, 0), c.Log("x")]
    rec = assemble(msgs, {"bad_crc": 3})
    assert rec.r_peaks[0]["sample"] == 12 and np.isclose(rec.r_peak_times()[0], 0.014)
    assert np.isclose(rec.r_peak_true_times()[0], 0.014 - 0.000974)
    assert rec.pulses[0]["pat_ms"] == 250 and np.isclose(rec.pulse_foot_times()[0], 0.03)
    assert np.isclose(rec.r_peaks[0]["time_s"], 0.014)
    assert rec.leads[0]["contact"] == 1 and rec.states[0]["mid_code"] == 2040 and rec.buttons[0]["button"] == 1
    p = tmp_path / "r.npz"
    rec.save(p)
    r2 = Recording.load(p)
    assert np.array_equal(r2.ecg_raw, rec.ecg_raw) and np.array_equal(r2.ppg_fine_raw, rec.ppg_fine_raw)
    assert r2.r_peaks == rec.r_peaks and r2.meta["decoder"]["bad_crc"] == 3
    paths = r2.export_csv(tmp_path / "r")
    assert all(q.exists() for q in paths)
    assert len(paths[0].read_text().splitlines()) == 31
    assert "r_peaks" in paths[2].read_text()


def test_window_and_recent():
    rec = assemble(ecg_blocks(100) + [c.RPeakEvent(900, 60), c.RPeakEvent(100, 60)])
    w = rec.window(1.0, 2.0)
    # t = 1.0 s is sample 505; the data end at sample 999
    assert w.ecg_offset == 505 and w.n_ecg == 495 and len(w.r_peaks) == 1
    assert np.isclose(w.ecg_t[0], 1.0)
    b = RecordingBuilder()
    b.add_all(ecg_blocks(100))
    r = b.build(last_s=0.5)
    assert r.n_ecg == 250 and r.ecg_offset == 750 and np.isclose(r.ecg_t[0], 1.49)
