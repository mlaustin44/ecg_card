// Session export: a one-page PDF (numbers, the ECG strip on a paper-style
// grid, heart rate over time) and a CSV of the RR intervals.
// Wording: a personal recording, never a diagnosis.
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:intl/intl.dart';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;

import '../analysis/models.dart';
import 'session_record.dart';

const String exportFooter =
    'A personal recording from an ECG Card. Not a medical device; not a diagnosis.';

String sessionKindLabel(SessionKind k) => switch (k) {
  SessionKind.checkIn => 'Check-in',
  SessionKind.breathing => 'Breathing session',
  SessionKind.free => 'Recording',
};

/// File name without extension, e.g. "ecg-card-2026-11-17-0930".
String exportBaseName(SessionRecord r) =>
    'ecg-card-${DateFormat('yyyy-MM-dd-HHmm').format(r.start)}';

/// CSV of the beat-to-beat intervals: beat, time_s, rr_ms, hr_bpm, artifact.
String sessionCsv(SessionRecord r) {
  final b = StringBuffer('beat,time_s,rr_ms,hr_bpm,artifact\n');
  for (var i = 0; i < r.rr.length; i++) {
    final x = r.rr[i];
    b.writeln(
      '${i + 1},${x.tSec.toStringAsFixed(3)},${x.ms},'
      '${x.ms > 0 ? (60000 / x.ms).toStringAsFixed(1) : ''},${x.artifact ? 1 : 0}',
    );
  }
  return b.toString();
}

/// The PDF uses the built-in Helvetica (Latin-1 only): map common
/// typographic characters and drop what it cannot draw (e.g. emoji).
String pdfSafe(String s) {
  const map = {
    '\u2014': '-',
    '\u2013': '-',
    '\u2018': "'",
    '\u2019': "'",
    '\u201c': '"',
    '\u201d': '"',
    '\u2026': '...',
  };
  final b = StringBuffer();
  for (final r in s.runes) {
    final c = String.fromCharCode(r);
    if (map.containsKey(c)) {
      b.write(map[c]);
    } else if (r <= 0xFF) {
      b.write(c);
    }
  }
  return b.toString();
}

String _fmtDuration(Duration d) {
  final m = d.inMinutes, s = d.inSeconds % 60;
  return m > 0 ? '$m min ${s.toString().padLeft(2, '0')} s' : '$s s';
}

/// One-page A4 landscape PDF of a session.
Future<Uint8List> sessionPdf(SessionRecord r) async {
  final doc = pw.Document(title: '${sessionKindLabel(r.kind)} - ECG Card', creator: 'ECG Card app');
  final st = r.stats;
  final numbers = <(String, String)>[
    ('Duration', _fmtDuration(r.duration)),
    if (st != null) ('Average heart rate', '${st.meanHr.round()} bpm'),
    if (st != null) ('Range', '${st.minHr.round()} - ${st.maxHr.round()} bpm'),
    if (st != null) ('RMSSD (HRV)', '${st.rmssdMs.round()} ms'),
    if (r.calmScore != null) ('Calm score', '${r.calmScore} of 5'),
    if (r.coherence != null) ('Coherence', '${(r.coherence! * 100).round()} %'),
    if (r.breathsPerMinTarget != null)
      ('Breathing pace', '${r.breathsPerMinTarget!.toStringAsFixed(1)} / min'),
  ];
  final small = pw.TextStyle(fontSize: 8, color: PdfColors.grey700);

  doc.addPage(
    pw.Page(
      pageFormat: PdfPageFormat.a4.landscape,
      margin: const pw.EdgeInsets.all(18 * PdfPageFormat.mm),
      build: (ctx) => pw.Column(
        crossAxisAlignment: pw.CrossAxisAlignment.start,
        children: [
          pw.Text(
            sessionKindLabel(r.kind),
            style: pw.TextStyle(fontSize: 20, fontWeight: pw.FontWeight.bold),
          ),
          pw.SizedBox(height: 2),
          pw.Text(
            DateFormat('EEEE d MMMM y, HH:mm').format(r.start) +
                (r.cardName != null ? '  -  ${pdfSafe(r.cardName!)}' : ''),
            style: const pw.TextStyle(fontSize: 11, color: PdfColors.grey800),
          ),
          pw.SizedBox(height: 10),
          pw.Wrap(
            spacing: 22,
            runSpacing: 6,
            children: [
              for (final (k, v) in numbers)
                pw.Column(
                  crossAxisAlignment: pw.CrossAxisAlignment.start,
                  children: [
                    pw.Text(k, style: small),
                    pw.Text(v, style: pw.TextStyle(fontSize: 13, fontWeight: pw.FontWeight.bold)),
                  ],
                ),
            ],
          ),
          if (r.note != null && r.note!.isNotEmpty) ...[
            pw.SizedBox(height: 6),
            pw.Text('Note: ${pdfSafe(r.note!)}', style: const pw.TextStyle(fontSize: 10)),
          ],
          pw.SizedBox(height: 10),
          pw.Text(
            'Your recording (${(r.stripUv.length / r.stripHz).toStringAsFixed(0)} s)',
            style: small,
          ),
          pw.SizedBox(height: 2),
          _strip(r.stripUv, r.stripHz),
          pw.SizedBox(height: 2),
          pw.Text('Grid: 1 mm squares, 25 mm/s, 10 mm/mV', style: small),
          pw.SizedBox(height: 10),
          pw.Text('Heart rate over the session (bpm)', style: small),
          pw.SizedBox(height: 2),
          pw.Expanded(child: _hrChart(r.rr)),
          pw.SizedBox(height: 6),
          pw.Text(exportFooter, style: small),
        ],
      ),
    ),
  );
  return doc.save();
}

const _gridMinor = PdfColor.fromInt(0xFFF6C9C9);
const _gridMajor = PdfColor.fromInt(0xFFE59A9A);
const _trace = PdfColor.fromInt(0xFF1E2A3A);

/// The strip at 25 mm/s and 10 mm/mV on a 1 mm / 5 mm grid, as a vector path.
pw.Widget _strip(List<double> uv, int hz) {
  const mm = PdfPageFormat.mm;
  const heightMm = 40.0;
  final seconds = uv.isEmpty ? 10.0 : uv.length / hz;
  final widthMm = (seconds * 25).clamp(25.0, 260.0);
  return pw.CustomPaint(
    size: PdfPoint(widthMm * mm, heightMm * mm),
    painter: (canvas, size) {
      // Grid.
      for (final major in [false, true]) {
        canvas
          ..setStrokeColor(major ? _gridMajor : _gridMinor)
          ..setLineWidth(major ? 0.6 : 0.25);
        final stepMm = major ? 5 : 1;
        for (var x = 0; x <= widthMm; x += stepMm) {
          canvas
            ..moveTo(x * mm, 0)
            ..lineTo(x * mm, size.y);
        }
        for (var y = 0; y <= heightMm; y += stepMm) {
          canvas
            ..moveTo(0, y * mm)
            ..lineTo(size.x, y * mm);
        }
        canvas.strokePath();
      }
      if (uv.isEmpty) return;
      // Baseline at the median so the trace sits centred.
      final sorted = List<double>.of(uv)..sort();
      final base = sorted[sorted.length ~/ 2];
      final mid = size.y / 2;
      final maxDev = (heightMm / 2 - 0.5) * mm;
      final n = math.min(uv.length, (widthMm / 25 * hz).floor());
      canvas
        ..setStrokeColor(_trace)
        ..setLineWidth(0.7)
        ..setLineJoin(PdfLineJoin.round);
      for (var i = 0; i < n; i++) {
        final x = i / hz * 25 * mm;
        final y = mid + ((uv[i] - base) / 1000 * 10 * mm).clamp(-maxDev, maxDev);
        if (i == 0) {
          canvas.moveTo(x, y);
        } else {
          canvas.lineTo(x, y);
        }
      }
      canvas.strokePath();
    },
  );
}

/// Heart rate (accepted beats, 3-beat moving mean) against session time.
pw.Widget _hrChart(List<RrInterval> rr) {
  final ok = rr.where((r) => !r.artifact && r.ms > 0).toList();
  return pw.LayoutBuilder(
    builder: (ctx, constraints) {
      final w = constraints!.maxWidth,
          h = constraints.maxHeight.isFinite ? constraints.maxHeight : 120.0;
      return pw.CustomPaint(
        size: PdfPoint(w, h),
        painter: (canvas, size) {
          const left = 26.0, bottom = 14.0;
          final pw0 = size.x - left, ph = size.y - bottom;
          final axis = pw.Font.helvetica();
          final font = axis.getFont(ctx);
          if (ok.length < 2) {
            canvas
              ..setFillColor(PdfColors.grey600)
              ..drawString(font, 9, 'Not enough beats for a graph', left, size.y / 2);
            return;
          }
          final hr = [for (final r in ok) 60000 / r.ms];
          final sm = [
            for (var i = 0; i < hr.length; i++)
              (hr[math.max(0, i - 1)] + hr[i] + hr[math.min(hr.length - 1, i + 1)]) / 3,
          ];
          var lo = sm.reduce(math.min), hi = sm.reduce(math.max);
          lo = (lo / 10).floor() * 10 - 5;
          hi = (hi / 10).ceil() * 10 + 5;
          final t0 = ok.first.tSec, t1 = math.max(ok.last.tSec, t0 + 1);
          double xOf(double t) => left + (t - t0) / (t1 - t0) * pw0;
          double yOf(double v) => bottom + (v - lo) / (hi - lo) * ph;

          // Horizontal guides every 10 bpm, labelled.
          canvas
            ..setStrokeColor(PdfColors.grey300)
            ..setLineWidth(0.4)
            ..setFillColor(PdfColors.grey700);
          for (var v = (lo / 10).ceil() * 10; v <= hi; v += 10) {
            canvas
              ..moveTo(left, yOf(v.toDouble()))
              ..lineTo(size.x, yOf(v.toDouble()))
              ..drawString(font, 7, '$v', 2, yOf(v.toDouble()) - 2.5);
          }
          canvas.strokePath();
          // Time labels (minutes or seconds).
          final span = t1 - t0;
          final tick = span > 300 ? 60.0 : (span > 90 ? 30.0 : 10.0);
          for (var t = 0.0; t <= span + 0.01; t += tick) {
            final label = tick >= 60 ? '${(t / 60).round()} min' : '${t.round()} s';
            canvas.drawString(font, 7, label, xOf(t0 + t) - 4, 2);
          }
          canvas
            ..setStrokeColor(_trace)
            ..setLineWidth(1)
            ..setLineJoin(PdfLineJoin.round);
          for (var i = 0; i < ok.length; i++) {
            final x = xOf(ok[i].tSec), y = yOf(sm[i]);
            // Break the line across gaps of more than 3 s (contact lost).
            if (i == 0 || ok[i].tSec - ok[i - 1].tSec > 3) {
              canvas.moveTo(x, y);
            } else {
              canvas.lineTo(x, y);
            }
          }
          canvas.strokePath();
        },
      );
    },
  );
}
