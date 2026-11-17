import 'dart:math' as math;

import 'package:ecg_card_app/analysis/hrv.dart';
import 'package:ecg_card_app/analysis/models.dart';
import 'package:flutter_test/flutter_test.dart';

/// Successive beats from a list of ms values.
List<RrInterval> chain(List<int> ms, {double t0 = 0}) {
  var t = t0;
  return [for (final m in ms) RrInterval(tSec: t += m / 1000, ms: m)];
}

void main() {
  group('hrvStats', () {
    test('null with fewer than 2 accepted', () {
      expect(hrvStats(chain([900])), isNull);
    });

    test('known values', () {
      final s = hrvStats(chain([800, 900, 800, 900, 1000]))!;
      expect(s.beats, 5);
      expect(s.meanHr, closeTo(60000 / 880, 1e-9));
      expect(s.minHr, closeTo(60, 1e-9));
      expect(s.maxHr, closeTo(75, 1e-9));
      // Successive differences: 100, -100, 100, 100 -> RMSSD 100.
      expect(s.rmssdMs, closeTo(100, 1e-9));
      final mean = 880.0;
      final sd = math.sqrt(
        [800, 900, 800, 900, 1000].map((v) => (v - mean) * (v - mean)).reduce((a, b) => a + b) / 4,
      );
      expect(s.sdnnMs, closeTo(sd, 1e-9));
      expect(s.pnn50, 1.0);
    });

    test('artifacts and gaps break successive pairs', () {
      final a = chain([900, 920]);
      final gap = chain([700, 720], t0: a.last.tSec + 30); // after a contact gap
      final art = RrInterval(tSec: gap.last.tSec + 0.3, ms: 300, artifact: true);
      final after = chain([710], t0: art.tSec);
      final s = hrvStats([...a, ...gap, art, ...after])!;
      expect(s.beats, 5);
      // Pairs: (900,920) and (700,720) only -> RMSSD 20.
      expect(s.rmssdMs, closeTo(20, 1e-9));
      expect(s.pnn50, 0);
    });
  });

  test('calmScore bands', () {
    expect(calmScore(5), 1);
    expect(calmScore(14.9), 1);
    expect(calmScore(15), 2);
    expect(calmScore(30), 3);
    expect(calmScore(45), 4);
    expect(calmScore(60), 5);
    expect(calmScore(150), 5);
    for (var s = 1; s <= 5; s++) {
      expect(calmWord(s), isNotEmpty);
    }
  });
}
