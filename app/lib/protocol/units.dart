// Stream units -> physical units, sample rates and stream timing
// (fw/include/stream_fmt.h, tools/ble_client.html). Pure Dart.
import 'dart:typed_data';

import 'link_status.dart';
import 'stream_fmt.dart';

export 'stream_fmt.dart' show ecgHz, ppgHz;

/// Microvolts per 12-bit ADC code at the ADC input (3.3 V / 4096).
const double uvPerAdcCode = 805.7;

/// Stream units per ADC code (ECG samples are ADC codes x 8).
const int ecgStreamScale = 8;

/// ECG front-end gains as built (EV_STATE in the 7-byte form implies these).
const int defaultGainLo = 558;
const int defaultGainHi = 1098;

/// PPG: nA per coarse code, nA per fine unit, uA of LED current per DAC code.
const double naPerPpgCoarse = 0.8057;
const double naPerPpgFine = 0.08057;
const double ledUaPerCode = 3.718;

/// ECG stream sample m is the signal at m / 500 + [ecgT0Seconds] after
/// acquisition start (decimator delay; tools/ecgcard/record.py).
const double ecgT0Seconds = -0.010;

/// Seconds from acquisition start of ECG sample index [m] (also R_PEAK and
/// PULSE indices).
double ecgSeconds(int m) => m / ecgHz + ecgT0Seconds;

/// Microvolts per ECG stream unit for front-end [gain]: 805.7 / (8 * gain).
double ecgUvPerUnit(int gain) => uvPerAdcCode / (ecgStreamScale * gain);

/// The card's two ECG front-end gains.
class EcgGains {
  const EcgGains({this.lo = defaultGainLo, this.hi = defaultGainHi});

  /// From a STATE event (its gains are 558 / 1098 in the short form).
  factory EcgGains.fromState(StateEvent e) =>
      e.gainLo > 0 && e.gainHi > e.gainLo ? EcgGains(lo: e.gainLo, hi: e.gainHi) : const EcgGains();

  final int lo, hi;

  int gain(bool gainHi) => gainHi ? hi : lo;
  double uvPerUnit(bool gainHi) => ecgUvPerUnit(gain(gainHi));

  @override
  bool operator ==(Object other) => other is EcgGains && other.lo == lo && other.hi == hi;
  @override
  int get hashCode => Object.hash(lo, hi);
  @override
  String toString() => 'EcgGains($lo / $hi)';
}

/// Converts ECG blocks to microvolts, tracking the gains the card reports.
///
/// Feed it every [CardEvent] (STATE carries the gains) and, optionally, every
/// [CardStatus] (its gain_hi flag is the fallback before the first block).
/// A block's own gain_hi flag selects the gain for that block.
class EcgScaler {
  EcgGains gains = const EcgGains();
  bool _statusGainHi = false;

  void onEvent(CardEvent e) {
    if (e is StateEvent) gains = EcgGains.fromState(e);
  }

  void onStatus(CardStatus s) => _statusGainHi = s.gainHi;

  /// Gain in use as last reported by Status (before any block arrives).
  int get statusGain => gains.gain(_statusGainHi);

  double uvPerUnit(EcgBlock b) => gains.uvPerUnit(b.gainHi);

  /// The block's 10 samples in microvolts.
  Float32List toUv(EcgBlock b) {
    final k = uvPerUnit(b);
    final out = Float32List(b.samples.length);
    for (var i = 0; i < out.length; i++) {
      out[i] = b.samples[i] * k;
    }
    return out;
  }
}

/// One-shot conversion with explicit gains.
Float32List ecgBlockToUv(EcgBlock b, [EcgGains gains = const EcgGains()]) {
  final k = gains.uvPerUnit(b.gainHi);
  final out = Float32List(b.samples.length);
  for (var i = 0; i < out.length; i++) {
    out[i] = b.samples[i] * k;
  }
  return out;
}

/// Stream units for [uv] microvolts at [gain] (rounded, clamped to int16).
int ecgUnitsFromUv(double uv, int gain) => (uv / ecgUvPerUnit(gain)).round().clamp(-32768, 32767);
