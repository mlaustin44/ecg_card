import 'package:ecg_card_app/analysis/rr.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('RrTracker', () {
    test('intervals from 500 Hz indices, t from the first index', () {
      final t = RrTracker()..setOrigin(1000);
      expect(t.add(1500), isNull); // first beat starts the chain
      final r = t.add(1950)!;
      expect(r.ms, 900);
      expect(r.tSec, closeTo(1.9, 1e-9));
      expect(r.artifact, isFalse);
      expect(t.intervals, hasLength(1));
    });

    test('range filter 300..2000 ms', () {
      final t = RrTracker();
      t.add(0);
      expect(t.add(100)!.artifact, isTrue); // 200 ms
      expect(t.add(1200)!.artifact, isTrue); // 2200 ms
      expect(t.add(1650)!.artifact, isFalse); // 900 ms
    });

    test('25 % from the median of recent accepted is an artifact', () {
      final t = RrTracker();
      var i = 0;
      t.add(i);
      for (var k = 0; k < 9; k++) {
        i += 450; // 900 ms
        t.add(i);
      }
      i += 300; // 600 ms: 33 % short (extra beat)
      expect(t.add(i)!.artifact, isTrue);
      i += 450;
      expect(t.add(i)!.artifact, isFalse);
      i += 900; // missed beat, 1800 ms
      expect(t.add(i)!.artifact, isTrue);
      expect(t.acceptedCount, 10);
    });

    test('recovers when the level really changes', () {
      final t = RrTracker();
      var i = 0;
      t.add(i);
      for (var k = 0; k < 9; k++) {
        i += 500; // 1000 ms
        t.add(i);
      }
      final flags = <bool>[];
      for (var k = 0; k < 8; k++) {
        i += 300; // 600 ms, sustained
        flags.add(t.add(i)!.artifact);
      }
      expect(flags.take(3), everyElement(isTrue));
      expect(flags.skip(4), everyElement(isFalse));
    });

    test('reset: no interval spans a gap', () {
      final t = RrTracker();
      t.add(0);
      t.add(450);
      t.reset();
      expect(t.add(5000), isNull);
      final r = t.add(5450)!;
      expect(r.ms, 900);
      expect(t.intervals, hasLength(2));
    });

    test('index restart continues the clock and starts a new chain', () {
      final t = RrTracker();
      t.add(10000);
      t.add(10450);
      expect(t.add(100), isNull);
      final r = t.add(550)!;
      expect(r.tSec, greaterThan(0.9));
    });

    test('fastBpm is 60000 / mean of the last 2 accepted', () {
      final t = RrTracker();
      t.add(0);
      t.add(500); // 1000 ms
      expect(t.fastBpm, closeTo(60, 1e-9));
      t.add(900); // 800 ms
      expect(t.fastBpm, closeTo(60000 / 900, 1e-9));
      t.add(1000); // 200 ms artifact: ignored
      expect(t.fastBpm, closeTo(60000 / 900, 1e-9));
    });

    test('rrFromIndices', () {
      final rr = rrFromIndices([0, 400, 800, 1200]);
      expect(rr.map((r) => r.ms), [800, 800, 800]);
    });
  });
}
