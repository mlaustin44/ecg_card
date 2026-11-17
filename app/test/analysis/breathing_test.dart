import 'dart:math' as math;
import 'dart:typed_data';

import 'package:ecg_card_app/analysis/breathing.dart';
import 'package:ecg_card_app/analysis/models.dart';
import 'package:flutter_test/flutter_test.dart';

import 'synth.dart';

void main() {
  test('fft of a sinusoid', () {
    final re = Float64List(64), im = Float64List(64);
    for (var i = 0; i < 64; i++) {
      re[i] = math.cos(2 * math.pi * 5 * i / 64);
    }
    fft(re, im);
    expect(re[5], closeTo(32, 1e-9));
    expect(re[59], closeTo(32, 1e-9));
    expect(re[4].abs() + im[4].abs(), lessThan(1e-9));
  });

  test('RSA at 6 / min: high score, ~6 breaths / min', () {
    final c = coherence(rsaSeries(breathsPerMin: 6, ampMs: 60, noiseMs: 10))!;
    expect(c.score, greaterThan(0.9));
    expect(c.breathsPerMin, closeTo(6, 0.3));
  });

  test('other paced rates are found', () {
    for (final bpm in [4.5, 5.5, 7.0, 10.0]) {
      final c = coherence(rsaSeries(breathsPerMin: bpm, ampMs: 50, noiseMs: 8))!;
      expect(c.breathsPerMin, closeTo(bpm, 0.35), reason: '$bpm / min');
      expect(c.score, greaterThan(0.8), reason: '$bpm / min');
    }
  });

  test('random RR: low score, no breathing rate', () {
    for (var seed = 0; seed < 10; seed++) {
      final c = coherence(randomSeries(seed: seed))!;
      expect(c.score, lessThan(0.3), reason: 'seed $seed');
      expect(c.breathsPerMin, isNull, reason: 'seed $seed');
    }
  });

  test('uses only the last window and ignores artifacts', () {
    final calm = rsaSeries(seconds: 200, breathsPerMin: 6, ampMs: 60, noiseMs: 5);
    // Sprinkle artifacts: they must not hurt the score.
    final withArt = [
      for (var i = 0; i < calm.length; i++) ...[
        calm[i],
        if (i % 17 == 0) RrInterval(tSec: calm[i].tSec + 0.01, ms: 250, artifact: true),
      ],
    ];
    expect(coherence(withArt)!.score, greaterThan(0.9));
  });

  test('not enough data -> null', () {
    expect(coherence(rsaSeries(seconds: 20)), isNull);
    expect(coherence(const []), isNull);
  });
}
