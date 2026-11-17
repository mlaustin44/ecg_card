// A painted picture of the ECG Card (Card screen hero).
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class BreathCardArt extends StatelessWidget {
  const BreathCardArt({super.key, this.width = 220, this.connected = true});

  final double width;
  final bool connected;

  @override
  Widget build(BuildContext context) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    return SizedBox(
      width: width,
      height: width * 0.66,
      child: CustomPaint(
        painter: _CardPainter(dark: dark, connected: connected),
      ),
    );
  }
}

class _CardPainter extends CustomPainter {
  _CardPainter({required this.dark, required this.connected});
  final bool dark, connected;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width;
    final cardH = w * 0.60;
    final body = RRect.fromRectAndRadius(
      Rect.fromLTWH(w * 0.02, (size.height - cardH) / 2, w * 0.96, cardH),
      Radius.circular(w * 0.07),
    );

    // Shadow, body, sheen.
    canvas.drawRRect(
      body.shift(Offset(0, w * 0.03)),
      Paint()
        ..color = Colors.black.withValues(alpha: dark ? 0.45 : 0.16)
        ..maskFilter = MaskFilter.blur(BlurStyle.normal, w * 0.04),
    );
    canvas.drawRRect(
      body,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [Color(0xFF5A5EC0), Color(0xFF3E4194)],
        ).createShader(body.outerRect),
    );
    canvas.drawRRect(
      body,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.center,
          colors: [Colors.white.withValues(alpha: 0.16), Colors.white.withValues(alpha: 0)],
        ).createShader(body.outerRect),
    );

    // Two gold thumb pads.
    final padR = cardH * 0.20;
    final padY = body.outerRect.center.dy + cardH * 0.04;
    final pads = [Offset(body.left + w * 0.16, padY), Offset(body.right - w * 0.16, padY)];
    for (final p in pads) {
      canvas.drawCircle(
        p,
        padR,
        Paint()
          ..shader = const RadialGradient(
            center: Alignment(-0.3, -0.4),
            colors: [Color(0xFFFFD58A), AppColors.sun, Color(0xFFE09A35)],
            stops: [0, 0.6, 1],
          ).createShader(Rect.fromCircle(center: p, radius: padR)),
      );
    }

    // Little screen with a pulse line.
    final scr = RRect.fromRectAndRadius(
      Rect.fromCenter(
        center: Offset(body.outerRect.center.dx, padY - cardH * 0.02),
        width: w * 0.34,
        height: cardH * 0.36,
      ),
      Radius.circular(w * 0.025),
    );
    canvas.drawRRect(scr, Paint()..color = const Color(0xFF0D0E16));
    final s = scr.outerRect.deflate(w * 0.025);
    final pulse = Path()..moveTo(s.left, s.center.dy);
    pulse
      ..lineTo(s.left + s.width * 0.30, s.center.dy)
      ..lineTo(s.left + s.width * 0.40, s.top + s.height * 0.1)
      ..lineTo(s.left + s.width * 0.50, s.bottom - s.height * 0.05)
      ..lineTo(s.left + s.width * 0.58, s.center.dy)
      ..lineTo(s.right, s.center.dy);
    canvas.drawPath(
      pulse,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = math.max(1.5, w * 0.009)
        ..strokeJoin = StrokeJoin.round
        ..color = connected ? const Color(0xFF6EE7D8) : const Color(0xFF6B6E85),
    );

    // Two buttons along the top edge.
    for (final fx in [0.40, 0.60]) {
      canvas.drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromCenter(
            center: Offset(body.left + body.width * fx, body.top + cardH * 0.10),
            width: w * 0.08,
            height: cardH * 0.05,
          ),
          Radius.circular(w * 0.02),
        ),
        Paint()..color = Colors.white.withValues(alpha: 0.28),
      );
    }
  }

  @override
  bool shouldRepaint(_CardPainter old) => old.dark != dark || old.connected != connected;
}
