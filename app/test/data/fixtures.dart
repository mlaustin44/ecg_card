import 'dart:math' as math;

import 'package:ecg_card_app/analysis/models.dart';
import 'package:ecg_card_app/data/session_record.dart';

SessionRecord sampleRecord({
  String id = 'abc',
  DateTime? start,
  SessionKind kind = SessionKind.breathing,
  String? note,
}) {
  var t = 0.0;
  final rr = <RrInterval>[];
  for (var i = 0; i < 80; i++) {
    final ms = (900 + 60 * math.sin(i / 1.6)).round();
    t += ms / 1000;
    rr.add(RrInterval(tSec: t, ms: ms, artifact: i == 17));
  }
  return SessionRecord(
    id: id,
    start: start ?? DateTime(2026, 11, 17, 9, 30, 12),
    kind: kind,
    duration: const Duration(seconds: 72, milliseconds: 400),
    rr: rr,
    stats: const HrvStats(
      beats: 79,
      meanHr: 66.6,
      minHr: 62.5,
      maxHr: 71.4,
      rmssdMs: 42.25,
      sdnnMs: 40.5,
      pnn50: 0.25,
    ),
    calmScore: 4,
    coherence: kind == SessionKind.breathing ? 0.72 : null,
    breathsPerMinTarget: kind == SessionKind.breathing ? 6 : null,
    stripUv: [
      for (var i = 0; i < 2500; i++) 1200 * math.exp(-math.pow((i % 225) - 112, 2) / 18) - 50 + 0.0,
    ],
    note: note,
    cardName: 'ECG Card 1A2B',
  );
}
