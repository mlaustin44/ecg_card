// A static ECG strip on a soft "paper" grid (results, history detail).
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class StripView extends StatelessWidget {
  const StripView({
    super.key,
    required this.samples,
    this.hz = 250,
    this.height = 150,
    this.pxPerSec,
    this.color,
    this.gridColor = AppColors.heart,
  });

  /// Microvolts.
  final List<double> samples;
  final int hz;
  final double height;

  /// Horizontal scale. Null fits the whole strip to the width; a value makes
  /// the strip wider than the screen and horizontally scrollable.
  final double? pxPerSec;

  /// Trace colour (defaults to the main ink colour, like pen on paper).
  final Color? color;
  final Color gridColor;

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    final painter = StripPainter(
      samples: samples,
      trace: color ?? tones.ink,
      grid: gridColor.withValues(alpha: context.isDark ? 0.16 : 0.13),
      paper: tones.soft(gridColor, context.isDark ? 0.05 : 0.035),
    );
    final seconds = samples.length / hz;
    final radius = BorderRadius.circular(AppRadius.small);
    if (pxPerSec == null) {
      return ClipRRect(
        borderRadius: radius,
        child: SizedBox(
          height: height,
          width: double.infinity,
          child: CustomPaint(painter: painter),
        ),
      );
    }
    return ClipRRect(
      borderRadius: radius,
      child: SizedBox(
        height: height,
        child: SingleChildScrollView(
          scrollDirection: Axis.horizontal,
          physics: const BouncingScrollPhysics(),
          child: CustomPaint(
            painter: painter,
            size: Size(math.max(1, seconds * pxPerSec!), height),
          ),
        ),
      ),
    );
  }
}

class StripPainter extends CustomPainter {
  StripPainter({
    required this.samples,
    required this.trace,
    required this.grid,
    required this.paper,
  });
  final List<double> samples;
  final Color trace, grid, paper;

  static const box = 6.0; // minor square, px

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    canvas.drawRect(Offset.zero & size, Paint()..color = paper);

    final minor = Paint()
      ..color = grid.withValues(alpha: grid.a * 0.55)
      ..strokeWidth = 0.6;
    final major = Paint()
      ..color = grid
      ..strokeWidth = 1.0;
    var i = 0;
    for (var x = 0.0; x <= w; x += box, i++) {
      canvas.drawLine(Offset(x, 0), Offset(x, h), i % 5 == 0 ? major : minor);
    }
    i = 0;
    for (var y = h / 2; y <= h; y += box, i++) {
      final p = i % 5 == 0 ? major : minor;
      canvas.drawLine(Offset(0, y), Offset(w, y), p);
      if (i > 0) canvas.drawLine(Offset(0, h - y), Offset(w, h - y), p);
    }

    if (samples.length < 2) return;
    // Robust range (ignores the odd spike), mapped to ~76 % of the height.
    final sorted = List<double>.of(samples)..sort();
    final lo = sorted[(sorted.length * 0.005).floor()];
    final hi = sorted[math.min(sorted.length - 1, (sorted.length * 0.995).floor())];
    final range = math.max(200.0, hi - lo);
    final mid = (hi + lo) / 2;
    double y(double uv) => (h / 2 - (uv - mid) / range * h * 0.76).clamp(1.0, h - 1.0);

    final n = samples.length;
    final perPx = n / w;
    final path = Path();
    if (perPx <= 2) {
      for (var k = 0; k < n; k++) {
        final x = k / (n - 1) * w;
        k == 0 ? path.moveTo(x, y(samples[k])) : path.lineTo(x, y(samples[k]));
      }
    } else {
      // Min/max per pixel column, in time order.
      for (var px = 0; px < w.ceil(); px++) {
        final a = (px * perPx).floor(), b = math.min(n, ((px + 1) * perPx).floor());
        if (b <= a) continue;
        var lo = samples[a], hi = lo;
        var loK = a, hiK = a;
        for (var k = a + 1; k < b; k++) {
          final v = samples[k];
          if (v < lo) {
            lo = v;
            loK = k;
          } else if (v > hi) {
            hi = v;
            hiK = k;
          }
        }
        final first = loK <= hiK ? lo : hi, second = loK <= hiK ? hi : lo;
        final x = px.toDouble();
        px == 0 ? path.moveTo(x, y(first)) : path.lineTo(x, y(first));
        path.lineTo(x + 0.5, y(second));
      }
    }
    canvas.drawPath(
      path,
      Paint()
        ..color = trace
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.7
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round,
    );
  }

  @override
  bool shouldRepaint(StripPainter old) =>
      old.samples != samples || old.trace != trace || old.grid != grid || old.paper != paper;
}
