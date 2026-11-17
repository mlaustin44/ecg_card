// A soft circular progress ring (session timer, settling progress).
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class TimerRing extends StatelessWidget {
  const TimerRing({
    super.key,
    required this.progress,
    this.size = 220,
    this.stroke = 12,
    this.gradient = AppGradients.heart,
    this.paused = false,
    this.child,
  });

  /// 0..1 (animated between updates).
  final double progress;
  final double size;
  final double stroke;
  final Gradient gradient;

  /// Dims the ring while the timer waits (contact lost).
  final bool paused;
  final Widget? child;

  @override
  Widget build(BuildContext context) {
    final track = context.tones.line;
    return SizedBox.square(
      dimension: size,
      child: TweenAnimationBuilder<double>(
        tween: Tween(end: progress.clamp(0, 1)),
        duration: const Duration(milliseconds: 900),
        curve: Curves.linear,
        builder: (_, p, child) => AnimatedOpacity(
          opacity: paused ? 0.45 : 1,
          duration: AppMotion.medium,
          child: CustomPaint(painter: _RingPainter(p, stroke, gradient, track), child: child),
        ),
        child: Center(child: child),
      ),
    );
  }
}

class _RingPainter extends CustomPainter {
  _RingPainter(this.p, this.stroke, this.gradient, this.track);
  final double p, stroke;
  final Gradient gradient;
  final Color track;

  @override
  void paint(Canvas canvas, Size size) {
    final rect = (Offset.zero & size).deflate(stroke / 2);
    canvas.drawArc(
      rect,
      0,
      2 * math.pi,
      false,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = stroke
        ..color = track,
    );
    if (p <= 0) return;
    final sweep = 2 * math.pi * p;
    final shader = SweepGradient(
      startAngle: 0,
      endAngle: 2 * math.pi,
      colors: [...gradient.colors.reversed, gradient.colors.last],
      transform: const GradientRotation(-math.pi / 2),
    ).createShader(rect);
    canvas.drawArc(
      rect,
      -math.pi / 2,
      sweep,
      false,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = stroke
        ..strokeCap = StrokeCap.round
        ..shader = shader,
    );
  }

  @override
  bool shouldRepaint(_RingPainter old) =>
      old.p != p || old.stroke != stroke || old.gradient != gradient || old.track != track;
}
