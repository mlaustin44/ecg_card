// History: small 30-day trend cards (resting heart rate, calm score).
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';
import 'breath_kit.dart';

/// One value per day; [day] is 0 (oldest) .. days-1 (today).
class BreathDayValue {
  const BreathDayValue(this.day, this.value);
  final int day;
  final double value;
}

enum BreathTrendStyle { line, bars }

class BreathTrendCard extends StatelessWidget {
  const BreathTrendCard({
    super.key,
    required this.title,
    required this.value,
    required this.unit,
    required this.points,
    required this.color,
    required this.icon,
    this.style = BreathTrendStyle.line,
    this.days = 30,
    this.barMax = 5,
  });

  final String title;
  final String? value;
  final String unit;
  final List<BreathDayValue> points;
  final Color color;
  final IconData icon;
  final BreathTrendStyle style;
  final int days;
  final double barMax;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return BreathCard(
      padding: const EdgeInsets.fromLTRB(14, 14, 14, 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(icon, size: 16, color: color),
              const SizedBox(width: 6),
              Expanded(
                child: Text(
                  title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: t.inkSoft),
                ),
              ),
            ],
          ),
          const SizedBox(height: AppSpace.s),
          Text.rich(
            TextSpan(
              children: [
                TextSpan(text: value ?? '–'),
                if (value != null)
                  TextSpan(
                    text: ' $unit',
                    style: TextStyle(fontSize: 13, fontWeight: FontWeight.w400, color: t.inkFaint),
                  ),
              ],
            ),
            style: TextStyle(fontSize: 24, fontWeight: FontWeight.w600, color: t.ink),
          ),
          const SizedBox(height: AppSpace.s),
          SizedBox(
            height: 48,
            width: double.infinity,
            child: CustomPaint(
              painter: _TrendPainter(
                points: points,
                days: days,
                color: color,
                track: t.track,
                style: style,
                barMax: barMax,
              ),
            ),
          ),
          const SizedBox(height: 6),
          Row(
            children: [
              Expanded(
                child: Text(
                  '30 days ago',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(fontSize: 10.5, color: t.inkFaint),
                ),
              ),
              Text('Today', maxLines: 1, style: TextStyle(fontSize: 10.5, color: t.inkFaint)),
            ],
          ),
        ],
      ),
    );
  }
}

class _TrendPainter extends CustomPainter {
  _TrendPainter({
    required this.points,
    required this.days,
    required this.color,
    required this.track,
    required this.style,
    required this.barMax,
  });

  final List<BreathDayValue> points;
  final int days;
  final Color color, track;
  final BreathTrendStyle style;
  final double barMax;

  @override
  void paint(Canvas canvas, Size size) {
    final slot = size.width / days;
    if (style == BreathTrendStyle.bars) {
      final byDay = {for (final p in points) p.day: p.value};
      for (var d = 0; d < days; d++) {
        final v = byDay[d];
        final w = math.max(2.0, slot * 0.55);
        final x = d * slot + (slot - w) / 2;
        final h = v == null ? 3.0 : math.max(4.0, size.height * (v / barMax));
        canvas.drawRRect(
          RRect.fromRectAndRadius(Rect.fromLTWH(x, size.height - h, w, h), Radius.circular(w / 2)),
          Paint()..color = v == null ? track : color.withValues(alpha: 0.35 + 0.65 * (v / barMax)),
        );
      }
      return;
    }

    // Line: faint baseline, then a smooth curve through the days that have values.
    canvas.drawLine(
      Offset(0, size.height - 1),
      Offset(size.width, size.height - 1),
      Paint()
        ..color = track
        ..strokeWidth = 1,
    );
    if (points.isEmpty) return;
    var lo = points.first.value, hi = points.first.value;
    for (final p in points) {
      lo = math.min(lo, p.value);
      hi = math.max(hi, p.value);
    }
    final mid = (lo + hi) / 2;
    final span = math.max(hi - lo, 6.0) * 1.2;
    lo = mid - span / 2;
    hi = mid + span / 2;
    final pts = [
      for (final p in points)
        Offset((p.day + 0.5) * slot, 4 + (size.height - 8) * (1 - (p.value - lo) / (hi - lo))),
    ];
    if (pts.length == 1) {
      canvas.drawCircle(pts.first, 3.5, Paint()..color = color);
      return;
    }
    final path = Path()..moveTo(pts.first.dx, pts.first.dy);
    for (var i = 0; i < pts.length - 1; i++) {
      final p0 = i == 0 ? pts[i] : pts[i - 1];
      final p1 = pts[i], p2 = pts[i + 1];
      final p3 = i + 2 < pts.length ? pts[i + 2] : p2;
      final c1 = p1 + (p2 - p0) / 6, c2 = p2 - (p3 - p1) / 6;
      path.cubicTo(c1.dx, c1.dy, c2.dx, c2.dy, p2.dx, p2.dy);
    }
    final area = Path.from(path)
      ..lineTo(pts.last.dx, size.height)
      ..lineTo(pts.first.dx, size.height)
      ..close();
    canvas.drawPath(
      area,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [color.withValues(alpha: 0.22), color.withValues(alpha: 0)],
        ).createShader(Offset.zero & size),
    );
    canvas.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.2
        ..strokeCap = StrokeCap.round
        ..color = color,
    );
    canvas.drawCircle(pts.last, 4.5, Paint()..color = color.withValues(alpha: 0.25));
    canvas.drawCircle(pts.last, 3, Paint()..color = color);
  }

  @override
  bool shouldRepaint(_TrendPainter old) => !identical(old.points, points) || old.color != color;
}
