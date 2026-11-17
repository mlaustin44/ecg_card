// LINK_STATUS (Status characteristic, 20 bytes; fw/include/link_fmt.h). Pure Dart.
import 'dart:typed_data';

/// LINK_MODE_*.
enum CardMode { idle, ecg, ppg, both, hrMonitor }

/// LINK_MODE_* value to [CardMode]; unknown values map to [CardMode.idle].
CardMode cardModeOf(int v) =>
    v >= 0 && v < CardMode.values.length ? CardMode.values[v] : CardMode.idle;

/// ui_meas_state_t (fw/include/ui_model.h), while a mode is active.
/// [none] = 0xFF (no mode active) or an unknown value.
enum MeasState { waitContact, settling, running, usbBlocked, fault, none }

MeasState measStateOf(int v) =>
    v >= 0 && v < MeasState.none.index ? MeasState.values[v] : MeasState.none;

/// LINK_ST_* flag bits of [CardStatus.flags].
abstract final class StatusFlag {
  static const usb = 0x01, charging = 0x02, chargeFault = 0x04, noContact = 0x08;
  static const battLow = 0x10, gainHi = 0x20, timeValid = 0x40;
}

class CardStatus {
  const CardStatus({
    required this.proto,
    required this.mode,
    required this.meas,
    required this.battPct,
    required this.flags,
    required this.gain,
    required this.notch,
    required this.rld,
    required this.ledLimit,
    required this.sweep,
    required this.notchHz,
    required this.hrBpm,
    required this.battMv,
    required this.fwMajor,
    required this.fwMinor,
    required this.fwPatch,
    required this.uptimeMin,
  });

  final int proto;
  final CardMode mode;
  final MeasState meas;
  final int? battPct; // null = unknown (255)
  final int flags; // LINK_ST_*
  final int gain, notch, rld, ledLimit, sweep; // settings.h enums
  final int notchHz;
  final int hrBpm; // 0 = n/a
  final int? battMv; // null = unknown
  final int fwMajor, fwMinor, fwPatch;
  final int uptimeMin;

  bool get usb => flags & 0x01 != 0;
  bool get charging => flags & 0x02 != 0;
  bool get chargeFault => flags & 0x04 != 0;
  bool get noContact => flags & 0x08 != 0;
  bool get battLow => flags & 0x10 != 0;
  bool get gainHi => flags & 0x20 != 0;
  bool get timeValid => flags & 0x40 != 0;

  String get firmware => '$fwMajor.$fwMinor.$fwPatch';

  static const int bytes = 20;

  /// Parse a Status read / notification. Throws [FormatException] when
  /// shorter than [bytes]; extra trailing bytes are ignored.
  static CardStatus parse(Uint8List data) {
    if (data.length < bytes) {
      throw FormatException('Status payload ${data.length} B, expected $bytes');
    }
    final bd = ByteData.sublistView(data);
    final batt = data[3];
    final mv = bd.getUint16(12, Endian.little);
    return CardStatus(
      proto: data[0],
      mode: cardModeOf(data[1]),
      meas: measStateOf(data[2]),
      battPct: batt == 255 ? null : batt,
      flags: data[4],
      gain: data[5],
      notch: data[6],
      rld: data[7],
      ledLimit: data[8],
      sweep: data[9],
      notchHz: data[10],
      hrBpm: data[11],
      battMv: mv == 0 ? null : mv,
      fwMajor: data[14],
      fwMinor: data[15],
      fwPatch: data[16],
      uptimeMin: bd.getUint16(18, Endian.little),
    );
  }

  /// Encode (the inverse of [parse]; used by the fake card and tests).
  Uint8List encode() {
    final b = Uint8List(bytes);
    final bd = ByteData.sublistView(b);
    b[0] = proto;
    b[1] = mode.index;
    b[2] = meas == MeasState.none ? 0xFF : meas.index;
    b[3] = battPct ?? 255;
    b[4] = flags;
    b[5] = gain;
    b[6] = notch;
    b[7] = rld;
    b[8] = ledLimit;
    b[9] = sweep;
    b[10] = notchHz;
    b[11] = hrBpm.clamp(0, 255);
    bd.setUint16(12, battMv ?? 0, Endian.little);
    b[14] = fwMajor;
    b[15] = fwMinor;
    b[16] = fwPatch;
    bd.setUint16(18, uptimeMin, Endian.little);
    return b;
  }

  CardStatus copyWith({
    CardMode? mode,
    MeasState? meas,
    int? battPct,
    int? flags,
    int? gain,
    int? notch,
    int? rld,
    int? ledLimit,
    int? sweep,
    int? notchHz,
    int? hrBpm,
    int? battMv,
    int? uptimeMin,
  }) => CardStatus(
    proto: proto,
    mode: mode ?? this.mode,
    meas: meas ?? this.meas,
    battPct: battPct ?? this.battPct,
    flags: flags ?? this.flags,
    gain: gain ?? this.gain,
    notch: notch ?? this.notch,
    rld: rld ?? this.rld,
    ledLimit: ledLimit ?? this.ledLimit,
    sweep: sweep ?? this.sweep,
    notchHz: notchHz ?? this.notchHz,
    hrBpm: hrBpm ?? this.hrBpm,
    battMv: battMv ?? this.battMv,
    fwMajor: fwMajor,
    fwMinor: fwMinor,
    fwPatch: fwPatch,
    uptimeMin: uptimeMin ?? this.uptimeMin,
  );

  @override
  String toString() =>
      'CardStatus(mode: ${mode.name}, meas: ${meas.name}, batt: $battPct %, '
      'flags: 0x${flags.toRadixString(16)}, hr: $hrBpm, fw: $firmware)';
}
