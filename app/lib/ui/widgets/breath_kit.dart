// Small shared pieces for the Breathe, History, Card and Developer screens,
// built on the theme tokens (AppTones / AppText) and the shared SectionCard.
import 'package:material_ui/material_ui.dart';

import '../theme.dart';
import 'section_card.dart';

/// Brightness-dependent colours, read from the theme's [AppTones].
class BreathTones {
  BreathTones.of(BuildContext context) : tones = context.tones, dark = context.isDark;

  final AppTones tones;
  final bool dark;

  Color get bg => dark ? AppColors.darkBg : AppColors.lightBg;
  Color get card => tones.card;
  Color get ink => tones.ink;
  Color get inkSoft => tones.inkSoft;
  Color get inkFaint => tones.inkSoft.withValues(alpha: 0.75);

  /// Hairlines, meter tracks, chart guides.
  Color get track => tones.line;

  Color get breath => dark ? const Color(0xFF55C9BA) : AppColors.breath;
  Color get heart => dark ? const Color(0xFFFF8577) : AppColors.heart;
  Color get indigo => tones.primary;
}

/// The shared [SectionCard] with this area's default padding.
class BreathCard extends StatelessWidget {
  const BreathCard({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(AppSpace.m),
    this.onTap,
    this.color,
  });

  final Widget child;
  final EdgeInsetsGeometry padding;
  final VoidCallback? onTap;
  final Color? color;

  @override
  Widget build(BuildContext context) =>
      SectionCard(padding: padding, onTap: onTap, color: color, child: child);
}

/// Small upper-case label above a group.
class BreathSectionLabel extends StatelessWidget {
  const BreathSectionLabel(this.text, {super.key});
  final String text;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.fromLTRB(AppSpace.xs, AppSpace.l, AppSpace.xs, AppSpace.s + 2),
    child: Text(text.toUpperCase(), style: AppText.eyebrow(context)),
  );
}

/// Screen header: a round back button and a centred title.
class BreathHeader extends StatelessWidget {
  const BreathHeader({super.key, required this.title, this.onBack, this.trailing});
  final String title;
  final VoidCallback? onBack;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(AppSpace.s, AppSpace.s, AppSpace.s, 0),
      child: Row(
        children: [
          SizedBox(
            width: 48,
            child: onBack == null
                ? null
                : IconButton(
                    onPressed: onBack,
                    tooltip: 'Back',
                    icon: Icon(Icons.arrow_back_rounded, color: t.ink),
                  ),
          ),
          Expanded(
            child: Text(title, textAlign: TextAlign.center, style: context.text.titleMedium),
          ),
          SizedBox(width: 48, child: trailing),
        ],
      ),
    );
  }
}
