// Heart-rate variability summary numbers and the "calm" score. Pure Dart.
import 'dart:math' as math;

import 'models.dart';

/// Two intervals are successive beats (no artifact or gap between them) when
/// the second one ends one interval-length after the first one.
bool _adjacent(RrInterval a, RrInterval b) => ((b.tSec - a.tSec) * 1000 - b.ms).abs() <= 40;

/// Stats over the accepted intervals of [rr] (artifacts are skipped).
/// Null when fewer than 2 accepted intervals.
///
/// RMSSD and pNN50 use only pairs of intervals that are really successive
/// beats (an artifact, a skipped beat or a contact gap between them breaks
/// the pair), so a gap never shows up as a large "variability".
HrvStats? hrvStats(Iterable<RrInterval> rr) {
  final list = rr.toList();
  final ok = list.where((r) => !r.artifact).toList();
  if (ok.length < 2) return null;

  var sum = 0.0;
  var minMs = ok.first.ms, maxMs = ok.first.ms;
  for (final r in ok) {
    sum += r.ms;
    minMs = math.min(minMs, r.ms);
    maxMs = math.max(maxMs, r.ms);
  }
  final mean = sum / ok.length;
  var ss = 0.0;
  for (final r in ok) {
    ss += (r.ms - mean) * (r.ms - mean);
  }
  final sdnn = math.sqrt(ss / (ok.length - 1));

  var sumSq = 0.0;
  var pairs = 0, over50 = 0;
  for (var i = 1; i < list.length; i++) {
    final a = list[i - 1], b = list[i];
    if (a.artifact || b.artifact || !_adjacent(a, b)) continue;
    final d = (b.ms - a.ms).toDouble();
    sumSq += d * d;
    pairs++;
    if (d.abs() > 50) over50++;
  }
  final rmssd = pairs == 0 ? 0.0 : math.sqrt(sumSq / pairs);

  return HrvStats(
    beats: ok.length,
    meanHr: 60000 / mean,
    minHr: 60000 / maxMs,
    maxHr: 60000 / minMs,
    rmssdMs: rmssd,
    sdnnMs: sdnn,
    pnn50: pairs == 0 ? 0 : over50 / pairs,
  );
}

/// RMSSD (ms) thresholds between calm scores 1|2, 2|3, 3|4, 4|5.
///
/// Why these numbers: short-term (1..5 min) resting RMSSD in healthy adults
/// is typically about 19..75 ms with a middle near 40 ms (Nunan et al. 2010,
/// a review of short-term HRV norms). The bands split that range so the
/// middle score (3) covers the usual resting middle, 1 sits below the common
/// range and 5 above most of it. They are deliberately age-agnostic and only
/// a relaxation / recovery cue for comparing a person with themselves over
/// time; they say nothing about health.
const List<double> calmThresholdsMs = [15, 25, 40, 60];

/// A 1..5 "calm" score from RMSSD (ms): <15 -> 1, 15..25 -> 2, 25..40 -> 3,
/// 40..60 -> 4, >= 60 -> 5. A wellness number, not a health assessment.
int calmScore(double rmssdMs) {
  var s = 1;
  for (final t in calmThresholdsMs) {
    if (rmssdMs >= t) s++;
  }
  return s;
}

/// Friendly words for a calm score (wellness wording only).
String calmWord(int score) => switch (score) {
  <= 1 => 'Busy',
  2 => 'Steady',
  3 => 'Balanced',
  4 => 'Calm',
  _ => 'Very calm',
};
