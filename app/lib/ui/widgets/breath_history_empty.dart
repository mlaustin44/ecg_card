// History: a friendly empty state with a small painted illustration.
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';
import 'breath_kit.dart';
import 'primary_button.dart';

class BreathHistoryEmpty extends StatelessWidget {
  const BreathHistoryEmpty({super.key, required this.onCheckIn, required this.onBreathe});
  final VoidCallback onCheckIn;
  final VoidCallback onBreathe;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: AppSpace.l),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          SizedBox(width: 220, height: 170, child: CustomPaint(painter: _EmptyArt(t))),
          const SizedBox(height: AppSpace.l),
          Text(
            'Your story starts here',
            textAlign: TextAlign.center,
            style: TextStyle(fontSize: 22, fontWeight: FontWeight.w600, color: t.ink),
          ),
          const SizedBox(height: AppSpace.s),
          Text(
            'Every check-in and breathing session you do will gather here, '
            'so you can look back on your days.',
            textAlign: TextAlign.center,
            style: TextStyle(fontSize: 15, color: t.inkSoft, height: 1.4),
          ),
          const SizedBox(height: AppSpace.l),
          PrimaryButton(
            label: 'Do a check-in',
            icon: Icons.favorite_rounded,
            expand: false,
            onPressed: onCheckIn,
          ),
          const SizedBox(height: AppSpace.s),
          TextButton.icon(
            onPressed: onBreathe,
            icon: const Icon(Icons.air_rounded, size: 18),
            label: const Text('Or try a breathing session'),
            style: TextButton.styleFrom(foregroundColor: t.breath),
          ),
        ],
      ),
    );
  }
}

class _EmptyArt extends CustomPainter {
  _EmptyArt(this.t);
  final BreathTones t;

  @override
  void paint(Canvas canvas, Size size) {
    final c = Offset(size.width / 2, size.height / 2 + 6);

    // Soft sun-like glow behind.
    canvas.drawCircle(
      c,
      size.height * 0.5,
      Paint()
        ..shader = RadialGradient(
          colors: [
            AppColors.sun.withValues(alpha: t.dark ? 0.18 : 0.22),
            AppColors.sun.withValues(alpha: 0),
          ],
        ).createShader(Rect.fromCircle(center: c, radius: size.height * 0.5)),
    );

    // A little journal page.
    final page = RRect.fromRectAndRadius(
      Rect.fromCenter(center: c, width: 128, height: 100),
      const Radius.circular(18),
    );
    canvas.save();
    canvas.translate(c.dx, c.dy);
    canvas.rotate(-0.06);
    canvas.translate(-c.dx, -c.dy);
    canvas.drawRRect(
      page.shift(const Offset(0, 6)),
      Paint()
        ..color = Colors.black.withValues(alpha: t.dark ? 0.35 : 0.07)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 8),
    );
    canvas.drawRRect(page, Paint()..color = t.card);
    canvas.drawRRect(
      page,
      Paint()
        ..style = PaintingStyle.stroke
        ..color = t.track
        ..strokeWidth = 1.2,
    );

    // A heartbeat line across the page.
    final left = page.left + 16, right = page.right - 16, y0 = c.dy - 8;
    final beat = Path()..moveTo(left, y0);
    final w = right - left;
    beat
      ..lineTo(left + w * 0.30, y0)
      ..lineTo(left + w * 0.37, y0 - 6)
      ..lineTo(left + w * 0.43, y0 + 4)
      ..lineTo(left + w * 0.50, y0 - 24)
      ..lineTo(left + w * 0.57, y0 + 12)
      ..lineTo(left + w * 0.63, y0)
      ..lineTo(right, y0);
    canvas.drawPath(
      beat,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 3
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round
        ..color = t.heart,
    );

    // Lines of "writing".
    final line = Paint()
      ..color = t.track
      ..strokeWidth = 5
      ..strokeCap = StrokeCap.round;
    canvas.drawLine(Offset(left, c.dy + 20), Offset(right - 20, c.dy + 20), line);
    canvas.drawLine(Offset(left, c.dy + 33), Offset(left + w * 0.5, c.dy + 33), line);
    canvas.restore();

    // A breath bubble and a heart floating above.
    final bubble = Offset(c.dx + 64, c.dy - 52);
    canvas.drawCircle(bubble, 18, Paint()..color = t.breath.withValues(alpha: 0.22));
    canvas.drawCircle(bubble, 10, Paint()..color = t.breath.withValues(alpha: 0.5));
    _heart(canvas, Offset(c.dx - 66, c.dy - 44), 13, t.heart.withValues(alpha: 0.85));
    canvas.drawCircle(Offset(c.dx - 40, c.dy - 66), 4, Paint()..color = AppColors.sun);
    canvas.drawCircle(
      Offset(c.dx + 84, c.dy + 2),
      3,
      Paint()..color = t.indigo.withValues(alpha: 0.6),
    );
  }

  static void _heart(Canvas canvas, Offset c, double r, Color color) {
    final p = Path()
      ..moveTo(c.dx, c.dy + r)
      ..cubicTo(
        c.dx - r * 1.6,
        c.dy - r * 0.1,
        c.dx - r * 0.7,
        c.dy - r * 1.3,
        c.dx,
        c.dy - r * 0.45,
      )
      ..cubicTo(c.dx + r * 0.7, c.dy - r * 1.3, c.dx + r * 1.6, c.dy - r * 0.1, c.dx, c.dy + r)
      ..close();
    canvas.save();
    canvas.translate(c.dx, c.dy);
    canvas.rotate(-math.pi / 14);
    canvas.translate(-c.dx, -c.dy);
    canvas.drawPath(p, Paint()..color = color);
    canvas.restore();
  }

  @override
  bool shouldRepaint(_EmptyArt old) => old.t.dark != t.dark;
}
