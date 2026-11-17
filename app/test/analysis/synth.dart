// Synthetic RR series for analysis tests.
import 'dart:math' as math;

import 'package:ecg_card_app/analysis/models.dart';

/// Beats with RSA: RR(t) = base + amp * sin(2 pi f t) + gaussian noise (ms).
List<RrInterval> rsaSeries({
  double seconds = 120,
  double baseMs = 900,
  double ampMs = 80,
  double breathsPerMin = 6,
  double noiseMs = 0,
  int seed = 1,
}) {
  final rnd = math.Random(seed);
  final f = breathsPerMin / 60;
  final out = <RrInterval>[];
  var t = 0.0;
  while (t < seconds) {
    final ms = (baseMs + ampMs * math.sin(2 * math.pi * f * t) + noiseMs * gauss(rnd)).round();
    t += ms / 1000;
    out.add(RrInterval(tSec: t, ms: ms));
  }
  return out;
}

/// Independent random intervals around [baseMs].
List<RrInterval> randomSeries({
  double seconds = 120,
  double baseMs = 850,
  double sdMs = 50,
  int seed = 2,
}) {
  final rnd = math.Random(seed);
  final out = <RrInterval>[];
  var t = 0.0;
  while (t < seconds) {
    final ms = (baseMs + sdMs * gauss(rnd)).round();
    t += ms / 1000;
    out.add(RrInterval(tSec: t, ms: ms));
  }
  return out;
}

double gauss(math.Random r) {
  final u = 1 - r.nextDouble(), v = r.nextDouble();
  return math.sqrt(-2 * math.log(u)) * math.cos(2 * math.pi * v);
}
