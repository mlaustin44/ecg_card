// ECG display filtering (live trace and saved strip) and strip selection.
// Pure Dart. The chain mirrors tools/ble_client.html ("0.5-40 Hz + notch").
import 'dart:math' as math;
import 'dart:typed_data';

// Stream units -> microvolts: see protocol/units.dart (EcgScaler).

/// First-order high-pass, y = a (y + x - x_prev).
class HighPass1 {
  HighPass1(double fc, double fs) : _a = 1 / (1 + 2 * math.pi * fc / fs);
  final double _a;
  bool _has = false;
  double _x = 0, _y = 0;

  void reset() => _has = false;

  double step(double x) {
    if (!_has) {
      _has = true;
      _x = x;
      _y = 0;
      return 0;
    }
    _y = _a * (_y + x - _x);
    _x = x;
    return _y;
  }
}

/// RBJ-cookbook biquad (low-pass or notch), direct form I.
class Biquad {
  Biquad.lowPass(double f0, double fs, [double q = math.sqrt1_2]) : this._(false, f0, fs, q);
  Biquad.notch(double f0, double fs, [double q = 30]) : this._(true, f0, fs, q);

  Biquad._(bool notch, double f0, double fs, double q) {
    final w = 2 * math.pi * f0 / fs, c = math.cos(w), al = math.sin(w) / (2 * q);
    final double b0, b1, b2;
    if (notch) {
      b0 = 1;
      b1 = -2 * c;
      b2 = 1;
    } else {
      b0 = (1 - c) / 2;
      b1 = 1 - c;
      b2 = (1 - c) / 2;
    }
    final a0 = 1 + al;
    _b0 = b0 / a0;
    _b1 = b1 / a0;
    _b2 = b2 / a0;
    _a1 = -2 * c / a0;
    _a2 = (1 - al) / a0;
  }

  late final double _b0, _b1, _b2, _a1, _a2;
  bool _has = false;
  double _x1 = 0, _x2 = 0, _y1 = 0, _y2 = 0;

  void reset() => _has = false;

  double step(double x) {
    if (!_has) {
      // Start in steady state for a constant input.
      _has = true;
      _x1 = _x2 = x;
      _y1 = _y2 = x * (_b0 + _b1 + _b2) / (1 + _a1 + _a2);
    }
    final y = _b0 * x + _b1 * _x1 + _b2 * _x2 - _a1 * _y1 - _a2 * _y2;
    _x2 = _x1;
    _x1 = x;
    _y2 = _y1;
    _y1 = y;
    return y;
  }
}

/// Causal display chain for microvolts at [fs]: 0.5 Hz high-pass (baseline
/// wander), mains notch (50 or 60 Hz, Q 30), 40 Hz low-pass. Call [reset] at
/// a gap in the samples.
class EcgDisplayFilter {
  EcgDisplayFilter({int notchHz = 50, this.fs = 500}) : _notchHz = notchHz == 60 ? 60 : 50 {
    _build();
  }

  final double fs;
  int _notchHz;
  late HighPass1 _hp;
  late Biquad _nt, _lp;

  int get notchHz => _notchHz;

  /// Switch the mains frequency (keeps nothing of the old state).
  set notchHz(int hz) {
    final v = hz == 60 ? 60 : 50;
    if (v == _notchHz) return;
    _notchHz = v;
    _build();
  }

  void _build() {
    _hp = HighPass1(0.5, fs);
    _nt = Biquad.notch(_notchHz.toDouble(), fs);
    _lp = Biquad.lowPass(40, fs);
  }

  void reset() {
    _hp.reset();
    _nt.reset();
    _lp.reset();
  }

  double step(double uv) => _lp.step(_nt.step(_hp.step(uv)));

  /// Filters a chunk into a new Float32List.
  Float32List process(List<double> uv) {
    final out = Float32List(uv.length);
    for (var i = 0; i < uv.length; i++) {
      out[i] = step(uv[i]);
    }
    return out;
  }
}

/// Zero-phase version of the display chain (forward, then backward over the
/// result) for a saved stretch: no phase shift, so the strip's waves keep
/// their shape. Starts from the first sample's level to avoid a start-up step.
Float64List filterZeroPhase(List<double> uv, {int notchHz = 50, double fs = 500}) {
  final n = uv.length;
  final out = Float64List(n);
  if (n == 0) return out;
  final f = EcgDisplayFilter(notchHz: notchHz, fs: fs);
  // Pre-roll half a second of the mirrored start so the high-pass has settled.
  final pre = math.min(n - 1, (fs / 2).round());
  for (var i = pre; i >= 1; i--) {
    f.step(2 * uv[0] - uv[i]);
  }
  for (var i = 0; i < n; i++) {
    out[i] = f.step(uv[i]);
  }
  final b = EcgDisplayFilter(notchHz: notchHz, fs: fs);
  final preB = math.min(n - 1, (fs / 2).round());
  for (var i = n - 1 - preB; i < n - 1; i++) {
    b.step(2 * out[n - 1] - out[i]);
  }
  for (var i = n - 1; i >= 0; i--) {
    out[i] = b.step(out[i]);
  }
  return out;
}

/// Chooses the cleanest [seconds]-long stretch of raw microvolts [uv]
/// (sampled at [hz]) for the saved strip and returns it display-filtered
/// (zero-phase) and downsampled to [outHz] (pairwise mean for 500 -> 250).
///
/// [bad] marks samples that must not be shown (settling, lead off, right
/// after a gap). Candidates are scored, lower is cleaner:
///  * baseline wobble: variance of the medians of 200 ms segments (a median
///    ignores the narrow QRS, so it tracks the baseline),
///  * noise: per segment, the median absolute second difference (sample to
///    sample jitter; the median ignores the QRS), and
///  * a large penalty for every bad or clipped (|uv| > 4 mV) sample.
/// Windows are tried every 0.5 s. Shorter recordings return what there is.
List<double> pickStrip(
  List<double> uv, {
  List<bool>? bad,
  int hz = 500,
  double seconds = 10,
  int outHz = 250,
  int notchHz = 50,
}) {
  if (uv.isEmpty) return const [];
  final filtered = filterZeroPhase(uv, notchHz: notchHz, fs: hz.toDouble());
  final win = math.min(uv.length, (seconds * hz).round());
  final step = math.max(1, hz ~/ 2);
  final seg = math.max(1, hz ~/ 5);

  // Per-segment median and jitter of the filtered signal.
  final nSeg = filtered.length ~/ seg;
  final med = Float64List(nSeg), jit = Float64List(nSeg), badSeg = Float64List(nSeg);
  for (var s = 0; s < nSeg; s++) {
    final a = s * seg;
    final part = List<double>.generate(seg, (i) => filtered[a + i])..sort();
    med[s] = part[seg ~/ 2];
    // Jitter: median absolute second difference (robust to the QRS).
    final d = <double>[];
    for (var i = a + 2; i < a + seg; i++) {
      d.add((filtered[i] - 2 * filtered[i - 1] + filtered[i - 2]).abs());
    }
    d.sort();
    jit[s] = d.isEmpty ? 0 : d[d.length ~/ 2];
    var nb = 0;
    for (var i = a; i < a + seg; i++) {
      if ((bad != null && i < bad.length && bad[i]) ||
          uv[i].abs() > 4000 ||
          filtered[i].abs() > 4000) {
        nb++;
      }
    }
    badSeg[s] = nb.toDouble();
  }

  var bestStart = 0;
  var bestScore = double.infinity;
  for (var start = 0; start + win <= uv.length; start += step) {
    final s0 = start ~/ seg, s1 = math.min(nSeg, (start + win) ~/ seg);
    double score;
    if (s1 - s0 < 2) {
      score = 0;
    } else {
      var m = 0.0, j = 0.0, b = 0.0;
      for (var s = s0; s < s1; s++) {
        m += med[s];
        j += jit[s];
        b += badSeg[s];
      }
      final k = s1 - s0;
      m /= k;
      var v = 0.0;
      for (var s = s0; s < s1; s++) {
        v += (med[s] - m) * (med[s] - m);
      }
      v /= k;
      score = v + 4 * (j / k) * (j / k) + 1e6 * b;
    }
    if (score < bestScore) {
      bestScore = score;
      bestStart = start;
    }
  }
  // Downsample.
  final ratio = math.max(1, (hz / outHz).round());
  final out = <double>[];
  for (var i = bestStart; i + ratio <= bestStart + win; i += ratio) {
    var acc = 0.0;
    for (var k = 0; k < ratio; k++) {
      acc += filtered[i + k];
    }
    out.add(acc / ratio);
  }
  return out;
}
