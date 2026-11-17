import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/protocol/commands.dart';
import 'package:flutter_test/flutter_test.dart';

import 'harness.dart';

void main() {
  for (final dark in [false, true]) {
    final mode = dark ? 'dark' : 'light';
    testWidgets('developer ($mode)', (tester) async {
      final card = FakeCardLink();
      await pumpAt(tester, Routes.developer, overrides: cardOverrides(card), dark: dark);
      await tester.pump(const Duration(milliseconds: 300));
      card.logCtl.add('I ble: connected\nI acq: start ecg gain auto');
      card.logCtl.add('I ui: page HR monitor');
      await tester.pump(const Duration(milliseconds: 300));
      await golden('developer_$mode');
    });
  }

  testWidgets('commands and setting cycles reach the card', (tester) async {
    final card = FakeCardLink();
    await pumpAt(tester, Routes.developer, overrides: cardOverrides(card));
    await tester.pump(const Duration(milliseconds: 300));
    await tester.tap(find.text('HR monitor'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(card.calls, contains('start(hrMonitor)'));
    expect(find.textContaining('start(hrMonitor) → ok'), findsOneWidget);
    await tester.drag(find.text('STATUS'), const Offset(0, -500));
    await tester.pump(const Duration(milliseconds: 300));
    await tester.tap(find.byTooltip('Next gain value'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(card.calls, contains('set(${SettingItem.gain},1)'));
    // Restore isn't in the status: cycles from the firmware default (Off -> Fast).
    await tester.tap(find.byTooltip('Next restore value'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(card.calls, contains('set(${SettingItem.restore},0)'));
  });
}
