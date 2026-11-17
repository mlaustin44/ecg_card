import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/ui/screens/history_screen.dart';
import 'package:flutter_test/flutter_test.dart';

import 'harness.dart';

void main() {
  test('trends: resting HR from check-ins, calm per day', () {
    final all = [
      rec('a', testNow.subtract(const Duration(hours: 2)), hr: 60, calm: 4),
      rec('b', testNow.subtract(const Duration(hours: 3)), hr: 70, calm: 2),
      rec(
        'c',
        testNow.subtract(const Duration(hours: 1)),
        kind: SessionKind.breathing,
        hr: 90,
        calm: 5,
      ),
      rec('old', testNow.subtract(const Duration(days: 40)), hr: 99),
    ];
    final t = BreathTrends.from(all, testNow);
    expect(t.restingHr.single.day, 29);
    expect(t.restingHr.single.value, 65); // breathing session not counted
    expect(t.calm.single.value, closeTo(11 / 3, 1e-9));
    expect(BreathTrends.recent(t.restingHr, 30), 65);
  });

  for (final dark in [false, true]) {
    final mode = dark ? 'dark' : 'light';
    testWidgets('history with a month of sessions ($mode)', (tester) async {
      await pumpAt(
        tester,
        Routes.history,
        overrides: sessionOverrides(sampleHistory()),
        dark: dark,
      );
      await tester.pump(const Duration(seconds: 1));
      expect(find.text('Today'), findsWidgets);
      await golden('history_$mode');
    });

    testWidgets('history empty ($mode)', (tester) async {
      await pumpAt(tester, Routes.history, overrides: sessionOverrides(const []), dark: dark);
      await tester.pump(const Duration(seconds: 1));
      expect(find.text('Your story starts here'), findsOneWidget);
      await golden('history_empty_$mode');
    });
  }

  testWidgets('tap a day filters; tap a session opens it', (tester) async {
    await pumpAt(tester, Routes.history, overrides: sessionOverrides(sampleHistory()));
    await tester.pump(const Duration(seconds: 1));
    await tester.tap(find.text('16'));
    await tester.pump(const Duration(milliseconds: 400));
    expect(find.text('MONDAY 16 NOVEMBER'), findsOneWidget);
    expect(find.text('Show all'), findsOneWidget);
    await tester.tap(find.text('Check-in').first);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.text('stub:session:c1'), findsOneWidget);
  });
}
