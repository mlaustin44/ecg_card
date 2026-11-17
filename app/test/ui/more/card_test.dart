import 'package:ecg_card_app/app.dart';
import 'package:flutter_test/flutter_test.dart';

import 'harness.dart';

void main() {
  for (final dark in [false, true]) {
    final mode = dark ? 'dark' : 'light';
    testWidgets('card screen ($mode)', (tester) async {
      final card = FakeCardLink(status: sampleStatus(batt: 82));
      await pumpAt(tester, Routes.card, overrides: cardOverrides(card), dark: dark);
      await tester.pump(const Duration(seconds: 1));
      expect(find.text('ECG Card 7F3A'), findsOneWidget);
      expect(find.text('82%'), findsOneWidget);
      expect(find.text('1.4.2'), findsOneWidget);
      await golden('card_$mode');
    });
  }

  testWidgets('card screen scrolled to the explainers', (tester) async {
    final card = FakeCardLink(status: sampleStatus(batt: 40, charging: true));
    await pumpAt(tester, Routes.card, overrides: cardOverrides(card));
    await tester.pump(const Duration(seconds: 1));
    expect(find.text('40% · charging'), findsOneWidget);
    await tester.drag(find.text('Battery'), const Offset(0, -560));
    await tester.pump(const Duration(seconds: 1));
    await golden('card_explainer_light');
  });

  testWidgets('no card yet', (tester) async {
    await pumpAt(tester, Routes.card, overrides: cardOverrides(null));
    await tester.pump(const Duration(seconds: 1));
    expect(find.text('No card yet'), findsOneWidget);
    await golden('card_none_light');
  });

  testWidgets('long-press firmware opens Developer', (tester) async {
    final card = FakeCardLink();
    await pumpAt(tester, Routes.card, overrides: cardOverrides(card));
    await tester.pump(const Duration(seconds: 1));
    await tester.longPress(find.text('1.4.2'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 600));
    expect(find.text('Developer'), findsOneWidget);
  });

  testWidgets('forget asks, then forgets and goes to connect', (tester) async {
    final card = FakeCardLink();
    final current = FakeCurrentCard(card);
    await pumpAt(tester, Routes.card, overrides: cardOverrides(card, current: current));
    await tester.pump(const Duration(seconds: 1));
    await tester.tap(find.text('Forget this card'));
    await tester.pump(const Duration(milliseconds: 400));
    expect(find.text('Forget this card?'), findsOneWidget);
    await tester.tap(find.text('Forget'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 600));
    expect(current.forgot, isTrue);
    expect(find.text('stub:connect'), findsOneWidget);
  });
}
