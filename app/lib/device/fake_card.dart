// A synthetic ECG Card (and replay of a tools/ble_client.html "Save session"
// file) behind the CardLink interface, so every screen runs without hardware.
//
// The card is simulated on its own device clock in 20 ms ticks (one ECG block,
// one PPG block per tick). With autoTick (default) a periodic Timer drives the
// clock, so widget tests can drive it with tester.pump(); with autoTick: false
// tests call advance() and nothing runs on its own (deterministic and fast).
// Everything random comes from a seeded generator.
//
// Card behaviour modelled (fw/include/link_fmt.h, stream_fmt.h):
// - Control writes answered by a CMD_RESULT event after ~40 ms, with the
//   firmware's rules: START during a session -> busy, ECG with USB power ->
//   refusedUsb, gain / notch / RLD / LED limit / restore during a session -> busy,
//   bad values -> badArg, SET_TIME before 2024 or offset outside -720..840 -> badArg.
// - start(mode): STATE event, Status; meas waitContact ~1.5 s, settling ~2.5 s,
//   then running. ECG blocks every 20 ms (flags settling / lead_off as the card
//   sets them), R_PEAK events ~250 ms after each beat while running, with hr_bpm
//   the median of the last 5 RR; RR follows respiratory sinus arrhythmia.
//   PPG blocks + PULSE events in ppg / both modes.
// - Status every 5 s and on every change; LEAD events on contact changes.
// - Device log lines every 10 s.
// - The session keeps running on the card across a disconnect (as on the real
//   card); a START while one runs is answered busy, so stop first.
import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import '../protocol/commands.dart';
import '../protocol/link_status.dart';
import '../protocol/stream_fmt.dart';
import '../protocol/units.dart';
import 'card_link.dart';
import 'env_stub.dart' if (dart.library.io) 'env_io.dart';

class FakeCardConfig {
  const FakeCardConfig({
    this.hrBpm = 64,
    this.rsaBpm = 7,
    this.breathsPerMin = 6,
    this.rrJitterMs = 8,
    this.qrsUv = 1200,
    this.noiseUv = 8,
    this.wanderUv = 60,
    this.patMs = 205,
    this.waitContact = const Duration(milliseconds: 1500),
    this.settling = const Duration(milliseconds: 2500),
    this.resettle = const Duration(milliseconds: 1500),
    this.batteryPct = 82,
    this.usbPower = false,
    this.connectDelay = const Duration(milliseconds: 300),
    this.cmdLatency = const Duration(milliseconds: 40),
    this.seed = 12345,
  });

  /// Mean heart rate and the respiratory swing around it (± bpm).
  final double hrBpm;
  final double rsaBpm;
  final double breathsPerMin;

  /// Beat-to-beat jitter (standard deviation, ms) on top of the RSA.
  final double rrJitterMs;

  /// R-wave peak, noise (sd) and baseline wander (peak), microvolts.
  final double qrsUv;
  final double noiseUv;
  final double wanderUv;

  /// Pulse arrival time (R to PPG foot).
  final int patMs;

  /// Time to "place thumbs" after START, the first settling, and settling
  /// after contact is regained.
  final Duration waitContact;
  final Duration settling;
  final Duration resettle;

  final int batteryPct;
  final bool usbPower;
  final Duration connectDelay;
  final Duration cmdLatency;
  final int seed;

  FakeCardConfig copyWith({
    double? hrBpm,
    double? rsaBpm,
    double? breathsPerMin,
    Duration? waitContact,
    Duration? settling,
    Duration? resettle,
    int? batteryPct,
    bool? usbPower,
    Duration? connectDelay,
    int? seed,
  }) => FakeCardConfig(
    hrBpm: hrBpm ?? this.hrBpm,
    rsaBpm: rsaBpm ?? this.rsaBpm,
    breathsPerMin: breathsPerMin ?? this.breathsPerMin,
    rrJitterMs: rrJitterMs,
    qrsUv: qrsUv,
    noiseUv: noiseUv,
    wanderUv: wanderUv,
    patMs: patMs,
    waitContact: waitContact ?? this.waitContact,
    settling: settling ?? this.settling,
    resettle: resettle ?? this.resettle,
    batteryPct: batteryPct ?? this.batteryPct,
    usbPower: usbPower ?? this.usbPower,
    connectDelay: connectDelay ?? this.connectDelay,
    cmdLatency: cmdLatency,
    seed: seed ?? this.seed,
  );
}

/// Simulated device clock step: one ECG block (10 samples at 500 Hz).
const int fakeTickMs = 20;

const _cmdTimeoutMs = 3000;
const _statusPeriodMs = 5000;
const _logPeriodMs = 10000;

class FakeCard implements CardLink {
  FakeCard({
    this.id = 'fake-card-3f2a',
    this.name = 'ECG Card 3F2A',
    FakeCardConfig config = const FakeCardConfig(),
    this.autoTick = true,
  }) : _cfg = config,
       _rng = _Rng(config.seed),
       _battPct = config.batteryPct,
       _usb = config.usbPower,
       _hrBase = config.hrBpm,
       _rsa = config.rsaBpm,
       _breathHz = config.breathsPerMin / 60;

  /// Replays the ECG of a tools/ble_client.html "Save session" text file
  /// (section "## ECG index,uV,flags"; R-peaks from its timeline, else detected).
  /// Recordings are personal data: load them at run time, never commit them.
  factory FakeCard.replay(
    String sessionText, {
    String id = 'fake-replay',
    String name = 'ECG Card REPL',
    FakeCardConfig config = const FakeCardConfig(),
    bool autoTick = true,
  }) {
    final rec = Recording.parse(sessionText);
    return FakeCard(id: id, name: name, config: config, autoTick: autoTick).._rec = rec;
  }

  @override
  final String id;
  @override
  final String name;

  /// When false, the device clock only moves in [advance].
  final bool autoTick;

  final FakeCardConfig _cfg;
  final _Rng _rng;
  Recording? _rec;

  // ---- streams
  final _linkCtl = StreamController<LinkState>.broadcast();
  final _statusCtl = StreamController<CardStatus>.broadcast();
  final _ecgCtl = StreamController<EcgBlock>.broadcast();
  final _ppgCtl = StreamController<PpgBlock>.broadcast();
  final _eventCtl = StreamController<CardEvent>.broadcast();
  final _logCtl = StreamController<String>.broadcast();

  LinkState _link = LinkState.disconnected;
  CardStatus? _lastStatus;
  bool _wantConnected = false; // between connect() and disconnect()
  int _outageUntilMs = -1; // simulated link loss
  Timer? _timer;
  Stopwatch? _wall;
  int _ticksFired = 0;

  // ---- device state
  int _ms = 0; // device clock
  int _battPct;
  bool _usb;
  double _hrBase, _rsa, _breathHz;
  bool _timeValid = false;
  final _settings = [0, 0, 0, 2, 1, 1, 2, 0]; // settings.h defaults
  int _lastStatusMs = -_statusPeriodMs;
  int _lastLogMs = 0;
  int _dropCommands = 0;
  final _cmdQueue = <({int due, Uint8List bytes})>[];
  final _waiters = <({int cmd, int deadline, Completer<CmdResult> done})>[];

  // ---- session
  CardMode _mode = CardMode.idle;
  MeasState _meas = MeasState.none;
  int _runStartMs = 0;
  int _settleStartMs = 0;
  bool _settledOnce = false;
  double _runningSinceS = double.infinity;
  int _liftFromMs = -1, _liftUntilMs = -1;
  int _ecgNext = 0, _ppgNext = 0;
  final _beats = <_Beat>[]; // window for synthesis
  final _pendR = <_Beat>[];
  final _pendP = <_Beat>[];
  double _nextBeatS = 0.35;
  int _recBeatIdx = 0, _recLoop = 0;
  double? _lastEmittedR;
  final _rrHist = <double>[];
  int _hrBpm = 0;
  int _beatsSent = 0;

  // ---------------------------------------------------------------- CardLink

  @override
  Stream<LinkState> get linkState => _linkCtl.stream;
  @override
  LinkState get currentLinkState => _link;
  @override
  Stream<CardStatus> get status => _statusCtl.stream;
  @override
  CardStatus? get lastStatus => _lastStatus;
  @override
  Stream<EcgBlock> get ecg => _ecgCtl.stream;
  @override
  Stream<PpgBlock> get ppg => _ppgCtl.stream;
  @override
  Stream<CardEvent> get events => _eventCtl.stream;
  @override
  Stream<String> get log => _logCtl.stream;

  @override
  Future<void> connect() async {
    if (_link == LinkState.connected) return;
    _wantConnected = true;
    _setLink(LinkState.connecting);
    if (_cfg.connectDelay > Duration.zero) {
      await Future<void>.delayed(_cfg.connectDelay);
    }
    if (!_wantConnected) return; // disconnect() while connecting
    _outageUntilMs = -1;
    _setLink(LinkState.connected);
    if (autoTick) _startTimer();
    _sendStatus(); // the initial Status read
    _logLine('radio: ble: connected, int 20 ms, latency 0, mtu 247, phy 2M');
    unawaited(setTime(DateTime.now()));
  }

  @override
  Future<void> disconnect() async {
    _wantConnected = false;
    _stopTimer();
    _failWaiters();
    _setLink(LinkState.disconnected);
  }

  @override
  Future<CmdResult> start(CardMode mode) => _command(Commands.start(mode));
  @override
  Future<CmdResult> stop() => _command(Commands.stop());
  @override
  Future<CmdResult> setSetting(int item, int value) => _command(Commands.set(item, value));
  @override
  Future<CmdResult> setTime(DateTime now) => _command(Commands.setTime(now));

  /// Releases timers and closes the streams.
  Future<void> dispose() async {
    await disconnect();
    for (final c in [_linkCtl, _statusCtl, _ecgCtl, _ppgCtl, _eventCtl, _logCtl]) {
      await c.close();
    }
  }

  // ---------------------------------------------------------------- scripting

  /// Device clock (time since the fake was created, advanced while connected).
  Duration get deviceTime => Duration(milliseconds: _ms);

  /// Current measurement state on the card.
  MeasState get meas => _meas;
  CardMode get mode => _mode;

  /// Runs the device clock forward by [d] (in 20 ms ticks). For autoTick: false.
  void advance(Duration d) {
    final n = (d.inMicroseconds / (fakeTickMs * 1000)).round();
    for (var i = 0; i < n; i++) {
      _tick();
    }
  }

  /// Thumbs off the electrodes for [d] from now (contact loss, then settling).
  void liftThumbs(Duration d) {
    _liftFromMs = _ms;
    _liftUntilMs = _ms + d.inMilliseconds;
  }

  /// USB power present: ECG starts are refused (LINK_RES_REFUSED_USB).
  set usbPower(bool on) {
    _usb = on;
    _sendStatus();
  }

  set batteryPct(int pct) {
    _battPct = pct.clamp(0, 100);
    _sendStatus();
  }

  /// Heart rate and RSA of the synthetic signal from the next beat on.
  void setHeart({double? hrBpm, double? rsaBpm, double? breathsPerMin}) {
    _hrBase = hrBpm ?? _hrBase;
    _rsa = rsaBpm ?? _rsa;
    _breathHz = (breathsPerMin ?? _breathHz * 60) / 60;
  }

  /// The next [n] Control writes get no CMD_RESULT (they time out after 3 s).
  void dropCommands(int n) => _dropCommands = n;

  /// Link lost for [outage]: disconnected, then connecting, then connected again.
  /// The card keeps measuring meanwhile; data of the outage is lost.
  void simulateLinkLoss(Duration outage) {
    if (_link != LinkState.connected) return;
    _outageUntilMs = _ms + outage.inMilliseconds;
    _failWaiters();
    _setLink(LinkState.disconnected);
  }

  // ---------------------------------------------------------------- clock

  void _startTimer() {
    _timer?.cancel();
    // follow wall time when timers run late (real devices); under flutter test
    // only the (fake) timer count, so widget tests stay deterministic
    _wall = runningInFlutterTest ? null : (Stopwatch()..start());
    _ticksFired = 0;
    _timer = Timer.periodic(const Duration(milliseconds: fakeTickMs), (_) {
      _ticksFired++;
      final wall = _wall;
      final due = wall == null
          ? _ticksFired
          : math.max(_ticksFired, wall.elapsedMilliseconds ~/ fakeTickMs);
      var n = 0;
      while (_ticksDone < due && n++ < 50) {
        _tick();
      }
      if (_ticksDone < due) {
        _ticksDone = due; // far behind: skip rather than burst
      }
    });
    _ticksDone = 0;
  }

  int _ticksDone = 0;

  void _stopTimer() {
    _timer?.cancel();
    _timer = null;
    _wall = null;
  }

  bool get _linkUp => _link == LinkState.connected;

  void _setLink(LinkState s) {
    if (s == _link) return;
    _link = s;
    _linkCtl.add(s);
  }

  void _tick() {
    _ticksDone++;
    _ms += fakeTickMs;
    if (_outageUntilMs >= 0 && _wantConnected) {
      if (_ms >= _outageUntilMs) {
        _outageUntilMs = -1;
        _setLink(LinkState.connecting);
        _setLink(LinkState.connected);
        _sendStatus();
      } else if (_ms >= _outageUntilMs - 500 && _link == LinkState.disconnected) {
        _setLink(LinkState.connecting);
      }
    }
    while (_cmdQueue.isNotEmpty && _cmdQueue.first.due <= _ms) {
      _apply(_cmdQueue.removeAt(0).bytes);
    }
    for (final w in _waiters.where((w) => w.deadline <= _ms).toList()) {
      _waiters.remove(w);
      w.done.complete(CmdResult.timeout);
    }
    if (_mode != CardMode.idle) _session();
    if (_ms - _lastStatusMs >= _statusPeriodMs) _sendStatus();
    if (_ms - _lastLogMs >= _logPeriodMs) {
      _lastLogMs = _ms;
      _logLine(
        _mode == CardMode.idle
            ? 'batt: mv=${_battMv()} pct=$_battPct usb=${_usb ? 1 : 0}'
            : 'ecg: ${_meas.name} beats=$_beatsSent hr=$_hrBpm',
      );
    }
  }

  // ---------------------------------------------------------------- commands

  Future<CmdResult> _command(Uint8List bytes) {
    if (_link != LinkState.connected) return Future.value(CmdResult.timeout);
    final done = Completer<CmdResult>();
    _waiters.add((cmd: bytes[0], deadline: _ms + _cmdTimeoutMs, done: done));
    if (_dropCommands > 0) {
      _dropCommands--;
    } else {
      _cmdQueue.add((due: _ms + _cfg.cmdLatency.inMilliseconds, bytes: bytes));
    }
    return done.future;
  }

  void _failWaiters() {
    for (final w in _waiters) {
      w.done.complete(CmdResult.timeout);
    }
    _waiters.clear();
  }

  static const _optionCounts = [3, 3, 4, 3, 3, 2, 3, 2];
  static const _lockedInSession = {0, 1, 2, 3, 6};

  int _result(Uint8List b) {
    final n = b.length, cmd = b[0], ses = _mode != CardMode.idle;
    switch (cmd) {
      case CmdId.start:
        if (n != 2 || b[1] < 1 || b[1] > 4) return 2;
        if (ses) return 5;
        if (_usb && b[1] != CardMode.ppg.index) return 3;
        if (_battPct < 3) return 4;
        return 0;
      case CmdId.stop || CmdId.getStatus:
        return n == 1 ? 0 : 2;
      case CmdId.set:
        if (n != 3 || b[1] >= _optionCounts.length || b[2] >= _optionCounts[b[1]]) {
          return 2;
        }
        return ses && _lockedInSession.contains(b[1]) ? 5 : 0;
      case CmdId.setTime:
        if (n != 7) return 2;
        final bd = ByteData.sublistView(b);
        final secs = bd.getUint32(1, Endian.little), off = bd.getInt16(5, Endian.little);
        return secs < 1704067200 || off < -720 || off > 840 ? 2 : 0;
      default:
        return 1;
    }
  }

  void _apply(Uint8List b) {
    final res = _result(b), cmd = b[0];
    if (res == 0) {
      switch (cmd) {
        case CmdId.start:
          _startSession(CardMode.values[b[1]]);
        case CmdId.stop:
          if (_mode != CardMode.idle) _stopSession();
        case CmdId.set:
          _settings[b[1]] = b[2];
        case CmdId.setTime:
          _timeValid = true;
      }
    }
    _emitEvent(CmdResultEvent(cmd: cmd, result: res));
    final i = _waiters.indexWhere((w) => w.cmd == cmd);
    if (i >= 0) _waiters.removeAt(i).done.complete(cmdResultOf(res));
    if (res == 0 && cmd != CmdId.start && cmd != CmdId.stop) _sendStatus();
    if (res == 0 && cmd == CmdId.set && b[1] == SettingItem.ble && b[2] == 0) {
      // Bluetooth off: the radio goes away
      _wantConnected = false;
      _stopTimer();
      _failWaiters();
      _setLink(LinkState.disconnected);
    }
  }

  // ---------------------------------------------------------------- session

  bool get _ecgOn => _mode == CardMode.ecg || _mode == CardMode.both || _mode == CardMode.hrMonitor;
  bool get _ppgOn => _mode == CardMode.ppg || _mode == CardMode.both;
  bool get _gainHi => _settings[SettingItem.gain] != 1; // auto -> hi for this signal
  int get _notchHz => _settings[SettingItem.notch] == 2 ? 60 : 50;

  /// Seconds on the measurement timeline (since START).
  double get _mt => (_ms - _runStartMs) / 1000;

  void _startSession(CardMode mode) {
    _mode = mode;
    _runStartMs = _ms;
    _meas = MeasState.waitContact;
    _settledOnce = false;
    _runningSinceS = double.infinity;
    _liftFromMs = _liftUntilMs = -1;
    _ecgNext = 0;
    _ppgNext = 0;
    _beats.clear();
    _pendR.clear();
    _pendP.clear();
    _nextBeatS = 0.35;
    _recBeatIdx = 0;
    _recLoop = 0;
    _lastEmittedR = null;
    _rrHist.clear();
    _hrBpm = 0;
    _beatsSent = 0;
    _emitState();
    _sendStatus();
    _logLine('session: start ${mode.name}');
  }

  void _stopSession() {
    _logLine('session: end user run_ms=${_ms - _runStartMs} beats=$_beatsSent hr=$_hrBpm');
    _mode = CardMode.idle;
    _meas = MeasState.none;
    _hrBpm = 0;
    _emitState();
    _sendStatus();
  }

  bool _contactAt(int ms) {
    if (ms < _runStartMs + _cfg.waitContact.inMilliseconds) return false;
    return !(ms >= _liftFromMs && ms < _liftUntilMs);
  }

  void _session() {
    // contact / measurement state
    final contact = _contactAt(_ms);
    final was = _meas;
    if (!contact) {
      if (_meas != MeasState.waitContact) {
        _meas = MeasState.waitContact;
        _runningSinceS = double.infinity;
        _lastEmittedR = null;
        _emitEvent(const LeadEvent(contact: false));
      }
    } else if (_meas == MeasState.waitContact) {
      _meas = MeasState.settling;
      _settleStartMs = _ms;
      _emitEvent(const LeadEvent(contact: true));
    } else if (_meas == MeasState.settling) {
      final need = (_settledOnce ? _cfg.resettle : _cfg.settling).inMilliseconds;
      if (_ms - _settleStartMs >= need) {
        _meas = MeasState.running;
        _settledOnce = true;
        _runningSinceS = _mt;
      }
    }
    if (_meas != was) _sendStatus();

    final mt = _mt;
    _ensureBeats(mt + 1.0);
    _beats.removeWhere((b) => b.t < mt - 1.5);

    if (_ecgOn) {
      while ((_ecgNext + 10) / ecgHz <= mt + 1e-9) {
        _ecgBlock();
      }
    }
    if (_ppgOn) {
      while ((_ppgNext + 2) / ppgHz <= mt + 1e-9) {
        _ppgBlock();
      }
    }
    // R_PEAK about 250 ms after the beat (detector latency)
    while (_pendR.isNotEmpty && _pendR.first.t + 0.25 <= mt) {
      final b = _pendR.removeAt(0);
      if (!_ecgOn || _meas != MeasState.running || b.t < _runningSinceS) {
        continue;
      }
      final prev = _lastEmittedR;
      _lastEmittedR = b.t;
      if (prev != null) {
        _rrHist.add(b.t - prev);
        if (_rrHist.length > 5) _rrHist.removeAt(0);
      }
      final rr = _rrHist.isEmpty ? b.rr : _median(_rrHist);
      _hrBpm = (60 / rr).round();
      _beatsSent++;
      _emitEvent(RPeakEvent(sampleIndex: (b.t * ecgHz).round(), hrBpm: _hrBpm));
    }
    // PULSE about 300 ms after the foot
    while (_pendP.isNotEmpty && _pendP.first.t + _cfg.patMs / 1000 + 0.3 <= mt) {
      final b = _pendP.removeAt(0);
      if (!_ppgOn || _meas != MeasState.running || b.t < _runningSinceS) {
        continue;
      }
      final pr = (60 / b.rr).round();
      if (!_ecgOn) _hrBpm = pr;
      final foot = b.t + _cfg.patMs / 1000;
      _emitEvent(
        PulseEvent(
          footIndex: (foot * ecgHz).round(),
          pulseBpm: pr,
          patMs: _ecgOn ? _cfg.patMs + 1 : 0,
          piPermille: 11,
        ),
      );
    }
  }

  void _ensureBeats(double untilS) {
    final rec = _rec;
    if (rec != null) {
      if (rec.beats.isEmpty) return;
      while (true) {
        final t = (rec.beats[_recBeatIdx] + _recLoop * rec.length) / ecgHz;
        if (t >= untilS) break;
        final nextIdx = _recBeatIdx + 1;
        final tn = nextIdx < rec.beats.length
            ? (rec.beats[nextIdx] + _recLoop * rec.length) / ecgHz
            : (rec.beats[0] + (_recLoop + 1) * rec.length) / ecgHz;
        _addBeat(_Beat(t, tn - t));
        _recBeatIdx = nextIdx;
        if (_recBeatIdx >= rec.beats.length) {
          _recBeatIdx = 0;
          _recLoop++;
        }
      }
      return;
    }
    while (_nextBeatS < untilS) {
      final t = _nextBeatS;
      final hr = _hrBase + _rsa * math.sin(2 * math.pi * _breathHz * t);
      final rr = 60 / hr + _cfg.rrJitterMs / 1000 * _rng.gauss();
      _addBeat(_Beat(t, rr));
      _nextBeatS += rr;
    }
  }

  void _addBeat(_Beat b) {
    _beats.add(b);
    _pendR.add(b);
    _pendP.add(b);
  }

  /// Clean ECG (microvolts) at measurement time t.
  double _ecgUv(double t) {
    var v = 0.0;
    final a = _cfg.qrsUv;
    final resp = 1 + 0.05 * math.sin(2 * math.pi * _breathHz * t);
    for (final b in _beats) {
      final d = t - b.t;
      if (d < -0.4 || d > 0.6) continue;
      v +=
          resp *
          (110 * _g(d + 0.19, 0.022) -
              90 * _g(d + 0.028, 0.009) +
              a * _g(d, 0.0105) -
              260 * _g(d - 0.03, 0.011) +
              290 * _g(d - 0.27, 0.048));
    }
    final w = _cfg.wanderUv;
    v +=
        0.6 * w * math.sin(2 * math.pi * 0.21 * t) +
        0.4 * w * math.sin(2 * math.pi * _breathHz * t + 1);
    return v;
  }

  void _ecgBlock() {
    final s0 = _ecgNext;
    _ecgNext += 10;
    final gain = _gainHi ? defaultGainHi : defaultGainLo;
    final k = 1 / ecgUvPerUnit(gain);
    final settling = _meas == MeasState.settling;
    final off = _meas == MeasState.waitContact;
    final flags = (_gainHi ? 0x01 : 0) | (settling ? 0x02 : 0) | (off ? 0x04 : 0);
    final settleT = (_settleStartMs - _runStartMs) / 1000;
    final s = Int16List(10);
    final rec = _rec;
    for (var i = 0; i < 10; i++) {
      final m = s0 + i;
      final t = m / ecgHz;
      double uv;
      if (off) {
        // open input: mains pickup and drift
        uv =
            350 * math.sin(2 * math.pi * _notchHz * t) +
            400 * math.sin(2 * math.pi * 0.3 * t) +
            30 * _rng.gauss();
      } else {
        uv = rec != null ? rec.uvAt(m) : _ecgUv(t) + _cfg.noiseUv * _rng.gauss();
        if (settling) uv += 2500 * math.exp(-(t - settleT) / 0.6);
      }
      s[i] = (uv * k).round().clamp(-32768, 32767);
    }
    _emit(_ecgCtl, EcgBlock(start: s0, flags: flags, samples: s));
  }

  /// PPG pulse shape (0..1) at time d since the foot.
  static double _pulse(double d, double rr) {
    if (d < 0 || d >= rr) return 0;
    const up = 0.12;
    final v = d < up
        ? 0.5 - 0.5 * math.cos(math.pi * d / up)
        : math.exp(-(d - up) / 0.32) + 0.13 * _g(d - up - 0.21, 0.035);
    return v * math.min(1, (rr - d) / 0.12);
  }

  void _ppgBlock() {
    final k0 = _ppgNext;
    _ppgNext += 2;
    final noFinger = _meas == MeasState.waitContact;
    final coarse = [0, 0], fine = [0, 0];
    for (var j = 0; j < 2; j++) {
      final t = (k0 + j) / ppgHz;
      var p = 0.0;
      for (final b in _beats) {
        p += _pulse(t - (b.t + _cfg.patMs / 1000), b.rr);
      }
      // photocurrent in 80.6 pA units: DC ~2.5 uA drifting; a pulse lowers the light ~1 %
      final dc =
          30600 +
          3000 * math.sin(2 * math.pi * t / 120) +
          80 * math.sin(2 * math.pi * _breathHz * t);
      final light = noFinger ? 900 + 40 * _rng.gauss() : dc * (1 - 0.011 * p) + 6 * _rng.gauss();
      fine[j] = _wrap16(-light.round());
      coarse[j] = (light * naPerPpgFine / naPerPpgCoarse + 1.5 * _rng.gauss()).round().clamp(
        -32768,
        32767,
      );
    }
    _emit(
      _ppgCtl,
      PpgBlock(start: k0, ledCode: 600, flags: noFinger ? 0x04 : 0, coarse: coarse, fine: fine),
    );
  }

  // ---------------------------------------------------------------- outputs

  void _emit<T>(StreamController<T> c, T v) {
    if (_linkUp && !c.isClosed) c.add(v);
  }

  void _emitEvent(CardEvent e) => _emit(_eventCtl, e);

  void _emitState() => _emitEvent(
    StateEvent(
      mode: _mode.index,
      notchHz: _notchHz,
      midCode: 2048,
      battPct: _battPct,
      flags: (_usb ? 1 : 0) | (_charging ? 2 : 0),
    ),
  );

  bool get _charging => _usb && _battPct < 100;

  int _battMv() => 3300 + (_battPct * 9.2).round();

  CardStatus _buildStatus() {
    final active = _mode != CardMode.idle;
    final flags =
        (_usb ? StatusFlag.usb : 0) |
        (_charging ? StatusFlag.charging : 0) |
        (active && _meas == MeasState.waitContact ? StatusFlag.noContact : 0) |
        (_battPct < 15 ? StatusFlag.battLow : 0) |
        (active && _ecgOn && _gainHi ? StatusFlag.gainHi : 0) |
        (_timeValid ? StatusFlag.timeValid : 0);
    return CardStatus(
      proto: 1,
      mode: _mode,
      meas: active ? _meas : MeasState.none,
      battPct: _battPct,
      flags: flags,
      gain: _settings[0],
      notch: _settings[1],
      rld: _settings[2],
      ledLimit: _settings[3],
      sweep: _settings[4],
      notchHz: _notchHz,
      hrBpm: active ? _hrBpm.clamp(0, 255) : 0,
      battMv: _battMv(),
      fwMajor: 0,
      fwMinor: 4,
      fwPatch: 0,
      uptimeMin: 17 + _ms ~/ 60000,
    );
  }

  void _sendStatus() {
    _lastStatusMs = _ms;
    final s = _buildStatus();
    if (_linkUp) {
      _lastStatus = s;
      _statusCtl.add(s);
    }
  }

  void _logLine(String text) {
    final line = text.startsWith('radio: ') ? text : '[${17 * 60000 + _ms}] $text';
    _emit(_logCtl, line);
  }
}

// ---------------------------------------------------------------- scanner

class FakeScanner implements CardScanner {
  FakeScanner({
    this.config = const FakeCardConfig(),
    this.cards = const [FoundCard(id: 'fake-card-3f2a', name: 'ECG Card 3F2A', rssi: -58)],
    this.scanDelay = const Duration(milliseconds: 400),
    this.autoTick = true,
    this.replayText,
  });

  final FakeCardConfig config;
  final List<FoundCard> cards;
  final Duration scanDelay;
  final bool autoTick;

  /// When set, linked cards replay this "Save session" text instead of the
  /// synthetic signal.
  final String? replayText;

  final _links = <String, FakeCard>{};
  final _scans = <StreamController<List<FoundCard>>>[];
  final _timers = <Timer>[];

  @override
  Stream<List<FoundCard>> scan() {
    late final StreamController<List<FoundCard>> c;
    c = StreamController<List<FoundCard>>(
      onListen: () {
        c.add(const []);
        final found = <FoundCard>[];
        for (final (i, card) in cards.indexed) {
          _timers.add(
            Timer(scanDelay * (i + 1), () {
              if (c.isClosed) return;
              found.add(card);
              c.add(List.unmodifiable(found));
            }),
          );
        }
      },
      onCancel: () => _scans.remove(c),
    );
    _scans.add(c);
    return c.stream;
  }

  @override
  Future<void> stop() async {
    for (final t in _timers) {
      t.cancel();
    }
    _timers.clear();
    for (final c in List.of(_scans)) {
      await c.close();
    }
    _scans.clear();
  }

  @override
  CardLink linkTo(FoundCard card) => _links.putIfAbsent(card.id, () {
    final text = replayText;
    return text != null
        ? FakeCard.replay(text, id: card.id, name: card.name, config: config, autoTick: autoTick)
        : FakeCard(id: card.id, name: card.name, config: config, autoTick: autoTick);
  });
}

// ---------------------------------------------------------------- replay

/// ECG of a tools/ble_client.html "Save session" file, in microvolts at 500 Hz.
class Recording {
  Recording(this.uv, this.beats);

  /// Contiguous samples (gaps filled by holding the last value), from the
  /// first recorded index.
  final Float32List uv;

  /// R-peak sample indices relative to the first sample, ascending.
  final List<int> beats;

  int get length => uv.length;
  double uvAt(int m) => uv.isEmpty ? 0 : uv[m % uv.length];

  /// Parses the "## ECG index,uV,flags" section and R-peaks ("R-peak @N" in
  /// the timeline, or "r_peak,N" rows of an "## Events" section). Without any,
  /// R-peaks are detected from the signal.
  static Recording parse(String text) {
    final idx = <int>[], val = <double>[];
    var inEcg = false;
    final peaks = <int>{};
    final rPeakText = RegExp(r'R-peak @(\d+)');
    final rPeakCsv = RegExp(r'^\s*r_peak\s*,\s*(\d+)');
    for (final raw in text.split('\n')) {
      final line = raw.trimRight();
      if (line.startsWith('## ')) {
        inEcg = line.startsWith('## ECG index,uV,flags');
        continue;
      }
      if (inEcg) {
        final p = line.split(',');
        if (p.length >= 2) {
          final i = int.tryParse(p[0].trim()), v = double.tryParse(p[1].trim());
          if (i != null && v != null) {
            idx.add(i);
            val.add(v);
          }
        }
        continue;
      }
      final m = rPeakText.firstMatch(line) ?? rPeakCsv.firstMatch(line);
      if (m != null) peaks.add(int.parse(m.group(1)!));
    }
    if (idx.isEmpty) {
      throw const FormatException('no "## ECG index,uV,flags" section');
    }
    final i0 = idx.first, n = idx.last - i0 + 1;
    final uv = Float32List(n);
    var last = 0.0, k = 0;
    for (var m = 0; m < n; m++) {
      if (k < idx.length && idx[k] - i0 == m) {
        last = val[k];
        k++;
      }
      uv[m] = last;
    }
    var beats = (peaks.map((p) => p - i0).where((p) => p >= 0 && p < n).toList()..sort());
    if (beats.length < 2) beats = detectRPeaks(uv);
    return Recording(uv, beats);
  }
}

/// A simple R-peak finder for replay: high-passed signal, maxima above half
/// the 98th percentile, at least 300 ms apart.
List<int> detectRPeaks(Float32List uv, {int hz = ecgHz}) {
  final n = uv.length;
  if (n < hz) return const [];
  final w = hz ~/ 10; // 100 ms moving mean
  final hp = Float32List(n);
  var acc = 0.0;
  for (var i = 0; i < n; i++) {
    acc += uv[i];
    if (i >= w) acc -= uv[i - w];
    hp[i] = uv[i] - acc / math.min(i + 1, w);
  }
  final sorted = Float32List.fromList(hp)..sort();
  final thr = 0.5 * sorted[(0.98 * (n - 1)).round()];
  final out = <int>[];
  final gap = (0.3 * hz).round();
  for (var i = 1; i < n - 1; i++) {
    if (hp[i] < thr || hp[i] < hp[i - 1] || hp[i] < hp[i + 1]) continue;
    if (out.isNotEmpty && i - out.last < gap) {
      if (hp[i] > hp[out.last]) out[out.length - 1] = i;
      continue;
    }
    out.add(i);
  }
  return out;
}

// ---------------------------------------------------------------- helpers

class _Beat {
  _Beat(this.t, this.rr);
  final double t; // R time, s on the measurement timeline
  final double rr; // to the next beat, s
}

double _g(double x, double s) => math.exp(-0.5 * (x / s) * (x / s));

int _wrap16(int v) => ((v + 32768) & 0xFFFF) - 32768;

double _median(List<double> v) {
  final s = List.of(v)..sort();
  final n = s.length;
  return n.isOdd ? s[n ~/ 2] : (s[n ~/ 2 - 1] + s[n ~/ 2]) / 2;
}

/// Seeded generator (dart:math Random: deterministic for a seed on a platform).
class _Rng {
  _Rng(int seed) : _r = math.Random(seed);
  final math.Random _r;

  double next() => _r.nextDouble();

  /// Approximately standard normal (sum of four uniforms).
  double gauss() => (next() + next() + next() + next() - 2) * 1.732;
}
