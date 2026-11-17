// A scripted CardLink for controller tests (sync broadcast streams, so the
// test drives time by emitting blocks).
import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';

CardStatus statusOf({
  CardMode mode = CardMode.ecg,
  MeasState meas = MeasState.waitContact,
  int flags = 0,
  int hrBpm = 0,
}) => CardStatus(
  proto: 1,
  mode: mode,
  meas: meas,
  battPct: 80,
  flags: flags,
  gain: 0,
  notch: 0,
  rld: 0,
  ledLimit: 2,
  sweep: 1,
  notchHz: 50,
  hrBpm: hrBpm,
  battMv: 3900,
  fwMajor: 0,
  fwMinor: 3,
  fwPatch: 1,
  uptimeMin: 1,
);

class TestCard implements CardLink {
  final _link = StreamController<LinkState>.broadcast(sync: true);
  final _status = StreamController<CardStatus>.broadcast(sync: true);
  final _ecg = StreamController<EcgBlock>.broadcast(sync: true);
  final _ppg = StreamController<PpgBlock>.broadcast(sync: true);
  final _events = StreamController<CardEvent>.broadcast(sync: true);
  final _log = StreamController<String>.broadcast(sync: true);

  CmdResult startResult = CmdResult.ok;
  int startCalls = 0, stopCalls = 0;
  CardMode? startedMode;

  @override
  LinkState currentLinkState = LinkState.connected;
  @override
  CardStatus? lastStatus = statusOf(mode: CardMode.idle, meas: MeasState.none);

  /// Next ECG sample index.
  int index = 1000;
  int _nextBeat = 1250;
  int beatNo = 0;

  /// RR in ms for beat number n (default 900 ms).
  int Function(int n) rrMs = (_) => 900;

  @override
  String get id => 'test';
  @override
  String get name => 'ECG Card TEST';
  @override
  Stream<LinkState> get linkState => _link.stream;
  @override
  Stream<CardStatus> get status => _status.stream;
  @override
  Stream<EcgBlock> get ecg => _ecg.stream;
  @override
  Stream<PpgBlock> get ppg => _ppg.stream;
  @override
  Stream<CardEvent> get events => _events.stream;
  @override
  Stream<String> get log => _log.stream;

  @override
  Future<void> connect() async {}
  @override
  Future<void> disconnect() async => setLink(LinkState.disconnected);

  @override
  Future<CmdResult> start(CardMode mode) async {
    startCalls++;
    startedMode = mode;
    return startResult;
  }

  @override
  Future<CmdResult> stop() async {
    stopCalls++;
    return CmdResult.ok;
  }

  @override
  Future<CmdResult> setSetting(int item, int value) async => CmdResult.ok;
  @override
  Future<CmdResult> setTime(DateTime now) async => CmdResult.ok;

  void setLink(LinkState s) {
    currentLinkState = s;
    _link.add(s);
  }

  void pushStatus(CardStatus s) {
    lastStatus = s;
    _status.add(s);
  }

  void meas(MeasState m, {CardMode mode = CardMode.ecg}) =>
      pushStatus(statusOf(mode: mode, meas: m));

  void event(CardEvent e) => _events.add(e);

  /// Streams [seconds] of ECG (blocks of 10) with flags; beats (R_PEAK events
  /// and a QRS in the samples) when [beats].
  void stream(double seconds, {int flags = 0, bool beats = true}) {
    final blocks = (seconds * 50).round();
    const unitsPerUv = 8 * 558 / 805.7;
    for (var b = 0; b < blocks; b++) {
      final s = Int16List(10);
      final peaks = <int>[];
      for (var i = 0; i < 10; i++) {
        final m = index + i;
        var uv = 30 * math.sin(m / 40); // a little baseline movement
        if (beats) {
          final d = (m - _nextBeat).toDouble();
          uv += 1200 * math.exp(-d * d / 18);
          if (m == _nextBeat) peaks.add(m);
        }
        s[i] = (uv * unitsPerUv).round();
      }
      _ecg.add(EcgBlock(start: index, flags: flags, samples: s));
      index += 10;
      for (final p in peaks) {
        _events.add(RPeakEvent(sampleIndex: p, hrBpm: (60000 / rrMs(beatNo)).round()));
      }
      // Beats keep their schedule even when not reported (contact lost).
      while (_nextBeat < index) {
        beatNo++;
        _nextBeat += rrMs(beatNo) ~/ 2;
      }
    }
  }
}
