// Screenshots of the core screens, light and dark, at phone size.
// Regenerate with: flutter test --update-goldens test/ui/core
import 'dart:typed_data';

import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';

import 'fakes.dart';
import 'golden_utils.dart';

/// Feeds [seconds] of synthetic ECG in 20 ms chunks, pumping as it goes.
Future<void> feedEcg(WidgetTester tester, FakeLive live, double seconds) async {
  final uv = synthEcg(seconds: seconds);
  for (var i = 0; i + 10 <= uv.length; i += 10) {
    live.addEcg(Float32List.fromList(uv.sublist(i, i + 10)));
    await tester.pump(const Duration(milliseconds: 20));
  }
}

LiveSession running({
  Duration elapsed = const Duration(seconds: 23),
  LivePhase phase = LivePhase.running,
}) => LiveSession(
  phase: phase,
  kind: SessionKind.checkIn,
  target: const Duration(seconds: 60),
  elapsed: elapsed,
  hrBpm: 64,
  fastBpm: 63.5,
  rr: synthRr(seconds: elapsed.inSeconds.toDouble()),
  status: fakeStatus(),
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(loadAppFonts);

  for (final dark in [false, true]) {
    final mode = dark ? 'dark' : 'light';

    group('core screens ($mode)', () {
      testWidgets('welcome pages', (tester) async {
        await pumpScreen(tester, Routes.welcome, fakes: Fakes(welcomeSeen: false), dark: dark);
        await expectLater(find.byType(MaterialApp), matchesGoldenFile(golden('welcome_1', dark)));
        await tester.tap(find.text('Next'));
        for (var i = 0; i < 10; i++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        await expectLater(find.byType(MaterialApp), matchesGoldenFile(golden('welcome_2', dark)));
        await tester.tap(find.text('Next'));
        for (var i = 0; i < 10; i++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        await expectLater(find.byType(MaterialApp), matchesGoldenFile(golden('welcome_3', dark)));
      });

      testWidgets('connect', (tester) async {
        final fakes = Fakes(
          found: const [
            FoundCard(id: 'a', name: 'ECG Card 3F2A', rssi: -58),
            FoundCard(id: 'b', name: 'ECG Card 91C0', rssi: -84),
          ],
        );
        await pumpScreen(tester, Routes.connect, fakes: fakes, dark: dark);
        await expectLater(find.byType(MaterialApp), matchesGoldenFile(golden('connect', dark)));
      });

      testWidgets('home, connected, with a result', (tester) async {
        final fakes = Fakes(card: FakeLink(), sessions: [sampleRecord()]);
        await pumpScreen(tester, Routes.home, fakes: fakes, dark: dark);
        await expectLater(find.byType(MaterialApp), matchesGoldenFile(golden('home', dark)));
      });

      testWidgets('home, first time, no card', (tester) async {
        await pumpScreen(tester, Routes.home, fakes: Fakes(), dark: dark);
        await expectLater(find.byType(MaterialApp), matchesGoldenFile(golden('home_new', dark)));
      });

      testWidgets('check-in: place thumbs', (tester) async {
        final fakes = Fakes(
          card: FakeLink(),
          live: const LiveSession(phase: LivePhase.waitContact, target: Duration(seconds: 60)),
        );
        await pumpScreen(tester, Routes.checkIn, fakes: fakes, dark: dark);
        await expectLater(
          find.byType(MaterialApp),
          matchesGoldenFile(golden('checkin_place', dark)),
        );
      });

      testWidgets('check-in: settling', (tester) async {
        final fakes = Fakes(
          card: FakeLink(),
          live: const LiveSession(
            phase: LivePhase.settling,
            settleProgress: 0.6,
            target: Duration(seconds: 60),
          ),
        );
        await pumpScreen(tester, Routes.checkIn, fakes: fakes, dark: dark);
        await expectLater(
          find.byType(MaterialApp),
          matchesGoldenFile(golden('checkin_settling', dark)),
        );
      });

      testWidgets('check-in: live', (tester) async {
        final fakes = Fakes(card: FakeLink(), live: running());
        await pumpScreen(tester, Routes.checkIn, fakes: fakes, dark: dark);
        await feedEcg(tester, fakes.liveApi, 6.5);
        await expectLater(
          find.byType(MaterialApp),
          matchesGoldenFile(golden('checkin_live', dark)),
        );
      });

      testWidgets('check-in: lost touch', (tester) async {
        final fakes = Fakes(
          card: FakeLink(),
          live: running(phase: LivePhase.waitContact),
        );
        await pumpScreen(tester, Routes.checkIn, fakes: fakes, dark: dark);
        await feedEcg(tester, fakes.liveApi, 3);
        await expectLater(
          find.byType(MaterialApp),
          matchesGoldenFile(golden('checkin_lost', dark)),
        );
      });

      testWidgets('results', (tester) async {
        final fakes = Fakes(
          card: FakeLink(),
          sessions: [sampleRecord(note: 'After a walk in the park')],
        );
        await pumpScreen(tester, Routes.session('s1'), fakes: fakes, dark: dark);
        await expectLater(find.byType(MaterialApp), matchesGoldenFile(golden('results', dark)));
      });

      testWidgets('results, whole page', (tester) async {
        final fakes = Fakes(card: FakeLink(), sessions: [sampleRecord()]);
        await pumpScreen(
          tester,
          Routes.session('s1'),
          fakes: fakes,
          dark: dark,
          size: const Size(390, 1340),
        );
        await expectLater(
          find.byType(MaterialApp),
          matchesGoldenFile(golden('results_full', dark)),
        );
      });
    });
  }
}
