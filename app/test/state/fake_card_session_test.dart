// The session controller against the device layer's FakeCard (device clock
// driven by advance(), so this runs fast and deterministically).
import 'package:ecg_card_app/data/memory_session_store.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/device/fake_card.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/state/session_controller.dart';
import 'package:flutter_test/flutter_test.dart';

Future<void> run(FakeCard card, Duration d) async {
  final steps = d.inMilliseconds ~/ 100;
  for (var i = 0; i < steps; i++) {
    card.advance(const Duration(milliseconds: 100));
    await Future<void>.delayed(Duration.zero);
  }
}

void main() {
  late FakeCard card;
  late InMemorySessionStore store;
  late SessionController ctl;

  setUp(() async {
    card = FakeCard(autoTick: false, config: const FakeCardConfig(connectDelay: Duration.zero));
    await card.connect();
    store = InMemorySessionStore();
    ctl = SessionController(card: () => card, store: store);
  });
  tearDown(() async {
    ctl.dispose();
    await card.dispose();
  });

  test('breathing session with the FakeCard', () async {
    final phases = <LivePhase>{};
    final sub = ctl.changes.listen((s) => phases.add(s.phase));
    final begun = ctl.begin(
      SessionKind.breathing,
      target: const Duration(seconds: 90),
      breathsPerMin: 6,
    );
    await run(card, const Duration(milliseconds: 200));
    await begun;
    await run(card, const Duration(seconds: 100));
    await pumpEventQueue();
    expect(
      phases,
      containsAll([
        LivePhase.waitContact,
        LivePhase.settling,
        LivePhase.running,
        LivePhase.finished,
      ]),
    );
    final s = ctl.state;
    expect(s.phase, LivePhase.finished);
    expect(s.saved, isTrue);
    final r = s.record!;
    expect(r.duration.inSeconds, 90);
    expect(r.stats!.meanHr, closeTo(64, 4));
    expect(r.rr.where((x) => x.artifact).length, lessThan(3));
    expect(r.coherence, greaterThan(0.5));
    expect(r.stripUv, hasLength(2500));
    // ignore: avoid_print
    print(
      'fake breathing: hr ${r.stats!.meanHr.toStringAsFixed(1)} rmssd ${r.stats!.rmssdMs.toStringAsFixed(1)} '
      'calm ${r.calmScore} coherence ${r.coherence!.toStringAsFixed(2)} '
      'live bpm ${s.coherence?.breathsPerMin?.toStringAsFixed(2)} beats ${r.rr.length}',
    );
    await sub.cancel();
  });

  test('contact lost mid check-in with the FakeCard', () async {
    final begun = ctl.begin(SessionKind.checkIn, target: const Duration(seconds: 40));
    await run(card, const Duration(milliseconds: 200));
    await begun;
    await run(card, const Duration(seconds: 20));
    expect(ctl.state.phase, LivePhase.running);
    final e1 = ctl.state.elapsed;
    card.liftThumbs(const Duration(seconds: 3));
    await run(card, const Duration(seconds: 2));
    expect(ctl.state.phase, isNot(LivePhase.running));
    final e2 = ctl.state.elapsed;
    expect((e2 - e1).inMilliseconds, lessThan(600));
    await run(card, const Duration(seconds: 40));
    await pumpEventQueue();
    expect(ctl.state.phase, LivePhase.finished);
    final r = ctl.state.record!;
    expect(r.duration.inSeconds, 40);
    expect(r.rr.every((x) => x.ms < 2000), isTrue);
  });

  test('USB power: start refused with the friendly text', () async {
    card.usbPower = true;
    final begun = ctl.begin(SessionKind.checkIn);
    await run(card, const Duration(milliseconds: 300));
    await begun;
    expect(ctl.state.phase, LivePhase.error);
    expect(ctl.state.error, SessionMessages.usb);
  });
}
