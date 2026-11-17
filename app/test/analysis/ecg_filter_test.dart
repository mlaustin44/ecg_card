import 'dart:math' as math;

import 'package:ecg_card_app/analysis/ecg_filter.dart';
import 'package:flutter_test/flutter_test.dart';

import 'synth.dart';

double rms(List<double> v) => math.sqrt(v.map((x) => x * x).reduce((a, b) => a + b) / v.length);

/// A crude ECG: Gaussian QRS of [qrsUv] every [rrSec], at 500 Hz.
List<double> ecg(double seconds, {double qrsUv = 1200, double rrSec = 0.9}) {
  final n = (seconds * 500).round();
  return List.generate(n, (i) {
    final t = i / 500;
    final ph = t % rrSec - rrSec / 2;
    return qrsUv * math.exp(-ph * ph / (2 * 0.012 * 0.012));
  });
}

void main() {
  group('EcgDisplayFilter', () {
    test('removes baseline offset and slow wander', () {
      final f = EcgDisplayFilter();
      final x = List.generate(5000, (i) => 3000 + 800 * math.sin(2 * math.pi * 0.05 * i / 500));
      final y = f.process(x);
      expect(rms(y.sublist(2500).map((v) => v.toDouble()).toList()), lessThan(60));
    });

    test('notch removes mains (50 and 60 Hz)', () {
      for (final hz in [50, 60]) {
        final f = EcgDisplayFilter(notchHz: hz);
        final x = List.generate(5000, (i) => 500 * math.sin(2 * math.pi * hz * i / 500));
        final y = f.process(x).sublist(2500).map((v) => v.toDouble()).toList();
        expect(rms(y), lessThan(15), reason: '$hz Hz');
      }
    });

    test('keeps the QRS', () {
      final f = EcgDisplayFilter();
      final y = f.process(ecg(5));
      final peak = y.sublist(1000).reduce(math.max);
      expect(peak, greaterThan(700));
    });
  });

  test('filterZeroPhase keeps the R peak in place', () {
    final x = ecg(4);
    final y = filterZeroPhase(x);
    // Peaks at t = 0.45 + k 0.9 s -> sample 225 + 450 k.
    var best = 900;
    for (var i = 900; i < 1350; i++) {
      if (y[i] > y[best]) best = i;
    }
    expect(best, closeTo(1125, 2));
  });

  group('pickStrip', () {
    test('picks the clean 10 s and downsamples to 250 Hz', () {
      final rnd = math.Random(3);
      final clean = ecg(30);
      // 0..12 s: heavy noise and wander; 12..30 s: clean.
      final x = [
        for (var i = 0; i < clean.length; i++)
          clean[i] +
              (i < 6000 ? 300 * gauss(rnd) + 1500 * math.sin(2 * math.pi * 0.7 * i / 500) : 0.0),
      ];
      final s = pickStrip(x);
      expect(s, hasLength(2500));
      // A clean strip: little noise between beats (median absolute value).
      final sorted = s.map((v) => v.abs()).toList()..sort();
      expect(sorted[sorted.length ~/ 2], lessThan(60));
    });

    test('avoids bad samples', () {
      // 0..10 s: tall beats but marked bad (settling); 10..20 s: normal.
      final tall = ecg(10, qrsUv: 2500), normal = ecg(10);
      final x = [...tall, ...normal];
      final bad = List<bool>.generate(x.length, (i) => i < 5000);
      final s = pickStrip(x, bad: bad);
      expect(s, hasLength(2500));
      expect(s.reduce(math.max), lessThan(1500));
    });

    test('short recordings return what there is', () {
      expect(pickStrip(ecg(4)), hasLength(1000));
      expect(pickStrip(const []), isEmpty);
    });
  });
}
