// Shared setup for the core UI screenshot tests: real fonts, phone-sized
// view, fake providers, light or dark theme.
import 'dart:convert';

import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/state/data_providers.dart';
import 'package:ecg_card_app/state/device_providers.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/ui/screens/welcome_screen.dart';
import 'package:ecg_card_app/ui/theme.dart';
import 'package:ecg_card_app/ui/widgets/format.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_riverpod/misc.dart' show Override;
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'fakes.dart';

/// A fixed "now" so greetings and dates are stable: Tue 17 Nov 2026, 9:05.
final fixedNow = DateTime(2026, 11, 17, 9, 5);

bool _fontsLoaded = false;

/// Loads the bundled Nunito and the Material icon font so text renders.
Future<void> loadAppFonts() async {
  if (_fontsLoaded) return;
  _fontsLoaded = true;
  final nunito = FontLoader('Nunito');
  for (final w in [400, 600, 700, 800]) {
    nunito.addFont(rootBundle.load('assets/fonts/Nunito-$w.ttf'));
  }
  await nunito.load();
  // The icon font's asset key comes from the font manifest.
  final manifest = jsonDecode(await rootBundle.loadString('FontManifest.json')) as List<Object?>;
  for (final f in manifest.cast<Map<String, Object?>>()) {
    final family = f['family'] as String;
    if (!family.contains('MaterialIcons')) continue;
    final loader = FontLoader(family);
    for (final a in (f['fonts'] as List).cast<Map<String, Object?>>()) {
      loader.addFont(rootBundle.load(a['asset'] as String));
    }
    await loader.load();
  }
}

class Fakes {
  Fakes({
    CardLink? card,
    List<FoundCard> found = const [],
    List<SessionRecord> sessions = const [],
    LiveSession live = const LiveSession(),
    this.welcomeSeen = true,
  }) : current = FakeCurrentCard(card),
       scanner = FakeScanner(found),
       store = FakeStore(sessions),
       liveApi = FakeLive(live),
       link = card?.currentLinkState ?? LinkState.disconnected;

  final FakeCurrentCard current;
  final FakeScanner scanner;
  final FakeStore store;
  final FakeLive liveApi;
  final LinkState link;
  final bool welcomeSeen;

  List<Override> get overrides => [
    cardScannerProvider.overrideWithValue(scanner),
    currentCardProvider.overrideWith(() => current),
    linkStateProvider.overrideWith((ref) => Stream.value(link)),
    sessionStoreProvider.overrideWithValue(store),
    liveSessionProvider.overrideWithValue(liveApi),
    clockProvider.overrideWithValue(() => fixedNow),
    welcomeSeenProvider.overrideWith((ref) async => welcomeSeen),
  ];
}

/// Pumps the app router at [location] on a 390x844 phone.
Future<void> pumpScreen(
  WidgetTester tester,
  String location, {
  required Fakes fakes,
  bool dark = false,
  Size size = const Size(390, 844),
}) async {
  await loadAppFonts();
  SharedPreferences.setMockInitialValues({welcomeSeenKey: fakes.welcomeSeen});
  tester.view.devicePixelRatio = 2;
  tester.view.physicalSize = size * 2;
  addTearDown(tester.view.reset);
  // Real blurred shadows in screenshots (tests draw them flat by default).
  debugDisableShadows = false;
  addTearDown(() => debugDisableShadows = true);
  await tester.pumpWidget(
    ProviderScope(
      overrides: fakes.overrides,
      child: MaterialApp.router(
        debugShowCheckedModeBanner: false,
        theme: AppTheme.light(),
        darkTheme: AppTheme.dark(),
        themeMode: dark ? ThemeMode.dark : ThemeMode.light,
        routerConfig: buildRouter(initialLocation: location),
      ),
    ),
  );
  // Let futures (prefs, streams) resolve and entrance animations finish.
  for (var i = 0; i < 12; i++) {
    await tester.pump(const Duration(milliseconds: 100));
  }
}

/// Golden file path for [name] in light or dark.
String golden(String name, bool dark) => 'goldens/${name}_${dark ? 'dark' : 'light'}.png';
