// The 1..5 calm score as five soft dots and a friendly word. Wellness wording
// only: every level is a fine place to be.
import 'package:material_ui/material_ui.dart';

import '../theme.dart';

/// Friendly word for a calm score 1..5.
String calmWord(int score) => switch (score) {
  <= 1 => 'Revved up',
  2 => 'Lively',
  3 => 'Steady',
  4 => 'Calm',
  _ => 'Settled',
};

/// One line about the level, warm and non-judgemental.
String calmLine(int score) => switch (score) {
  <= 1 => 'Your body is in go mode right now. A few slow breaths can help it ease off.',
  2 => 'A busy, lively rhythm. Plenty of energy about.',
  3 => 'A nice steady middle. Your heart is easing along.',
  4 => 'Your heartbeat is swaying gently with your breath.',
  _ => 'Lovely and settled. Your heart is in an easy, unhurried rhythm.',
};

class CalmDots extends StatelessWidget {
  const CalmDots({super.key, required this.score, this.dot = 16, this.color = AppColors.breath});
  final int score;
  final double dot;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    return Semantics(
      label: 'Calm score $score of 5, ${calmWord(score)}',
      excludeSemantics: true,
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          for (var i = 1; i <= 5; i++) ...[
            if (i > 1) SizedBox(width: dot * 0.5),
            TweenAnimationBuilder<double>(
              tween: Tween(end: i <= score ? 1 : 0),
              duration: AppMotion.slow + Duration(milliseconds: 90 * i),
              curve: Curves.easeOutBack,
              builder: (_, v, _) {
                final size = dot * (0.72 + 0.28 * v.clamp(0, 1.2));
                return SizedBox.square(
                  dimension: dot,
                  child: Center(
                    child: Container(
                      width: size,
                      height: size,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        color: Color.lerp(tones.soft(color, 0.18), color, v.clamp(0, 1)),
                        boxShadow: v > 0.5
                            ? [
                                BoxShadow(
                                  color: color.withValues(alpha: 0.35 * v.clamp(0, 1)),
                                  blurRadius: dot * 0.5,
                                ),
                              ]
                            : null,
                      ),
                    ),
                  ),
                );
              },
            ),
          ],
        ],
      ),
    );
  }
}

/// Word + dots, used on the results and home screens.
class CalmScoreView extends StatelessWidget {
  const CalmScoreView({super.key, required this.score, this.large = true});
  final int score;
  final bool large;

  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    mainAxisSize: MainAxisSize.min,
    children: [
      Text(
        calmWord(score),
        style: (large ? context.text.headlineMedium : context.text.titleLarge)?.copyWith(
          color: AppColors.breath,
        ),
      ),
      SizedBox(height: large ? AppSpace.s + 2 : AppSpace.s),
      CalmDots(score: score, dot: large ? 18 : 12),
    ],
  );
}
