import random
import struct

import numpy as np
import pytest

from ecgcard import codec as c


def test_crc_check_value():
    assert c.crc16_ccitt_false(b"123456789") == 0x29B1
    assert c.crc16_ccitt_false(b"") == 0xFFFF


@pytest.mark.parametrize("n", [0, 1, 2, 253, 254, 255, 256, 507, 508, 509, 600])
def test_cobs_roundtrip_boundaries(n):
    for fill in (0x00, 0x01, 0xFF):
        data = bytes([fill]) * n
        enc = c.cobs_encode(data)
        assert 0 not in enc
        assert c.cobs_decode(enc) == data


def test_cobs_known_vectors():
    assert c.cobs_encode(b"\x00") == b"\x01\x01"
    assert c.cobs_encode(b"\x11\x22\x00\x33") == b"\x03\x11\x22\x02\x33"
    assert c.cobs_encode(bytes(range(1, 255))) == b"\xff" + bytes(range(1, 255)) + b"\x01"
    assert c.cobs_encode(bytes(range(1, 256)))[:1] == b"\xff"


def test_cobs_random_roundtrip():
    rng = random.Random(5)
    for _ in range(300):
        d = bytes(rng.choice([0, 1, 255, rng.randrange(256)]) for _ in range(rng.randrange(0, 700)))
        assert c.cobs_decode(c.cobs_encode(d)) == d


def test_cobs_decode_rejects_bad():
    for bad in (b"", b"\x05\x01", b"\x00"):
        with pytest.raises(ValueError):
            c.cobs_decode(bad)


@pytest.mark.parametrize("payload", [b"", bytes(25), bytes(512), bytes([0xFF]) * 512, bytes(range(256)) * 2])
def test_frame_roundtrip(payload):
    wire = c.encode_frame(0x30, 7, payload)
    assert wire[-1] == 0 and 0 not in wire[:-1]
    ft, seq, pl = c.unpack_frame(c.cobs_decode(wire[:-1]))
    assert (ft, seq, pl) == (0x30, 7, payload)


def test_frame_max_len_enforced():
    with pytest.raises(ValueError):
        c.pack_frame(0x30, 0, bytes(513))


def test_frame_layout_and_errors():
    f = c.pack_frame(0x10, 3, b"abc")
    assert f[:4] == bytes([0x10, 3, 3, 0]) and len(f) == 9
    assert struct.unpack("<H", f[-2:])[0] == c.crc16_ccitt_false(f[:-2])
    bad = bytearray(f); bad[5] ^= 1
    with pytest.raises(c.FrameError) as e:
        c.unpack_frame(bytes(bad))
    assert e.value.kind == "crc"
    with pytest.raises(c.FrameError) as e:
        c.unpack_frame(f[:-1])
    assert e.value.kind == "length"


def all_messages():
    return [
        c.EcgBlock(0, 0, np.zeros(10, np.int16)),
        c.EcgBlock(123456, 7, np.array([-32768, 32767, 0, 1, -1, 5, 6, 7, 8, 9], np.int16)),
        c.PpgBlock(10, 1344, 5, (-3, 4), (-32768, 32767)),
        c.RPeakEvent(1000, 72),
        c.PulseEvent(1200, 71, 250, 18),
        c.LeadEvent(1),
        c.StateEvent(3, 50, 2048, 80, 3),
        c.ButtonEvent(2, 1),
        c.Log("hello µV"),
    ]


def test_message_roundtrip():
    enc = c.StreamEncoder(250)  # also exercises seq wrap
    msgs = all_messages()
    wire = b"".join(enc.encode(m) for m in msgs)
    out, st = c.decode_stream(wire)
    assert st.frames_ok == len(msgs) and st.bad_crc == st.bad_length == st.seq_gaps == 0
    for a, b in zip(msgs, out):
        assert type(a) is type(b)
        for k, v in a.__dict__.items():
            if k == "seq":
                continue
            assert np.array_equal(v, getattr(b, k)), k
    assert [m.seq for m in out][:7] == [250, 251, 252, 253, 254, 255, 0]


def test_ecg_block_sizes_match_header_comment():
    assert len(c.message_payload(all_messages()[0])[1]) == 25
    assert len(c.message_payload(all_messages()[2])[1]) == 15


def test_flags_properties():
    b = c.EcgBlock(0, 7, np.zeros(10, np.int16))
    assert b.gain_hi and b.settling and b.lead_off
    p = c.PpgBlock(0, 0, 0b101, (0, 0), (0, 0))
    assert p.disturbed == (True, False) and p.no_finger


# ---------------------------------------------------------------- decoder

def make_stream(n=200, seed=0):
    rng = np.random.default_rng(seed)
    enc = c.StreamEncoder()
    frames = []
    for i in range(n):
        m = c.EcgBlock(i * 10, 0, rng.integers(-3000, 3000, 10).astype(np.int16)) if i % 2 == 0 \
            else c.PpgBlock(i, 1344, 0, (1, 2), (3, 4))
        frames.append(enc.encode(m))
    return frames


def test_decoder_chunking_independent():
    wire = b"".join(make_stream())
    ref, _ = c.decode_stream(wire)
    rng = random.Random(1)
    for _ in range(20):
        d = c.StreamDecoder()
        out = []
        i = 0
        while i < len(wire):
            k = rng.choice([1, 2, 7, 64, 1000])
            out += d.feed(wire[i:i + k]); i += k
        assert len(out) == len(ref) == 200 and d.stats.seq_gaps == 0


def test_decoder_counts():
    frames = make_stream(20)
    # drop frame 5 (gap of 1), corrupt payload of frame 9 (CRC), drop frames 12-14 (gap of 3)
    raw = bytearray(c.cobs_decode(frames[9][:-1])); raw[6] ^= 0x55  # payload bit flips, CRC now wrong
    bad = bytearray(c.cobs_encode(bytes(raw)) + b"\x00")
    seq = frames[:5] + frames[6:9] + [bytes(bad)] + frames[10:12] + frames[15:]
    out, st = c.decode_stream(b"".join(seq))
    assert st.bad_crc == 1
    # lost by sequence: frame 5, frame 9 (CRC-dropped), frames 12-14 -> 5 frames in 3 gaps
    assert st.seq_gaps == 3 and st.seq_lost == 5
    assert st.frames_ok == len(out) == 20 - 5


def test_unknown_type_and_bad_length_counted():
    enc = c.StreamEncoder()
    wire = c.encode_frame(0x77, 0, b"xyz") + c.encode_frame(c.STREAM_ECG_BLOCK, 1, b"short") \
        + c.encode_frame(c.STREAM_EVENT, 2, bytes([99, 1])) + c.encode_frame(c.STREAM_LOG, 3, b"ok")
    out, st = c.decode_stream(wire)
    assert st.unknown_type == 2 and st.bad_length == 1 and len(out) == 1 and st.seq_gaps == 0


def test_repeated_frame_is_not_a_huge_gap():
    f = c.encode_frame(c.STREAM_LOG, 4, b"a")
    g = c.encode_frame(c.STREAM_LOG, 5, b"b")
    out, st = c.decode_stream(f + g + g + c.encode_frame(c.STREAM_LOG, 6, b"c"))
    assert st.seq_repeats == 1 and st.seq_lost == 0 and len(out) == 4


def test_leading_zeros_and_empty_frames_harmless():
    out, st = c.decode_stream(b"\x00\x00\x00" + c.encode_frame(c.STREAM_LOG, 0, b"hi") + b"\x00\x00")
    assert len(out) == 1 and st.bad_length == st.bad_cobs == 0


def test_garbage_without_delimiter_is_bounded_and_recovers():
    d = c.StreamDecoder()
    d.feed(b"\x05" * 5000)
    out = d.feed(b"\x00" + c.encode_frame(c.STREAM_LOG, 0, b"back"))
    assert [m.text for m in out] == ["back"]
    assert d.stats.overflow > 0 and len(d._buf) == 0


def test_fuzz_never_raises_and_resyncs():
    rng = random.Random(42)
    frames = make_stream(300, seed=3)
    for trial in range(60):
        parts = []
        for f in frames:
            r = rng.random()
            if r < 0.05:
                continue  # lost
            if r < 0.10:
                b = bytearray(f); b[rng.randrange(len(b))] = rng.randrange(256); f = bytes(b)  # corrupt
            elif r < 0.14:
                f = f[:rng.randrange(1, len(f))]  # truncated (merges with the next frame)
            elif r < 0.17:
                f = f + f  # duplicate
            elif r < 0.20:
                f = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 40))) + f  # junk
            parts.append(f)
        wire = b"".join(parts)
        d = c.StreamDecoder()
        out = []
        i = 0
        while i < len(wire):
            k = rng.randrange(1, 90)
            out += d.feed(wire[i:i + k]); i += k
        # most of the surviving frames must still decode
        assert len(out) > 150
        assert d.stats.frames_ok == len(out)
    # pure random bytes
    d = c.StreamDecoder()
    d.feed(bytes(rng.randrange(256) for _ in range(100000)))


def test_resync_after_corrupt_delimiter():
    frames = make_stream(10)
    wire = bytearray(b"".join(frames))
    wire[len(frames[0]) - 1] = 0x42  # first delimiter destroyed: frames 0 and 1 merge
    out, st = c.decode_stream(bytes(wire))
    assert len(out) == 8 and (st.bad_crc + st.bad_length + st.bad_cobs) >= 1
