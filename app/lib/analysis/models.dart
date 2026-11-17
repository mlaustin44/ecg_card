// Analysis data shapes (contract). Implementations live beside this file
// (rr.dart, hrv.dart, breathing.dart). Pure Dart.

/// One beat-to-beat interval.
class RrInterval {
  const RrInterval({required this.tSec, required this.ms, this.artifact = false});

  /// Time of the beat that ends the interval, seconds from session start.
  final double tSec;
  final int ms;

  /// Rejected by the artifact filter (kept for display, excluded from stats).
  final bool artifact;
}

class HrvStats {
  const HrvStats({
    required this.beats,
    required this.meanHr,
    required this.minHr,
    required this.maxHr,
    required this.rmssdMs,
    required this.sdnnMs,
    required this.pnn50,
  });
  final int beats;
  final double meanHr, minHr, maxHr; // bpm
  final double rmssdMs, sdnnMs;
  final double pnn50; // 0..1
}

class CoherenceResult {
  const CoherenceResult({required this.score, this.breathsPerMin});

  /// 0..1: share of HRV power in the breathing peak (HRV biofeedback "coherence").
  final double score;

  /// Breathing rate estimated from the RSA peak, if one is clear.
  final double? breathsPerMin;
}
