// Control characteristic writes (LINK_CMD; fw/include/link_fmt.h) and their
// results. Pure Dart.
import 'dart:typed_data';

import 'link_status.dart';

/// LINK_RES_* (result of a Control write, from the CMD_RESULT event).
enum CmdResult { ok, unknown, badArg, refusedUsb, refusedBattery, busy, fault, timeout }

/// settings_item_t (fw/include/settings.h), for the Developer screen.
abstract final class SettingItem {
  static const gain = 0, notch = 1, rld = 2, ledLimit = 3, sweep = 4, ble = 5;
  static const restore = 6, view = 7;
}

/// Result code (LINK_RES_*) to [CmdResult]; unknown codes map to [CmdResult.fault].
CmdResult cmdResultOf(int code) =>
    code >= 0 && code < CmdResult.timeout.index ? CmdResult.values[code] : CmdResult.fault;

/// LINK_CMD_* command bytes (first byte of a Control write, and
/// [CmdResultEvent.cmd]).
abstract final class CmdId {
  static const start = 1, stop = 2, set = 3, setTime = 4, getStatus = 5;
}

abstract final class Commands {
  static Uint8List start(CardMode mode) => Uint8List.fromList([CmdId.start, mode.index]);
  static Uint8List stop() => Uint8List.fromList([CmdId.stop]);
  static Uint8List set(int item, int value) =>
      Uint8List.fromList([CmdId.set, item & 0xFF, value & 0xFF]);

  /// u32 unix seconds (UTC) + i16 UTC offset in minutes (east positive) of [now]
  /// (pass a local DateTime, e.g. DateTime.now(), so the card shows local time).
  static Uint8List setTime(DateTime now) =>
      setTimeRaw(now.millisecondsSinceEpoch ~/ 1000, now.timeZoneOffset.inMinutes);

  static Uint8List setTimeRaw(int unixSeconds, int utcOffsetMinutes) {
    final b = Uint8List(7);
    final bd = ByteData.sublistView(b);
    b[0] = CmdId.setTime;
    bd.setUint32(1, unixSeconds & 0xFFFFFFFF, Endian.little);
    bd.setInt16(5, utcOffsetMinutes.clamp(-32768, 32767), Endian.little);
    return b;
  }

  static Uint8List getStatus() => Uint8List.fromList([CmdId.getStatus]);
}
