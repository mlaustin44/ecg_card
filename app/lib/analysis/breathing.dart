// Breathing / HRV-biofeedback coherence from an RR series. Pure Dart.
//
// Method (in the style of the HeartMath coherence ratio):
//  1. take the accepted intervals of the last [windowSec] seconds,
//  2. resample the tachogram (RR in ms against beat time) at 4 Hz by linear
//     interpolation (artifacts are bridged by the interpolation),
//  3. remove the linear trend, apply a Hann window, zero-pad, FFT,
//  4. find the largest peak in 0.04..0.26 Hz (2.4..15.6 breaths / min),
//  5. ratio = power within +-0.015 Hz of the peak / power in 0.0033..0.4 Hz.
// The ratio is already 0..1; it is mapped linearly so that unpaced, random
// variability (ratio ~0.1..0.3 on synthetic white RR noise) reads near 0 and
// a clean resonance-breathing pattern (ratio >= 0.8: a sinusoidal RSA with up
// to ~25 ms of beat noise) reads 1. The breathing rate is the peak frequency
// x 60 when the peak holds at least [clearPeakRatio] of the power.
import 'dart:math' as math;
import 'dart:typed_data';

import 'models.dart';

const double _fs = 4; // resampling rate, Hz
const double _bandLo = 0.04, _bandHi = 0.26; // breathing peak search band
const double _totLo = 0.0033, _totHi = 0.4; // total power band
const double _peakHalfWidth = 0.015; // Hz

/// Ratio -> score mapping (see the file comment).
const double ratioAtZero = 0.2, ratioAtOne = 0.8;

/// A peak counts as a breathing rate when it holds this share of the power.
const double clearPeakRatio = 0.4;

/// Coherence over the last [windowSec] seconds of [rr]. Null when there is
/// not enough data yet (less than [minSec] seconds or fewer than 12 accepted
/// beats in the window).
CoherenceResult? coherence(List<RrInterval> rr, {double windowSec = 64, double minSec = 30}) {
  final raw = coherenceRatio(rr, windowSec: windowSec, minSec: minSec);
  if (raw == null) return null;
  final score = ((raw.ratio - ratioAtZero) / (ratioAtOne - ratioAtZero)).clamp(0.0, 1.0);
  return CoherenceResult(
    score: score,
    breathsPerMin: raw.ratio >= clearPeakRatio ? raw.peakHz * 60 : null,
  );
}

/// The unmapped peak ratio (0..1) and peak frequency, or null.
({double ratio, double peakHz})? coherenceRatio(
  List<RrInterval> rr, {
  double windowSec = 64,
  double minSec = 30,
}) {
  final ok = rr.where((r) => !r.artifact).toList();
  if (ok.length < 12) return null;
  final tEnd = ok.last.tSec;
  final w = ok.where((r) => r.tSec >= tEnd - windowSec).toList();
  if (w.length < 12) return null;
  final t0 = w.first.tSec;
  final span = tEnd - t0;
  if (span < minSec) return null;

  // 1. Resample at 4 Hz.
  final n = (span * _fs).floor() + 1;
  final y = Float64List(n);
  var j = 0;
  for (var i = 0; i < n; i++) {
    final t = t0 + i / _fs;
    while (j < w.length - 2 && w[j + 1].tSec < t) {
      j++;
    }
    final a = w[j], b = w[j + 1];
    final dt = b.tSec - a.tSec;
    final f = dt <= 0 ? 0.0 : ((t - a.tSec) / dt).clamp(0.0, 1.0);
    y[i] = a.ms + (b.ms - a.ms) * f;
  }

  // 2. Linear detrend.
  _detrend(y);

  // 3. Hann window, zero-pad to a power of two (at least 512 points: 4 Hz /
  //    512 = 0.0078 Hz bins), FFT.
  var size = 512;
  while (size < n) {
    size <<= 1;
  }
  final re = Float64List(size), im = Float64List(size);
  for (var i = 0; i < n; i++) {
    final hann = n == 1 ? 1.0 : 0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1));
    re[i] = y[i] * hann;
  }
  fft(re, im);
  final df = _fs / size;
  double p(int k) => re[k] * re[k] + im[k] * im[k];

  // 4. Peak in the breathing band.
  final kLo = (_bandLo / df).ceil(), kHi = (_bandHi / df).floor();
  var kPk = kLo;
  for (var k = kLo; k <= kHi; k++) {
    if (p(k) > p(kPk)) kPk = k;
  }

  // 5. Peak power / total power.
  final half = (_peakHalfWidth / df).round();
  final tLo = (_totLo / df).ceil(), tHi = (_totHi / df).floor();
  var peak = 0.0, total = 0.0;
  for (var k = tLo; k <= tHi; k++) {
    final v = p(k);
    total += v;
    if ((k - kPk).abs() <= half) peak += v;
  }
  if (total <= 0) return (ratio: 0.0, peakHz: kPk * df);

  // Parabolic interpolation of the peak frequency.
  var kf = kPk.toDouble();
  if (kPk > 0 && kPk < size ~/ 2) {
    final a = p(kPk - 1), b = p(kPk), c = p(kPk + 1);
    final d = a - 2 * b + c;
    if (d != 0) kf += 0.5 * (a - c) / d;
  }
  return (ratio: peak / total, peakHz: kf * df);
}

void _detrend(Float64List y) {
  final n = y.length;
  if (n < 2) {
    if (n == 1) y[0] = 0;
    return;
  }
  final xm = (n - 1) / 2;
  var ym = 0.0;
  for (final v in y) {
    ym += v;
  }
  ym /= n;
  var sxy = 0.0, sxx = 0.0;
  for (var i = 0; i < n; i++) {
    sxy += (i - xm) * (y[i] - ym);
    sxx += (i - xm) * (i - xm);
  }
  final slope = sxy / sxx;
  for (var i = 0; i < n; i++) {
    y[i] -= ym + slope * (i - xm);
  }
}

/// In-place iterative radix-2 FFT. [re] and [im] have the same power-of-two
/// length.
void fft(Float64List re, Float64List im) {
  final n = re.length;
  assert(n == im.length && n > 0 && (n & (n - 1)) == 0, 'power-of-two length');
  // Bit reversal.
  for (var i = 1, j = 0; i < n; i++) {
    var bit = n >> 1;
    for (; j & bit != 0; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      final tr = re[i], ti = im[i];
      re[i] = re[j];
      im[i] = im[j];
      re[j] = tr;
      im[j] = ti;
    }
  }
  for (var len = 2; len <= n; len <<= 1) {
    final ang = -2 * math.pi / len;
    final wr = math.cos(ang), wi = math.sin(ang);
    for (var i = 0; i < n; i += len) {
      var cr = 1.0, ci = 0.0;
      for (var k = 0; k < len ~/ 2; k++) {
        final a = i + k, b = a + len ~/ 2;
        final xr = re[b] * cr - im[b] * ci;
        final xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
        final nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}
