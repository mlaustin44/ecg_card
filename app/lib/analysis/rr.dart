// Beat-to-beat (RR) series from R_PEAK sample indices or PPG pulse feet.
// Pure Dart.
import 'models.dart';

/// Builds an RR series from beat sample indices (500 Hz by default).
///
/// * The first beat after construction or [reset] only starts the chain; each
///   later beat closes one interval, so an interval never spans a contact gap
///   or an acquisition restart (call [reset] when contact is lost).
/// * Artifact filter: an interval outside [minMs]..[maxMs], or more than
///   [maxDeviation] (25 %) away from the median of the last [medianOf] accepted
///   intervals, is kept but flagged [RrInterval.artifact]. If [recoverAfter]
///   flagged-by-median intervals in a row agree with each other, the median
///   history is cleared (the heart rate really changed, or the history started
///   on a bad beat) so the filter cannot lock out a genuine new rhythm level.
/// * [fastBpm] = 60000 / mean of the last 2 accepted intervals. This mirrors
///   the card's live view: 2 beats keep about 80 % of the breathing (RSA) swing.
class RrTracker {
  RrTracker({
    this.hz = 500,
    this.minMs = 300,
    this.maxMs = 2000,
    this.maxDeviation = 0.25,
    this.medianOf = 9,
    this.recoverAfter = 4,
  });

  final int hz;
  final int minMs, maxMs;
  final double maxDeviation;
  final int medianOf;
  final int recoverAfter;

  final List<RrInterval> _all = [];
  final List<int> _recent = []; // last accepted intervals, ms
  final List<int> _suspects = []; // consecutive median rejections, ms

  int? _originIndex; // sample index of t = 0
  int? _lastIndex; // previous beat (null after reset)
  double _tOffsetSec = 0; // added after an index restart
  double _lastTSec = 0;

  /// Every interval so far (artifacts included), oldest first.
  List<RrInterval> get intervals => List.unmodifiable(_all);

  /// Accepted intervals only.
  Iterable<RrInterval> get accepted => _all.where((r) => !r.artifact);

  int get acceptedCount => _all.where((r) => !r.artifact).length;

  /// Sets the sample index that is t = 0 (e.g. the first ECG sample of the
  /// session). Without it, the first beat is t = 0.
  void setOrigin(int sampleIndex) => _originIndex ??= sampleIndex;

  /// Forget the previous beat: the next beat starts a new chain.
  void reset() {
    _lastIndex = null;
    _suspects.clear();
  }

  /// Clears everything (a new session).
  void clear() {
    _all.clear();
    _recent.clear();
    _suspects.clear();
    _originIndex = null;
    _lastIndex = null;
    _tOffsetSec = 0;
    _lastTSec = 0;
  }

  /// A beat at [sampleIndex]. Returns the interval it closes, or null for the
  /// first beat of a chain (or a repeated / out-of-order index).
  RrInterval? add(int sampleIndex) {
    _originIndex ??= sampleIndex;
    final last = _lastIndex;
    if (last != null && sampleIndex == last) return null;
    if (last != null && sampleIndex < last) {
      // Acquisition restarted (indices start again): continue the clock from
      // the last beat and start a new chain.
      _tOffsetSec = _lastTSec - (sampleIndex - _originIndex!) / hz;
      _lastIndex = sampleIndex;
      _suspects.clear();
      return null;
    }
    final t = _tOffsetSec + (sampleIndex - _originIndex!) / hz;
    _lastTSec = t;
    _lastIndex = sampleIndex;
    if (last == null) return null;

    final ms = ((sampleIndex - last) * 1000 / hz).round();
    final artifact = _isArtifact(ms);
    final r = RrInterval(tSec: t, ms: ms, artifact: artifact);
    _all.add(r);
    if (!artifact) _remember(ms);
    return r;
  }

  bool _isArtifact(int ms) {
    if (ms < minMs || ms > maxMs) return true;
    final med = medianMs;
    if (med == null || _recent.length < 3) {
      _suspects.clear();
      return false;
    }
    if ((ms - med).abs() <= maxDeviation * med) {
      _suspects.clear();
      return false;
    }
    // Deviates from the history. Several in a row that agree with each other
    // mean the level moved: restart the history from them.
    _suspects.add(ms);
    if (_suspects.length >= recoverAfter) {
      final s = List<int>.of(_suspects)..sort();
      final sm = s[s.length ~/ 2];
      if (s.every((v) => (v - sm).abs() <= maxDeviation * sm)) {
        _recent
          ..clear()
          ..addAll(_suspects.take(_suspects.length - 1));
        _suspects.clear();
        return false;
      }
      _suspects.removeAt(0);
    }
    return true;
  }

  void _remember(int ms) {
    _recent.add(ms);
    if (_recent.length > medianOf) _recent.removeAt(0);
  }

  /// Median of the recent accepted intervals (ms), or null.
  double? get medianMs {
    if (_recent.isEmpty) return null;
    final s = List<int>.of(_recent)..sort();
    final n = s.length;
    return n.isOdd ? s[n ~/ 2].toDouble() : (s[n ~/ 2 - 1] + s[n ~/ 2]) / 2;
  }

  /// Heart rate from the median of the recent accepted intervals.
  double? get medianBpm {
    final m = medianMs;
    return m == null ? null : 60000 / m;
  }

  /// 60000 / mean of the last 2 accepted intervals (1 if only one so far).
  double? get fastBpm {
    if (_recent.isEmpty) return null;
    final n = _recent.length;
    final mean = n == 1 ? _recent[0].toDouble() : (_recent[n - 1] + _recent[n - 2]) / 2;
    return 60000 / mean;
  }
}

/// RR intervals from a plain list of beat indices (no gaps), e.g. for replays.
List<RrInterval> rrFromIndices(Iterable<int> indices, {int hz = 500}) {
  final t = RrTracker(hz: hz);
  for (final i in indices) {
    t.add(i);
  }
  return t.intervals;
}
