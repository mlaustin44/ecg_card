// Ripples spreading from a small card glyph: "looking for your card".
import 'package:material_ui/material_ui.dart';

import '../theme.dart';

class ScanningPulse extends StatefulWidget {
  const ScanningPulse({super.key, this.size = 200, this.active = true, this.color});
  final double size;
  final bool active;
  final Color? color;

  @override
  State<ScanningPulse> createState() => _ScanningPulseState();
}

class _ScanningPulseState extends State<ScanningPulse> with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 2400),
  );

  @override
  void initState() {
    super.initState();
    if (widget.active) _c.repeat();
  }

  @override
  void didUpdateWidget(ScanningPulse old) {
    super.didUpdateWidget(old);
    if (widget.active && !_c.isAnimating) _c.repeat();
    if (!widget.active && _c.isAnimating) _c.stop();
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final color = widget.color ?? context.tones.primary;
    final s = widget.size;
    return SizedBox.square(
      dimension: s,
      child: AnimatedBuilder(
        animation: _c,
        builder: (_, child) => Stack(
          alignment: Alignment.center,
          children: [
            for (var i = 0; i < 3; i++)
              Builder(
                builder: (_) {
                  // Three ripples, evenly staggered; still frames show them spread out.
                  final t = widget.active ? (_c.value + i / 3) % 1.0 : (i + 1) / 4;
                  final d = s * (0.32 + 0.68 * Curves.easeOut.transform(t));
                  return Container(
                    width: d,
                    height: d,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: color.withValues(alpha: 0.10 * (1 - t)),
                      border: Border.all(
                        color: color.withValues(alpha: 0.30 * (1 - t)),
                        width: 1.5,
                      ),
                    ),
                  );
                },
              ),
            child!,
          ],
        ),
        child: Container(
          width: s * 0.32,
          height: s * 0.32,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            gradient: AppGradients.checkIn,
            boxShadow: [
              BoxShadow(
                color: AppColors.indigo.withValues(alpha: 0.35),
                blurRadius: 20,
                offset: const Offset(0, 6),
              ),
            ],
          ),
          child: Icon(Icons.bluetooth_searching_rounded, color: Colors.white, size: s * 0.14),
        ),
      ),
    );
  }
}
