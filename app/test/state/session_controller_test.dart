import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:ecg_card_app/data/memory_session_store.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:ecg_card_app/state/session_controller.dart';
import 'package:flutter_test/flutter_test.dart';

import 'test_card.dart';

void main() {
  late TestCard card;
  late InMemorySessionStore store;
  late SessionController ctl;
  CardLink? current;

  setUp(() {
    card = TestCard();
    current = card;
    store = InMemorySessionStore();
    ctl = SessionController(
      card: () => current,
      store: store,
      settleDuration: const Duration(milliseconds: 200),
    );
  });
  tearDown(() => ctl.dispose());

  /// begin -> wait contact -> settling (blocks flagged) -> running.
  Future<void> startRunning(SessionKind kind, {Duration? target, double? bpm}) async {
    await ctl.begin(kind, target: target, breathsPerMin: bpm);
    expect(card.startCalls, 1);
    expect(card.startedMode, CardMode.ecg);
    card.meas(MeasState.waitContact);
    expect(ctl.state.phase, LivePhase.waitContact);
    card.event(const LeadEvent(contact: true));
    expect(ctl.state.phase, LivePhase.settling);
    card.meas(MeasState.settling);
    card.stream(2.5, flags: 0x02, beats: false);
    expect(ctl.state.elapsed, Duration.zero);
    card.meas(MeasState.running);
    expect(ctl.state.phase, LivePhase.running);
    expect(ctl.state.settleProgress, 1);
  }

  test('check-in: runs to the target, auto-finishes and saves', () async {
    final chunks = <Float32List>[];
    final sub = ctl.ecgUv.listen(chunks.add);
    await startRunning(SessionKind.checkIn, target: const Duration(seconds: 30));
    card.rrMs = (n) => 860 + 40 * (n % 3);
    card.stream(35);
    await pumpEventQueue();
    final s = ctl.state;
    expect(s.phase, LivePhase.finished);
    expect(s.saved, isTrue);
    final r = s.record!;
    expect(r.kind, SessionKind.checkIn);
    expect(r.duration.inMilliseconds, closeTo(30000, 40));
    expect(r.rr.length, inInclusiveRange(30, 36));
    expect(r.stats!.meanHr, closeTo(60000 / 900, 2));
    expect(r.calmScore, isNotNull);
    expect(r.coherence, isNull);
    expect(r.stripHz, 250);
    expect(r.stripUv, hasLength(2500));
    expect(r.stripUv.reduce(math.max), greaterThan(800)); // QRS visible
    expect(r.cardName, 'ECG Card TEST');
    expect(card.stopCalls, 1);
    expect((await store.get(r.id))!.id, r.id);
    expect(chunks, isNotEmpty);
    expect(chunks.first, hasLength(10));
    // Idempotent.
    expect(await ctl.finish(), same(r));
    await sub.cancel();
  });

  test('live numbers: hrBpm, fastBpm, rr list', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(10);
    final s = ctl.state;
    expect(s.hrBpm, closeTo(67, 1));
    expect(s.fastBpm, closeTo(60000 / 900, 0.5));
    expect(s.rr.length, greaterThan(8));
    expect(s.elapsed.inSeconds, 10);
  });

  test('settling progress is estimated over the settle time', () async {
    await ctl.begin(SessionKind.checkIn);
    card.meas(MeasState.settling);
    expect(ctl.state.phase, LivePhase.settling);
    expect(ctl.state.settleProgress, 0);
    await Future<void>.delayed(const Duration(milliseconds: 130));
    expect(ctl.state.settleProgress, inExclusiveRange(0.2, 0.96));
    await ctl.cancel();
  });

  test('contact loss pauses the timer and no interval spans the gap', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(10);
    final before = ctl.state.rr.length;
    card.event(const LeadEvent(contact: false));
    expect(ctl.state.phase, LivePhase.waitContact);
    card.stream(5, flags: 0x04);
    expect(ctl.state.elapsed.inSeconds, 10);
    expect(ctl.state.rr.length, before);
    card.meas(MeasState.settling);
    card.stream(2.5, flags: 0x02, beats: false);
    card.meas(MeasState.running);
    card.stream(10);
    expect(ctl.state.elapsed.inSeconds, 20);
    final rr = ctl.state.rr;
    expect(rr.every((r) => r.ms < 1000), isTrue, reason: 'no 7 s interval');
    expect(rr.where((r) => r.artifact), isEmpty);
  });

  test('block flags move the phase before Status does', () async {
    await ctl.begin(SessionKind.checkIn);
    card.stream(0.1, flags: 0x02, beats: false);
    expect(ctl.state.phase, LivePhase.settling);
    card.stream(0.1);
    expect(ctl.state.phase, LivePhase.running);
    card.stream(0.1, flags: 0x04, beats: false);
    expect(ctl.state.phase, LivePhase.waitContact);
    await ctl.cancel();
  });

  test('breathing: coherence every 5 s, high for paced RSA; saved with the target pace', () async {
    await startRunning(SessionKind.breathing, bpm: 6);
    // RSA at 6 / min: RR = 900 + 80 sin(2 pi 0.1 t).
    final rrList = <int>[];
    for (var t = 0.0; t < 200;) {
      final ms = (900 + 80 * math.sin(2 * math.pi * 0.1 * t)).round() & ~1;
      t += ms / 1000;
      rrList.add(ms);
    }
    card.rrMs = (n) => rrList[n.clamp(0, rrList.length - 1)];
    final scores = <double>[];
    final sub = ctl.changes.listen((s) {
      if (s.coherence != null && (scores.isEmpty || scores.last != s.coherence!.score)) {
        scores.add(s.coherence!.score);
      }
    });
    card.stream(90);
    expect(ctl.state.coherence, isNotNull);
    expect(ctl.state.coherence!.score, greaterThan(0.8));
    expect(ctl.state.coherence!.breathsPerMin, closeTo(6, 0.5));
    final r = (await ctl.finish())!;
    expect(r.kind, SessionKind.breathing);
    expect(r.breathsPerMinTarget, 6);
    expect(r.coherence, greaterThan(0.6));
    expect(ctl.state.saved, isTrue);
    await sub.cancel();
  });

  test('too short: record returned but not saved', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(12);
    final r = await ctl.finish();
    expect(r, isNotNull);
    expect(ctl.state.phase, LivePhase.finished);
    expect(ctl.state.saved, isFalse);
    expect(await store.get(r!.id), isNull);
  });

  test('finish(save: false) does not save', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(25);
    final r = await ctl.finish(save: false);
    expect(r!.duration.inSeconds, 25);
    expect(await store.get(r.id), isNull);
  });

  group('start refused', () {
    for (final (res, msg) in [
      (CmdResult.refusedUsb, SessionMessages.usb),
      (CmdResult.refusedBattery, SessionMessages.lowBattery),
      (CmdResult.timeout, SessionMessages.timeout),
      (CmdResult.busy, SessionMessages.busy),
    ]) {
      test(res.name, () async {
        card.startResult = res;
        await ctl.begin(SessionKind.checkIn);
        expect(ctl.state.phase, LivePhase.error);
        expect(ctl.state.error, msg);
        expect(await ctl.finish(), isNull);
      });
    }

    test('USB text', () {
      expect(SessionMessages.usb, 'Unplug the USB cable — the card measures on battery only.');
    });

    test('busy while the card is already measuring ECG: adopted', () async {
      card.startResult = CmdResult.busy;
      card.lastStatus = statusOf(mode: CardMode.ecg, meas: MeasState.running);
      await ctl.begin(SessionKind.checkIn);
      expect(ctl.state.phase, LivePhase.running);
      card.stream(3);
      expect(ctl.state.elapsed.inSeconds, 3);
      await ctl.cancel();
    });

    test('USB blocked from Status, then unplugged: recovers', () async {
      await ctl.begin(SessionKind.checkIn);
      card.meas(MeasState.usbBlocked);
      expect(ctl.state.phase, LivePhase.error);
      expect(ctl.state.error, SessionMessages.usb);
      card.meas(MeasState.waitContact);
      expect(ctl.state.phase, LivePhase.waitContact);
      expect(ctl.state.error, isNull);
      await ctl.cancel();
    });
  });

  test('no card', () async {
    current = null;
    await ctl.begin(SessionKind.checkIn);
    expect(ctl.state.phase, LivePhase.error);
    expect(ctl.state.error, SessionMessages.noCard);
  });

  test('card not connected', () async {
    card.currentLinkState = LinkState.connecting;
    await ctl.begin(SessionKind.checkIn);
    expect(ctl.state.error, SessionMessages.notConnected);
    expect(card.startCalls, 0);
  });

  // A dropped link no longer ends the session at once (it used to fail with
  // SessionMessages.disconnected and drop what was measured): the session
  // pauses and carries on when the card is back within linkGrace.
  test('a dropped link pauses; the session carries on when it is back', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(25);
    card.setLink(LinkState.disconnected);
    expect(ctl.state.phase, LivePhase.running);
    expect(ctl.state.linkLost, isTrue);
    expect(ctl.state.elapsed.inSeconds, 25);
    card.setLink(LinkState.connected);
    expect(ctl.state.linkLost, isFalse);
    card.stream(10);
    expect(ctl.state.elapsed.inSeconds, 35);
    final r = await ctl.finish(save: true);
    expect(r!.duration.inSeconds, 35);
    expect(ctl.state.saved, isTrue);
  });

  void withLinkGrace() {
    ctl.dispose();
    ctl = SessionController(
      card: () => current,
      store: store,
      settleDuration: const Duration(milliseconds: 200),
      linkGrace: const Duration(milliseconds: 50),
    );
  }

  test('a link that does not come back keeps what was measured', () async {
    withLinkGrace();
    await startRunning(SessionKind.checkIn);
    card.stream(25);
    card.setLink(LinkState.disconnected);
    card.stream(5); // nothing arrives while disconnected in reality; ignored after the grace anyway
    await Future<void>.delayed(const Duration(milliseconds: 120));
    expect(ctl.state.phase, LivePhase.finished);
    expect(ctl.state.saved, isTrue);
    expect(card.stopCalls, 0);
    final r = ctl.state.record!;
    expect(await store.get(r.id), isNotNull);
  });

  test('a link gone before enough was measured: a friendly message', () async {
    withLinkGrace();
    await startRunning(SessionKind.checkIn);
    card.stream(5);
    card.setLink(LinkState.disconnected);
    await Future<void>.delayed(const Duration(milliseconds: 120));
    expect(ctl.state.phase, LivePhase.error);
    expect(ctl.state.error, SessionMessages.disconnected);
    expect(ctl.state.linkLost, isFalse);
    card.stream(5); // ignored
    expect(ctl.state.elapsed.inSeconds, 5);
  });

  test('stopped on the card: finishes and saves', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(25);
    card.meas(MeasState.none, mode: CardMode.idle);
    await pumpEventQueue();
    expect(ctl.state.phase, LivePhase.finished);
    expect(ctl.state.saved, isTrue);
  });

  test('cancel stops the card and discards', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(25);
    await ctl.cancel();
    expect(ctl.state.phase, LivePhase.idle);
    expect(card.stopCalls, 1);
    expect(await store.watchAll().first, isEmpty);
  });

  test('a second session starts clean', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(25);
    final a = await ctl.finish();
    card.startCalls = 0;
    await startRunning(SessionKind.checkIn);
    expect(ctl.state.rr, isEmpty);
    expect(ctl.state.record, isNull);
    card.stream(22);
    final b = await ctl.finish();
    expect(b!.id, isNot(a!.id));
    expect(b.duration.inSeconds, 22);
    expect((await store.watchAll().first).length, 2);
  });

  test('state changes are broadcast', () async {
    final phases = <LivePhase>[];
    final sub = ctl.changes.listen((s) => phases.add(s.phase));
    await startRunning(SessionKind.checkIn);
    await pumpEventQueue();
    expect(
      phases,
      containsAllInOrder([
        LivePhase.starting,
        LivePhase.waitContact,
        LivePhase.settling,
        LivePhase.running,
      ]),
    );
    await sub.cancel();
  });

  test('newSessionId is unique', () {
    final t = DateTime(2026, 11, 17);
    final ids = {for (var i = 0; i < 1000; i++) newSessionId(t)};
    expect(ids.length, 1000);
  });

  test('R peaks outside running are ignored', () async {
    await ctl.begin(SessionKind.checkIn);
    card.meas(MeasState.waitContact);
    card.event(const RPeakEvent(sampleIndex: 100, hrBpm: 60));
    card.event(const RPeakEvent(sampleIndex: 550, hrBpm: 60));
    expect(ctl.state.rr, isEmpty);
    await ctl.cancel();
  });

  test('unawaited finish while finishing returns the same future', () async {
    await startRunning(SessionKind.checkIn);
    card.stream(21);
    final f1 = ctl.finish();
    final f2 = ctl.finish();
    expect(await f1, same(await f2));
    unawaited(ctl.cancel());
  });
}
