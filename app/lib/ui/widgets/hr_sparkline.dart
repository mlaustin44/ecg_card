// Heart rate over time as a soft smoothed line with a gradient fill.
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../../analysis/models.dart';
import '../theme.dart';

/// (seconds, bpm) points from RR intervals, artifacts dropped.
List<Offset> hrPointsFromRr(List<RrInterval> rr) => [
  for (final r in rr)
    if (!r.artifact && r.ms > 0) Offset(r.tSec, 60000 / r.ms),
];

class HrSparkline extends StatelessWidget {
  HrSparkline({
    super.key,
    required List<RrInterval> rr,
    this.color = AppColors.heart,
    this.height = 96,
    this.showRange = true,
    this.smooth = 3,
  }) : points = hrPointsFromRr(rr);

  /// Direct (x, value) points, x increasing.
  const HrSparkline.points({
    super.key,
    required this.points,
    this.color = AppColors.heart,
    this.height = 96,
    this.showRange = true,
    this.smooth = 3,
  });

  final List<Offset> points;
  final Color color;
  final double height;

  /// Small "low / high" labels at the right.
  final bool showRange;

  /// Moving-average width (points); 1 = raw.
  final int smooth;

  @override
  Widget build(BuildContext context) {
    final pts = _smoothed(points, smooth);
    final tones = context.tones;
    final chart = SizedBox(
      height: height,
      child: CustomPaint(
        painter: _SparkPainter(pts, color, tones.line, tones.card),
        size: Size.infinite,
      ),
    );
    if (!showRange || pts.length < 2) return chart;
    final lo = pts.map((p) => p.dy).reduce(math.min);
    final hi = pts.map((p) => p.dy).reduce(math.max);
    final style = context.text.labelSmall;
    return Row(
      children: [
        Expanded(child: chart),
        const SizedBox(width: AppSpace.s + 2),
        SizedBox(
          height: height,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.end,
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text('${hi.round()}', style: style?.copyWith(color: tones.ink)),
              Text('${lo.round()}', style: style?.copyWith(color: tones.ink)),
            ],
          ),
        ),
      ],
    );
  }

  static List<Offset> _smoothed(List<Offset> p, int w) {
    if (w <= 1 || p.length < w) return p;
    final half = w ~/ 2;
    return [
      for (var i = 0; i < p.length; i++)
        Offset(p[i].dx, () {
          var s = 0.0, n = 0;
          for (var j = math.max(0, i - half); j <= math.min(p.length - 1, i + half); j++) {
            s += p[j].dy;
            n++;
          }
          return s / n;
        }()),
    ];
  }
}

class _SparkPainter extends CustomPainter {
  _SparkPainter(this.pts, this.color, this.line, this.card);
  final List<Offset> pts;
  final Color color, line, card;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    // Soft guide lines.
    final guide = Paint()
      ..color = line
      ..strokeWidth = 1;
    for (final f in [0.0, 0.5, 1.0]) {
      final y = 6 + (h - 12) * f;
      for (var x = 0.0; x < w; x += 8) {
        canvas.drawLine(Offset(x, y), Offset(math.min(w, x + 3), y), guide);
      }
    }
    if (pts.length < 2) return;

    final x0 = pts.first.dx, x1 = pts.last.dx;
    var lo = pts.map((p) => p.dy).reduce(math.min);
    var hi = pts.map((p) => p.dy).reduce(math.max);
    if (hi - lo < 6) {
      final mid = (hi + lo) / 2;
      lo = mid - 3;
      hi = mid + 3;
    }
    final top = 6.0, bottom = h - 6.0;
    Offset map(Offset p) => Offset(
      x1 == x0 ? 0 : 2 + (p.dx - x0) / (x1 - x0) * (w - 8),
      bottom - (p.dy - lo) / (hi - lo) * (bottom - top),
    );
    final m = pts.map(map).toList();

    // Catmull-Rom through the points, as cubic Béziers.
    final path = Path()..moveTo(m.first.dx, m.first.dy);
    for (var i = 0; i < m.length - 1; i++) {
      final p0 = m[math.max(0, i - 1)],
          p1 = m[i],
          p2 = m[i + 1],
          p3 = m[math.min(m.length - 1, i + 2)];
      final c1 = p1 + (p2 - p0) / 6;
      final c2 = p2 - (p3 - p1) / 6;
      path.cubicTo(c1.dx, c1.dy, c2.dx, c2.dy, p2.dx, p2.dy);
    }
    final fill = Path.from(path)
      ..lineTo(m.last.dx, h)
      ..lineTo(m.first.dx, h)
      ..close();
    canvas.drawPath(
      fill,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [color.withValues(alpha: 0.28), color.withValues(alpha: 0.0)],
        ).createShader(Offset.zero & size),
    );
    canvas.drawPath(
      path,
      Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.6
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round,
    );
    final end = m.last;
    canvas.drawCircle(end, 6, Paint()..color = card);
    canvas.drawCircle(end, 4, Paint()..color = color);
  }

  @override
  bool shouldRepaint(_SparkPainter old) =>
      old.pts != pts || old.color != color || old.line != line || old.card != card;
}
