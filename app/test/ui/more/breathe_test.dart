import 'dart:math' as math;

import 'package:ecg_card_app/analysis/models.dart';
import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/ui/screens/breathe_screen.dart';
import 'package:ecg_card_app/ui/widgets/breath_coherence.dart';
import 'package:ecg_card_app/ui/widgets/breath_orb.dart';
import 'package:ecg_card_app/ui/widgets/breath_wave.dart';
import 'package:flutter/widgets.dart' show SizedBox;
import 'package:flutter_test/flutter_test.dart';

import 'harness.dart';

/// Heart rate that follows a 6/min pacer, peaking a little after full inhale.
double rsaBpm(double t) =>
    66 - 7 * math.cos(2 * math.pi * (t - 1.0) / 10) + 0.8 * math.sin(t * 1.7);

/// Runs a session from Begin: waiting, settling, then beats from [beatsFrom] s
/// until [until] s on the screen's clock.
Future<void> runSession(
  WidgetTester tester,
  FakeLiveApi api, {
  required double until,
  double beatsFrom = 8,
  Duration target = const Duration(minutes: 5),
}) async {
  await tester.tap(find.text('Begin'));
  await tester.pump();
  var t = 0.0;
  const step = 0.25;
  var nextBeat = beatsFrom;
  final rr = <RrInterval>[];
  while (t < until - 1e-9) {
    if (t >= 3 && t < beatsFrom) {
      api.emit(
        LiveSession(
          phase: LivePhase.settling,
          kind: SessionKind.breathing,
          target: target,
          settleProgress: (t - 3) / (beatsFrom - 3),
        ),
      );
    }
    if (t >= nextBeat) {
      final bpm = rsaBpm(t);
      rr.add(RrInterval(tSec: t, ms: (60000 / bpm).round()));
      final run = t - beatsFrom;
      api.emit(
        LiveSession(
          phase: LivePhase.running,
          kind: SessionKind.breathing,
          target: target,
          elapsed: Duration(milliseconds: (run * 1000).round()),
          fastBpm: bpm,
          hrBpm: bpm.round(),
          rr: List.of(rr),
          coherence: run > 15
              ? CoherenceResult(score: math.min(0.82, 0.2 + run / 80), breathsPerMin: 6)
              : null,
        ),
      );
      nextBeat += 60 / bpm;
    }
    await tester.pump(const Duration(milliseconds: 250));
    t += step;
  }
}

void main() {
  group('pacer', () {
    test('sinusoidal level and phases at 6/min, 1:1', () {
      const p = BreathPacer();
      expect(p.cycleSec, 10);
      expect(p.level(0), closeTo(0, 1e-9));
      expect(p.level(2.5), closeTo(0.5, 1e-9));
      expect(p.level(5), closeTo(1, 1e-9));
      expect(p.level(7.5), closeTo(0.5, 1e-9));
      expect(p.inhaling(4.9), isTrue);
      expect(p.inhaling(5.1), isFalse);
      expect(p.breathIndex(19.9), 1);
      expect(p.describe(), '5 s in, 5 s out');
    });

    test('4:6 at 5.5/min', () {
      const p = BreathPacer(breathsPerMin: 5.5, inhaleShare: 0.4);
      expect(p.inhaleSec, closeTo(4.36, 0.01));
      expect(p.describe(), '4.4 s in, 6.5 s out');
    });
  });

  test('heart-rate swing is the mean peak-to-trough per breath', () {
    const p = BreathPacer();
    final samples = [
      for (var t = 0.0; t < 60; t += 0.9) BreathSample(t, 66 - 7 * math.cos(2 * math.pi * t / 10)),
    ];
    expect(BreathSummary.hrSwing(samples, p, 60), closeTo(14, 0.6));
    expect(BreathSummary.hrSwing(const [], p, 60), isNull);
  });

  test('coherence words', () {
    expect(breathCoherenceWord(null), 'Finding your rhythm');
    expect(breathCoherenceWord(0.45), 'Getting closer');
    expect(breathCoherenceWord(0.7), 'In sync');
  });

  for (final dark in [false, true]) {
    final mode = dark ? 'dark' : 'light';

    testWidgets('setup ($mode)', (tester) async {
      muteHaptics(tester);
      await pumpAt(tester, Routes.breathe, overrides: [...sessionOverrides(const [])], dark: dark);
      await tester.pump(const Duration(milliseconds: 3200)); // orb mid-breath
      await golden('breathe_setup_$mode');
    });

    testWidgets('session mid-inhale ($mode)', (tester) async {
      muteHaptics(tester);
      final api = FakeLiveApi();
      await pumpAt(
        tester,
        Routes.breathe,
        overrides: sessionOverrides(const [], live: api),
        dark: dark,
      );
      await runSession(tester, api, until: 63.25);
      expect(find.text('Breathe in…'), findsOneWidget);
      await golden('breathe_session_$mode');
      await tester.pumpWidget(const SizedBox());
    });
  }

  testWidgets('settling: orb runs, wave waits for beats', (tester) async {
    muteHaptics(tester);
    final api = FakeLiveApi();
    await pumpAt(tester, Routes.breathe, overrides: sessionOverrides(const [], live: api));
    await runSession(tester, api, until: 6, beatsFrom: 20);
    expect(api.begins.single.kind, SessionKind.breathing);
    expect(api.begins.single.bpm, 6);
    expect(api.begins.single.target, const Duration(minutes: 5));
    expect(find.text('Listening for your heartbeat…'), findsOneWidget);
    expect(find.byType(BreathOrb), findsOneWidget);
    await golden('breathe_settling_light');
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('thumbs off shows a gentle prompt', (tester) async {
    muteHaptics(tester);
    final api = FakeLiveApi();
    await pumpAt(tester, Routes.breathe, overrides: sessionOverrides(const [], live: api));
    await runSession(tester, api, until: 30);
    api.emit(api.state.copyWith(phase: LivePhase.waitContact));
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.text('Rest your thumbs back on the pads'), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
  });

  for (final dark in [false, true]) {
    final mode = dark ? 'dark' : 'light';
    testWidgets('2 min session ends in a summary and opens the record ($mode)', (tester) async {
      muteHaptics(tester);
      final api = FakeLiveApi()
        ..record = rec('br1', testNow, kind: SessionKind.breathing, coherence: 0.7, calm: 4);
      await pumpAt(
        tester,
        Routes.breathe,
        overrides: sessionOverrides(const [], live: api),
        dark: dark,
      );
      await tester.tap(find.text('2 min'));
      await tester.pump(const Duration(milliseconds: 300));
      await runSession(tester, api, until: 130, beatsFrom: 6, target: const Duration(minutes: 2));
      expect(api.finishes, 1);
      await tester.pump(const Duration(seconds: 1));
      expect(find.text('Lovely work'), findsOneWidget);
      expect(find.text('Time in sync'), findsOneWidget);
      await golden('breathe_summary_$mode');
      await tester.tap(find.text('See your session'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('stub:session:br1'), findsOneWidget);
    });
  }
}
