// Small pill showing the card: link state dot, name, battery.
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:material_ui/material_ui.dart';

import '../../device/card_link.dart';
import '../../state/device_providers.dart';
import '../theme.dart';

class ConnectionPill extends StatelessWidget {
  const ConnectionPill({
    super.key,
    required this.name,
    required this.state,
    this.batteryPct,
    this.charging = false,
    this.onTap,
  });

  /// Card name, or null when no card has been chosen yet.
  final String? name;
  final LinkState state;
  final int? batteryPct;
  final bool charging;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    final (dot, label) = switch ((name, state)) {
      (null, _) => (tones.primary, 'Add your card'),
      (_, LinkState.connected) => (AppColors.breath, name!),
      (_, LinkState.connecting) => (AppColors.sun, 'Connecting…'),
      (_, LinkState.disconnected) => (tones.inkSoft.withValues(alpha: 0.6), 'Not connected'),
    };
    return Semantics(
      button: onTap != null,
      label: name == null
          ? 'Add your card'
          : '$label${batteryPct != null ? ', battery $batteryPct percent' : ''}',
      excludeSemantics: true,
      child: Material(
        color: tones.card,
        shape: StadiumBorder(side: BorderSide(color: tones.line)),
        child: InkWell(
          onTap: onTap,
          customBorder: const StadiumBorder(),
          child: AnimatedSize(
            duration: AppMotion.medium,
            curve: AppMotion.curve,
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 14, 8),
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  if (name == null)
                    Icon(Icons.add_rounded, size: 18, color: tones.primary)
                  else
                    _Dot(color: dot, pulsing: state == LinkState.connecting),
                  const SizedBox(width: AppSpace.s),
                  Flexible(
                    child: Text(
                      label,
                      overflow: TextOverflow.ellipsis,
                      style: context.text.labelMedium?.copyWith(
                        color: name == null ? tones.primary : tones.ink,
                      ),
                    ),
                  ),
                  if (name != null && state == LinkState.connected && batteryPct != null) ...[
                    const SizedBox(width: AppSpace.s + 2),
                    BatteryGlyph(pct: batteryPct!, charging: charging),
                    const SizedBox(width: 4),
                    Text('$batteryPct%', style: context.text.labelSmall),
                  ],
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _Dot extends StatefulWidget {
  const _Dot({required this.color, required this.pulsing});
  final Color color;
  final bool pulsing;

  @override
  State<_Dot> createState() => _DotState();
}

class _DotState extends State<_Dot> with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 900),
  );

  @override
  void initState() {
    super.initState();
    if (widget.pulsing) _c.repeat(reverse: true);
  }

  @override
  void didUpdateWidget(_Dot old) {
    super.didUpdateWidget(old);
    if (widget.pulsing && !_c.isAnimating) _c.repeat(reverse: true);
    if (!widget.pulsing && _c.isAnimating) _c.value = 1;
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => FadeTransition(
    opacity: widget.pulsing
        ? Tween(begin: 0.35, end: 1.0).animate(_c)
        : const AlwaysStoppedAnimation(1),
    child: AnimatedContainer(
      duration: AppMotion.medium,
      width: 9,
      height: 9,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: widget.color,
        boxShadow: [BoxShadow(color: widget.color.withValues(alpha: 0.45), blurRadius: 6)],
      ),
    ),
  );
}

/// A tiny rounded battery outline filled to [pct].
class BatteryGlyph extends StatelessWidget {
  const BatteryGlyph({super.key, required this.pct, this.charging = false, this.width = 22});
  final int pct;
  final bool charging;
  final double width;

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    final fill = charging
        ? AppColors.breath
        : pct <= 15
        ? AppColors.heart
        : tones.ink.withValues(alpha: 0.75);
    final h = width * 0.5;
    return SizedBox(
      width: width + 2,
      height: h,
      child: Row(
        children: [
          Container(
            width: width,
            height: h,
            padding: const EdgeInsets.all(1.6),
            decoration: BoxDecoration(
              border: Border.all(color: tones.inkSoft.withValues(alpha: 0.7), width: 1.3),
              borderRadius: BorderRadius.circular(h * 0.32),
            ),
            child: Align(
              alignment: Alignment.centerLeft,
              child: FractionallySizedBox(
                widthFactor: (pct.clamp(4, 100)) / 100,
                heightFactor: 1,
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: fill,
                    borderRadius: BorderRadius.circular(h * 0.18),
                  ),
                ),
              ),
            ),
          ),
          Container(
            width: 2,
            height: h * 0.4,
            decoration: BoxDecoration(
              color: tones.inkSoft.withValues(alpha: 0.7),
              borderRadius: const BorderRadius.horizontal(right: Radius.circular(1)),
            ),
          ),
        ],
      ),
    );
  }
}

/// [ConnectionPill] wired to the current card.
class CardConnectionPill extends ConsumerWidget {
  const CardConnectionPill({super.key, this.onTap});
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final card = ref.watch(currentCardProvider);
    final link =
        ref.watch(linkStateProvider).value ?? card?.currentLinkState ?? LinkState.disconnected;
    final status = ref.watch(cardStatusProvider).value;
    return ConnectionPill(
      name: card?.name,
      state: link,
      batteryPct: status?.battPct,
      charging: status?.charging ?? false,
      onTap: onTap,
    );
  }
}
