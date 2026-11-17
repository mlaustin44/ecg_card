// Drawn illustration: the card with its two pads and two thumbs resting on
// them. Thumbs drift gently down onto the pads; the pads glow when touched.
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class ThumbsIllustration extends StatefulWidget {
  const ThumbsIllustration({
    super.key,
    this.touching = false,
    this.animate = true,
    this.width = 300,
  });

  /// Pads glow teal and the thumbs sit down.
  final bool touching;

  /// Thumbs hover and settle in a loop (while not touching).
  final bool animate;
  final double width;

  @override
  State<ThumbsIllustration> createState() => _ThumbsIllustrationState();
}

class _ThumbsIllustrationState extends State<ThumbsIllustration>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 2600),
  );

  @override
  void initState() {
    super.initState();
    _sync();
  }

  @override
  void didUpdateWidget(ThumbsIllustration old) {
    super.didUpdateWidget(old);
    _sync();
  }

  void _sync() {
    if (widget.animate && !widget.touching) {
      if (!_c.isAnimating) _c.repeat();
    } else {
      _c.stop();
      _c.value = 0.5;
    }
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final dark = context.isDark;
    return Semantics(
      image: true,
      label: 'The card held flat, with a thumb resting on each of the two round pads',
      child: SizedBox(
        width: widget.width,
        height: widget.width * 0.78,
        child: AnimatedBuilder(
          animation: _c,
          builder: (_, _) {
            // 0..1 → hover down onto the pads, rest, lift a little.
            final t = _c.value;
            final press = widget.touching || !widget.animate
                ? 1.0
                : (0.5 - 0.5 * math.cos(t * 2 * math.pi));
            return TweenAnimationBuilder<double>(
              tween: Tween(end: widget.touching ? 1 : 0),
              duration: AppMotion.slow,
              builder: (_, glow, _) => CustomPaint(
                painter: _ThumbsPainter(
                  press: press,
                  glow: glow,
                  dark: dark,
                  line: context.tones.line,
                ),
              ),
            );
          },
        ),
      ),
    );
  }
}

class _ThumbsPainter extends CustomPainter {
  _ThumbsPainter({required this.press, required this.glow, required this.dark, required this.line});
  final double press; // 0 = lifted, 1 = resting on the pads
  final double glow; // 0..1 contact glow
  final bool dark;
  final Color line;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;

    // Soft shadow under the card.
    final card = RRect.fromRectAndRadius(
      Rect.fromCenter(center: Offset(w / 2, h * 0.42), width: w * 0.86, height: w * 0.50),
      Radius.circular(w * 0.05),
    );
    canvas.drawRRect(
      card.shift(Offset(0, w * 0.03)),
      Paint()
        ..color = Colors.black.withValues(alpha: dark ? 0.35 : 0.10)
        ..maskFilter = MaskFilter.blur(BlurStyle.normal, w * 0.03),
    );

    // Card body (a friendly indigo board).
    canvas.drawRRect(
      card,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [Color(0xFF5A5EC0), Color(0xFF3E4194)],
        ).createShader(card.outerRect),
    );
    canvas.drawRRect(
      card.deflate(w * 0.012),
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = Colors.white.withValues(alpha: 0.12),
    );

    // Little screen with a heartbeat squiggle.
    final screen = RRect.fromRectAndRadius(
      Rect.fromCenter(center: Offset(w / 2, h * 0.36), width: w * 0.26, height: w * 0.14),
      Radius.circular(w * 0.018),
    );
    canvas.drawRRect(screen, Paint()..color = const Color(0xFF16173A));
    final sq = Path();
    final sr = screen.outerRect.deflate(w * 0.02);
    final pts = [0.0, 0.35, 0.42, 0.48, 0.55, 0.62, 1.0];
    final ys = [0.6, 0.6, 0.15, 0.95, 0.45, 0.6, 0.6];
    for (var i = 0; i < pts.length; i++) {
      final p = Offset(sr.left + sr.width * pts[i], sr.top + sr.height * ys[i]);
      i == 0 ? sq.moveTo(p.dx, p.dy) : sq.lineTo(p.dx, p.dy);
    }
    canvas.drawPath(
      sq,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = w * 0.008
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round
        ..color = AppColors.mint,
    );
    // A button dot under the screen.
    canvas.drawCircle(
      Offset(w / 2, h * 0.53),
      w * 0.016,
      Paint()..color = Colors.white.withValues(alpha: 0.25),
    );

    // Pads (gold), glowing teal when touched.
    final padR = w * 0.09;
    final pads = [Offset(card.left + w * 0.13, h * 0.44), Offset(card.right - w * 0.13, h * 0.44)];
    for (final p in pads) {
      if (glow > 0) {
        canvas.drawCircle(
          p,
          padR * (1.35 + 0.15 * glow),
          Paint()
            ..color = AppColors.breath.withValues(alpha: 0.45 * glow)
            ..maskFilter = MaskFilter.blur(BlurStyle.normal, padR * 0.5),
        );
      }
      canvas.drawCircle(
        p,
        padR,
        Paint()
          ..shader = RadialGradient(
            center: const Alignment(-0.3, -0.4),
            colors: [const Color(0xFFFFE3A3), const Color(0xFFE0A83E)],
          ).createShader(Rect.fromCircle(center: p, radius: padR)),
      );
      canvas.drawCircle(
        p,
        padR * 0.62,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.2
          ..color = const Color(0xFFB98426).withValues(alpha: 0.5),
      );
    }

    // Thumbs, coming in from the lower corners.
    final lift = (1 - press) * w * 0.15;
    _thumb(canvas, pads[0] + Offset(-lift * 0.5, -lift), w, mirror: false);
    _thumb(canvas, pads[1] + Offset(lift * 0.5, -lift), w, mirror: true);
  }

  void _thumb(Canvas canvas, Offset tip, double w, {required bool mirror}) {
    canvas.save();
    canvas.translate(tip.dx, tip.dy);
    canvas.rotate((mirror ? -1 : 1) * 0.42);
    final tw = w * 0.19, len = w * 0.55;
    // Thumb pad sits over the card pad: the rounded tip is at the origin.
    final body = RRect.fromRectAndCorners(
      Rect.fromLTWH(-tw / 2, -tw * 0.42, tw, len),
      topLeft: Radius.circular(tw / 2),
      topRight: Radius.circular(tw / 2),
      bottomLeft: Radius.circular(tw * 0.3),
      bottomRight: Radius.circular(tw * 0.3),
    );
    canvas.drawRRect(
      body.shift(Offset(0, w * 0.012)),
      Paint()
        ..color = Colors.black.withValues(alpha: dark ? 0.35 : 0.12)
        ..maskFilter = MaskFilter.blur(BlurStyle.normal, w * 0.015),
    );
    canvas.drawRRect(
      body,
      Paint()
        ..shader = LinearGradient(
          begin: mirror ? Alignment.centerRight : Alignment.centerLeft,
          end: mirror ? Alignment.centerLeft : Alignment.centerRight,
          colors: const [Color(0xFFF6C7A6), Color(0xFFE9A983)],
        ).createShader(body.outerRect),
    );
    // Nail (the thumb is seen from above, nail up).
    final nail = RRect.fromRectAndRadius(
      Rect.fromLTWH(-tw * 0.3, -tw * 0.28, tw * 0.6, tw * 0.62),
      Radius.circular(tw * 0.28),
    );
    canvas.drawRRect(nail, Paint()..color = const Color(0xFFFBE0D0));
    canvas.drawRRect(
      nail,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = const Color(0xFFD99A78).withValues(alpha: 0.6),
    );
    // Knuckle crease.
    canvas.drawLine(
      Offset(-tw * 0.22, tw * 0.75),
      Offset(tw * 0.22, tw * 0.75),
      Paint()
        ..strokeWidth = 1.4
        ..strokeCap = StrokeCap.round
        ..color = const Color(0xFFD08E6C).withValues(alpha: 0.55),
    );
    canvas.restore();
  }

  @override
  bool shouldRepaint(_ThumbsPainter old) =>
      old.press != press || old.glow != glow || old.dark != dark || old.line != line;
}
