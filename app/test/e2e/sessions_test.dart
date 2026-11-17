// E2E: check-in, contact loss, a refused start and a breathing session, each
// against the real FakeCard, SessionController, store and screens.
import 'dart:async';

import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/state/session_controller.dart';
import 'package:ecg_card_app/ui/widgets/ecg_trace_view.dart';
import 'package:ecg_card_app/ui/widgets/strip_view.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';

import 'e2e_harness.dart';

/// Home with the remembered card connected.
Future<E2e> home(WidgetTester tester) async {
  final app = await pumpApp(tester, prefs: returningUser);
  await app.waitFor(find.text('82%'));
  return app;
}

void main() {
  testWidgets('check-in: thumbs, settling, live, auto-finish at 60 s, results, history', (
    tester,
  ) async {
    final app = await home(tester);
    var ecgSamples = 0;
    final sub = app.live.ecgUv.listen((c) => ecgSamples += c.length);

    await app.tap(find.text('Check in'));
    await app.waitFor(find.text('Place both thumbs\non the pads'));
    expect(app.wakeLock.on, isTrue, reason: 'screen kept on while measuring');
    await app.waitFor(find.text('Lovely. Hold still…'));
    await app.waitFor(find.text('YOUR HEARTBEAT'));
    expect(find.byType(EcgTraceView), findsOneWidget);

    // A plausible heart rate once a few beats are in (FakeCard: 64 ± 7 bpm).
    await app.run(const Duration(seconds: 10));
    expect(app.live.state.phase, LivePhase.running);
    final hr = shownHeartRate(tester);
    expect(hr, isNotNull);
    expect(hr!, inInclusiveRange(55, 75));
    expect(ecgSamples, greaterThan(500 * 8)); // the trace is fed at 500 Hz
    expect(find.textContaining('left'), findsOneWidget);

    // Finishes by itself at the target and opens the results.
    await app.runUntil(
      () => find.text('HEART RATE').evaluate().isNotEmpty,
      what: 'results',
      timeout: const Duration(seconds: 70),
    );
    await tester.pump(const Duration(seconds: 1));
    expect(app.store.saved, hasLength(1));
    final rec = app.store.saved.single;
    expect(rec.kind, SessionKind.checkIn);
    expect(rec.duration.inSeconds, inInclusiveRange(60, 61));
    expect(rec.stats!.meanHr, inInclusiveRange(55, 75));
    expect(rec.calmScore, isNotNull);
    expect(rec.stripUv, isNotEmpty);
    expect(find.text('Check-in'), findsOneWidget); // app bar
    expect(find.text('${rec.stats!.meanHr.round()}'), findsWidgets);
    expect(find.text('CALM SCORE'), findsOneWidget);
    await app.tap(find.byType(StripView));
    expect(find.byType(StripView), findsOneWidget);
    expect(app.wakeLock.on, isFalse);
    expect(app.wakeLock.calls, [true, false]);
    expect(app.card!.mode.name, 'idle', reason: 'the card was stopped');

    // Back home: the latest card, then History lists it.
    await app.tap(find.byTooltip('Back'));
    await app.waitFor(find.text('Last check-in'.toUpperCase()));
    await app.tap(find.text('History'));
    await app.waitFor(find.textContaining('Check-in'));
    await tester.pump(const Duration(seconds: 1));
    expect(find.text('${rec.stats!.meanHr.round()} bpm'), findsWidgets); // the list tile

    unawaited(sub.cancel()); // a root-zone future: awaiting it would leave the fake zone
    await app.tearDown();
  });

  testWidgets('check-in: thumbs lifted -> "Lost touch", timer paused, then resumes', (
    tester,
  ) async {
    final app = await home(tester);
    await app.tap(find.text('Check in'));
    await app.waitFor(find.text('YOUR HEARTBEAT'));
    await app.run(const Duration(seconds: 15));
    expect(find.text('Lost touch. Rest your thumbs back on the pads.'), findsNothing);

    app.card!.liftThumbs(const Duration(seconds: 4));
    await app.run(const Duration(seconds: 1));
    expect(find.text('Lost touch. Rest your thumbs back on the pads.'), findsOneWidget);
    expect(find.text('paused'), findsOneWidget);
    final pausedAt = app.live.state.elapsed;
    await app.run(const Duration(seconds: 2));
    expect(app.live.state.elapsed, pausedAt, reason: 'the timer does not run without contact');
    expect(app.live.state.phase, LivePhase.waitContact);

    // Contact back: settles again, then the timer runs on.
    await app.runUntil(() => app.live.state.phase == LivePhase.settling, what: 'resettling');
    expect(find.text('Nice, hold still a moment while it settles again.'), findsOneWidget);
    await app.runUntil(() => app.live.state.phase == LivePhase.running, what: 'running again');
    await app.run(const Duration(seconds: 3));
    expect(find.text('Lost touch. Rest your thumbs back on the pads.'), findsNothing);
    expect(find.text('paused'), findsNothing);
    expect(app.live.state.elapsed, greaterThan(pausedAt + const Duration(seconds: 2)));

    // Close: confirm stop; nothing saved, the card stopped, the screen may sleep.
    await app.tap(find.byTooltip('Close'));
    await app.waitFor(find.text('Stop this check-in?'));
    await app.tap(find.widgetWithText(TextButton, 'Stop'));
    await app.waitFor(find.text('Check in'));
    await app.run(const Duration(seconds: 1));
    expect(app.store.saved, isEmpty);
    expect(app.live.state.phase, LivePhase.idle);
    expect(app.card!.mode.name, 'idle');
    expect(app.wakeLock.on, isFalse);
    await app.tearDown();
  });

  testWidgets('check-in: a short Bluetooth drop pauses, reconnects and the check-in completes', (
    tester,
  ) async {
    final app = await home(tester);
    await app.tap(find.text('Check in'));
    await app.waitFor(find.text('YOUR HEARTBEAT'));
    await app.run(const Duration(seconds: 25));
    final before = app.live.state.elapsed;

    app.card!.simulateLinkLoss(const Duration(seconds: 3));
    await app.run(const Duration(seconds: 1));
    expect(find.text("Let's try that again"), findsNothing);
    expect(find.textContaining('Reconnecting'), findsOneWidget);
    expect(find.text('paused'), findsOneWidget);
    expect(app.live.state.elapsed - before, lessThan(const Duration(seconds: 2)));
    expect(app.wakeLock.on, isTrue);

    // Back: the timer runs on and the check-in finishes and is saved.
    await app.runUntil(
      () => find.text('HEART RATE').evaluate().isNotEmpty,
      what: 'results',
      timeout: const Duration(seconds: 60),
    );
    expect(app.store.saved, hasLength(1));
    expect(app.store.saved.single.duration.inSeconds, inInclusiveRange(60, 61));
    expect(app.wakeLock.on, isFalse);
    await app.tearDown();
  });

  testWidgets('check-in: the card gone for good keeps what was measured', (tester) async {
    final app = await home(tester);
    await app.tap(find.text('Check in'));
    await app.waitFor(find.text('YOUR HEARTBEAT'));
    await app.run(const Duration(seconds: 30));

    app.card!.simulateLinkLoss(const Duration(minutes: 5));
    await app.runUntil(
      () => find.text('HEART RATE').evaluate().isNotEmpty,
      what: 'results',
      timeout: const Duration(seconds: 60),
    );
    expect(app.store.saved, hasLength(1));
    expect(app.store.saved.single.duration.inSeconds, inInclusiveRange(29, 31));
    expect(app.wakeLock.on, isFalse);
    await app.tearDown();
  });

  testWidgets('start refused on USB power: a friendly message, then it works unplugged', (
    tester,
  ) async {
    final app = await home(tester);
    app.card!.usbPower = true;
    await app.tap(find.text('Check in'));
    await app.waitFor(find.text(SessionMessages.usb));
    expect(find.text("Let's try that again"), findsOneWidget);
    expect(tester.takeException(), isNull);
    expect(app.wakeLock.on, isFalse);

    // Unplugged: Try again goes on to "place your thumbs".
    app.card!.usbPower = false;
    await app.tap(find.text('Try again'));
    await app.waitFor(find.text('Place both thumbs\non the pads'));
    await app.tap(find.byTooltip('Close'));
    await app.waitFor(find.text('Check in'));
    await app.run(const Duration(seconds: 1)); // the route transition

    // The breathing screen says the same.
    app.card!.usbPower = true;
    await app.tap(find.text('Breathe'));
    await app.waitFor(find.text('Begin'));
    await app.tap(find.text('Begin'));
    await app.waitFor(find.text(SessionMessages.usb));
    expect(tester.takeException(), isNull);
    await app.tap(find.byTooltip('Leave'));
    await app.waitFor(find.text('Begin'));
    expect(app.store.saved, isEmpty);
    await app.tearDown();
  });

  testWidgets('breathe: 2 min at 6/min, in sync, summary, saved exactly once, results', (
    tester,
  ) async {
    final app = await home(tester);
    await app.tap(find.text('Breathe'));
    await app.waitFor(find.text('Begin'));
    await app.tap(find.text('2 min'));
    expect(find.text('6 breaths a minute'), findsOneWidget);
    await app.tap(find.text('Begin'));
    await app.waitFor(find.text('Breathe in…'));
    expect(app.wakeLock.on, isTrue);

    var sawInSync = false;
    final words = <String>{};
    await app.runUntil(
      () {
        for (final w in [
          'Finding your rhythm',
          'Getting closer',
          'In sync',
          'Beautifully in sync',
        ]) {
          if (find.text(w).evaluate().isNotEmpty) words.add(w);
        }
        sawInSync |= words.contains('In sync') || words.contains('Beautifully in sync');
        return find.text('Lovely work').evaluate().isNotEmpty;
      },
      what: 'summary',
      timeout: const Duration(minutes: 3),
      step: const Duration(milliseconds: 500),
    );
    expect(sawInSync, isTrue, reason: 'coherence words seen: $words');

    // Let every pending save settle, then count.
    await app.run(const Duration(seconds: 5));
    expect(app.store.saved, hasLength(1), reason: 'the breathing session is saved exactly once');
    final rec = app.store.saved.single;
    expect(rec.kind, SessionKind.breathing);
    expect(rec.breathsPerMinTarget, 6);
    expect(rec.duration.inSeconds, inInclusiveRange(120, 121));
    expect(rec.coherence, isNotNull);
    expect(rec.coherence!, greaterThanOrEqualTo(0.6));
    expect(app.wakeLock.on, isFalse);
    expect(find.text('Time in sync'), findsOneWidget);

    await app.tap(find.text('See your session'));
    await app.waitFor(find.text('Breathing session'));
    expect(find.text('HEART RATE'), findsOneWidget);
    await app.run(const Duration(seconds: 2));
    expect(app.store.saved, hasLength(1));

    // A check-in right after starts fresh (not from the finished breathing session).
    await app.tap(find.byTooltip('Back'));
    await app.waitFor(find.text('Check in'));
    await app.run(const Duration(seconds: 1));
    await app.tap(find.text('Check in'));
    await tester.pump();
    expect(find.text('YOUR HEARTBEAT'), findsNothing);
    await app.waitFor(find.text('Place both thumbs\non the pads'));
    expect(app.live.state.kind, SessionKind.checkIn);
    await app.tearDown();
  });

  testWidgets('breathe: Finish early saves once and stops the card', (tester) async {
    final app = await home(tester);
    await app.tap(find.text('Breathe'));
    await app.waitFor(find.text('Begin'));
    await app.tap(find.text('Begin'));
    await app.waitFor(find.text('Finish early'), timeout: const Duration(seconds: 20));
    await app.run(const Duration(seconds: 40));
    await app.tap(find.text('Finish early'));
    await app.waitFor(find.text('Lovely work'));
    await app.run(const Duration(seconds: 5));
    expect(app.store.saved, hasLength(1));
    expect(app.store.saved.single.duration.inSeconds, inInclusiveRange(38, 45));
    expect(app.live.state.phase, LivePhase.finished);
    expect(app.card!.mode.name, 'idle');
    expect(app.wakeLock.on, isFalse);
    await app.tap(find.text('Done'));
    await app.waitFor(find.text('Check in'));
    await app.tearDown();
  });
}
