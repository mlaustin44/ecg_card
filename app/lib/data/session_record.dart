// A saved session (contract). JSON round-trip is implemented by the data layer.
import '../analysis/models.dart';

enum SessionKind { checkIn, breathing, free }

class SessionRecord {
  const SessionRecord({
    required this.id,
    required this.start,
    required this.kind,
    required this.duration,
    required this.rr,
    this.stats,
    this.calmScore,
    this.coherence,
    this.breathsPerMinTarget,
    this.stripUv = const [],
    this.stripHz = 250,
    this.note,
    this.cardName,
  });

  final String id;
  final DateTime start;
  final SessionKind kind;
  final Duration duration;
  final List<RrInterval> rr;
  final HrvStats? stats;
  final int? calmScore; // 1..5
  final double? coherence; // 0..1, breathing sessions
  final double? breathsPerMinTarget;

  /// About 10 s of ECG in microvolts (the cleanest stretch), for the results
  /// view and the PDF.
  final List<double> stripUv;
  final int stripHz;
  final String? note;
  final String? cardName;

  SessionRecord copyWith({String? note}) => SessionRecord(
    id: id,
    start: start,
    kind: kind,
    duration: duration,
    rr: rr,
    stats: stats,
    calmScore: calmScore,
    coherence: coherence,
    breathsPerMinTarget: breathsPerMinTarget,
    stripUv: stripUv,
    stripHz: stripHz,
    note: note ?? this.note,
    cardName: cardName,
  );

  /// JSON format version written by [toJson].
  static const int jsonVersion = 1;

  /// JSON-compatible map (also the sembast record value). RR intervals are
  /// stored as parallel arrays to keep long sessions compact.
  Map<String, Object?> toJson() => {
    'v': jsonVersion,
    'id': id,
    'start': start.millisecondsSinceEpoch,
    'kind': kind.name,
    'durationMs': duration.inMilliseconds,
    'rrT': [for (final r in rr) (r.tSec * 1000).round()],
    'rrMs': [for (final r in rr) r.ms],
    'rrArtifact': [
      for (var i = 0; i < rr.length; i++)
        if (rr[i].artifact) i,
    ],
    if (stats != null)
      'stats': {
        'beats': stats!.beats,
        'meanHr': stats!.meanHr,
        'minHr': stats!.minHr,
        'maxHr': stats!.maxHr,
        'rmssdMs': stats!.rmssdMs,
        'sdnnMs': stats!.sdnnMs,
        'pnn50': stats!.pnn50,
      },
    'calmScore': ?calmScore,
    'coherence': ?coherence,
    'breathsPerMinTarget': ?breathsPerMinTarget,
    'stripHz': stripHz,
    // 0.1 uV resolution is plenty for a display strip and halves the size.
    'stripUv': [for (final v in stripUv) (v * 10).round() / 10],
    'note': ?note,
    'cardName': ?cardName,
  };

  static SessionRecord fromJson(Map<String, Object?> json) {
    double d(Object? v) => (v as num).toDouble();
    double? dn(Object? v) => v == null ? null : (v as num).toDouble();
    final rrT = (json['rrT'] as List?) ?? const [];
    final rrMs = (json['rrMs'] as List?) ?? const [];
    final art = {...((json['rrArtifact'] as List?) ?? const []).map((e) => (e as num).toInt())};
    final n = rrT.length < rrMs.length ? rrT.length : rrMs.length;
    final st = json['stats'] as Map?;
    final kindName = json['kind'] as String?;
    return SessionRecord(
      id: json['id'] as String,
      start: DateTime.fromMillisecondsSinceEpoch((json['start'] as num).toInt()),
      kind: SessionKind.values.firstWhere(
        (k) => k.name == kindName,
        orElse: () => SessionKind.free,
      ),
      duration: Duration(milliseconds: ((json['durationMs'] as num?) ?? 0).toInt()),
      rr: [
        for (var i = 0; i < n; i++)
          RrInterval(
            tSec: (rrT[i] as num) / 1000,
            ms: (rrMs[i] as num).toInt(),
            artifact: art.contains(i),
          ),
      ],
      stats: st == null
          ? null
          : HrvStats(
              beats: (st['beats'] as num).toInt(),
              meanHr: d(st['meanHr']),
              minHr: d(st['minHr']),
              maxHr: d(st['maxHr']),
              rmssdMs: d(st['rmssdMs']),
              sdnnMs: d(st['sdnnMs']),
              pnn50: d(st['pnn50']),
            ),
      calmScore: (json['calmScore'] as num?)?.toInt(),
      coherence: dn(json['coherence']),
      breathsPerMinTarget: dn(json['breathsPerMinTarget']),
      stripUv: [for (final v in (json['stripUv'] as List?) ?? const []) (v as num).toDouble()],
      stripHz: ((json['stripHz'] as num?) ?? 250).toInt(),
      note: json['note'] as String?,
      cardName: json['cardName'] as String?,
    );
  }
}
