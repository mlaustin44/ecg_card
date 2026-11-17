import 'dart:convert';

import 'package:ecg_card_app/data/export.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:flutter_test/flutter_test.dart';

import 'fixtures.dart';

void main() {
  test('CSV: header and one row per interval', () {
    final r = sampleRecord();
    final lines = const LineSplitter().convert(sessionCsv(r));
    expect(lines.first, 'beat,time_s,rr_ms,hr_bpm,artifact');
    expect(lines, hasLength(r.rr.length + 1));
    final row = lines[18].split(',');
    expect(row[0], '18');
    expect(row[2], '${r.rr[17].ms}');
    expect(row[4], '1'); // the artifact
  });

  test('PDF renders for each kind and for an empty session', () async {
    for (final k in SessionKind.values) {
      final bytes = await sessionPdf(sampleRecord(kind: k, note: 'evening'));
      expect(ascii.decode(bytes.sublist(0, 5)), '%PDF-');
      expect(bytes.length, greaterThan(2000));
    }
    final empty = SessionRecord(
      id: 'e',
      start: DateTime(2026),
      kind: SessionKind.free,
      duration: Duration.zero,
      rr: const [],
    );
    expect((await sessionPdf(empty)).length, greaterThan(500));
  });

  test('pdfSafe keeps Latin-1, maps dashes and quotes, drops emoji', () {
    expect(pdfSafe('Caf\u00e9 \u2014 \u201cok\u201d \u{1F600}'), 'Caf\u00e9 - "ok" ');
  });

  test('file name', () {
    expect(exportBaseName(sampleRecord()), 'ecg-card-2026-11-17-0930');
  });

  test('footer wording', () {
    expect(exportFooter, contains('Not a medical device; not a diagnosis.'));
  });
}
