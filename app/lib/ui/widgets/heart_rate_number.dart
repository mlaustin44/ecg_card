// Big heart-rate number with a heart that gives a little pulse on each beat.
import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class HeartRateNumber extends StatefulWidget {
  const HeartRateNumber({
    super.key,
    required this.bpm,
    this.beats = 0,
    this.size = 72,
    this.color = AppColors.heart,
    this.unit = 'bpm',
    this.showHeart = true,
  });

  /// Shown rounded; null shows a soft placeholder.
  final num? bpm;

  /// Any counter that changes once per beat (e.g. LiveSession.rr.length);
  /// each change makes the heart pulse.
  final int beats;
  final double size;
  final Color color;
  final String unit;
  final bool showHeart;

  @override
  State<HeartRateNumber> createState() => _HeartRateNumberState();
}

class _HeartRateNumberState extends State<HeartRateNumber> with SingleTickerProviderStateMixin {
  late final AnimationController _beat = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 420),
  );

  // Quick swell then a softer settle, like a real beat.
  late final Animation<double> _scale = TweenSequence<double>([
    TweenSequenceItem(
      tween: Tween(begin: 1.0, end: 1.22).chain(CurveTween(curve: Curves.easeOut)),
      weight: 25,
    ),
    TweenSequenceItem(
      tween: Tween(begin: 1.22, end: 0.96).chain(CurveTween(curve: Curves.easeInOut)),
      weight: 35,
    ),
    TweenSequenceItem(
      tween: Tween(begin: 0.96, end: 1.0).chain(CurveTween(curve: Curves.easeOut)),
      weight: 40,
    ),
  ]).animate(_beat);

  @override
  void didUpdateWidget(HeartRateNumber old) {
    super.didUpdateWidget(old);
    if (widget.beats != old.beats && widget.bpm != null) _beat.forward(from: 0);
  }

  @override
  void dispose() {
    _beat.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    final bpm = widget.bpm;
    final numberStyle = AppText.numeral(widget.size, color: tones.ink);
    final number = bpm == null
        ? Text('--', style: numberStyle.copyWith(color: tones.inkSoft.withValues(alpha: 0.5)))
        : TweenAnimationBuilder<double>(
            tween: Tween(end: bpm.toDouble()),
            duration: AppMotion.slow,
            curve: AppMotion.curve,
            builder: (_, v, _) => Text('${v.round()}', style: numberStyle),
          );
    final heartSize = widget.size * 0.42;
    return Semantics(
      label: bpm == null
          ? 'Heart rate not yet measured'
          : 'Heart rate ${bpm.round()} beats per minute',
      excludeSemantics: true,
      child: Row(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          if (widget.showHeart) ...[
            ScaleTransition(
              scale: _scale,
              child: Container(
                width: heartSize * 1.6,
                height: heartSize * 1.6,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: tones.soft(widget.color, 0.16),
                ),
                child: Icon(Icons.favorite_rounded, size: heartSize, color: widget.color),
              ),
            ),
            SizedBox(width: widget.size * 0.18),
          ],
          number,
          SizedBox(width: widget.size * 0.08),
          Padding(
            padding: EdgeInsets.only(top: widget.size * 0.28),
            child: Text(
              widget.unit,
              style: context.text.titleMedium?.copyWith(
                color: tones.inkSoft,
                fontSize: widget.size * 0.24,
              ),
            ),
          ),
        ],
      ),
    );
  }
}
