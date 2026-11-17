// The live session controller: implements LiveSessionApi against a CardLink.
//
// It starts the card in ECG mode, maps Status (meas) / Events / ECG block
// flags to a LivePhase, measures running time from the ECG sample count (so
// it pauses while contact is lost and never counts settling), feeds R peaks
// to the RR tracker, emits display-filtered microvolts for the live trace,
// keeps the raw running ECG for the saved strip, and on finish builds and
// (when enough was measured) saves a SessionRecord.
import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import '../analysis/breathing.dart';
import '../analysis/ecg_filter.dart';
import '../analysis/hrv.dart';
import '../analysis/models.dart';
import '../analysis/rr.dart';
import '../data/session_record.dart';
import '../data/session_store.dart';
import '../device/card_link.dart';
import '../protocol/commands.dart';
import '../protocol/link_status.dart';
import '../protocol/stream_fmt.dart';
import '../protocol/units.dart';
import 'live_session.dart';
import 'wake_lock.dart';

/// Friendly texts for LivePhase.error (no alarm words, no medical words).
abstract final class SessionMessages {
  static const usb = 'Unplug the USB cable — the card measures on battery only.';
  static const lowBattery =
      "The card's battery is too low to measure. Charge it for a while, then unplug it and try again.";
  static const noCard = 'No card is connected. Connect your ECG Card and try again.';
  static const notConnected =
      "Your card isn't connected right now. Make sure it's switched on and close by, then try again.";
  static const busy =
      'The card is busy with another measurement. Stop it on the card, then try again.';
  static const timeout =
      "The card didn't answer. Make sure it's switched on and close by, then try again.";
  static const fault =
      "The card couldn't start measuring. Switch it off and on again, then try again.";
  static const refused =
      "The card didn't accept the request. Try again, or check for a card update.";
  static const disconnected =
      "Your card disconnected before there was enough to keep. Make sure it's switched on and close by, then try again.";

  static String forResult(CmdResult r) => switch (r) {
    CmdResult.refusedUsb => usb,
    CmdResult.refusedBattery => lowBattery,
    CmdResult.busy => busy,
    CmdResult.timeout => timeout,
    CmdResult.fault => fault,
    _ => refused,
  };
}

class SessionController implements LiveSessionApi {
  SessionController({
    required CardLink? Function() card,
    required this.store,
    this.settleDuration = const Duration(milliseconds: 2500),
    this.minSaveDuration = const Duration(seconds: 20),
    this.minSaveBeats = 10,
    this.coherenceEvery = const Duration(seconds: 5),
    this.maxStripBufferSeconds = 20 * 60,
    this.wakeLock = const NoWakeLock(),
    this.linkGrace = const Duration(seconds: 20),
  }) : _cardOf = card;

  final CardLink? Function() _cardOf;
  final SessionStore store;

  /// The card's front-end settling time (fw ECG_SETTLE_MS); settle progress
  /// is estimated over it because the card does not send progress over BLE.
  final Duration settleDuration;

  /// A session is saved when at least this much was measured ...
  final Duration minSaveDuration;

  /// ... and at least this many beats were accepted.
  final int minSaveBeats;

  /// Breathing sessions: coherence is recomputed this often (measured time).
  final Duration coherenceEvery;

  /// Raw ECG kept for the strip (the cleanest 10 s is chosen from it).
  final int maxStripBufferSeconds;

  /// How long a dropped Bluetooth link may take to come back before the
  /// session ends (saved when enough was measured).
  final Duration linkGrace;
  Timer? _linkTimer;

  /// Keeps the screen on while a session is active (subscribed to the card).
  final WakeLock wakeLock;
  bool _awake = false;

  LiveSession _state = const LiveSession();
  final _changes = StreamController<LiveSession>.broadcast();
  final _ecgOut = StreamController<Float32List>.broadcast();

  CardLink? _card;
  final List<StreamSubscription<Object?>> _subs = [];
  Timer? _ticker;
  final Stopwatch _settleWatch = Stopwatch();

  final RrTracker _tracker = RrTracker(hz: ecgHz);
  final EcgScaler _scaler = EcgScaler();
  final EcgDisplayFilter _filter = EcgDisplayFilter();
  final _StripBuffer _strip = _StripBuffer();

  bool _active = false; // subscribed, measuring or waiting
  bool _cmdAcked = false; // the card accepted start (or was already measuring)
  bool _sawActiveMode = false; // a non-idle Status mode after the ack
  bool _recoverable = false; // error from Status meas (e.g. USB): may recover
  Future<SessionRecord?>? _finishing;
  int _gen = 0; // session generation, to drop stale async results

  DateTime _startedAt = DateTime.now();
  double? _breathsTarget;
  int _measuredSamples = 0;
  int? _lastBlockEnd;
  int _badRun = 0; // strip samples still to mark bad after a discontinuity
  int _nextCohSample = 0;
  final List<double> _cohScores = [];
  int _lastEmitTick = -1;

  @override
  LiveSession get state => _state;

  @override
  Stream<LiveSession> get changes => _changes.stream;

  @override
  Stream<Float32List> get ecgUv => _ecgOut.stream;

  Duration get _elapsed => Duration(microseconds: _measuredSamples * (1000000 ~/ ecgHz));

  bool get _phaseLive => switch (_state.phase) {
    LivePhase.starting || LivePhase.waitContact || LivePhase.settling || LivePhase.running => true,
    _ => false,
  };

  void _emit(LiveSession s) {
    _state = s;
    if (!_changes.isClosed) _changes.add(s);
  }

  // ---------------------------------------------------------------- begin

  @override
  Future<void> begin(SessionKind kind, {Duration? target, double? breathsPerMin}) async {
    if (_finishing != null) await _finishing;
    if (_active) await cancel();
    final gen = ++_gen;
    _resetSession();
    _breathsTarget = breathsPerMin;
    _startedAt = DateTime.now();
    _emit(LiveSession(phase: LivePhase.starting, kind: kind, target: target));

    final card = _cardOf();
    if (card == null) return _fail(SessionMessages.noCard);
    if (card.currentLinkState != LinkState.connected) return _fail(SessionMessages.notConnected);
    _card = card;
    _active = true;
    _keepAwake(true);
    final st = card.lastStatus;
    if (st != null) {
      _scaler.onStatus(st);
      _setNotch(st.notchHz);
      _emit(_state.copyWith(status: st));
    }
    _subs
      ..add(card.status.listen(_onStatus))
      ..add(card.events.listen(_onEvent))
      ..add(card.ecg.listen(_onEcg))
      ..add(card.linkState.listen(_onLinkState));
    _ticker = Timer.periodic(const Duration(milliseconds: 100), (_) => _onTick());

    CmdResult res;
    try {
      res = await card.start(CardMode.ecg);
    } catch (_) {
      res = CmdResult.timeout;
    }
    if (gen != _gen || !_active) return;
    if (res == CmdResult.busy) {
      // Already measuring ECG (e.g. started from the card's button): adopt it.
      final m = card.lastStatus?.mode;
      if (m == CardMode.ecg || m == CardMode.both) res = CmdResult.ok;
    }
    if (res != CmdResult.ok) {
      _fail(SessionMessages.forResult(res));
      _unsubscribe();
      _active = false;
      _keepAwake(false);
      return;
    }
    _cmdAcked = true;
    if (_state.phase == LivePhase.starting) {
      final meas = card.lastStatus?.meas;
      if (meas != null && meas != MeasState.none && meas != MeasState.waitContact) {
        _onMeas(meas);
      } else {
        _enter(LivePhase.waitContact);
      }
    }
  }

  void _resetSession() {
    _tracker.clear();
    _scaler.gains = const EcgGains();
    _filter.reset();
    _strip.clear();
    _cmdAcked = false;
    _sawActiveMode = false;
    _recoverable = false;
    _measuredSamples = 0;
    _lastBlockEnd = null;
    _badRun = 0;
    _nextCohSample = coherenceEvery.inMilliseconds * ecgHz ~/ 1000;
    _cohScores.clear();
    _lastEmitTick = -1;
    _settleWatch
      ..stop()
      ..reset();
  }

  // ---------------------------------------------------------------- inputs

  void _onStatus(CardStatus s) {
    if (!_active) return;
    _scaler.onStatus(s);
    _setNotch(s.notchHz);
    _emit(_state.copyWith(status: s, hrBpm: s.hrBpm > 0 && _state.hrBpm == null ? s.hrBpm : null));
    if (!_cmdAcked) {
      if (s.meas == MeasState.usbBlocked) _onMeas(s.meas);
      return;
    }
    if (s.mode != CardMode.idle) {
      _sawActiveMode = true;
    } else if (_sawActiveMode) {
      // The session was ended on the card (button): finish and keep it.
      unawaited(finish(save: true));
      return;
    }
    _onMeas(s.meas);
  }

  void _onMeas(MeasState m) {
    switch (m) {
      case MeasState.waitContact:
        _enter(LivePhase.waitContact);
      case MeasState.settling:
        _enter(LivePhase.settling);
      case MeasState.running:
        _enter(LivePhase.running);
      case MeasState.usbBlocked:
        _fail(SessionMessages.usb, recoverable: true);
      case MeasState.fault:
        _fail(SessionMessages.fault, recoverable: true);
      case MeasState.none:
        break;
    }
  }

  void _onEvent(CardEvent e) {
    if (!_active) return;
    _scaler.onEvent(e);
    switch (e) {
      case StateEvent():
        _setNotch(e.notchHz);
      case LeadEvent(contact: false):
        if (_state.phase == LivePhase.running || _state.phase == LivePhase.settling) {
          _enter(LivePhase.waitContact);
        }
      case LeadEvent(contact: true):
        if (_state.phase == LivePhase.waitContact && _cmdAcked) _enter(LivePhase.settling);
      case RPeakEvent():
        _onBeat(e);
      default:
        break;
    }
  }

  void _onBeat(RPeakEvent e) {
    if (_state.phase != LivePhase.running) return;
    final r = _tracker.add(e.sampleIndex);
    final hr = e.hrBpm > 0 ? e.hrBpm : _tracker.medianBpm?.round();
    _emit(
      _state.copyWith(
        hrBpm: hr,
        fastBpm: _tracker.fastBpm,
        rr: r == null ? null : _tracker.intervals,
      ),
    );
  }

  void _onEcg(EcgBlock b) {
    if (!_active) return;
    final n = b.samples.length;
    _tracker.setOrigin(b.start);
    var missing = 0;
    final last = _lastBlockEnd;
    if (last != null && b.start != last) {
      _filter.reset();
      _strip.breakHere();
      _badRun = ecgHz ~/ 2;
      // Dropped notifications: count up to 1 s of missing samples as measured.
      if (b.start > last && b.start - last <= ecgHz) missing = b.start - last;
    }
    _lastBlockEnd = b.start + n;

    // Flags are faster than Status notifications: use them as hints.
    if (_cmdAcked) {
      final p = _state.phase;
      if (b.settling && p == LivePhase.waitContact) {
        _enter(LivePhase.settling);
      } else if (!b.settling && !b.leadOff && p == LivePhase.settling) {
        _enter(LivePhase.running);
      } else if (b.leadOff && p == LivePhase.running) {
        _enter(LivePhase.waitContact);
      }
    }

    final uv = _scaler.toUv(b);
    final shown = Float32List(n);
    for (var i = 0; i < n; i++) {
      shown[i] = _filter.step(uv[i]);
    }
    if (!_ecgOut.isClosed) _ecgOut.add(shown);

    if (_state.phase != LivePhase.running) return;
    final bad = b.settling || b.leadOff;
    if (_strip.length < maxStripBufferSeconds * ecgHz) {
      for (var i = 0; i < n; i++) {
        _strip.add(uv[i], bad || _badRun > 0);
        if (_badRun > 0) _badRun--;
      }
    }
    _measuredSamples += n + missing;
    _afterMeasured();
  }

  void _afterMeasured() {
    final elapsed = _elapsed;
    CoherenceResult? coh;
    if (_state.kind == SessionKind.breathing && _measuredSamples >= _nextCohSample) {
      _nextCohSample += coherenceEvery.inMilliseconds * ecgHz ~/ 1000;
      coh = coherence(_tracker.intervals);
      if (coh != null) _cohScores.add(coh.score);
    }
    final tick = elapsed.inMilliseconds ~/ 200;
    if (coh != null || tick != _lastEmitTick) {
      _lastEmitTick = tick;
      _emit(_state.copyWith(elapsed: elapsed, coherence: coh));
    }
    final target = _state.target;
    if (target != null && elapsed >= target) unawaited(finish(save: true));
  }

  void _onLinkState(LinkState s) {
    if (!_active) return;
    if (s == LinkState.connected) {
      if (!_state.linkLost) return;
      // Back: the card kept measuring; Status and ECG resume the phases.
      _linkTimer?.cancel();
      _linkTimer = null;
      if (_state.phase == LivePhase.settling) _settleWatch.start();
      _emit(_state.copyWith(linkLost: false));
      return;
    }
    if (s != LinkState.disconnected || _state.linkLost) return;
    if (!_cmdAcked) {
      // Not started yet: start() fails with a timeout; say why instead.
      _linkGone();
      return;
    }
    // Dropped mid-session: pause (nothing arrives) and wait for the link
    // (BleCard reconnects on its own); no RR interval spans the gap.
    _tracker.reset();
    _strip.breakHere();
    _settleWatch.stop();
    _emit(_state.copyWith(linkLost: true));
    _linkTimer?.cancel();
    _linkTimer = Timer(linkGrace, _linkGone);
  }

  /// The link did not come back: keep what was measured, or explain.
  void _linkGone() {
    _linkTimer = null;
    if (!_active) return;
    final beats = _tracker.intervals.where((x) => !x.artifact).length;
    if (_elapsed >= minSaveDuration && beats >= minSaveBeats) {
      unawaited(finish(save: true));
      return;
    }
    _state = _state.copyWith(linkLost: false); // emitted with the error below
    _fail(SessionMessages.disconnected);
    _active = false;
    _unsubscribe();
    _keepAwake(false);
  }

  void _onTick() {
    if (_state.phase != LivePhase.settling) return;
    final p = math.min(0.95, _settleWatch.elapsedMilliseconds / settleDuration.inMilliseconds);
    if ((p - _state.settleProgress).abs() >= 0.01) _emit(_state.copyWith(settleProgress: p));
  }

  void _setNotch(int hz) {
    if (hz == 50 || hz == 60) _filter.notchHz = hz;
  }

  // ---------------------------------------------------------------- phases

  void _enter(LivePhase p) {
    final cur = _state.phase;
    if (cur == LivePhase.error && _recoverable && _active) {
      _recoverable = false;
    } else if (!_phaseLive) {
      return;
    }
    if (cur == p && cur != LivePhase.error) return;
    if (cur == LivePhase.running) {
      // Contact lost: the next beat starts a new chain, no interval spans the gap.
      _tracker.reset();
      _strip.breakHere();
    }
    switch (p) {
      case LivePhase.settling:
        _settleWatch
          ..reset()
          ..start();
        _emit(_withPhase(p, settleProgress: 0));
      case LivePhase.running:
        _settleWatch.stop();
        _emit(_withPhase(p, settleProgress: 1));
      default:
        _settleWatch.stop();
        _emit(_withPhase(p, settleProgress: 0));
    }
  }

  /// A copy with a new phase and the error cleared.
  LiveSession _withPhase(LivePhase p, {double? settleProgress, String? error}) => LiveSession(
    phase: p,
    kind: _state.kind,
    settleProgress: settleProgress ?? _state.settleProgress,
    elapsed: _elapsed,
    target: _state.target,
    hrBpm: _state.hrBpm,
    fastBpm: _state.fastBpm,
    rr: _state.rr,
    coherence: _state.coherence,
    status: _state.status,
    error: error,
    record: _state.record,
    saved: _state.saved,
    linkLost: _state.linkLost,
  );

  void _fail(String message, {bool recoverable = false}) {
    if (_state.phase == LivePhase.finished) return;
    if (_state.phase == LivePhase.error && _state.error == message) return;
    if (_state.phase == LivePhase.running) _tracker.reset();
    _settleWatch.stop();
    _recoverable = recoverable;
    _emit(_withPhase(LivePhase.error, error: message));
  }

  void _keepAwake(bool on) {
    if (on == _awake) return;
    _awake = on;
    unawaited((on ? wakeLock.enable() : wakeLock.disable()).catchError((Object _) {}));
  }

  void _unsubscribe() {
    for (final s in _subs) {
      unawaited(s.cancel());
    }
    _subs.clear();
    _ticker?.cancel();
    _ticker = null;
    _linkTimer?.cancel();
    _linkTimer = null;
  }

  // ---------------------------------------------------------------- finish

  @override
  Future<SessionRecord?> finish({bool save = true}) {
    final f = _finishing;
    if (f != null) return f;
    if (_state.phase == LivePhase.finished) return Future.value(_state.record);
    if (_state.phase == LivePhase.idle) return Future.value(null);
    final fut = _finish(save);
    _finishing = fut;
    return fut.whenComplete(() => _finishing = null);
  }

  Future<SessionRecord?> _finish(bool save) async {
    final gen = _gen;
    final wasActive = _active;
    _active = false;
    _unsubscribe();
    _keepAwake(false);
    _settleWatch.stop();
    final stopping = wasActive && _cmdAcked ? _stopCard() : Future<void>.value();

    final record = _buildRecord();
    var saved = false;
    if (record != null && save && _enoughToSave(record)) {
      try {
        await store.save(record);
        saved = true;
      } catch (_) {
        saved = false;
      }
    }
    await stopping;
    if (gen == _gen) {
      _emit(
        LiveSession(
          phase: LivePhase.finished,
          kind: _state.kind,
          settleProgress: _state.settleProgress,
          elapsed: _elapsed,
          target: _state.target,
          hrBpm: _state.hrBpm,
          fastBpm: _state.fastBpm,
          rr: _tracker.intervals,
          coherence: _state.coherence,
          status: _state.status,
          record: record,
          saved: saved,
        ),
      );
    }
    return record;
  }

  bool _enoughToSave(SessionRecord r) =>
      r.duration >= minSaveDuration && r.rr.where((x) => !x.artifact).length >= minSaveBeats;

  Future<void> _stopCard() async {
    final c = _card;
    if (c == null || c.currentLinkState != LinkState.connected) return;
    try {
      await c.stop();
    } catch (_) {
      // Best effort: the card also stops on its own when contact is gone.
    }
  }

  SessionRecord? _buildRecord() {
    final rr = _tracker.intervals;
    if (_measuredSamples == 0 && rr.isEmpty) return null;
    final stats = hrvStats(rr);
    final kind = _state.kind;
    double? coh;
    if (kind == SessionKind.breathing) {
      if (_cohScores.isNotEmpty) {
        coh = _cohScores.reduce((a, b) => a + b) / _cohScores.length;
      } else {
        coh = coherence(rr)?.score;
      }
    }
    final strip = pickStrip(
      _strip.values,
      bad: _strip.badMask,
      hz: ecgHz,
      notchHz: _filter.notchHz,
    );
    return SessionRecord(
      id: newSessionId(_startedAt),
      start: _startedAt,
      kind: kind,
      duration: _elapsed,
      rr: rr,
      stats: stats,
      calmScore: stats == null || stats.beats < 3 ? null : calmScore(stats.rmssdMs),
      coherence: coh,
      breathsPerMinTarget: kind == SessionKind.breathing ? _breathsTarget : null,
      stripUv: strip,
      stripHz: 250,
      cardName: _card?.name,
    );
  }

  @override
  Future<void> cancel() async {
    _gen++;
    final wasActive = _active;
    _active = false;
    _unsubscribe();
    _keepAwake(false);
    _settleWatch.stop();
    if (wasActive && _cmdAcked) await _stopCard();
    _resetSession();
    _emit(const LiveSession());
  }

  /// Stops listening; stops the card if a session is still running.
  void dispose() {
    if (_active && _cmdAcked) unawaited(_stopCard());
    _active = false;
    _unsubscribe();
    _keepAwake(false);
    unawaited(_changes.close());
    unawaited(_ecgOut.close());
  }
}

final math.Random _rand = math.Random();

/// A unique-enough id: start time (base 36) plus 32 random bits.
String newSessionId(DateTime start) =>
    '${start.millisecondsSinceEpoch.toRadixString(36)}-${_rand.nextInt(1 << 32).toRadixString(36).padLeft(7, '0')}';

/// Growable raw-ECG buffer with a "bad sample" mask (Float32 to keep long
/// sessions small).
class _StripBuffer {
  Float32List _v = Float32List(1 << 14);
  Uint8List _bad = Uint8List(1 << 14);
  int length = 0;

  void clear() {
    length = 0;
    _v = Float32List(1 << 14);
    _bad = Uint8List(1 << 14);
  }

  void add(double v, bool bad) {
    if (length == _v.length) {
      final nv = Float32List(_v.length * 2)..setAll(0, _v);
      final nb = Uint8List(_bad.length * 2)..setAll(0, _bad);
      _v = nv;
      _bad = nb;
    }
    _v[length] = v;
    _bad[length] = bad ? 1 : 0;
    length++;
  }

  /// Marks the last 0.1 s before a discontinuity bad (the next samples are
  /// marked by the caller), so no strip is cut across a jump.
  void breakHere() {
    for (var i = math.max(0, length - 50); i < length; i++) {
      _bad[i] = 1;
    }
  }

  List<double> get values => Float32List.sublistView(_v, 0, length);
  List<bool> get badMask => [for (var i = 0; i < length; i++) _bad[i] != 0];
}
