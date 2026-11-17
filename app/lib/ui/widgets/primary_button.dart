// The big friendly pill button (gradient fill, soft glow, gentle press).
import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class PrimaryButton extends StatefulWidget {
  const PrimaryButton({
    super.key,
    required this.label,
    required this.onPressed,
    this.icon,
    this.gradient,
    this.busy = false,
    this.expand = true,
  });

  final String label;
  final VoidCallback? onPressed;
  final IconData? icon;

  /// Defaults to the indigo check-in gradient.
  final Gradient? gradient;

  /// Shows a small spinner and ignores taps.
  final bool busy;
  final bool expand;

  @override
  State<PrimaryButton> createState() => _PrimaryButtonState();
}

class _PrimaryButtonState extends State<PrimaryButton> {
  bool _down = false;

  @override
  Widget build(BuildContext context) {
    final enabled = widget.onPressed != null && !widget.busy;
    final gradient = widget.gradient ?? AppGradients.checkIn;
    final glow = gradient.colors.first;
    final child = Row(
      mainAxisSize: widget.expand ? MainAxisSize.max : MainAxisSize.min,
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        if (widget.busy)
          const SizedBox(
            width: 20,
            height: 20,
            child: CircularProgressIndicator(
              strokeWidth: 2.5,
              color: Colors.white,
              backgroundColor: Colors.white24,
            ),
          )
        else if (widget.icon != null)
          Icon(widget.icon, color: Colors.white, size: 22),
        if (widget.busy || widget.icon != null) const SizedBox(width: AppSpace.s + 2),
        Flexible(
          child: Text(
            widget.label,
            overflow: TextOverflow.ellipsis,
            style: context.text.labelLarge?.copyWith(color: Colors.white, fontSize: 17),
          ),
        ),
      ],
    );
    return Semantics(
      button: true,
      enabled: enabled,
      label: widget.label,
      excludeSemantics: true,
      child: GestureDetector(
        onTapDown: enabled ? (_) => setState(() => _down = true) : null,
        onTapUp: enabled ? (_) => setState(() => _down = false) : null,
        onTapCancel: enabled ? () => setState(() => _down = false) : null,
        onTap: enabled ? widget.onPressed : null,
        child: AnimatedScale(
          scale: _down ? 0.97 : 1,
          duration: AppMotion.fast,
          curve: AppMotion.curve,
          child: AnimatedOpacity(
            opacity: widget.onPressed == null ? 0.45 : 1,
            duration: AppMotion.fast,
            child: Container(
              height: 58,
              padding: const EdgeInsets.symmetric(horizontal: AppSpace.l),
              decoration: BoxDecoration(
                gradient: gradient,
                borderRadius: BorderRadius.circular(AppRadius.pill),
                boxShadow: [
                  BoxShadow(
                    color: glow.withValues(alpha: _down ? 0.18 : 0.32),
                    blurRadius: _down ? 10 : 20,
                    offset: Offset(0, _down ? 3 : 8),
                  ),
                ],
              ),
              child: child,
            ),
          ),
        ),
      ),
    );
  }
}

/// Quiet secondary action, same height as [PrimaryButton].
class SoftButton extends StatelessWidget {
  const SoftButton({
    super.key,
    required this.label,
    required this.onPressed,
    this.icon,
    this.color,
  });
  final String label;
  final VoidCallback? onPressed;
  final IconData? icon;
  final Color? color;

  @override
  Widget build(BuildContext context) {
    final c = color ?? context.tones.primary;
    return SizedBox(
      height: 54,
      child: TextButton.icon(
        onPressed: onPressed,
        icon: icon == null ? null : Icon(icon, size: 20),
        label: Text(label),
        style: TextButton.styleFrom(
          foregroundColor: c,
          backgroundColor: context.tones.soft(c, context.isDark ? 0.18 : 0.10),
          padding: const EdgeInsets.symmetric(horizontal: AppSpace.l),
        ),
      ),
    );
  }
}
