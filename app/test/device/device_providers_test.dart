// Device providers with the fake scanner and mocked shared_preferences.
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/device/fake_card.dart';
import 'package:ecg_card_app/state/device_providers.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

const quick = FakeCardConfig(connectDelay: Duration.zero);

Future<void> settle() => Future<void>.delayed(const Duration(milliseconds: 10));

ProviderContainer make(FakeScanner scanner) =>
    ProviderContainer(overrides: [cardScannerProvider.overrideWithValue(scanner)]);

void main() {
  test('under flutter test the default scanner is the fake one', () {
    expect(useFakeCardByDefault, isTrue);
    final c = ProviderContainer();
    addTearDown(c.dispose);
    expect(c.read(cardScannerProvider), isA<FakeScanner>());
  });

  test('choose connects and remembers; a new app start reconnects; forget clears', () async {
    SharedPreferences.setMockInitialValues({});
    final scanner = FakeScanner(config: quick, autoTick: false, scanDelay: Duration.zero);
    final c = make(scanner);
    expect(c.read(currentCardProvider), isNull);
    expect(await c.read(rememberedCardProvider.future), isNull);
    final sub = c.listen(linkStateProvider, (_, _) {});
    await settle();
    expect(sub.read().value, LinkState.disconnected);

    final found = (await scanner.scan().firstWhere((l) => l.isNotEmpty)).single;
    await c.read(currentCardProvider.notifier).choose(found);
    final link = c.read(currentCardProvider)!;
    expect(link.id, found.id);
    expect(link.currentLinkState, LinkState.connected);
    await settle();
    expect(sub.read().value, LinkState.connected);
    expect((await c.read(rememberedCardProvider.future))!.id, found.id);
    c.dispose();

    // next launch: restores and connects to the remembered card
    final c2 = make(FakeScanner(config: quick, autoTick: false));
    c2.read(currentCardProvider);
    await settle();
    final again = c2.read(currentCardProvider);
    expect(again?.id, found.id);
    expect(again?.currentLinkState, LinkState.connected);

    await c2.read(currentCardProvider.notifier).forget();
    expect(c2.read(currentCardProvider), isNull);
    expect(again!.currentLinkState, LinkState.disconnected);
    expect((await SharedPreferences.getInstance()).getString('card.id'), isNull);
    c2.dispose();
  });

  test('status provider follows the current card', () async {
    SharedPreferences.setMockInitialValues({});
    final scanner = FakeScanner(config: quick, autoTick: false);
    final c = make(scanner);
    addTearDown(c.dispose);
    final sub = c.listen(cardStatusProvider, (_, _) {});
    await c.read(currentCardProvider.notifier).choose(scanner.cards.first);
    await settle();
    expect(sub.read().value?.battPct, 82);
  });
}
