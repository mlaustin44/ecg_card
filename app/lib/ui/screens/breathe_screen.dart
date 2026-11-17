// Breathe: paced, slow breathing with live heart-rate biofeedback.
//
// Setup (pace, rhythm, length) -> session (orb + live heart wave + sync
// meter) -> summary (time in sync, average sync, heart-rate swing), saved
// through LiveSessionApi.finish() and opened as Routes.session(id).
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

import '../../app.dart';
import '../../data/session_record.dart';
import '../../state/data_providers.dart';
import '../../state/live_session.dart';
import '../theme.dart';
import '../widgets/breath_coherence.dart';
import '../widgets/breath_kit.dart';
import '../widgets/breath_orb.dart';
import '../widgets/breath_wave.dart';
import '../widgets/format.dart';
import '../widgets/primary_button.dart';

enum _Stage { setup, session, summary }

/// What the summary shows (pure; see [BreathSummary.from]).
class BreathSummary {
  const BreathSummary({
    required this.length,
    required this.inSync,
    required this.avgCoherence,
    required this.hrSwingBpm,
    required this.breaths,
  });

  final Duration length;
  final Duration inSync;
  final double? avgCoherence; // 0..1
  final double? hrSwingBpm; // mean peak-to-trough per breath
  final int breaths;

  /// Mean peak-to-trough of the heart rate within each complete breath that
  /// has at least three readings.
  static double? hrSwing(List<BreathSample> samples, BreathPacer pacer, double endSec) {
    final byBreath = <int, List<double>>{};
    final lastFull = pacer.breathIndex(endSec) - 1;
    for (final s in samples) {
      final i = pacer.breathIndex(s.tSec);
      if (i > lastFull) continue;
      (byBreath[i] ??= []).add(s.bpm);
    }
    final swings = [
      for (final v in byBreath.values)
        if (v.length >= 3) v.reduce(math.max) - v.reduce(math.min),
    ];
    if (swings.isEmpty) return null;
    return swings.reduce((a, b) => a + b) / swings.length;
  }
}

class BreatheScreen extends ConsumerStatefulWidget {
  const BreatheScreen({super.key});

  @override
  ConsumerState<BreatheScreen> createState() => _BreatheScreenState();
}

class _BreatheScreenState extends ConsumerState<BreatheScreen> with SingleTickerProviderStateMixin {
  static const paces = [5.0, 5.5, 6.0, 6.5, 7.0];
  static const lengths = [2, 5, 10];

  // Choices.
  double _bpm = 6;
  double _inhaleShare = 0.5;
  int _minutes = 5;
  bool _haptics = true;

  // Clock: the ticker drives the orb; [_clock] is seconds since the session began.
  late final Ticker _ticker;
  final _clock = ValueNotifier<double>(0);
  Duration _now = Duration.zero;
  Duration _startedAt = Duration.zero;
  double _lastTickSec = 0;

  _Stage _stage = _Stage.setup;
  LiveSessionApi? _api;
  StreamSubscription<LiveSession>? _sub;
  LiveSession _live = const LiveSession();
  String? _startError;

  // Measurements gathered on the pacer clock.
  final List<BreathSample> _samples = [];
  int _lastRrCount = -1;
  double? _lastBpm;
  double _syncSec = 0, _cohSum = 0, _cohSec = 0;
  bool? _wasInhaling;

  BreathSummary? _summary;
  Future<SessionRecord?>? _saving;
  bool _leaving = false;

  BreathPacer get _pacer => BreathPacer(breathsPerMin: _bpm, inhaleShare: _inhaleShare);
  double get _sessionSec => (_now - _startedAt).inMicroseconds / 1e6;

  @override
  void initState() {
    super.initState();
    _ticker = createTicker(_onTick)..start();
  }

  @override
  void dispose() {
    _ticker.dispose();
    _sub?.cancel();
    if (_stage == _Stage.session) _api?.cancel();
    _clock.dispose();
    super.dispose();
  }

  void _onTick(Duration now) {
    _now = now;
    if (_stage == _Stage.setup) {
      _clock.value = now.inMicroseconds / 1e6;
      return;
    }
    if (_stage != _Stage.session) return;
    final t = _sessionSec;
    final dt = (t - _lastTickSec).clamp(0.0, 0.5);
    _lastTickSec = t;
    _clock.value = t;

    if (_live.phase == LivePhase.running && !_thumbsOff && !_live.linkLost) {
      final c = _live.coherence?.score;
      if (c != null) {
        _cohSum += c * dt;
        _cohSec += dt;
        if (c >= breathInSyncAt) _syncSec += dt;
      }
    }

    final inhaling = _pacer.inhaling(t);
    if (_wasInhaling != null && inhaling != _wasInhaling && _haptics && !kIsWeb) {
      inhaling ? HapticFeedback.lightImpact() : HapticFeedback.selectionClick();
    }
    _wasInhaling = inhaling;
  }

  bool get _hadBeats => _samples.isNotEmpty;

  bool get _thumbsOff =>
      _hadBeats && (_live.phase == LivePhase.waitContact || (_live.status?.noContact ?? false));

  // ---- session flow ----

  Future<void> _begin() async {
    final api = ref.read(liveSessionProvider);
    _api = api;
    _samples.clear();
    _lastRrCount = -1;
    _lastBpm = null;
    _syncSec = _cohSum = _cohSec = 0;
    _wasInhaling = null;
    _startedAt = _now;
    _lastTickSec = 0;
    _clock.value = 0;
    setState(() {
      _stage = _Stage.session;
      _startError = null;
      _live = const LiveSession(phase: LivePhase.starting, kind: SessionKind.breathing);
    });
    await _sub?.cancel();
    _sub = api.changes.listen(_onLive);
    try {
      await api.begin(
        SessionKind.breathing,
        target: Duration(minutes: _minutes),
        breathsPerMin: _bpm,
      );
    } catch (_) {
      if (!mounted) return;
      setState(() => _startError = "We couldn't wake your card. Is it on and nearby?");
    }
  }

  void _onLive(LiveSession s) {
    if (!mounted || _stage != _Stage.session) return;
    final bpm = s.fastBpm;
    if (s.phase == LivePhase.running && bpm != null && bpm > 25 && bpm < 220) {
      if (s.rr.length != _lastRrCount || bpm != _lastBpm) {
        _samples.add(BreathSample(_sessionSec, bpm));
      }
    }
    _lastRrCount = s.rr.length;
    _lastBpm = bpm;
    setState(() => _live = s);

    final target = s.target;
    final reached = target != null && s.phase == LivePhase.running && s.elapsed >= target;
    if (reached || s.phase == LivePhase.finished) _end();
  }

  void _end() {
    if (_stage != _Stage.session) return;
    final end = _sessionSec;
    final summary = BreathSummary(
      length: _live.elapsed > Duration.zero ? _live.elapsed : Duration(seconds: end.round()),
      inSync: Duration(milliseconds: (_syncSec * 1000).round()),
      avgCoherence: _cohSec > 1 ? _cohSum / _cohSec : _live.coherence?.score,
      hrSwingBpm: BreathSummary.hrSwing(_samples, _pacer, end),
      breaths: _pacer.breathIndex(end),
    );
    _sub?.cancel();
    _sub = null;
    setState(() {
      _summary = summary;
      _stage = _Stage.summary;
      _saving = _api?.finish(save: true).catchError((_) => null);
    });
  }

  Future<void> _leaveSession() async {
    if (_hadBeats && _samples.length > 10) {
      _end();
      return;
    }
    await _sub?.cancel();
    _sub = null;
    await _api?.cancel();
    if (!mounted) return;
    setState(() => _stage = _Stage.setup);
  }

  void _exitScreen() {
    if (context.canPop()) {
      context.pop();
    } else {
      context.go(Routes.home);
    }
  }

  Future<void> _openSession() async {
    if (_leaving) return;
    setState(() => _leaving = true);
    final rec = await _saving;
    if (!mounted) return;
    if (rec != null) {
      context.go(Routes.session(rec.id));
    } else {
      context.go(Routes.home);
    }
  }

  // ---- build ----

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    final top = t.dark ? const Color(0xFF171A2B) : const Color(0xFFEAF4F1);
    return Scaffold(
      backgroundColor: t.bg,
      body: DecoratedBox(
        decoration: BoxDecoration(
          gradient: LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [top, t.bg],
            stops: const [0, 0.7],
          ),
        ),
        child: SafeArea(
          child: AnimatedSwitcher(
            duration: const Duration(milliseconds: 450),
            child: switch (_stage) {
              _Stage.setup => KeyedSubtree(key: const ValueKey('setup'), child: _setup(t)),
              _Stage.session => KeyedSubtree(key: const ValueKey('session'), child: _session(t)),
              _Stage.summary => KeyedSubtree(
                key: const ValueKey('summary'),
                child: _summaryView(t),
              ),
            },
          ),
        ),
      ),
    );
  }

  // Setup -----------------------------------------------------------------

  Widget _setup(BreathTones t) {
    final pacer = _pacer;
    return Column(
      children: [
        BreathHeader(title: 'Breathe', onBack: _exitScreen),
        Expanded(
          child: LayoutBuilder(
            builder: (context, box) => SingleChildScrollView(
              padding: const EdgeInsets.symmetric(horizontal: AppSpace.m),
              child: ConstrainedBox(
                constraints: BoxConstraints(minHeight: box.maxHeight),
                child: Column(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    Column(
                      children: [
                        const SizedBox(height: AppSpace.s),
                        ValueListenableBuilder<double>(
                          valueListenable: _clock,
                          builder: (_, now, _) => BreathOrb(
                            level: pacer.level(now),
                            spin: now * 0.05,
                            size: (box.maxHeight - 520).clamp(120.0, 230.0),
                          ),
                        ),
                        const SizedBox(height: AppSpace.s),
                        Text(
                          'Breathe with your heart',
                          textAlign: TextAlign.center,
                          style: context.text.headlineSmall,
                        ),
                        const SizedBox(height: AppSpace.s),
                        Text(
                          'Follow the orb, and watch your heart rate rise as you breathe in '
                          'and ease off as you breathe out.',
                          textAlign: TextAlign.center,
                          style: TextStyle(fontSize: 15, color: t.inkSoft, height: 1.4),
                        ),
                      ],
                    ),
                    Padding(
                      padding: const EdgeInsets.only(top: AppSpace.m, bottom: AppSpace.s),
                      child: BreathCard(
                        padding: const EdgeInsets.fromLTRB(
                          AppSpace.m,
                          AppSpace.m,
                          AppSpace.m,
                          AppSpace.l - 4,
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            _optionTitle(t, 'Pace', '${_fmt(_bpm)} breaths a minute'),
                            _BreathSegments<double>(
                              values: paces,
                              selected: _bpm,
                              label: _fmt,
                              onChanged: (v) => setState(() => _bpm = v),
                            ),
                            const SizedBox(height: AppSpace.m),
                            _optionTitle(t, 'Rhythm', pacer.describe()),
                            _BreathSegments<double>(
                              values: const [0.5, 0.4],
                              selected: _inhaleShare,
                              label: (v) => v == 0.5 ? 'Even' : 'Longer out',
                              onChanged: (v) => setState(() => _inhaleShare = v),
                            ),
                            const SizedBox(height: AppSpace.m),
                            _optionTitle(t, 'Length', null),
                            _BreathSegments<int>(
                              values: lengths,
                              selected: _minutes,
                              label: (v) => '$v min',
                              onChanged: (v) => setState(() => _minutes = v),
                            ),
                            if (!kIsWeb) ...[
                              const SizedBox(height: AppSpace.xs),
                              Row(
                                children: [
                                  Expanded(
                                    child: Text(
                                      'Gentle taps at each turn',
                                      style: TextStyle(fontSize: 14, color: t.inkSoft),
                                    ),
                                  ),
                                  Switch(
                                    value: _haptics,
                                    activeTrackColor: t.breath,
                                    onChanged: (v) => setState(() => _haptics = v),
                                  ),
                                ],
                              ),
                            ] else
                              const SizedBox(height: AppSpace.m),
                            PrimaryButton(
                              label: 'Begin',
                              icon: Icons.air_rounded,
                              gradient: AppGradients.breath,
                              onPressed: _begin,
                            ),
                          ],
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _optionTitle(BreathTones t, String title, String? hint) => Padding(
    padding: const EdgeInsets.only(bottom: 6, left: 2, right: 2),
    child: Row(
      children: [
        Text(
          title,
          style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: t.ink),
        ),
        const Spacer(),
        if (hint != null) Text(hint, style: TextStyle(fontSize: 13, color: t.inkFaint)),
      ],
    ),
  );

  static String _fmt(double v) =>
      v == v.roundToDouble() ? v.toStringAsFixed(0) : v.toStringAsFixed(1);

  // Session ---------------------------------------------------------------

  Widget _session(BreathTones t) {
    final pacer = _pacer;
    final target = _live.target ?? Duration(minutes: _minutes);
    final left = target - _live.elapsed;
    final running = _live.phase == LivePhase.running;
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(AppSpace.s, AppSpace.s, AppSpace.s, 0),
          child: Row(
            children: [
              IconButton(
                tooltip: 'Leave',
                onPressed: _leaveSession,
                icon: Icon(Icons.close_rounded, color: t.inkSoft),
              ),
              const Spacer(),
              _Pill(
                text: running || _hadBeats ? '${clockText(left)} left' : clockText(target),
                color: t.inkSoft,
                bg: t.card.withValues(alpha: t.dark ? 0.6 : 0.8),
              ),
              const Spacer(),
              const SizedBox(width: 48),
            ],
          ),
        ),
        Expanded(
          child: LayoutBuilder(
            builder: (context, box) {
              final orb = math.min(box.maxWidth - 48, box.maxHeight - 96).clamp(120.0, 320.0);
              return ValueListenableBuilder<double>(
                valueListenable: _clock,
                builder: (_, now, _) {
                  final inhaling = pacer.inhaling(now);
                  return Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      BreathOrb(
                        level: pacer.level(now),
                        spin: now * 0.05,
                        size: orb,
                        dimmed: _thumbsOff,
                      ),
                      const SizedBox(height: AppSpace.s),
                      AnimatedSwitcher(
                        duration: const Duration(milliseconds: 700),
                        child: Text(
                          inhaling ? 'Breathe in…' : 'Breathe out…',
                          key: ValueKey(inhaling),
                          style: TextStyle(
                            fontSize: 30,
                            fontWeight: FontWeight.w300,
                            letterSpacing: 0.3,
                            color: t.ink,
                          ),
                        ),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        '${pacer.phaseLeft(now).ceil()}',
                        style: TextStyle(
                          fontSize: 15,
                          color: t.inkFaint,
                          fontFeatures: const [FontFeature.tabularFigures()],
                        ),
                      ),
                    ],
                  );
                },
              );
            },
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: AppSpace.m),
          child: BreathCard(
            padding: const EdgeInsets.fromLTRB(AppSpace.m, 14, AppSpace.m, AppSpace.m),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _heartRow(t),
                const SizedBox(height: AppSpace.xs),
                Stack(
                  children: [
                    ValueListenableBuilder<double>(
                      valueListenable: _clock,
                      builder: (_, now, _) =>
                          BreathWave(samples: _samples, nowSec: now, pacer: pacer, height: 132),
                    ),
                    Positioned.fill(child: _waveOverlay(t)),
                  ],
                ),
                const SizedBox(height: AppSpace.s),
                _legend(t),
                const SizedBox(height: AppSpace.m),
                BreathCoherenceMeter(score: running ? _live.coherence?.score : null),
              ],
            ),
          ),
        ),
        Padding(
          padding: const EdgeInsets.only(top: AppSpace.xs, bottom: AppSpace.xs),
          child: TextButton(
            onPressed: _hadBeats ? _end : _leaveSession,
            style: TextButton.styleFrom(foregroundColor: t.inkSoft),
            child: Text(_hadBeats ? 'Finish early' : 'Not now'),
          ),
        ),
      ],
    );
  }

  Widget _heartRow(BreathTones t) {
    final bpm = _live.fastBpm;
    final showBpm =
        _live.phase == LivePhase.running && bpm != null && !_thumbsOff && !_live.linkLost;
    return Row(
      crossAxisAlignment: CrossAxisAlignment.center,
      children: [
        Icon(Icons.favorite_rounded, size: 18, color: t.heart),
        const SizedBox(width: AppSpace.s),
        Text(
          'Your heart',
          style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: t.ink),
        ),
        const Spacer(),
        if (showBpm)
          Text.rich(
            TextSpan(
              children: [
                TextSpan(
                  text: bpm.round().toString(),
                  style: TextStyle(fontSize: 22, fontWeight: FontWeight.w600, color: t.heart),
                ),
                TextSpan(
                  text: ' bpm',
                  style: TextStyle(fontSize: 13, color: t.inkFaint),
                ),
              ],
            ),
            style: const TextStyle(fontFeatures: [FontFeature.tabularFigures()]),
          )
        else
          const SizedBox(height: 28),
      ],
    );
  }

  Widget _legend(BreathTones t) {
    Widget dot(Color c, String s) => Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          width: 14,
          height: 3,
          decoration: BoxDecoration(color: c, borderRadius: BorderRadius.circular(2)),
        ),
        const SizedBox(width: 6),
        Text(s, style: TextStyle(fontSize: 12, color: t.inkFaint)),
      ],
    );
    // A Wrap, so large text moves the second entry to a new line.
    return Wrap(
      spacing: AppSpace.m,
      runSpacing: AppSpace.xs,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: [
        dot(t.heart, 'Heart rate'),
        dot(t.breath.withValues(alpha: 0.6), 'Your breath guide'),
      ],
    );
  }

  /// Gentle messages over the wave until beats arrive, or when thumbs lift.
  Widget _waveOverlay(BreathTones t) {
    String? msg;
    IconData icon = Icons.touch_app_rounded;
    Widget? extra;
    if (_startError != null) {
      msg = _startError;
      icon = Icons.bluetooth_searching_rounded;
    } else if (_live.phase == LivePhase.error) {
      msg = _live.error ?? 'Something went quiet. Try again in a moment.';
      icon = Icons.bluetooth_searching_rounded;
    } else if (_live.linkLost) {
      msg = 'Reconnecting to your card…\nKeep it close by and keep breathing.';
      icon = Icons.bluetooth_searching_rounded;
    } else if (_thumbsOff) {
      msg = 'Rest your thumbs back on the pads';
    } else if (!_hadBeats) {
      switch (_live.phase) {
        case LivePhase.idle || LivePhase.starting:
          msg = 'Waking your card…';
          icon = Icons.bluetooth_rounded;
        case LivePhase.waitContact:
          msg = 'Rest your thumbs on the pads.\nKeep breathing with the orb.';
        case LivePhase.settling:
          msg = 'Listening for your heartbeat…';
          icon = Icons.favorite_border_rounded;
          extra = Padding(
            padding: const EdgeInsets.only(top: AppSpace.s),
            child: SizedBox(
              width: 120,
              child: ClipRRect(
                borderRadius: BorderRadius.circular(4),
                child: LinearProgressIndicator(
                  value: _live.settleProgress.clamp(0.0, 1.0),
                  minHeight: 4,
                  color: t.heart,
                  backgroundColor: t.track,
                ),
              ),
            ),
          );
        case LivePhase.running:
          msg = 'Your heart will appear in a few beats';
          icon = Icons.favorite_border_rounded;
        case LivePhase.finished:
          msg = null;
        case LivePhase.error:
          msg = null;
      }
    }
    return IgnorePointer(
      child: AnimatedOpacity(
        opacity: msg == null ? 0 : 1,
        duration: const Duration(milliseconds: 400),
        child: Center(
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: AppSpace.m, vertical: 10),
            decoration: BoxDecoration(
              color: t.card.withValues(alpha: 0.92),
              borderRadius: BorderRadius.circular(18),
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(icon, size: 18, color: t.breath),
                    const SizedBox(width: AppSpace.s),
                    Flexible(
                      child: Text(
                        msg ?? '',
                        textAlign: TextAlign.center,
                        style: TextStyle(fontSize: 14, color: t.ink, height: 1.35),
                      ),
                    ),
                  ],
                ),
                ?extra,
              ],
            ),
          ),
        ),
      ),
    );
  }

  // Summary ---------------------------------------------------------------

  Widget _summaryView(BreathTones t) {
    final s = _summary!;
    final swing = s.hrSwingBpm;
    final avg = s.avgCoherence;
    return Column(
      children: [
        BreathHeader(title: 'Breathe', onBack: null),
        Expanded(
          child: SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(AppSpace.m, AppSpace.s, AppSpace.m, AppSpace.m),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Center(child: BreathOrb(level: 0.55, spin: 0.3, size: 120)),
                const SizedBox(height: AppSpace.s),
                Text(
                  'Lovely work',
                  textAlign: TextAlign.center,
                  style: TextStyle(fontSize: 28, fontWeight: FontWeight.w600, color: t.ink),
                ),
                const SizedBox(height: AppSpace.xs),
                Text(
                  '${clockText(s.length)} of slow breathing · ${s.breaths} breaths',
                  textAlign: TextAlign.center,
                  style: TextStyle(fontSize: 15, color: t.inkSoft),
                ),
                const SizedBox(height: AppSpace.l),
                Row(
                  children: [
                    Expanded(
                      child: _Stat(
                        label: 'Time in sync',
                        value: clockText(s.inSync),
                        color: t.breath,
                        icon: Icons.waves_rounded,
                      ),
                    ),
                    const SizedBox(width: AppSpace.s),
                    Expanded(
                      child: _Stat(
                        label: 'Average sync',
                        value: avg == null ? '–' : '${(avg * 100).round()}%',
                        color: t.indigo,
                        icon: Icons.blur_on_rounded,
                      ),
                    ),
                    const SizedBox(width: AppSpace.s),
                    Expanded(
                      child: _Stat(
                        label: 'Heart swing',
                        value: swing == null ? '–' : '${swing.round()}',
                        unit: swing == null ? null : 'bpm',
                        color: t.heart,
                        icon: Icons.favorite_rounded,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: AppSpace.m),
                BreathCard(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Text(
                        'Your heart through one breath',
                        style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: t.ink),
                      ),
                      const SizedBox(height: AppSpace.s),
                      BreathCycleChart(samples: _samples, pacer: _pacer, height: 130),
                      const SizedBox(height: AppSpace.s),
                      Text(
                        swing == null
                            ? 'Next time, keep your thumbs resting on the pads to see your heart follow your breath.'
                            : 'With each breath your heart rose and settled by about ${swing.round()} beats a minute. '
                                  '${(avg ?? 0) >= breathInSyncAt ? 'Your heart and breath moved together beautifully.' : 'Syncing up gets easier with each session.'}',
                        style: TextStyle(fontSize: 14, color: t.inkSoft, height: 1.4),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(AppSpace.m, 0, AppSpace.m, AppSpace.m),
          child: FutureBuilder<SessionRecord?>(
            future: _saving,
            builder: (context, snap) {
              final done = snap.connectionState == ConnectionState.done;
              final saved = snap.data != null;
              return Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  PrimaryButton(
                    label: done && !saved ? 'Too short to save' : 'See your session',
                    gradient: AppGradients.breath,
                    busy: _leaving && !done,
                    onPressed: done && !saved ? null : _openSession,
                  ),
                  const SizedBox(height: AppSpace.xs),
                  TextButton(
                    onPressed: () => context.go(Routes.home),
                    style: TextButton.styleFrom(foregroundColor: t.inkSoft),
                    child: const Text('Done'),
                  ),
                ],
              );
            },
          ),
        ),
      ],
    );
  }
}

class _Stat extends StatelessWidget {
  const _Stat({
    required this.label,
    required this.value,
    required this.color,
    required this.icon,
    this.unit,
  });
  final String label, value;
  final String? unit;
  final Color color;
  final IconData icon;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return BreathCard(
      padding: const EdgeInsets.fromLTRB(12, 14, 12, 14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, size: 20, color: color),
          const SizedBox(height: AppSpace.s),
          Text.rich(
            TextSpan(
              children: [
                TextSpan(text: value),
                if (unit != null)
                  TextSpan(
                    text: ' $unit',
                    style: TextStyle(fontSize: 12, fontWeight: FontWeight.w400, color: t.inkFaint),
                  ),
              ],
            ),
            maxLines: 1,
            style: TextStyle(fontSize: 22, fontWeight: FontWeight.w600, color: t.ink),
          ),
          const SizedBox(height: 2),
          Text(
            label,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: TextStyle(fontSize: 12, color: t.inkSoft),
          ),
        ],
      ),
    );
  }
}

class _Pill extends StatelessWidget {
  const _Pill({required this.text, required this.color, required this.bg});
  final String text;
  final Color color, bg;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
    decoration: BoxDecoration(color: bg, borderRadius: BorderRadius.circular(AppRadius.pill)),
    child: Text(
      text,
      style: TextStyle(
        fontSize: 14,
        fontWeight: FontWeight.w500,
        color: color,
        fontFeatures: const [FontFeature.tabularFigures()],
      ),
    ),
  );
}

/// A row of pill segments (one selected).
class _BreathSegments<T> extends StatelessWidget {
  const _BreathSegments({
    required this.values,
    required this.selected,
    required this.label,
    required this.onChanged,
  });
  final List<T> values;
  final T selected;
  final String Function(T) label;
  final ValueChanged<T> onChanged;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return Container(
      padding: const EdgeInsets.all(4),
      decoration: BoxDecoration(
        color: t.track,
        borderRadius: BorderRadius.circular(AppRadius.pill),
      ),
      child: Row(
        children: [
          for (final v in values)
            Expanded(
              child: Semantics(
                selected: v == selected,
                button: true,
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onTap: () => onChanged(v),
                  child: AnimatedContainer(
                    duration: const Duration(milliseconds: 220),
                    curve: Curves.easeOut,
                    height: 36,
                    alignment: Alignment.center,
                    decoration: BoxDecoration(
                      color: v == selected
                          ? (t.dark ? t.tones.soft(t.breath, 0.16) : t.card)
                          : Colors.transparent,
                      borderRadius: BorderRadius.circular(AppRadius.pill),
                      boxShadow: v == selected
                          ? [
                              BoxShadow(
                                color: Colors.black.withValues(alpha: t.dark ? 0.3 : 0.08),
                                blurRadius: 6,
                                offset: const Offset(0, 2),
                              ),
                            ]
                          : null,
                    ),
                    child: Text(
                      label(v),
                      style: TextStyle(
                        fontSize: 14,
                        fontWeight: v == selected ? FontWeight.w600 : FontWeight.w500,
                        color: v == selected ? (t.dark ? t.breath : AppColors.indigo) : t.inkSoft,
                      ),
                    ),
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }
}
