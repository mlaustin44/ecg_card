// Device providers (owned by the device layer).
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../device/ble_card.dart';
import '../device/card_link.dart';
import '../device/env_stub.dart' if (dart.library.io) '../device/env_io.dart';
import '../device/fake_card.dart';
import '../protocol/link_status.dart';

/// --dart-define=FAKE_CARD=true selects the synthetic card.
const bool fakeCardRequested = bool.fromEnvironment('FAKE_CARD');

/// Whether [cardScannerProvider] defaults to the fake scanner (FAKE_CARD, or
/// running under flutter test). main.dart also overrides it with the fake one
/// when the platform has no Bluetooth (e.g. a browser without Web Bluetooth).
bool get useFakeCardByDefault => fakeCardRequested || runningInFlutterTest;

/// BLE scanner, or the fake one (--dart-define=FAKE_CARD=true, tests, no BLE).
final cardScannerProvider = Provider<CardScanner>((ref) {
  final CardScanner s = useFakeCardByDefault ? FakeScanner() : BleScanner();
  ref.onDispose(s.stop);
  return s;
});

const _kCardId = 'card.id';
const _kCardName = 'card.name';

/// The card remembered from an earlier launch (null if none). Lets the UI pick
/// its first screen (onboarding vs home) before a reconnect completes.
final rememberedCardProvider = FutureProvider<FoundCard?>((ref) async {
  ref.watch(currentCardProvider); // re-read after choose() / forget()
  try {
    final p = await SharedPreferences.getInstance();
    final id = p.getString(_kCardId);
    if (id == null) return null;
    return FoundCard(id: id, name: p.getString(_kCardName) ?? 'ECG Card');
  } catch (_) {
    return null; // no shared_preferences (e.g. a test without mock values)
  }
});

/// The card the app is using (connected or reconnecting); null before one is chosen.
/// Remembered across launches (shared_preferences) and reconnected on start.
final currentCardProvider = NotifierProvider<CurrentCard, CardLink?>(CurrentCard.new);

class CurrentCard extends Notifier<CardLink?> {
  int _epoch = 0; // bumps on choose() / forget(): stops a running restore loop

  @override
  CardLink? build() {
    ref.onDispose(() => _epoch++);
    if (!kIsWeb) {
      Future.microtask(_restore); // web: a card must be picked in each page session
    }
    return null;
  }

  /// Reconnects to the remembered card: connect by id (Android: address,
  /// iOS: retrieved peripheral), retried with backoff while the card is off or
  /// out of range, until it connects or [choose] / [forget] is called.
  Future<void> _restore() async {
    final epoch = _epoch;
    FoundCard? card;
    try {
      final p = await SharedPreferences.getInstance();
      final id = p.getString(_kCardId);
      if (id != null) {
        card = FoundCard(id: id, name: p.getString(_kCardName) ?? 'ECG Card');
      }
    } catch (_) {
      return;
    }
    if (card == null || !ref.mounted || epoch != _epoch || state != null) {
      return;
    }
    final link = ref.read(cardScannerProvider).linkTo(card);
    state = link;
    var wait = 2;
    while (ref.mounted && epoch == _epoch && link.currentLinkState != LinkState.connected) {
      try {
        await link.connect();
        return;
      } catch (_) {
        await Future<void>.delayed(Duration(seconds: wait));
        wait = (wait * 2).clamp(2, 30);
      }
    }
  }

  /// Connect to [card] and remember it. Throws when the connection fails
  /// (the previous card, if any, is then no longer current).
  Future<void> choose(FoundCard card) async {
    _epoch++;
    final scanner = ref.read(cardScannerProvider);
    await scanner.stop();
    final old = state;
    final link = scanner.linkTo(card);
    if (old != null && !identical(old, link)) await old.disconnect();
    state = link;
    try {
      await link.connect();
    } catch (_) {
      if (identical(state, link)) state = null;
      rethrow;
    }
    try {
      final p = await SharedPreferences.getInstance();
      await p.setString(_kCardId, card.id);
      await p.setString(_kCardName, card.name);
    } catch (_) {
      /* not remembered; still connected */
    }
  }

  /// Disconnect and forget.
  Future<void> forget() async {
    _epoch++;
    final old = state;
    state = null;
    await old?.disconnect();
    try {
      final p = await SharedPreferences.getInstance();
      await p.remove(_kCardId);
      await p.remove(_kCardName);
    } catch (_) {}
  }
}

/// Link state of the current card (disconnected when none).
final linkStateProvider = StreamProvider<LinkState>((ref) async* {
  final link = ref.watch(currentCardProvider);
  if (link == null) {
    yield LinkState.disconnected;
    return;
  }
  yield link.currentLinkState;
  yield* link.linkState;
});

/// Latest Status of the current card (null until one arrives / no card).
final cardStatusProvider = StreamProvider<CardStatus?>((ref) async* {
  final link = ref.watch(currentCardProvider);
  if (link == null) {
    yield null;
    return;
  }
  yield link.lastStatus;
  yield* link.status;
});
