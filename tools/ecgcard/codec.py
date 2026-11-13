"""Stream codec: COBS, CRC-16/CCITT-FALSE, frame pack/unpack and an incremental decoder.

Wire format (fw/include/stream_fmt.h): COBS(frame) + 0x00, where the decoded frame is
    u8 type, u8 seq, u16 len (LE), payload[len], u16 crc (LE)
and crc is CRC-16/CCITT-FALSE over type, seq, len and payload.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field, fields
from typing import Union

import numpy as np

STREAM_ECG_BLOCK = 0x10
STREAM_PPG_BLOCK = 0x11
STREAM_EVENT = 0x12
STREAM_LOG = 0x30

EV_R_PEAK, EV_PULSE, EV_LEAD, EV_STATE, EV_BUTTON = 1, 2, 3, 4, 5

ECG_SAMPLES_PER_BLOCK = 10
PPG_FRAMES_PER_BLOCK = 2
ECG_SCALE = 8
MAX_PAYLOAD = 512
HEADER_LEN = 4
CRC_LEN = 2
MAX_FRAME = HEADER_LEN + MAX_PAYLOAD + CRC_LEN
# Longest legal COBS body: frame plus one overhead byte per 254 (rounded up generously).
MAX_ENCODED = MAX_FRAME + MAX_FRAME // 254 + 2

ECG_FLAG_GAIN_HI = 0x01
ECG_FLAG_SETTLING = 0x02
ECG_FLAG_LEAD_OFF = 0x04
PPG_FLAG_DIST0 = 0x01
PPG_FLAG_DIST1 = 0x02
PPG_FLAG_NO_FINGER = 0x04

ECG_PAYLOAD_LEN = 4 + 1 + 2 * ECG_SAMPLES_PER_BLOCK  # 25
PPG_PAYLOAD_LEN = 4 + 2 + 1 + 4 * PPG_FRAMES_PER_BLOCK  # 15


# ---------------------------------------------------------------- COBS / CRC

def cobs_encode(data: bytes) -> bytes:
    """Consistent-overhead byte stuffing. The result contains no 0x00 byte."""
    out = bytearray()
    block = bytearray()
    for b in data:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """Inverse of cobs_encode. Raises ValueError on malformed input."""
    out = bytearray()
    i = 0
    n = len(data)
    if n == 0:
        raise ValueError("empty COBS block")
    while i < n:
        code = data[i]
        if code == 0:
            raise ValueError("zero byte in COBS data")
        end = i + code
        if end > n:
            raise ValueError("COBS run overruns the data")
        out += data[i + 1:end]
        i = end
        if code != 255 and i < n:
            out.append(0)
    return bytes(out)


def _make_crc_table() -> list[int]:
    table = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
        table.append(c & 0xFFFF)
    return table


_CRC_TABLE = _make_crc_table()


def crc16_ccitt_false(data: bytes, init: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0."""
    crc = init
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[(crc >> 8) ^ b]
    return crc


# ---------------------------------------------------------------- frames

def pack_frame(ftype: int, seq: int, payload: bytes) -> bytes:
    """Build the decoded frame (header, payload, CRC), without COBS."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    body = struct.pack("<BBH", ftype, seq & 0xFF, len(payload)) + payload
    return body + struct.pack("<H", crc16_ccitt_false(body))


def encode_frame(ftype: int, seq: int, payload: bytes) -> bytes:
    """Frame as sent on the wire: COBS(frame) + 0x00."""
    return cobs_encode(pack_frame(ftype, seq, payload)) + b"\x00"


class FrameError(ValueError):
    """Raised by unpack_frame; `kind` is 'length' or 'crc'."""

    def __init__(self, kind: str, msg: str) -> None:
        super().__init__(msg)
        self.kind = kind


def unpack_frame(frame: bytes) -> tuple[int, int, bytes]:
    """Validate a decoded frame and return (type, seq, payload)."""
    if len(frame) < HEADER_LEN + CRC_LEN:
        raise FrameError("length", "frame too short")
    ftype, seq, ln = struct.unpack_from("<BBH", frame)
    if ln > MAX_PAYLOAD or len(frame) != HEADER_LEN + ln + CRC_LEN:
        raise FrameError("length", "length field does not match frame size")
    (crc,) = struct.unpack_from("<H", frame, HEADER_LEN + ln)
    if crc != crc16_ccitt_false(frame[:HEADER_LEN + ln]):
        raise FrameError("crc", "bad CRC")
    return ftype, seq, frame[HEADER_LEN:HEADER_LEN + ln]


# ---------------------------------------------------------------- messages

@dataclass
class EcgBlock:
    start: int
    flags: int
    samples: np.ndarray  # int16[10], stream units (ADC codes x 8, relative to mid)
    seq: int = 0

    @property
    def gain_hi(self) -> bool:
        return bool(self.flags & ECG_FLAG_GAIN_HI)

    @property
    def settling(self) -> bool:
        return bool(self.flags & ECG_FLAG_SETTLING)

    @property
    def lead_off(self) -> bool:
        return bool(self.flags & ECG_FLAG_LEAD_OFF)


@dataclass
class PpgBlock:
    start: int
    led_code: int
    flags: int
    coarse: tuple[int, int]
    fine: tuple[int, int]  # raw int16 (wraps modulo 2^16)
    seq: int = 0

    @property
    def disturbed(self) -> tuple[bool, bool]:
        return bool(self.flags & PPG_FLAG_DIST0), bool(self.flags & PPG_FLAG_DIST1)

    @property
    def no_finger(self) -> bool:
        return bool(self.flags & PPG_FLAG_NO_FINGER)


@dataclass
class RPeakEvent:
    sample: int
    hr_bpm: int
    seq: int = 0


@dataclass
class PulseEvent:
    foot_sample: int  # in ECG sample units (500 Hz)
    pulse_bpm: int
    pat_ms: int  # 0 = n/a
    pi_permille: int
    seq: int = 0


@dataclass
class LeadEvent:
    contact: int
    seq: int = 0


@dataclass
class StateEvent:
    mode: int  # 0 idle, 1 ECG, 2 PPG, 3 both
    notch_hz: int
    mid_code: int
    batt_pct: int
    flags: int  # bit0 usb, bit1 charging
    seq: int = 0


@dataclass
class ButtonEvent:
    button: int
    kind: int  # 0 short, 1 long
    seq: int = 0


@dataclass
class Log:
    text: str
    seq: int = 0


Event = Union[RPeakEvent, PulseEvent, LeadEvent, StateEvent, ButtonEvent]
Message = Union[EcgBlock, PpgBlock, RPeakEvent, PulseEvent, LeadEvent, StateEvent, ButtonEvent, Log]


def message_payload(msg: Message) -> tuple[int, bytes]:
    """Return (frame type, payload bytes) for a message."""
    if isinstance(msg, EcgBlock):
        s = np.asarray(msg.samples).astype(np.int64)
        if s.shape != (ECG_SAMPLES_PER_BLOCK,):
            raise ValueError("ECG block needs 10 samples")
        s = ((s + 32768) & 0xFFFF) - 32768  # wrap to int16
        return STREAM_ECG_BLOCK, struct.pack("<IB10h", msg.start & 0xFFFFFFFF, msg.flags, *s.tolist())
    if isinstance(msg, PpgBlock):
        c = [((int(v) + 32768) & 0xFFFF) - 32768 for v in (msg.coarse[0], msg.fine[0], msg.coarse[1], msg.fine[1])]
        return STREAM_PPG_BLOCK, struct.pack("<IHB4h", msg.start & 0xFFFFFFFF, msg.led_code, msg.flags, *c)
    if isinstance(msg, RPeakEvent):
        return STREAM_EVENT, struct.pack("<BIH", EV_R_PEAK, msg.sample, msg.hr_bpm)
    if isinstance(msg, PulseEvent):
        return STREAM_EVENT, struct.pack("<BIHHH", EV_PULSE, msg.foot_sample, msg.pulse_bpm, msg.pat_ms, msg.pi_permille)
    if isinstance(msg, LeadEvent):
        return STREAM_EVENT, struct.pack("<BB", EV_LEAD, msg.contact)
    if isinstance(msg, StateEvent):
        return STREAM_EVENT, struct.pack("<BBBHBB", EV_STATE, msg.mode, msg.notch_hz, msg.mid_code, msg.batt_pct, msg.flags)
    if isinstance(msg, ButtonEvent):
        return STREAM_EVENT, struct.pack("<BBB", EV_BUTTON, msg.button, msg.kind)
    if isinstance(msg, Log):
        return STREAM_LOG, msg.text.encode("utf-8")
    raise TypeError(f"cannot encode {type(msg)!r}")


def parse_payload(ftype: int, seq: int, payload: bytes) -> Message | None:
    """Parse a payload into a message. Raises FrameError('length') on a size mismatch,
    returns None for an unknown type or event kind."""
    n = len(payload)
    if ftype == STREAM_ECG_BLOCK:
        if n != ECG_PAYLOAD_LEN:
            raise FrameError("length", "ECG block size")
        start, flags, *s = struct.unpack("<IB10h", payload)
        return EcgBlock(start, flags, np.array(s, dtype=np.int16), seq)
    if ftype == STREAM_PPG_BLOCK:
        if n != PPG_PAYLOAD_LEN:
            raise FrameError("length", "PPG block size")
        start, led, flags, c0, f0, c1, f1 = struct.unpack("<IHB4h", payload)
        return PpgBlock(start, led, flags, (c0, c1), (f0, f1), seq)
    if ftype == STREAM_EVENT:
        if n < 1:
            raise FrameError("length", "empty event")
        kind = payload[0]
        sizes = {EV_R_PEAK: 7, EV_PULSE: 11, EV_LEAD: 2, EV_STATE: 7, EV_BUTTON: 3}
        if kind not in sizes:
            return None
        if n != sizes[kind]:
            raise FrameError("length", "event size")
        if kind == EV_R_PEAK:
            return RPeakEvent(*struct.unpack_from("<IH", payload, 1), seq=seq)
        if kind == EV_PULSE:
            return PulseEvent(*struct.unpack_from("<IHHH", payload, 1), seq=seq)
        if kind == EV_LEAD:
            return LeadEvent(payload[1], seq)
        if kind == EV_STATE:
            mode, notch, mid, batt, flags = struct.unpack_from("<BBHBB", payload, 1)
            return StateEvent(mode, notch, mid, batt, flags, seq)
        return ButtonEvent(payload[1], payload[2], seq)
    if ftype == STREAM_LOG:
        return Log(payload.decode("utf-8", errors="replace"), seq)
    return None


def encode_message(msg: Message, seq: int) -> bytes:
    ftype, payload = message_payload(msg)
    return encode_frame(ftype, seq, payload)


class StreamEncoder:
    """Encodes messages with an auto-incrementing sequence counter (like the firmware)."""

    def __init__(self, seq: int = 0) -> None:
        self.seq = seq & 0xFF

    def encode(self, msg: Message) -> bytes:
        out = encode_message(msg, self.seq)
        self.seq = (self.seq + 1) & 0xFF
        return out

    def skip(self, n: int = 1) -> None:
        """Advance the counter without emitting a frame (simulates a lost frame)."""
        self.seq = (self.seq + n) & 0xFF


# ---------------------------------------------------------------- decoder

@dataclass
class DecoderStats:
    bytes: int = 0
    frames_ok: int = 0       # frames delivered as typed messages
    bad_crc: int = 0
    bad_length: int = 0      # bad frame size, length field, or payload size for the type
    bad_cobs: int = 0        # delimited chunk that is not valid COBS
    unknown_type: int = 0    # valid frame, unknown type or event kind (still counted in seq)
    seq_gaps: int = 0        # number of places where the sequence jumped
    seq_lost: int = 0        # frames missing according to the sequence counter
    seq_repeats: int = 0     # frames whose seq is behind the expected one (duplicate/reorder)
    overflow: int = 0        # bytes dropped without a delimiter (garbage)
    by_type: dict[int, int] = field(default_factory=dict)

    def as_dict(self) -> dict[str, int]:
        d = {f.name: getattr(self, f.name) for f in fields(self) if f.name != "by_type"}
        return d


def delta_ok(expect: int, seq: int) -> bool:
    """True if seq is at or ahead of the expected value (modulo 256)."""
    return ((seq - expect) & 0xFF) < 128


class StreamDecoder:
    """Incremental decoder: feed() arbitrary byte chunks, get typed messages.

    Frames are delimited by 0x00, so after any corruption the decoder recovers at the next
    delimiter. A bad-CRC frame is dropped, and the sequence counter shows it as a gap in
    the following frame. Garbage longer than the largest legal frame is discarded until the
    next delimiter.
    """

    def __init__(self) -> None:
        self.stats = DecoderStats()
        self._buf = bytearray()
        self._skipping = False
        self._expect: int | None = None

    def reset_sequence(self) -> None:
        self._expect = None

    def feed(self, data: bytes) -> list[Message]:
        out: list[Message] = []
        st = self.stats
        st.bytes += len(data)
        start = 0
        n = len(data)
        while start < n:
            end = data.find(b"\x00", start)
            if end < 0:
                if not self._skipping:
                    self._buf += data[start:]
                    if len(self._buf) > MAX_ENCODED:
                        st.overflow += len(self._buf)
                        st.bad_length += 1
                        self._buf.clear()
                        self._skipping = True
                else:
                    st.overflow += n - start
                break
            if self._skipping:
                st.overflow += end - start
                self._skipping = False
            else:
                self._buf += data[start:end]
                if self._buf:
                    msg = self._frame(bytes(self._buf))
                    if msg is not None:
                        out.append(msg)
                self._buf.clear()
            start = end + 1
        return out

    def _frame(self, chunk: bytes) -> Message | None:
        st = self.stats
        if len(chunk) > MAX_ENCODED:
            st.bad_length += 1
            return None
        try:
            frame = cobs_decode(chunk)
        except ValueError:
            st.bad_cobs += 1
            return None
        try:
            ftype, seq, payload = unpack_frame(frame)
        except FrameError as e:
            if e.kind == "crc":
                st.bad_crc += 1
            else:
                st.bad_length += 1
            return None
        # The frame is intact, so its sequence number is trustworthy.
        if self._expect is not None and seq != self._expect:
            delta = (seq - self._expect) & 0xFF
            if delta >= 128:  # repeated or reordered frame: do not move the counter
                st.seq_repeats += 1
            else:
                st.seq_gaps += 1
                st.seq_lost += delta
        if self._expect is None or delta_ok(self._expect, seq):
            self._expect = (seq + 1) & 0xFF
        try:
            msg = parse_payload(ftype, seq, payload)
        except FrameError:
            st.bad_length += 1
            return None
        if msg is None:
            st.unknown_type += 1
            return None
        st.frames_ok += 1
        st.by_type[ftype] = st.by_type.get(ftype, 0) + 1
        return msg


def decode_stream(data: bytes) -> tuple[list[Message], DecoderStats]:
    """Decode a complete byte string."""
    dec = StreamDecoder()
    msgs = dec.feed(data)
    return msgs, dec.stats
