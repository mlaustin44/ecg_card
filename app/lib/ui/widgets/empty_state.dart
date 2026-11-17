// Friendly empty / waiting state: soft icon blob, title, message, action.
import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class EmptyState extends StatelessWidget {
  const EmptyState({
    super.key,
    required this.icon,
    required this.title,
    this.message,
    this.action,
    this.color = AppColors.indigo,
    this.art,
  });

  final IconData icon;
  final String title;
  final String? message;

  /// Usually a [PrimaryButton] or [SoftButton].
  final Widget? action;
  final Color color;

  /// Replaces the icon blob with an illustration.
  final Widget? art;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(horizontal: AppSpace.l, vertical: AppSpace.xl),
    child: Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        art ??
            Container(
              width: 96,
              height: 96,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                gradient: RadialGradient(
                  colors: [
                    context.tones.soft(color, context.isDark ? 0.30 : 0.22),
                    context.tones.soft(color, context.isDark ? 0.10 : 0.06),
                  ],
                ),
              ),
              child: Icon(icon, size: 42, color: color),
            ),
        const SizedBox(height: AppSpace.l),
        Text(title, textAlign: TextAlign.center, style: context.text.titleLarge),
        if (message != null) ...[
          const SizedBox(height: AppSpace.s),
          Text(message!, textAlign: TextAlign.center, style: context.text.bodyMedium),
        ],
        if (action != null) ...[const SizedBox(height: AppSpace.l), action!],
      ],
    ),
  );
}
