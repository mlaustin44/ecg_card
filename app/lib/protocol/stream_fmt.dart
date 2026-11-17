// Card stream payloads (fw/include/stream_fmt.h). Pure Dart.
//
// Data shapes are fixed here (the UI and analysis layers code against them);
// the parsers are implemented in this file by the protocol layer.
import 'dart:typed_data';

/// ECG sample rate of the stream (and of R_PEAK / PULSE indices).
const int ecgHz = 500;

/// PPG frame rate.
const int ppgHz = 100;

/// STREAM_ECG_BLOCK: 10 samples, every 20 ms.
class EcgBlock {
  const EcgBlock({required this.start, required this.flags, required this.samples});

  /// Index of the first sample (500 Hz, from acquisition start).
  final int start;
  final int flags;

  /// Stream units (ADC codes relative to mid-scale, x8). See units.dart.
  final Int16List samples;

  bool get gainHi => flags & 0x01 != 0;
  bool get settling => flags & 0x02 != 0;
  bool get leadOff => flags & 0x04 != 0;

  static const int bytes = 25;
}

/// STREAM_PPG_BLOCK: 2 frames, every 20 ms.
class PpgBlock {
  const PpgBlock({
    required this.start,
    required this.ledCode,
    required this.flags,
    required this.coarse,
    required this.fine,
  });

  /// Index of the first frame (100 Hz).
  final int start;
  final int ledCode;
  final int flags;
  final List<int> coarse; // 2 frames
  final List<int> fine; // 2 frames, wraps modulo 2^16

  bool get noFinger => flags & 0x04 != 0;

  /// Frame [i] (0 or 1) disturbed by an AGC or offset step.
  bool disturbed(int i) => flags & (1 << i) != 0;

  static const int bytes = 15;
}

/// STREAM_EVENT payloads.
sealed class CardEvent {
  const CardEvent();
}

class RPeakEvent extends CardEvent {
  const RPeakEvent({required this.sampleIndex, required this.hrBpm});
  final int sampleIndex; // 500 Hz
  final int hrBpm;
}

class PulseEvent extends CardEvent {
  const PulseEvent({
    required this.footIndex,
    required this.pulseBpm,
    required this.patMs,
    required this.piPermille,
  });
  final int footIndex; // in ECG sample units (500 Hz)
  final int pulseBpm;
  final int patMs; // 0 = n/a
  final int piPermille;
}

class LeadEvent extends CardEvent {
  const LeadEvent({required this.contact});
  final bool contact;
}

class StateEvent extends CardEvent {
  const StateEvent({
    required this.mode,
    required this.notchHz,
    required this.midCode,
    required this.battPct,
    required this.flags,
    this.gainLo = 558,
    this.gainHi = 1098,
  });
  final int mode; // LINK_MODE_*
  final int notchHz;
  final int midCode;
  final int battPct;
  final int flags; // bit0 usb, bit1 charging
  final int gainLo;
  final int gainHi;
}

class ButtonEvent extends CardEvent {
  const ButtonEvent({required this.button, required this.long});
  final int button; // 1 or 2
  final bool long;
}

class CmdResultEvent extends CardEvent {
  const CmdResultEvent({required this.cmd, required this.result});
  final int cmd;
  final int result; // LINK_RES_*
}

class UnknownEvent extends CardEvent {
  const UnknownEvent({required this.kind, required this.payload});
  final int kind;
  final Uint8List payload;
}

/// STREAM_EVENT kinds (fw/include/stream_fmt.h; CMD_RESULT from link_fmt.h).
abstract final class EventKind {
  static const rPeak = 1, pulse = 2, lead = 3, state = 4, button = 5, cmdResult = 6;
}

/// Split an ECG notification (n whole blocks back to back) into blocks.
/// A trailing partial block is ignored.
List<EcgBlock> parseEcgNotification(Uint8List data) {
  final bd = ByteData.sublistView(data);
  final n = data.length ~/ EcgBlock.bytes;
  return List.generate(n, (k) {
    final o = k * EcgBlock.bytes;
    final s = Int16List(10);
    for (var i = 0; i < 10; i++) {
      s[i] = bd.getInt16(o + 5 + 2 * i, Endian.little);
    }
    return EcgBlock(start: bd.getUint32(o, Endian.little), flags: bd.getUint8(o + 4), samples: s);
  }, growable: false);
}

/// Split a PPG notification into blocks. A trailing partial block is ignored.
List<PpgBlock> parsePpgNotification(Uint8List data) {
  final bd = ByteData.sublistView(data);
  final n = data.length ~/ PpgBlock.bytes;
  return List.generate(n, (k) {
    final o = k * PpgBlock.bytes;
    return PpgBlock(
      start: bd.getUint32(o, Endian.little),
      ledCode: bd.getUint16(o + 4, Endian.little),
      flags: bd.getUint8(o + 6),
      coarse: [bd.getInt16(o + 7, Endian.little), bd.getInt16(o + 11, Endian.little)],
      fine: [bd.getInt16(o + 9, Endian.little), bd.getInt16(o + 13, Endian.little)],
    );
  }, growable: false);
}

/// Smallest payload (kind byte included) for each known event kind. Longer
/// payloads are accepted (fields appended by newer firmware are ignored).
const Map<int, int> _eventMinLen = {
  EventKind.rPeak: 7,
  EventKind.pulse: 11,
  EventKind.lead: 2,
  EventKind.state: 7, // 11 with the gains; 7 = older firmware
  EventKind.button: 3,
  EventKind.cmdResult: 3,
};

/// Parse one Events notification (one STREAM_EVENT payload). Never throws:
/// an unknown kind, an empty or a too-short payload gives an [UnknownEvent]
/// (kind -1 when empty).
CardEvent parseEvent(Uint8List data) {
  if (data.isEmpty) return UnknownEvent(kind: -1, payload: Uint8List(0));
  final kind = data[0];
  final min = _eventMinLen[kind];
  if (min == null || data.length < min) {
    return UnknownEvent(kind: kind, payload: Uint8List.fromList(data));
  }
  final bd = ByteData.sublistView(data);
  int u16(int o) => bd.getUint16(o, Endian.little);
  int u32(int o) => bd.getUint32(o, Endian.little);
  switch (kind) {
    case EventKind.rPeak:
      return RPeakEvent(sampleIndex: u32(1), hrBpm: u16(5));
    case EventKind.pulse:
      return PulseEvent(footIndex: u32(1), pulseBpm: u16(5), patMs: u16(7), piPermille: u16(9));
    case EventKind.lead:
      return LeadEvent(contact: data[1] != 0);
    case EventKind.state:
      final full = data.length >= 11;
      return StateEvent(
        mode: data[1],
        notchHz: data[2],
        midCode: u16(3),
        battPct: data[5],
        flags: data[6],
        gainLo: full ? u16(7) : 558,
        gainHi: full ? u16(9) : 1098,
      );
    case EventKind.button:
      return ButtonEvent(button: data[1], long: data[2] == 1);
    default: // EventKind.cmdResult
      return CmdResultEvent(cmd: data[1], result: data[2]);
  }
}
