// A soft coherence meter: how closely the heart's rhythm follows the breath.
import 'package:material_ui/material_ui.dart';

import '../theme.dart';
import 'breath_kit.dart';

/// Coherence at or above this counts as "in sync" (time in sync, history).
const double breathInSyncAt = 0.6;

/// Friendly words for a coherence score (0..1); null while it is being worked out.
String breathCoherenceWord(double? score) {
  if (score == null) return 'Finding your rhythm';
  if (score < 0.3) return 'Finding your rhythm';
  if (score < breathInSyncAt) return 'Getting closer';
  if (score < 0.8) return 'In sync';
  return 'Beautifully in sync';
}

class BreathCoherenceMeter extends StatelessWidget {
  const BreathCoherenceMeter({super.key, required this.score});

  /// 0..1, null before the first estimate.
  final double? score;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    final s = (score ?? 0).clamp(0.0, 1.0);
    final word = breathCoherenceWord(score);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      mainAxisSize: MainAxisSize.min,
      children: [
        Row(
          children: [
            Icon(Icons.waves_rounded, size: 18, color: t.breath),
            const SizedBox(width: AppSpace.s),
            Expanded(
              child: AnimatedSwitcher(
                duration: const Duration(milliseconds: 600),
                child: Text(
                  word,
                  key: ValueKey(word),
                  style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: t.ink),
                ),
              ),
            ),
            Text('Sync', style: TextStyle(fontSize: 12, color: t.inkFaint)),
          ],
        ),
        const SizedBox(height: AppSpace.s),
        TweenAnimationBuilder<double>(
          tween: Tween(end: s),
          duration: const Duration(milliseconds: 1400),
          curve: Curves.easeInOutCubic,
          builder: (context, v, _) => SizedBox(
            height: 14,
            child: CustomPaint(
              painter: _MeterPainter(
                value: v,
                track: t.track,
                from: t.indigo,
                to: t.breath,
                glow: score != null,
              ),
            ),
          ),
        ),
      ],
    );
  }
}

class _MeterPainter extends CustomPainter {
  _MeterPainter({
    required this.value,
    required this.track,
    required this.from,
    required this.to,
    required this.glow,
  });

  final double value;
  final Color track, from, to;
  final bool glow;

  @override
  void paint(Canvas canvas, Size size) {
    const h = 8.0;
    final r = Rect.fromLTWH(0, (size.height - h) / 2, size.width, h);
    final rr = RRect.fromRectAndRadius(r, const Radius.circular(h / 2));
    canvas.drawRRect(rr, Paint()..color = track);

    // A faint tick where "in sync" begins.
    final tx = r.left + r.width * breathInSyncAt;
    canvas.drawLine(
      Offset(tx, r.top - 2),
      Offset(tx, r.bottom + 2),
      Paint()
        ..color = to.withValues(alpha: 0.45)
        ..strokeWidth = 1.5,
    );

    if (!glow) return;
    final w = (r.width * value).clamp(h, r.width);
    final fill = RRect.fromRectAndRadius(
      Rect.fromLTWH(r.left, r.top, w, h),
      const Radius.circular(h / 2),
    );
    canvas.drawRRect(
      fill,
      Paint()..shader = LinearGradient(colors: [from.withValues(alpha: 0.55), to]).createShader(r),
    );
    final knob = Offset(r.left + w - h / 2, r.center.dy);
    canvas.drawCircle(
      knob,
      10,
      Paint()
        ..color = to.withValues(alpha: 0.25)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 4),
    );
    canvas.drawCircle(knob, 6, Paint()..color = Colors.white);
    canvas.drawCircle(knob, 4, Paint()..color = to);
  }

  @override
  bool shouldRepaint(_MeterPainter old) =>
      old.value != value || old.track != track || old.to != to || old.glow != glow;
}
