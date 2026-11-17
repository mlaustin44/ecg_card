// End-to-end harness: the real EcgCardApp with the real providers. The only
// overrides are the session store (in memory, counting saves), the wake lock
// (recording) and mock shared preferences; the scanner is the device layer's
// FakeScanner (the default under flutter test), so every flow runs against a
// real FakeCard, the real SessionController and the real screens.
import 'dart:async';

import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/data/memory_session_store.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/data/session_store.dart';
import 'package:ecg_card_app/device/fake_card.dart';
import 'package:ecg_card_app/state/data_providers.dart';
import 'package:ecg_card_app/state/device_providers.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/state/wake_lock.dart';
import 'package:ecg_card_app/ui/widgets/heart_rate_number.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// The card FakeScanner finds (lib/device/fake_card.dart).
const fakeCardId = 'fake-card-3f2a';
const fakeCardName = 'ECG Card 3F2A';

/// Preferences of a returning user whose card is remembered.
const returningUser = <String, Object>{
  'welcome_seen_v1': true,
  'card.id': fakeCardId,
  'card.name': fakeCardName,
};

/// In-memory store that counts saves (a double save would show up here even
/// when both writes land on the same record).
class CountingStore implements SessionStore {
  final _inner = InMemorySessionStore();
  final saved = <SessionRecord>[];

  @override
  Future<void> save(SessionRecord record) {
    saved.add(record);
    return _inner.save(record);
  }

  @override
  Future<SessionRecord?> get(String id) => _inner.get(id);
  @override
  Future<void> delete(String id) => _inner.delete(id);
  @override
  Stream<List<SessionRecord>> watchAll() => _inner.watchAll();
}

class RecordingWakeLock implements WakeLock {
  bool on = false;
  final calls = <bool>[];

  @override
  Future<void> enable() async {
    on = true;
    calls.add(true);
  }

  @override
  Future<void> disable() async {
    on = false;
    calls.add(false);
  }
}

class E2e {
  E2e(this.tester, this.store, this.wakeLock);
  final WidgetTester tester;
  final CountingStore store;
  final RecordingWakeLock wakeLock;

  ProviderContainer get container =>
      ProviderScope.containerOf(tester.element(find.byType(EcgCardApp)));

  /// The real FakeCard behind the current card (null before one is chosen).
  FakeCard? get card => container.read(currentCardProvider) as FakeCard?;

  LiveSessionApi get live => container.read(liveSessionProvider);

  /// Advances the fake clock by [d] with a frame every [step] (the FakeCard
  /// ticks on the fake timers, the screens rebuild on each frame).
  Future<void> run(Duration d, {Duration step = const Duration(milliseconds: 250)}) async {
    var left = d;
    while (left > Duration.zero) {
      final s = left < step ? left : step;
      await tester.pump(s);
      left -= s;
    }
  }

  /// Pumps frames (up to [timeout]) until [finder] finds something.
  Future<void> waitFor(
    Finder finder, {
    Duration timeout = const Duration(seconds: 10),
    Duration step = const Duration(milliseconds: 100),
  }) async {
    var t = Duration.zero;
    while (finder.evaluate().isEmpty) {
      if (t >= timeout) {
        throw TestFailure('Timed out after $timeout waiting for $finder');
      }
      await tester.pump(step);
      t += step;
    }
  }

  /// Runs until [cond] holds (checked after each frame).
  Future<void> runUntil(
    bool Function() cond, {
    required String what,
    Duration timeout = const Duration(seconds: 30),
    Duration step = const Duration(milliseconds: 250),
  }) async {
    var t = Duration.zero;
    while (!cond()) {
      if (t >= timeout) throw TestFailure('Timed out after $timeout waiting for $what');
      await tester.pump(step);
      t += step;
    }
  }

  /// Scrolls the page's main list until [f] is built and visible.
  Future<void> scrollTo(Finder f) async {
    if (f.evaluate().isEmpty) {
      await tester.scrollUntilVisible(f, 200, scrollable: find.byType(Scrollable).first);
    }
    await tester.ensureVisible(f);
  }

  /// Taps [f], scrolling the page's list to it first when it is not built yet.
  Future<void> tap(Finder f) async {
    await scrollTo(f);
    await tester.pump();
    await tester.tap(f);
    await tester.pump();
  }

  /// Disconnects the card (stops its periodic timer) and unmounts the app,
  /// disposing every provider, then lets pending timers run out.
  Future<void> tearDown() async {
    // Not awaited directly: the fake zone's microtasks only run when pumped.
    unawaited(card?.disconnect());
    await tester.pump();
    await tester.pumpWidget(const SizedBox());
    await tester.pump(const Duration(seconds: 5));
  }
}

/// Pumps the real app on an iPhone-sized view.
Future<E2e> pumpApp(WidgetTester tester, {Map<String, Object> prefs = const {}}) async {
  SharedPreferences.setMockInitialValues(prefs);
  tester.view.devicePixelRatio = 2;
  tester.view.physicalSize = const Size(390 * 2, 844 * 2);
  addTearDown(tester.view.reset);
  final store = CountingStore();
  final wake = RecordingWakeLock();
  await tester.pumpWidget(
    ProviderScope(
      overrides: [
        sessionStoreProvider.overrideWithValue(store),
        wakeLockProvider.overrideWithValue(wake),
      ],
      child: const EcgCardApp(),
    ),
  );
  await tester.pump();
  return E2e(tester, store, wake);
}

/// The big heart-rate number shown on the check-in screen (null for "--").
num? shownHeartRate(WidgetTester tester) {
  final f = find.byType(HeartRateNumber);
  if (f.evaluate().isEmpty) return null;
  return tester.widget<HeartRateNumber>(f.first).bpm;
}
