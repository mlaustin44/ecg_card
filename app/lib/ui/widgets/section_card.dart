// A soft rounded card: the basic building block of every screen.
import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class SectionCard extends StatelessWidget {
  const SectionCard({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(AppSpace.l - 4),
    this.onTap,
    this.gradient,
    this.color,
    this.radius = AppRadius.card,
    this.title,
    this.trailing,
  });

  final Widget child;
  final EdgeInsetsGeometry padding;
  final VoidCallback? onTap;

  /// Fills the card instead of the plain card colour (white text expected).
  final Gradient? gradient;
  final Color? color;
  final double radius;

  /// Optional small upper-case heading inside the card.
  final String? title;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    final shape = BorderRadius.circular(radius);
    Widget content = child;
    if (title != null) {
      content = Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          Row(
            children: [
              Expanded(
                child: Text(
                  title!.toUpperCase(),
                  style: AppText.eyebrow(context, color: gradient != null ? Colors.white70 : null),
                ),
              ),
              ?trailing,
            ],
          ),
          const SizedBox(height: AppSpace.m - 4),
          child,
        ],
      );
    }
    return DecoratedBox(
      decoration: BoxDecoration(
        color: gradient == null ? (color ?? tones.card) : null,
        gradient: gradient,
        borderRadius: shape,
        boxShadow: [
          BoxShadow(
            color: gradient != null
                ? gradient!.colors.first.withValues(alpha: context.isDark ? 0.25 : 0.28)
                : tones.shadow,
            blurRadius: gradient != null ? 24 : 18,
            offset: const Offset(0, 8),
          ),
        ],
      ),
      child: Material(
        type: MaterialType.transparency,
        child: InkWell(
          onTap: onTap,
          borderRadius: shape,
          child: Padding(padding: padding, child: content),
        ),
      ),
    );
  }
}

/// A round tinted well holding an icon (list rows, small cards).
class IconWell extends StatelessWidget {
  const IconWell({
    super.key,
    required this.icon,
    required this.color,
    this.size = 44,
    this.onGradient = false,
  });
  final IconData icon;
  final Color color;
  final double size;
  final bool onGradient;

  @override
  Widget build(BuildContext context) => Container(
    width: size,
    height: size,
    decoration: BoxDecoration(
      shape: BoxShape.circle,
      color: onGradient
          ? Colors.white.withValues(alpha: 0.22)
          : context.tones.soft(color, context.isDark ? 0.22 : 0.14),
    ),
    child: Icon(icon, size: size * 0.5, color: onGradient ? Colors.white : color),
  );
}

/// A tappable row inside a card: icon well, title, subtitle, chevron.
class NavRow extends StatelessWidget {
  const NavRow({
    super.key,
    required this.icon,
    required this.color,
    required this.title,
    this.subtitle,
    this.onTap,
    this.trailing,
  });
  final IconData icon;
  final Color color;
  final String title;
  final String? subtitle;
  final VoidCallback? onTap;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) => InkWell(
    onTap: onTap,
    borderRadius: BorderRadius.circular(AppRadius.small),
    child: Padding(
      padding: const EdgeInsets.symmetric(vertical: AppSpace.s + 2, horizontal: AppSpace.xs),
      child: Row(
        children: [
          IconWell(icon: icon, color: color, size: 40),
          const SizedBox(width: AppSpace.m - 2),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(title, style: context.text.titleSmall),
                if (subtitle != null) ...[
                  const SizedBox(height: 2),
                  Text(subtitle!, style: context.text.bodySmall),
                ],
              ],
            ),
          ),
          trailing ??
              Icon(
                Icons.chevron_right_rounded,
                color: context.tones.inkSoft.withValues(alpha: 0.7),
              ),
        ],
      ),
    ),
  );
}
