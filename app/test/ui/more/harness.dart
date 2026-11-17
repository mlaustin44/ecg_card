// Shared test harness for the Breathe / History / Card / Developer screens:
// real fonts for goldens, fakes for the device and session layers, and a
// small router with stub pages for the screens owned elsewhere.
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:ecg_card_app/analysis/models.dart';
import 'package:ecg_card_app/app.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';
import 'package:ecg_card_app/state/data_providers.dart';
import 'package:ecg_card_app/state/device_providers.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/ui/screens/breathe_screen.dart';
import 'package:ecg_card_app/ui/screens/card_screen.dart';
import 'package:ecg_card_app/ui/screens/developer_screen.dart';
import 'package:ecg_card_app/ui/screens/history_screen.dart';
import 'package:ecg_card_app/ui/theme.dart';
import 'package:ecg_card_app/ui/widgets/format.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_riverpod/misc.dart' show Override;
import 'package:flutter_test/flutter_test.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

/// Tuesday evening; History and the fake data are relative to it.
final testNow = DateTime(2026, 11, 17, 18, 30);

bool _fontsLoaded = false;

Future<void> _family(String family, List<String> paths) async {
  final loader = FontLoader(family);
  var any = false;
  for (final p in paths) {
    final f = File(p);
    if (!f.existsSync()) continue;
    loader.addFont(Future.value(ByteData.sublistView(f.readAsBytesSync())));
    any = true;
  }
  if (any) await loader.load();
}

String _flutterRoot() {
  final env = Platform.environment['FLUTTER_ROOT'];
  if (env != null) return env;
  final exe = Platform.resolvedExecutable;
  final i = exe.indexOf('${Platform.pathSeparator}bin${Platform.pathSeparator}cache');
  return i > 0 ? exe.substring(0, i) : '${Platform.environment['HOME']}/development/flutter';
}

/// Nunito (bundled with the app), Material icons and a monospace stand-in.
Future<void> loadTestFonts() async {
  if (_fontsLoaded) return;
  _fontsLoaded = true;
  final mf = '${_flutterRoot()}/bin/cache/artifacts/material_fonts';
  await _family('Nunito', [
    for (final w in [400, 600, 700, 800]) 'assets/fonts/Nunito-$w.ttf',
  ]);
  await _family('MaterialIcons', ['$mf/MaterialIcons-Regular.otf']);
  await _family('Roboto', [
    '$mf/Roboto-Regular.ttf',
    '$mf/Roboto-Medium.ttf',
    '$mf/Roboto-Bold.ttf',
  ]);
  await _family('monospace', ['$mf/RobotoCondensed-Regular.ttf']);
}

/// An iPhone-sized view (390 x 844 pt, notch and home bar insets).
void phoneView(WidgetTester tester) {
  const dpr = 2.0;
  tester.view.devicePixelRatio = dpr;
  tester.view.physicalSize = const Size(390 * dpr, 844 * dpr);
  tester.view.padding = const FakeViewPadding(top: 47 * dpr, bottom: 34 * dpr);
  tester.view.viewPadding = const FakeViewPadding(top: 47 * dpr, bottom: 34 * dpr);
  addTearDown(tester.view.reset);
}

const goldenKey = ValueKey('golden');

/// Pumps the app shell at [location] with [overrides]; other agents' screens are stubs.
Future<void> pumpAt(
  WidgetTester tester,
  String location, {
  required List<Override> overrides,
  bool dark = false,
}) async {
  await loadTestFonts();
  phoneView(tester);
  Widget stub(String name) => Scaffold(body: Center(child: Text('stub:$name')));
  final router = GoRouter(
    initialLocation: location,
    routes: [
      GoRoute(path: Routes.home, builder: (_, _) => stub('home')),
      GoRoute(path: Routes.connect, builder: (_, _) => stub('connect')),
      GoRoute(path: Routes.checkIn, builder: (_, _) => stub('checkin')),
      GoRoute(path: Routes.breathe, builder: (_, _) => const BreatheScreen()),
      GoRoute(path: Routes.history, builder: (_, _) => const HistoryScreen()),
      GoRoute(path: '/session/:id', builder: (_, s) => stub('session:${s.pathParameters['id']}')),
      GoRoute(path: Routes.card, builder: (_, _) => const CardScreen()),
      GoRoute(path: Routes.developer, builder: (_, _) => const DeveloperScreen()),
    ],
  );
  await tester.pumpWidget(
    ProviderScope(
      overrides: [clockProvider.overrideWithValue(() => testNow), ...overrides],
      child: RepaintBoundary(
        key: goldenKey,
        child: MaterialApp.router(
          debugShowCheckedModeBanner: false,
          theme: AppTheme.light(),
          darkTheme: AppTheme.dark(),
          themeMode: dark ? ThemeMode.dark : ThemeMode.light,
          routerConfig: router,
        ),
      ),
    ),
  );
  await tester.pump();
}

Future<void> golden(String name) =>
    expectLater(find.byKey(goldenKey), matchesGoldenFile('goldens/$name.png'));

// ---- fakes ----

class FakeLiveApi implements LiveSessionApi {
  final _changes = StreamController<LiveSession>.broadcast(sync: true);
  LiveSession _state = const LiveSession();
  final begins = <({SessionKind kind, Duration? target, double? bpm})>[];
  int finishes = 0, cancels = 0;
  SessionRecord? record;

  void emit(LiveSession s) {
    _state = s;
    _changes.add(s);
  }

  @override
  LiveSession get state => _state;
  @override
  Stream<LiveSession> get changes => _changes.stream;
  @override
  Stream<Float32List> get ecgUv => const Stream.empty();

  @override
  Future<void> begin(SessionKind kind, {Duration? target, double? breathsPerMin}) async {
    begins.add((kind: kind, target: target, bpm: breathsPerMin));
    emit(LiveSession(phase: LivePhase.waitContact, kind: kind, target: target));
  }

  @override
  Future<SessionRecord?> finish({bool save = true}) async {
    finishes++;
    return record;
  }

  @override
  Future<void> cancel() async => cancels++;
}

CardStatus sampleStatus({int batt = 82, bool charging = false, CardMode mode = CardMode.idle}) =>
    CardStatus(
      proto: 1,
      mode: mode,
      meas: MeasState.none,
      battPct: batt,
      flags: (charging ? 0x03 : 0) | 0x40,
      gain: 0,
      notch: 0,
      rld: 0,
      ledLimit: 2,
      sweep: 1,
      notchHz: 50,
      hrBpm: 0,
      battMv: 3940,
      fwMajor: 1,
      fwMinor: 4,
      fwPatch: 2,
      uptimeMin: 37,
    );

class FakeCardLink implements CardLink {
  FakeCardLink({this.name = 'ECG Card 7F3A', CardStatus? status})
    : lastStatus = status ?? sampleStatus();

  @override
  final String name;
  @override
  String get id => 'C4:7F:3A:12:9B:01';
  @override
  CardStatus? lastStatus;
  final statusCtl = StreamController<CardStatus>.broadcast();
  final logCtl = StreamController<String>.broadcast(sync: true);
  final calls = <String>[];

  @override
  Stream<LinkState> get linkState => Stream.value(LinkState.connected);
  @override
  LinkState get currentLinkState => LinkState.connected;
  @override
  Stream<CardStatus> get status => statusCtl.stream;
  @override
  Stream<EcgBlock> get ecg => const Stream.empty();
  @override
  Stream<PpgBlock> get ppg => const Stream.empty();
  @override
  Stream<CardEvent> get events => const Stream.empty();
  @override
  Stream<String> get log => logCtl.stream;

  @override
  Future<void> connect() async => calls.add('connect');
  @override
  Future<void> disconnect() async => calls.add('disconnect');
  @override
  Future<CmdResult> start(CardMode mode) async {
    calls.add('start(${mode.name})');
    return CmdResult.ok;
  }

  @override
  Future<CmdResult> stop() async {
    calls.add('stop');
    return CmdResult.ok;
  }

  @override
  Future<CmdResult> setSetting(int item, int value) async {
    calls.add('set($item,$value)');
    return CmdResult.ok;
  }

  @override
  Future<CmdResult> setTime(DateTime now) async {
    calls.add('setTime');
    return CmdResult.ok;
  }
}

class FakeCurrentCard extends CurrentCard {
  FakeCurrentCard(this.card);
  final CardLink? card;
  bool forgot = false;

  @override
  CardLink? build() => card;

  @override
  Future<void> choose(FoundCard card) async {}

  @override
  Future<void> forget() async {
    forgot = true;
    state = null;
  }
}

List<Override> cardOverrides(CardLink? card, {FakeCurrentCard? current}) => [
  currentCardProvider.overrideWith(() => current ?? FakeCurrentCard(card)),
  linkStateProvider.overrideWith(
    (ref) => Stream.value(card == null ? LinkState.disconnected : LinkState.connected),
  ),
];

List<Override> sessionOverrides(List<SessionRecord> sessions, {FakeLiveApi? live}) => [
  sessionsProvider.overrideWith((ref) => Stream.value(sessions)),
  liveSessionProvider.overrideWithValue(live ?? FakeLiveApi()),
];

SessionRecord rec(
  String id,
  DateTime start, {
  SessionKind kind = SessionKind.checkIn,
  double hr = 64,
  int? calm = 4,
  double? coherence,
  Duration duration = const Duration(minutes: 1),
}) => SessionRecord(
  id: id,
  start: start,
  kind: kind,
  duration: duration,
  rr: const [],
  stats: HrvStats(
    beats: 60,
    meanHr: hr,
    minHr: hr - 6,
    maxHr: hr + 7,
    rmssdMs: 42,
    sdnnMs: 50,
    pnn50: 0.2,
  ),
  calmScore: calm,
  coherence: coherence,
  breathsPerMinTarget: kind == SessionKind.breathing ? 6 : null,
);

/// A month of friendly history: most mornings a check-in, some evenings breathing.
List<SessionRecord> sampleHistory() {
  final out = <SessionRecord>[];
  for (var d = 0; d < 30; d++) {
    final day = DateTime(testNow.year, testNow.month, testNow.day - d);
    if (d % 4 != 3) {
      final hr = 66 - d * 0.12 + ((d * 7) % 5) - 2.0;
      out.add(
        rec(
          'c$d',
          day.add(Duration(hours: 7, minutes: 40 + d % 15)),
          hr: hr,
          calm: 2 + (d * 3) % 4,
        ),
      );
    }
    if (d % 3 == 0) {
      out.add(
        rec(
          'b$d',
          day.add(Duration(hours: 17, minutes: 10 + d)),
          kind: SessionKind.breathing,
          hr: 63 + (d % 4).toDouble(),
          calm: 4,
          coherence: 0.45 + (d % 5) * 0.08,
          duration: const Duration(minutes: 5),
        ),
      );
    }
  }
  out.sort((a, b) => b.start.compareTo(a.start));
  return out;
}

/// HapticFeedback calls go through the platform channel; swallow them.
void muteHaptics(WidgetTester tester) {
  tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
    SystemChannels.platform,
    (call) async => null,
  );
}
