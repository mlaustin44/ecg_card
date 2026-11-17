// The live heart-rate wave of a breathing session: the 2-beat heart rate as
// a smooth curve, over the pacer's breath shape so people can see the heart
// speed up on the in-breath and ease off on the out-breath.
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';
import 'breath_kit.dart';
import 'breath_orb.dart';

/// One heart-rate reading on the pacer clock.
class BreathSample {
  const BreathSample(this.tSec, this.bpm);
  final double tSec;
  final double bpm;
}

class BreathWave extends StatelessWidget {
  const BreathWave({
    super.key,
    required this.samples,
    required this.nowSec,
    required this.pacer,
    this.windowSec = 60,
    this.nowAt = 0.86,
    this.height = 150,
    this.showLabels = true,
  });

  final List<BreathSample> samples;
  final double nowSec;
  final BreathPacer pacer;
  final double windowSec;

  /// Where "now" sits across the width; the pacer curve runs on ahead of it.
  final double nowAt;
  final double height;
  final bool showLabels;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return SizedBox(
      height: height,
      width: double.infinity,
      child: CustomPaint(
        painter: BreathWavePainter(
          samples: samples,
          nowSec: nowSec,
          pacer: pacer,
          windowSec: windowSec,
          nowAt: nowAt,
          heart: t.heart,
          breath: t.breath,
          guide: t.track,
          label: t.inkFaint,
          showLabels: showLabels,
        ),
      ),
    );
  }
}

class BreathWavePainter extends CustomPainter {
  BreathWavePainter({
    required this.samples,
    required this.nowSec,
    required this.pacer,
    required this.windowSec,
    required this.nowAt,
    required this.heart,
    required this.breath,
    required this.guide,
    required this.label,
    this.showLabels = true,
  });

  final List<BreathSample> samples;
  final double nowSec, windowSec, nowAt;
  final BreathPacer pacer;
  final Color heart, breath, guide, label;
  final bool showLabels;

  static const _gapSec = 5.0;

  @override
  void paint(Canvas canvas, Size size) {
    final labelW = showLabels ? 30.0 : 0.0;
    final plot = Rect.fromLTWH(0, 10, size.width - labelW, size.height - 20);
    final t0 = nowSec - windowSec * nowAt;
    final t1 = t0 + windowSec;
    double x(double t) => plot.left + (t - t0) / windowSec * plot.width;

    final vis = [
      for (final s in samples)
        if (s.tSec >= t0 - 2 && s.tSec <= nowSec) s,
    ];

    // Range: around the visible beats, never tighter than 10 bpm.
    double lo = 60, hi = 75;
    if (vis.isNotEmpty) {
      var mn = vis.first.bpm, mx = vis.first.bpm;
      for (final s in vis) {
        mn = math.min(mn, s.bpm);
        mx = math.max(mx, s.bpm);
      }
      final mid = (mn + mx) / 2;
      final span = math.max(mx - mn, 10.0) * 1.25;
      lo = (mid - span / 2).floorToDouble();
      hi = (mid + span / 2).ceilToDouble();
    }
    double y(double bpm) => plot.bottom - (bpm - lo) / (hi - lo) * plot.height;

    // Guides at the top and bottom of the range.
    final guidePaint = Paint()
      ..color = guide
      ..strokeWidth = 1;
    for (final g in [plot.top, plot.bottom]) {
      canvas.drawLine(Offset(plot.left, g), Offset(plot.right, g), guidePaint);
    }
    if (showLabels && vis.isNotEmpty) {
      _label(canvas, hi.round().toString(), Offset(plot.right + 6, plot.top));
      _label(canvas, lo.round().toString(), Offset(plot.right + 6, plot.bottom));
    }

    // Pacer: the breath shape as a soft band, a little ahead of now.
    final pace = Path()..moveTo(plot.left, plot.bottom);
    final line = Path();
    const step = 0.2;
    var first = true;
    for (var t = t0; t <= t1 + step; t += step) {
      final p = Offset(x(t), plot.bottom - plot.height * (0.10 + 0.80 * pacer.level(t)));
      pace.lineTo(p.dx, p.dy);
      if (first) {
        line.moveTo(p.dx, p.dy);
        first = false;
      } else {
        line.lineTo(p.dx, p.dy);
      }
    }
    pace
      ..lineTo(plot.right, plot.bottom)
      ..close();
    canvas.save();
    canvas.clipRect(plot.inflate(2));
    canvas.drawPath(
      pace,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [breath.withValues(alpha: 0.16), breath.withValues(alpha: 0.02)],
        ).createShader(plot),
    );
    canvas.drawPath(
      line,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.4
        ..color = breath.withValues(alpha: 0.45),
    );

    // Now marker.
    if (nowAt < 1) {
      final nx = x(nowSec);
      final dash = Paint()
        ..color = label.withValues(alpha: 0.5)
        ..strokeWidth = 1;
      for (var yy = plot.top; yy < plot.bottom; yy += 6) {
        canvas.drawLine(Offset(nx, yy), Offset(nx, math.min(yy + 3, plot.bottom)), dash);
      }
    }

    // Heart curve, broken where beats are missing.
    final runs = <List<Offset>>[];
    BreathSample? prev;
    for (final s in vis) {
      if (prev == null || s.tSec - prev.tSec > _gapSec) runs.add([]);
      runs.last.add(Offset(x(s.tSec), y(s.bpm)));
      prev = s;
    }
    final fade = LinearGradient(
      colors: [heart.withValues(alpha: 0), heart, heart],
      stops: const [0, 0.22, 1],
    ).createShader(plot);
    for (final run in runs) {
      if (run.length < 2) continue;
      final path = _smooth(run);
      canvas.drawPath(
        path,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 7
          ..strokeCap = StrokeCap.round
          ..color = heart.withValues(alpha: 0.12)
          ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 4),
      );
      canvas.drawPath(
        path,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2.6
          ..strokeCap = StrokeCap.round
          ..strokeJoin = StrokeJoin.round
          ..shader = fade,
      );
    }
    canvas.restore();

    // Latest beat.
    if (vis.isNotEmpty && nowSec - vis.last.tSec < _gapSec) {
      final p = Offset(x(vis.last.tSec), y(vis.last.bpm));
      canvas.drawCircle(p, 9, Paint()..color = heart.withValues(alpha: 0.18));
      canvas.drawCircle(p, 4.5, Paint()..color = heart);
    }
  }

  void _label(Canvas canvas, String s, Offset leftMid) {
    final tp = TextPainter(
      text: TextSpan(
        text: s,
        style: TextStyle(fontFamily: AppText.family, fontSize: 11, color: label),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, leftMid - Offset(0, tp.height / 2));
  }

  /// Catmull-Rom through the points as cubic Beziers.
  static Path _smooth(List<Offset> p) {
    final path = Path()..moveTo(p.first.dx, p.first.dy);
    for (var i = 0; i < p.length - 1; i++) {
      final p0 = i == 0 ? p[i] : p[i - 1];
      final p1 = p[i], p2 = p[i + 1];
      final p3 = i + 2 < p.length ? p[i + 2] : p2;
      final c1 = p1 + (p2 - p0) / 6;
      final c2 = p2 - (p3 - p1) / 6;
      path.cubicTo(c1.dx, c1.dy, c2.dx, c2.dy, p2.dx, p2.dy);
    }
    return path;
  }

  @override
  bool shouldRepaint(BreathWavePainter old) =>
      old.nowSec != nowSec ||
      old.samples.length != samples.length ||
      !identical(old.samples, samples) ||
      old.heart != heart ||
      old.pacer.breathsPerMin != pacer.breathsPerMin ||
      old.pacer.inhaleShare != pacer.inhaleShare;
}

/// The session folded into one breath: mean heart rate at each moment of the
/// breath cycle, over the pacer's shape. Shows the rise on the in-breath and
/// the ease on the out-breath however long the session was.
class BreathCycleChart extends StatelessWidget {
  const BreathCycleChart({
    super.key,
    required this.samples,
    required this.pacer,
    this.height = 120,
  });

  final List<BreathSample> samples;
  final BreathPacer pacer;
  final double height;

  static const bins = 20;

  /// Mean bpm per phase bin (null where empty), first bin at the start of the in-breath.
  static List<double?> fold(List<BreathSample> samples, BreathPacer pacer) {
    final sum = List<double>.filled(bins, 0);
    final n = List<int>.filled(bins, 0);
    for (final s in samples) {
      final i = ((s.tSec % pacer.cycleSec) / pacer.cycleSec * bins).floor().clamp(0, bins - 1);
      sum[i] += s.bpm;
      n[i]++;
    }
    return [for (var i = 0; i < bins; i++) n[i] == 0 ? null : sum[i] / n[i]];
  }

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return SizedBox(
      height: height,
      width: double.infinity,
      child: CustomPaint(
        painter: _CyclePainter(
          folded: fold(samples, pacer),
          pacer: pacer,
          heart: t.heart,
          breath: t.breath,
          guide: t.track,
          label: t.inkFaint,
        ),
      ),
    );
  }
}

class _CyclePainter extends CustomPainter {
  _CyclePainter({
    required this.folded,
    required this.pacer,
    required this.heart,
    required this.breath,
    required this.guide,
    required this.label,
  });

  final List<double?> folded;
  final BreathPacer pacer;
  final Color heart, breath, guide, label;

  @override
  void paint(Canvas canvas, Size size) {
    const labelH = 20.0, labelW = 30.0;
    final plot = Rect.fromLTWH(0, 8, size.width - labelW, size.height - 8 - labelH);
    final cyc = pacer.cycleSec;
    final split = plot.left + plot.width * pacer.inhaleShare;

    // In / out halves, softly tinted.
    final r = const Radius.circular(10);
    canvas.drawRRect(
      RRect.fromRectAndCorners(
        Rect.fromLTRB(plot.left, plot.top, split, plot.bottom),
        topLeft: r,
        bottomLeft: r,
      ),
      Paint()..color = breath.withValues(alpha: 0.07),
    );
    canvas.drawRRect(
      RRect.fromRectAndCorners(
        Rect.fromLTRB(split, plot.top, plot.right, plot.bottom),
        topRight: r,
        bottomRight: r,
      ),
      Paint()..color = guide.withValues(alpha: 0.35),
    );

    // Pacer shape.
    final pace = Path();
    for (var i = 0; i <= 60; i++) {
      final p = Offset(
        plot.left + plot.width * i / 60,
        plot.bottom - plot.height * (0.10 + 0.80 * pacer.level(cyc * i / 60)),
      );
      i == 0 ? pace.moveTo(p.dx, p.dy) : pace.lineTo(p.dx, p.dy);
    }
    canvas.drawPath(
      pace,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.4
        ..color = breath.withValues(alpha: 0.5),
    );

    _text(canvas, 'Breathe in', Offset((plot.left + split) / 2, plot.bottom + 4), center: true);
    _text(canvas, 'Breathe out', Offset((split + plot.right) / 2, plot.bottom + 4), center: true);

    final vals = [for (final v in folded) ?v];
    if (vals.length < 3) return;
    var lo = vals.reduce(math.min), hi = vals.reduce(math.max);
    final mid = (lo + hi) / 2, span = math.max(hi - lo, 6.0) * 1.3;
    lo = mid - span / 2;
    hi = mid + span / 2;
    final n = folded.length;
    final pts = <Offset>[
      for (var i = 0; i < n; i++)
        if (folded[i] != null)
          Offset(
            plot.left + plot.width * (i + 0.5) / n,
            plot.bottom - plot.height * (folded[i]! - lo) / (hi - lo),
          ),
    ];
    final path = BreathWavePainter._smooth(pts);
    canvas.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 7
        ..color = heart.withValues(alpha: 0.12)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 4),
    );
    canvas.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.8
        ..strokeCap = StrokeCap.round
        ..color = heart,
    );
    final top = vals.reduce(math.max), bottom = vals.reduce(math.min);
    _text(
      canvas,
      top.round().toString(),
      Offset(plot.right + 6, plot.bottom - plot.height * (top - lo) / (hi - lo) - 7),
    );
    _text(
      canvas,
      bottom.round().toString(),
      Offset(plot.right + 6, plot.bottom - plot.height * (bottom - lo) / (hi - lo) - 7),
    );
  }

  void _text(Canvas canvas, String s, Offset at, {bool center = false}) {
    final tp = TextPainter(
      text: TextSpan(
        text: s,
        style: TextStyle(fontFamily: AppText.family, fontSize: 11, color: label),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, center ? at - Offset(tp.width / 2, 0) : at);
  }

  @override
  bool shouldRepaint(_CyclePainter old) => old.folded != folded || old.heart != heart;
}
