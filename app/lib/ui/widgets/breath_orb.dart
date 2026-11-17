// The breathing pacer (pure timing) and the orb that follows it.
import 'dart:math' as math;

import 'package:material_ui/material_ui.dart';

import '../theme.dart';

/// Timing of paced breathing: one cycle is an inhale then an exhale, both
/// eased sinusoidally so the orb never jerks at the turn.
class BreathPacer {
  const BreathPacer({this.breathsPerMin = 6, this.inhaleShare = 0.5});

  final double breathsPerMin;

  /// Share of the cycle spent breathing in (0.5 = 1:1, 0.4 = 4:6).
  final double inhaleShare;

  double get cycleSec => 60 / breathsPerMin;
  double get inhaleSec => cycleSec * inhaleShare;
  double get exhaleSec => cycleSec - inhaleSec;

  double _inCycle(double tSec) => tSec % cycleSec;

  bool inhaling(double tSec) => _inCycle(tSec) < inhaleSec;

  /// 0 = lungs empty, 1 = full.
  double level(double tSec) {
    final c = _inCycle(tSec);
    if (c < inhaleSec) return (1 - math.cos(math.pi * c / inhaleSec)) / 2;
    return (1 + math.cos(math.pi * (c - inhaleSec) / exhaleSec)) / 2;
  }

  /// Breath number (0-based) at [tSec].
  int breathIndex(double tSec) => (tSec / cycleSec).floor();

  /// Seconds left in the current inhale or exhale.
  double phaseLeft(double tSec) {
    final c = _inCycle(tSec);
    return c < inhaleSec ? inhaleSec - c : cycleSec - c;
  }

  /// "5 s in, 5 s out".
  String describe() {
    String s(double v) => v == v.roundToDouble() ? v.toStringAsFixed(0) : v.toStringAsFixed(1);
    return '${s(inhaleSec)} s in, ${s(exhaleSec)} s out';
  }
}

/// A soft, petalled orb. [level] is 0 (small, exhaled) .. 1 (full, inhaled);
/// [spin] slowly turns the petals so it feels alive even when held.
class BreathOrb extends StatelessWidget {
  const BreathOrb({
    super.key,
    required this.level,
    this.spin = 0,
    this.size = 300,
    this.dimmed = false,
  });

  final double level;
  final double spin;
  final double size;

  /// Quieter colours (e.g. while waiting for the thumbs).
  final bool dimmed;

  @override
  Widget build(BuildContext context) {
    final dark = Theme.of(context).brightness == Brightness.dark;
    return SizedBox.square(
      dimension: size,
      child: CustomPaint(
        painter: BreathOrbPainter(level: level, spin: spin, dark: dark, dimmed: dimmed),
      ),
    );
  }
}

class BreathOrbPainter extends CustomPainter {
  BreathOrbPainter({
    required this.level,
    required this.spin,
    required this.dark,
    this.dimmed = false,
  });

  final double level, spin;
  final bool dark, dimmed;

  static const _petals = 6;

  @override
  void paint(Canvas canvas, Size size) {
    final c = size.center(Offset.zero);
    final maxR = size.shortestSide / 2;
    final l = level.clamp(0.0, 1.0);
    final a = dimmed ? 0.6 : 1.0;

    final teal = dark ? const Color(0xFF5CCFC0) : AppColors.breath;
    final lav = dark ? const Color(0xFF9A90F0) : const Color(0xFF9FA8F0);
    final sky = dark ? const Color(0xFF7FD8F0) : const Color(0xFF6CC3E0);

    // Halo: a wide soft glow that breathes with the orb.
    final haloR = maxR * (0.62 + 0.38 * l);
    canvas.drawCircle(
      c,
      haloR,
      Paint()
        ..shader = RadialGradient(
          colors: [
            teal.withValues(alpha: (dark ? 0.34 : 0.26) * a),
            teal.withValues(alpha: 0.0),
          ],
        ).createShader(Rect.fromCircle(center: c, radius: haloR)),
    );

    // Ripple rings, fading out as the breath fills.
    for (var i = 0; i < 2; i++) {
      final r = maxR * (0.46 + 0.42 * l) + i * maxR * 0.07;
      canvas.drawCircle(
        c,
        r,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.2
          ..color = teal.withValues(alpha: (0.22 - i * 0.09) * (0.35 + 0.65 * l) * a),
      );
    }

    // Petals: circles that drift outwards from the centre as the breath fills.
    final petalR = maxR * (0.25 + 0.22 * l);
    final reach = maxR * (0.04 + 0.30 * l);
    final turn = spin + l * math.pi / _petals;
    for (var i = 0; i < _petals; i++) {
      final ang = turn + i * 2 * math.pi / _petals;
      final p = c + Offset(math.cos(ang), math.sin(ang)) * reach;
      final mix = (math.sin(ang) + 1) / 2;
      final col = Color.lerp(
        teal,
        i.isEven ? sky : (dark ? lav : AppColors.mint),
        0.2 + 0.4 * mix,
      )!;
      canvas.drawCircle(
        p,
        petalR,
        Paint()
          ..shader = RadialGradient(
            center: const Alignment(-0.2, -0.3),
            colors: [
              col.withValues(alpha: (dark ? 0.55 : 0.42) * a),
              col.withValues(alpha: (dark ? 0.22 : 0.16) * a),
            ],
          ).createShader(Rect.fromCircle(center: p, radius: petalR)),
      );
    }

    // Core: a bright pearl in the middle.
    final coreR = maxR * (0.16 + 0.10 * l);
    canvas.drawCircle(
      c,
      coreR,
      Paint()
        ..shader = RadialGradient(
          center: const Alignment(-0.3, -0.35),
          colors: [
            Colors.white.withValues(alpha: (dark ? 0.85 : 0.95) * a),
            Color.lerp(Colors.white, teal, 0.35)!.withValues(alpha: 0.75 * a),
            teal.withValues(alpha: 0.0),
          ],
          stops: const [0, 0.55, 1],
        ).createShader(Rect.fromCircle(center: c, radius: coreR)),
    );
  }

  @override
  bool shouldRepaint(BreathOrbPainter old) =>
      old.level != level || old.spin != spin || old.dark != dark || old.dimmed != dimmed;
}
