// Small fakes for the core UI tests (the real device / data layers are not
// needed: every provider the screens read is overridden with these).
import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:ecg_card_app/analysis/models.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/data/session_store.dart';
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';
import 'package:ecg_card_app/state/device_providers.dart';
import 'package:ecg_card_app/state/live_session.dart';

CardStatus fakeStatus({int batt = 82, bool noContact = false}) => CardStatus(
  proto: 1,
  mode: CardMode.ecg,
  meas: MeasState.running,
  battPct: batt,
  flags: noContact ? 0x08 : 0,
  gain: 0,
  notch: 0,
  rld: 0,
  ledLimit: 0,
  sweep: 0,
  notchHz: 50,
  hrBpm: 64,
  battMv: 3900,
  fwMajor: 1,
  fwMinor: 4,
  fwPatch: 0,
  uptimeMin: 12,
);

class FakeLink implements CardLink {
  FakeLink({
    this.id = 'AA:BB',
    this.name = 'ECG Card 3F2A',
    this.state = LinkState.connected,
    CardStatus? status,
  }) : lastStatus = status ?? fakeStatus();

  @override
  final String id;
  @override
  final String name;
  LinkState state;
  @override
  CardStatus? lastStatus;

  @override
  Stream<LinkState> get linkState => Stream.value(state);
  @override
  LinkState get currentLinkState => state;
  @override
  Stream<CardStatus> get status => const Stream.empty();
  @override
  Stream<EcgBlock> get ecg => const Stream.empty();
  @override
  Stream<PpgBlock> get ppg => const Stream.empty();
  @override
  Stream<CardEvent> get events => const Stream.empty();
  @override
  Stream<String> get log => const Stream.empty();
  @override
  Future<void> connect() async {}
  @override
  Future<void> disconnect() async {}
  @override
  Future<CmdResult> start(CardMode mode) async => CmdResult.ok;
  @override
  Future<CmdResult> stop() async => CmdResult.ok;
  @override
  Future<CmdResult> setSetting(int item, int value) async => CmdResult.ok;
  @override
  Future<CmdResult> setTime(DateTime now) async => CmdResult.ok;
}

class FakeScanner implements CardScanner {
  FakeScanner(this.found);
  final List<FoundCard> found;
  bool stopped = false;

  @override
  Stream<List<FoundCard>> scan() => Stream.value(found);
  @override
  Future<void> stop() async => stopped = true;
  @override
  CardLink linkTo(FoundCard card) => FakeLink(id: card.id, name: card.name);
}

class FakeCurrentCard extends CurrentCard {
  FakeCurrentCard([this.initial]);
  final CardLink? initial;
  final chosen = <FoundCard>[];

  @override
  CardLink? build() => initial;

  @override
  Future<void> choose(FoundCard card) async {
    chosen.add(card);
    state = FakeLink(id: card.id, name: card.name);
  }

  @override
  Future<void> forget() async => state = null;
}

class FakeLive implements LiveSessionApi {
  FakeLive([this._state = const LiveSession()]);

  LiveSession _state;
  final _changes = StreamController<LiveSession>.broadcast();
  final _ecg = StreamController<Float32List>.broadcast();
  final begun = <(SessionKind, Duration?)>[];
  int cancels = 0;

  /// Whether begin() moves to waitContact (otherwise the state is left alone).
  bool beginMoves = false;

  void emit(LiveSession s) {
    _state = s;
    _changes.add(s);
  }

  void addEcg(Float32List chunk) => _ecg.add(chunk);

  @override
  LiveSession get state => _state;
  @override
  Stream<LiveSession> get changes => _changes.stream;
  @override
  Stream<Float32List> get ecgUv => _ecg.stream;

  @override
  Future<void> begin(SessionKind kind, {Duration? target, double? breathsPerMin}) async {
    begun.add((kind, target));
    if (beginMoves) emit(LiveSession(phase: LivePhase.waitContact, kind: kind, target: target));
  }

  @override
  Future<SessionRecord?> finish({bool save = true}) async => _state.record;

  @override
  Future<void> cancel() async {
    cancels++;
    _state = const LiveSession();
  }
}

class FakeStore implements SessionStore {
  FakeStore([List<SessionRecord> initial = const []]) : _all = [...initial];
  final List<SessionRecord> _all;
  final _ctl = StreamController<List<SessionRecord>>.broadcast();

  List<SessionRecord> get all => List.unmodifiable(_all);

  @override
  Future<void> save(SessionRecord record) async {
    _all.removeWhere((r) => r.id == record.id);
    _all.add(record);
    _all.sort((a, b) => b.start.compareTo(a.start));
    _ctl.add(all);
  }

  @override
  Future<SessionRecord?> get(String id) async => _all.where((r) => r.id == id).firstOrNull;

  @override
  Future<void> delete(String id) async {
    _all.removeWhere((r) => r.id == id);
    _ctl.add(all);
  }

  @override
  Stream<List<SessionRecord>> watchAll() async* {
    yield all;
    yield* _ctl.stream;
  }
}

/// Synthetic thumb ECG (µV): P, QRS, T bumps on a gently wandering baseline,
/// with the beat rate swaying with breathing (6 breaths / min).
List<double> synthEcg({
  required double seconds,
  int hz = 500,
  double bpm = 64,
  double qrsUv = 1100,
}) {
  final n = (seconds * hz).round();
  final out = List<double>.filled(n, 0);
  final beats = <double>[];
  var t = 0.35;
  while (t < seconds + 1) {
    beats.add(t);
    final rate = bpm + 6 * math.sin(2 * math.pi * t / 10);
    t += 60 / rate;
  }
  double g(double x, double mu, double sigma, double a) =>
      a * math.exp(-0.5 * math.pow((x - mu) / sigma, 2));
  final rnd = math.Random(7);
  for (var i = 0; i < n; i++) {
    final x = i / hz;
    var v = 40 * math.sin(2 * math.pi * x * 0.25) + (rnd.nextDouble() - 0.5) * 18;
    for (final b in beats) {
      final d = x - b;
      if (d < -0.3 || d > 0.5) continue;
      v +=
          g(d, -0.17, 0.022, qrsUv * 0.11) + // P
          g(d, -0.025, 0.008, -qrsUv * 0.12) + // Q
          g(d, 0, 0.010, qrsUv) + // R
          g(d, 0.028, 0.010, -qrsUv * 0.22) + // S
          g(d, 0.25, 0.045, qrsUv * 0.24); // T
    }
    out[i] = v;
  }
  return out;
}

/// RR intervals swaying with breathing, for [seconds].
List<RrInterval> synthRr({double seconds = 60, double bpm = 64}) {
  final rr = <RrInterval>[];
  var t = 0.0;
  while (t < seconds) {
    final rate = bpm + 6 * math.sin(2 * math.pi * t / 10) + 1.5 * math.sin(t * 0.7);
    final ms = (60000 / rate).round();
    t += ms / 1000;
    rr.add(RrInterval(tSec: t, ms: ms));
  }
  return rr;
}

SessionRecord sampleRecord({
  String id = 's1',
  DateTime? start,
  SessionKind kind = SessionKind.checkIn,
  int calm = 4,
  String? note,
}) => SessionRecord(
  id: id,
  start: start ?? DateTime(2026, 11, 17, 8, 42),
  kind: kind,
  duration: const Duration(seconds: 60),
  rr: synthRr(),
  stats: const HrvStats(
    beats: 64,
    meanHr: 64,
    minHr: 57,
    maxHr: 72,
    rmssdMs: 48,
    sdnnMs: 52,
    pnn50: 0.27,
  ),
  calmScore: calm,
  coherence: kind == SessionKind.breathing ? 0.72 : null,
  stripUv: synthEcg(seconds: 10, hz: 250),
  stripHz: 250,
  note: note,
  cardName: 'ECG Card 3F2A',
);
