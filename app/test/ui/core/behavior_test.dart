// Behaviour of the core screens and widgets, with fake providers.
import 'dart:typed_data';

import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/device/ble_card.dart' show CardScanException;
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/ui/screens/connect_screen.dart';
import 'package:ecg_card_app/ui/theme.dart';
import 'package:ecg_card_app/ui/widgets/calm_score.dart';
import 'package:ecg_card_app/ui/widgets/ecg_trace_view.dart';
import 'package:ecg_card_app/ui/widgets/format.dart';
import 'package:ecg_card_app/ui/widgets/heart_rate_number.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';

import 'fakes.dart';
import 'golden_utils.dart';

Future<void> settle(WidgetTester tester, [int frames = 10]) async {
  for (var i = 0; i < frames; i++) {
    await tester.pump(const Duration(milliseconds: 100));
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(loadAppFonts);

  group('check-in', () {
    testWidgets('begins a one-minute check-in', (tester) async {
      final fakes = Fakes(card: FakeLink());
      await pumpScreen(tester, Routes.checkIn, fakes: fakes);
      expect(fakes.liveApi.begun, [(SessionKind.checkIn, const Duration(seconds: 60))]);
      expect(find.text('Waking up your card…'), findsOneWidget);
    });

    testWidgets('guides through the phases and opens the result when saved', (tester) async {
      final record = sampleRecord();
      final fakes = Fakes(card: FakeLink(), sessions: [record]);
      fakes.liveApi.beginMoves = true;
      await pumpScreen(tester, Routes.checkIn, fakes: fakes);
      expect(find.text('Place both thumbs\non the pads'), findsOneWidget);

      fakes.liveApi.emit(const LiveSession(phase: LivePhase.settling, settleProgress: 0.5));
      await settle(tester);
      expect(find.text('Lovely. Hold still…'), findsOneWidget);

      fakes.liveApi.emit(
        const LiveSession(
          phase: LivePhase.running,
          target: Duration(seconds: 60),
          elapsed: Duration(seconds: 12),
          hrBpm: 61,
        ),
      );
      await settle(tester);
      expect(find.text('0:48 left'), findsOneWidget);

      fakes.liveApi.emit(
        const LiveSession(
          phase: LivePhase.waitContact,
          target: Duration(seconds: 60),
          elapsed: Duration(seconds: 12),
        ),
      );
      await settle(tester);
      expect(find.textContaining('Lost touch'), findsOneWidget);
      expect(find.text('paused'), findsOneWidget);

      fakes.liveApi.emit(
        LiveSession(
          phase: LivePhase.finished,
          elapsed: const Duration(seconds: 60),
          record: record,
          saved: true,
        ),
      );
      await settle(tester);
      expect(find.text('A calm check-in'), findsOneWidget);
    });

    testWidgets('a short session offers to try again', (tester) async {
      final fakes = Fakes(
        card: FakeLink(),
        live: const LiveSession(phase: LivePhase.finished),
      );
      await pumpScreen(tester, Routes.checkIn, fakes: fakes);
      // A finished session is not shown on open (the screen starts fresh) ...
      expect(find.text('That one was a little short'), findsNothing);
      // ... begin() is called again; the fake leaves the state alone, so emit it.
      fakes.liveApi.emit(const LiveSession(phase: LivePhase.finished));
      await settle(tester);
      expect(find.text('That one was a little short'), findsOneWidget);
      await tester.tap(find.text('Try again'));
      expect(fakes.liveApi.begun.length, 2);
    });

    testWidgets('shows the friendly error text', (tester) async {
      final fakes = Fakes(
        card: FakeLink(),
        live: const LiveSession(phase: LivePhase.error, error: 'Unplug the USB cable.'),
      );
      await pumpScreen(tester, Routes.checkIn, fakes: fakes);
      fakes.liveApi.emit(const LiveSession(phase: LivePhase.error, error: 'Unplug the USB cable.'));
      await settle(tester);
      expect(find.text('Unplug the USB cable.'), findsOneWidget);
    });

    testWidgets('closing cancels the session', (tester) async {
      final fakes = Fakes(
        card: FakeLink(),
        live: const LiveSession(phase: LivePhase.waitContact),
      );
      await pumpScreen(tester, Routes.checkIn, fakes: fakes);
      await tester.tap(find.byTooltip('Close'));
      await settle(tester);
      expect(fakes.liveApi.cancels, greaterThanOrEqualTo(1));
    });
  });

  testWidgets('connect: tapping a card chooses it and goes home', (tester) async {
    final fakes = Fakes(
      found: const [FoundCard(id: 'a', name: 'ECG Card 3F2A', rssi: -60)],
    );
    await pumpScreen(tester, Routes.connect, fakes: fakes);
    await tester.tap(find.text('ECG Card 3F2A'));
    await settle(tester);
    expect(fakes.current.chosen.single.id, 'a');
    expect(find.text('Good morning'), findsOneWidget);
  });

  testWidgets('home sends a first-time visitor to the welcome pages', (tester) async {
    await pumpScreen(tester, Routes.home, fakes: Fakes(welcomeSeen: false));
    expect(find.text('Hello! Meet your\nECG Card'), findsOneWidget);
  });

  group('results', () {
    testWidgets('saves a note', (tester) async {
      final fakes = Fakes(sessions: [sampleRecord()]);
      await pumpScreen(tester, Routes.session('s1'), fakes: fakes, size: const Size(390, 1400));
      await tester.enterText(find.byType(TextField), 'After coffee');
      await settle(tester, 6);
      await tester.ensureVisible(find.text('Save note'));
      await tester.tap(find.text('Save note'));
      await settle(tester);
      expect(fakes.store.all.single.note, 'After coffee');
    });

    testWidgets('deletes after confirming', (tester) async {
      final fakes = Fakes(sessions: [sampleRecord()]);
      await pumpScreen(tester, Routes.session('s1'), fakes: fakes);
      await tester.tap(find.byTooltip('Delete'));
      await settle(tester, 4);
      await tester.tap(find.text('Delete'));
      await settle(tester);
      expect(fakes.store.all, isEmpty);
    });

    testWidgets('unknown id shows a gentle empty state', (tester) async {
      await pumpScreen(tester, Routes.session('nope'), fakes: Fakes());
      expect(find.text("This recording isn't here"), findsOneWidget);
    });
  });

  group('widgets', () {
    testWidgets('HeartRateNumber shows a placeholder, then the number', (tester) async {
      await loadAppFonts();
      Widget at(num? bpm) => MaterialApp(
        theme: AppTheme.light(),
        home: Scaffold(body: HeartRateNumber(bpm: bpm)),
      );
      await tester.pumpWidget(at(null));
      expect(find.text('--'), findsOneWidget);
      await tester.pumpWidget(at(72));
      await settle(tester);
      expect(find.text('72'), findsOneWidget);
    });

    test('calm words are all friendly and distinct', () {
      final words = [for (var i = 1; i <= 5; i++) calmWord(i)];
      expect(words.toSet().length, 5);
      for (final w in words) {
        expect(
          w.toLowerCase(),
          isNot(anyOf(contains('normal'), contains('bad'), contains('poor'))),
        );
      }
    });

    test('scan problems get a friendly title per cause', () {
      expect(
        scanProblemText(const CardScanException('Bluetooth is off')).$1,
        'Bluetooth is switched off',
      );
      expect(
        scanProblemText(const CardScanException('Bluetooth permission is needed to find your card'))
            .$1,
        'Bluetooth permission needed',
      );
      expect(
        scanProblemText(const CardScanException('Bluetooth is not available on this device')).$1,
        "Bluetooth isn't available here",
      );
      expect(scanProblemText(StateError('x')).$1, "Couldn't look for cards");
    });

    test('signal bars from RSSI', () {
      expect(signalBars(-50), 4);
      expect(signalBars(-75), 2);
      expect(signalBars(-100), 0);
      expect(signalBars(null), 2);
    });

    test('formatting', () {
      expect(clockText(const Duration(seconds: 48)), '0:48');
      expect(durationWords(const Duration(seconds: 60)), '1 min');
      expect(greetingFor(DateTime(2026, 1, 1, 20)), 'Good evening');
      expect(friendlyWhen(DateTime(2026, 11, 16, 21, 10), fixedNow), startsWith('Yesterday'));
    });

    test('trace scale follows the QRS height and plays out smoothly', () {
      final m = EcgTraceModel(sampleHz: 500, windowSamples: 2000, minRangeUv: 400);
      final uv = synthEcg(seconds: 4, qrsUv: 1200);
      m.add(Float32List.fromList(uv));
      for (var i = 0; i < 60; i++) {
        m.advance(1 / 60, 30);
      }
      expect(m.shown, lessThanOrEqualTo(m.received.toDouble()));
      expect(m.shown, greaterThan(m.received - 300));
      // Peak-to-peak of the synthetic beat is ~1.5 mV (R up, S down).
      expect(m.range, inInclusiveRange(1200, 1800));
    });
  });
}
