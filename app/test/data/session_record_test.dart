import 'dart:convert';

import 'package:ecg_card_app/data/session_record.dart';
import 'package:flutter_test/flutter_test.dart';

import 'fixtures.dart';

void main() {
  test('JSON round trip (through a JSON string)', () {
    final r = sampleRecord(note: 'after a walk');
    final back = SessionRecord.fromJson(jsonDecode(jsonEncode(r.toJson())) as Map<String, Object?>);
    expect(back.id, r.id);
    expect(back.start, r.start);
    expect(back.kind, r.kind);
    expect(back.duration, r.duration);
    expect(back.rr.length, r.rr.length);
    for (var i = 0; i < r.rr.length; i++) {
      expect(back.rr[i].ms, r.rr[i].ms);
      expect(back.rr[i].tSec, closeTo(r.rr[i].tSec, 0.0005));
      expect(back.rr[i].artifact, r.rr[i].artifact);
    }
    expect(back.stats!.beats, 79);
    expect(back.stats!.rmssdMs, 42.25);
    expect(back.stats!.pnn50, 0.25);
    expect(back.calmScore, 4);
    expect(back.coherence, 0.72);
    expect(back.breathsPerMinTarget, 6);
    expect(back.stripHz, 250);
    expect(back.stripUv.length, 2500);
    for (var i = 0; i < 2500; i++) {
      expect(back.stripUv[i], closeTo(r.stripUv[i], 0.05));
    }
    expect(back.note, 'after a walk');
    expect(back.cardName, 'ECG Card 1A2B');
  });

  test('optional fields absent', () {
    final r = SessionRecord(
      id: 'x',
      start: DateTime(2026),
      kind: SessionKind.checkIn,
      duration: Duration.zero,
      rr: const [],
    );
    final j = r.toJson();
    expect(j.containsKey('stats'), isFalse);
    expect(j.containsKey('note'), isFalse);
    final back = SessionRecord.fromJson(j);
    expect(back.stats, isNull);
    expect(back.calmScore, isNull);
    expect(back.coherence, isNull);
    expect(back.stripUv, isEmpty);
    expect(back.kind, SessionKind.checkIn);
  });

  test('unknown kind falls back to free', () {
    final j = sampleRecord().toJson()..['kind'] = 'somethingNew';
    expect(SessionRecord.fromJson(j).kind, SessionKind.free);
  });
}
