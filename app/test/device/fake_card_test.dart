// FakeCard: deterministic, stepped with advance() (autoTick: false).
import 'dart:async';
import 'dart:math' as math;

import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/device/fake_card.dart';
import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';
import 'package:ecg_card_app/protocol/units.dart';
import 'package:flutter_test/flutter_test.dart';

const quick = FakeCardConfig(connectDelay: Duration.zero);

/// Records everything a card emits.
class Tape {
  Tape(CardLink c) {
    subs = [
      c.ecg.listen(ecg.add),
      c.ppg.listen(ppg.add),
      c.events.listen(events.add),
      c.status.listen(status.add),
      c.linkState.listen(links.add),
      c.log.listen(log.add),
    ];
  }
  late final List<StreamSubscription<Object?>> subs;
  final ecg = <EcgBlock>[];
  final ppg = <PpgBlock>[];
  final events = <CardEvent>[];
  final status = <CardStatus>[];
  final links = <LinkState>[];
  final log = <String>[];

  List<T> ev<T>() => events.whereType<T>().toList();
  void clear() {
    ecg.clear();
    ppg.clear();
    events.clear();
    status.clear();
    links.clear();
  }

  Future<void> cancel() async {
    for (final s in subs) {
      await s.cancel();
    }
  }
}

Future<void> flush() => Future<void>.delayed(Duration.zero);

Future<(FakeCard, Tape)> connected([FakeCardConfig cfg = quick]) async {
  final card = FakeCard(config: cfg, autoTick: false);
  final tape = Tape(card);
  await card.connect();
  card.advance(const Duration(milliseconds: 100)); // SET_TIME result
  await flush();
  return (card, tape);
}

/// Advance in steps, flushing streams in between.
Future<void> run(FakeCard c, Duration d) async {
  const step = Duration(milliseconds: 500);
  var left = d;
  while (left > Duration.zero) {
    final s = left < step ? left : step;
    c.advance(s);
    await flush();
    left -= s;
  }
}

Future<CmdResult> cmd(FakeCard c, Future<CmdResult> f) async {
  c.advance(const Duration(milliseconds: 100));
  await flush();
  return f;
}

void main() {
  test('connect: link states, initial status, clock set', () async {
    final (card, tape) = await connected();
    expect(tape.links, [LinkState.connecting, LinkState.connected]);
    expect(card.currentLinkState, LinkState.connected);
    expect(card.lastStatus!.mode, CardMode.idle);
    expect(card.lastStatus!.meas, MeasState.none);
    expect(card.lastStatus!.battPct, 82);
    expect(card.lastStatus!.timeValid, isTrue);
    expect(tape.ev<CmdResultEvent>().single.cmd, CmdId.setTime);
    expect(tape.log, isNotEmpty);
  });

  test('ECG session: wait contact, settling, running with beats', () async {
    final (card, tape) = await connected();
    tape.clear();
    expect(await cmd(card, card.start(CardMode.ecg)), CmdResult.ok);
    final st = tape.ev<StateEvent>().single;
    expect((st.mode, st.gainLo, st.gainHi), (1, 558, 1098));
    expect(tape.status.last.meas, MeasState.waitContact);
    expect(tape.status.last.noContact, isTrue);

    await run(card, const Duration(milliseconds: 1500));
    expect(card.meas, MeasState.settling);
    expect(tape.ev<LeadEvent>().single.contact, isTrue);
    expect(tape.ecg.where((b) => b.leadOff), isNotEmpty);
    expect(tape.ev<RPeakEvent>(), isEmpty);

    await run(card, const Duration(milliseconds: 2600));
    expect(card.meas, MeasState.running);
    expect(
      tape.status.map((s) => s.meas).toSet(),
      containsAll([MeasState.waitContact, MeasState.settling, MeasState.running]),
    );
    expect(tape.ecg.where((b) => b.settling), isNotEmpty);

    // blocks: contiguous 10-sample steps, one per 20 ms
    final starts = tape.ecg.map((b) => b.start).toList();
    for (var i = 1; i < starts.length; i++) {
      expect(starts[i] - starts[i - 1], 10);
    }
    expect(tape.ecg.length, closeTo(4100 / 20, 3));

    tape.clear();
    await run(card, const Duration(seconds: 60));
    // ~64 bpm for a minute
    final r = tape.ev<RPeakEvent>();
    expect(r.length, inInclusiveRange(58, 70));
    // QRS ~1.2 mV
    final sc = EcgScaler();
    var peak = 0.0;
    for (final b in tape.ecg) {
      expect(b.gainHi, isTrue);
      expect(b.leadOff || b.settling, isFalse);
      for (final v in sc.toUv(b)) {
        peak = math.max(peak, v);
      }
    }
    expect(peak, inInclusiveRange(1050, 1450));
    // R-peak index sits on the R wave of the stream
    final byStart = {for (final b in tape.ecg) b.start: b};
    final rp = r[10].sampleIndex;
    final blk = byStart[rp - rp % 10]!;
    expect(sc.toUv(blk)[rp % 10], greaterThan(900));
    // RSA: instantaneous HR swings with breathing (6/min, 64 +- 7)
    final hr = [
      for (var i = 1; i < r.length; i++) 60 * ecgHz / (r[i].sampleIndex - r[i - 1].sampleIndex),
    ];
    final mean = hr.reduce((a, b) => a + b) / hr.length;
    expect(mean, closeTo(64, 2.5));
    expect(hr.reduce(math.max) - hr.reduce(math.min), greaterThan(10));
    // card's hr_bpm is a median: smoother than the beat-to-beat rate
    expect(r.last.hrBpm, inInclusiveRange(54, 74));
    expect(tape.status.where((s) => s.hrBpm > 0), isNotEmpty);
    // status every 5 s
    expect(tape.status.length, greaterThanOrEqualTo(12));
    expect(tape.ppg, isEmpty);

    tape.clear();
    expect(await cmd(card, card.stop()), CmdResult.ok);
    expect(tape.ev<StateEvent>().single.mode, 0);
    expect(card.lastStatus!.mode, CardMode.idle);
    await run(card, const Duration(seconds: 1));
    expect(tape.ecg.length, lessThanOrEqualTo(5));
  });

  test('card rules: busy, locked settings, bad args, USB refusal', () async {
    final (card, _) = await connected();
    expect(await cmd(card, card.start(CardMode.idle)), CmdResult.badArg);
    expect(await cmd(card, card.setSetting(SettingItem.gain, 3)), CmdResult.badArg);
    expect(await cmd(card, card.setSetting(SettingItem.gain, 2)), CmdResult.ok);
    expect(card.lastStatus!.gain, 2);
    expect(await cmd(card, card.start(CardMode.both)), CmdResult.ok);
    expect(await cmd(card, card.start(CardMode.ecg)), CmdResult.busy);
    expect(await cmd(card, card.setSetting(SettingItem.notch, 1)), CmdResult.busy);
    expect(await cmd(card, card.setSetting(SettingItem.sweep, 2)), CmdResult.ok);
    expect(await cmd(card, card.stop()), CmdResult.ok);
    expect(await cmd(card, card.setTime(DateTime.utc(2023))), CmdResult.badArg);
    card.usbPower = true;
    await flush();
    expect(card.lastStatus!.usb, isTrue);
    expect(await cmd(card, card.start(CardMode.ecg)), CmdResult.refusedUsb);
    expect(await cmd(card, card.start(CardMode.ppg)), CmdResult.ok);
  });

  test('command without a result times out after 3 s', () async {
    final (card, _) = await connected();
    card.dropCommands(1);
    final f = card.start(CardMode.ecg);
    var done = false;
    unawaited(f.then((_) => done = true));
    await run(card, const Duration(milliseconds: 2900));
    expect(done, isFalse);
    await run(card, const Duration(milliseconds: 200));
    expect(await f, CmdResult.timeout);
    expect(card.mode, CardMode.idle);
  });

  test('lifting the thumbs: lead off, no beats, then settling again', () async {
    final (card, tape) = await connected();
    await cmd(card, card.start(CardMode.ecg));
    await run(card, const Duration(seconds: 8));
    expect(card.meas, MeasState.running);
    tape.clear();
    card.liftThumbs(const Duration(seconds: 3));
    await run(card, const Duration(milliseconds: 100));
    expect(card.meas, MeasState.waitContact);
    expect(tape.ev<LeadEvent>().single.contact, isFalse);
    expect(card.lastStatus!.noContact, isTrue);
    await run(card, const Duration(milliseconds: 2800));
    final during = tape.ev<RPeakEvent>().where((e) => e.sampleIndex > 8.4 * ecgHz);
    expect(during, isEmpty);
    expect(tape.ecg.last.leadOff, isTrue);
    await run(card, const Duration(milliseconds: 300));
    expect(card.meas, MeasState.settling);
    await run(card, const Duration(milliseconds: 1600));
    expect(card.meas, MeasState.running);
    await run(card, const Duration(seconds: 3));
    expect(tape.ev<RPeakEvent>().length, greaterThanOrEqualTo(2));
    expect(tape.ev<LeadEvent>().map((e) => e.contact), [false, true]);
  });

  test('PPG mode: PPG blocks and PULSE events, no ECG', () async {
    final (card, tape) = await connected();
    await cmd(card, card.start(CardMode.ppg));
    await run(card, const Duration(seconds: 15));
    expect(tape.ecg, isEmpty);
    expect(tape.ppg.length, closeTo(15000 / 20, 4));
    final starts = tape.ppg.map((b) => b.start).toList();
    for (var i = 1; i < starts.length; i++) {
      expect(starts[i] - starts[i - 1], 2);
    }
    final p = tape.ev<PulseEvent>();
    expect(p.length, greaterThan(8));
    expect(p.first.patMs, 0); // no ECG in this mode
    expect(p.first.pulseBpm, inInclusiveRange(50, 80));
    expect(tape.ev<RPeakEvent>(), isEmpty);
  });

  test('both: PULSE carries the PAT', () async {
    final (card, tape) = await connected();
    await cmd(card, card.start(CardMode.both));
    await run(card, const Duration(seconds: 10));
    expect(tape.ecg, isNotEmpty);
    expect(tape.ppg, isNotEmpty);
    expect(tape.ev<PulseEvent>().first.patMs, greaterThan(150));
  });

  test('link loss: disconnected, connecting, connected; data resumes with a gap', () async {
    final (card, tape) = await connected();
    await cmd(card, card.start(CardMode.ecg));
    await run(card, const Duration(seconds: 6));
    tape.clear();
    card.simulateLinkLoss(const Duration(seconds: 2));
    await run(card, const Duration(seconds: 3));
    expect(tape.links, [LinkState.disconnected, LinkState.connecting, LinkState.connected]);
    final starts = tape.ecg.map((b) => b.start).toList();
    expect(starts.first, greaterThan(6 * ecgHz + 2 * ecgHz - 50));
    expect(card.mode, CardMode.ecg); // the card kept measuring
  });

  test('disconnect: commands time out at once, the session stays on the card', () async {
    final (card, tape) = await connected();
    await cmd(card, card.start(CardMode.ecg));
    await card.disconnect();
    await flush();
    expect(tape.links.last, LinkState.disconnected);
    expect(await card.stop(), CmdResult.timeout);
    await card.connect();
    expect(await cmd(card, card.start(CardMode.ecg)), CmdResult.busy);
  });

  test('deterministic for a seed', () async {
    Future<List<int>> samples() async {
      final (card, tape) = await connected();
      await cmd(card, card.start(CardMode.ecg));
      await run(card, const Duration(seconds: 6));
      return [for (final b in tape.ecg.take(250)) ...b.samples];
    }

    expect(await samples(), await samples());
  });

  test('battery and heart settings are scriptable', () async {
    final (card, tape) = await connected();
    card.batteryPct = 9;
    await flush();
    expect(tape.status.last.battPct, 9);
    expect(tape.status.last.battLow, isTrue);
    card.setHeart(hrBpm: 90, rsaBpm: 0);
    await cmd(card, card.start(CardMode.ecg));
    await run(card, const Duration(seconds: 20));
    expect(tape.ev<RPeakEvent>().last.hrBpm, closeTo(90, 3));
  });

  group('replay', () {
    String session({bool withPeaks = true}) {
      final b = StringBuffer(
        '# ECG Card session dump\nsaved: 2026-11-17\n\n## Timeline (3 events)\n',
      );
      const i0 = 5000;
      for (var k = 0; k < 20; k++) {
        if (withPeaks) {
          b.writeln(
            '12:00:00.000 evt beat         ${k}s R-peak @${i0 + 100 + k * 400} (t=1 s) HR 75 bpm',
          );
        }
      }
      b.writeln(
        '\n## ECG index,uV,flags: last 120 s, 8000 of 8000 present (missing indices are gaps)',
      );
      for (var m = 0; m < 8000; m++) {
        if (m == 3000) continue; // a gap
        final d = (m - 100) % 400;
        final uv =
            900 * math.exp(-0.5 * math.pow(math.min(d, 400 - d) / 5, 2)) + 50 * math.sin(m / 200);
        b.writeln('${i0 + m},${uv.round()},1');
      }
      return b.toString();
    }

    test('parses samples and timeline R-peaks', () {
      final rec = Recording.parse(session());
      expect(rec.length, 8000);
      expect(rec.beats.first, 100);
      expect(rec.beats.length, 20);
      expect(rec.uv[100], closeTo(900, 60));
    });

    test('detects R-peaks when the file has none', () {
      final rec = Recording.parse(session(withPeaks: false));
      expect(rec.beats.length, inInclusiveRange(19, 20));
      for (final p in rec.beats) {
        expect((p - 100) % 400, anyOf(lessThan(3), greaterThan(397)));
      }
    });

    test('rejects text without an ECG section', () {
      expect(() => Recording.parse('# nothing\n'), throwsFormatException);
    });

    test('a replaying card streams the recording with its beats', () async {
      final card = FakeCard.replay(session(), config: quick, autoTick: false);
      final tape = Tape(card);
      await card.connect();
      await cmd(card, card.start(CardMode.ecg));
      await run(card, const Duration(seconds: 30));
      final r = tape.ev<RPeakEvent>();
      expect(r.length, greaterThan(20));
      expect(r.last.hrBpm, 75);
      // loops past the 16 s recording
      expect(r.last.sampleIndex, greaterThan(8000));
      final sc = EcgScaler();
      final b = tape.ecg.firstWhere((b) => !b.settling && !b.leadOff && b.start == 2100);
      expect(sc.toUv(b)[0], closeTo(900, 60));
    });
  });

  testWidgets('autoTick: the timer drives the card under tester.pump', (tester) async {
    final card = FakeCard(); // default config, periodic timer
    final tape = Tape(card);
    final connecting = card.connect();
    await tester.pump(const Duration(milliseconds: 300));
    await connecting;
    final started = card.start(CardMode.ecg);
    await tester.pump(const Duration(milliseconds: 100));
    expect(await started, CmdResult.ok);
    for (var i = 0; i < 10; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    expect(tape.ecg.length, closeTo(10100 / 20, 3));
    expect(tape.ev<RPeakEvent>().length, greaterThan(4));
    await card.dispose(); // no timer left pending
  });

  test('FakeScanner finds the fake card and links to one instance per id', () async {
    final s = FakeScanner(scanDelay: const Duration(milliseconds: 5), config: quick);
    final lists = await s.scan().take(2).toList();
    expect(lists.first, isEmpty);
    expect(lists.last.single.name, startsWith('ECG Card'));
    final a = s.linkTo(lists.last.single), b = s.linkTo(lists.last.single);
    expect(identical(a, b), isTrue);
    expect(a, isA<FakeCard>());
    await s.stop();
  });
}
