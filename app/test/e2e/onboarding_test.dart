// E2E: first launch (welcome -> connect -> home), and the card screen.
import 'package:ecg_card_app/device/card_link.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'e2e_harness.dart';

void main() {
  testWidgets('first launch: welcome pages, connect, home shows the card', (tester) async {
    final app = await pumpApp(tester);
    await app.waitFor(find.text('Hello! Meet your\nECG Card'));

    await app.tap(find.text('Next'));
    await app.waitFor(find.text('How to hold it'));
    await tester.pump(const Duration(seconds: 1));
    await app.tap(find.text('Next'));
    await app.waitFor(find.text('Yours, and only yours'));
    await tester.pump(const Duration(seconds: 1));
    await app.tap(find.text('Find my card'));

    await app.waitFor(find.text("Let's find your card"));
    await app.waitFor(find.text(fakeCardName));
    expect(find.text('FOUND 1 CARD'), findsOneWidget);
    await app.tap(find.text(fakeCardName));

    // Home with the card connected and its battery in the pill.
    await app.waitFor(find.text('Check in'));
    await app.waitFor(find.text('82%'));
    expect(app.card!.currentLinkState, LinkState.connected);
    expect(find.text(fakeCardName), findsWidgets);
    expect(find.text('Connected to $fakeCardName'), findsOneWidget); // snack bar

    final prefs = await SharedPreferences.getInstance();
    expect(prefs.getBool('welcome_seen_v1'), isTrue);
    expect(prefs.getString('card.id'), fakeCardId);
    await app.tearDown();
  });

  testWidgets('first launch: Skip goes straight to Connect', (tester) async {
    final app = await pumpApp(tester);
    await app.waitFor(find.text('Skip'));
    await app.tap(find.text('Skip'));
    await app.waitFor(find.text("Let's find your card"));
    await app.waitFor(find.text(fakeCardName));
    await app.tearDown();
  });

  testWidgets('returning user: the remembered card reconnects on start', (tester) async {
    final app = await pumpApp(tester, prefs: returningUser);
    await app.waitFor(find.text('82%'));
    expect(app.card!.currentLinkState, LinkState.connected);
    expect(find.text('Hello! Meet your\nECG Card'), findsNothing);
    await app.tearDown();
  });

  testWidgets('card screen shows firmware and battery; Forget goes to Connect', (tester) async {
    final app = await pumpApp(tester, prefs: returningUser);
    await app.waitFor(find.text('82%'));

    await app.tap(find.text('Your card'));
    await app.waitFor(find.text('Firmware'));
    expect(find.text('0.4.0'), findsOneWidget);
    expect(find.text('Battery'), findsOneWidget);
    expect(find.text('82%'), findsOneWidget);
    expect(find.text('Connected'), findsOneWidget);

    // Long-press the firmware version: the Developer screen talks to the card.
    await tester.longPress(find.text('0.4.0'));
    await app.waitFor(find.text('Developer'));
    await app.run(const Duration(seconds: 1));
    await app.tap(find.widgetWithText(FilledButton, 'Stop'));
    await app.waitFor(find.textContaining('stop → ok'));
    await app.run(const Duration(seconds: 11)); // a device log line every 10 s
    await app.scrollTo(find.textContaining('batt: mv='));
    expect(find.text('Nothing logged yet'), findsNothing);
    await app.tap(find.byIcon(Icons.arrow_back_rounded));
    await app.waitFor(find.text('Forget this card'));
    await app.run(const Duration(seconds: 1));

    final card = app.card!;
    await app.tap(find.text('Forget this card'));
    await app.waitFor(find.text('Forget this card?'));
    await app.tap(find.widgetWithText(TextButton, 'Forget'));
    await app.waitFor(find.text("Let's find your card"));

    expect(card.currentLinkState, LinkState.disconnected);
    expect(app.card, isNull);
    final prefs = await SharedPreferences.getInstance();
    expect(prefs.getString('card.id'), isNull);
    // The scanner lists the card again.
    await app.waitFor(find.text(fakeCardName));
    await app.tearDown();
  });
}
