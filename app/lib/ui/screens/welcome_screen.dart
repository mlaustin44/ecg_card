// First-launch welcome: what the card is, how to hold it, privacy. Then on
// to finding the card. Shown once (shared_preferences flag).
import 'dart:math' as math;

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../app.dart';
import '../theme.dart';
import '../widgets/primary_button.dart';
import '../widgets/thumbs_illustration.dart';

const welcomeSeenKey = 'welcome_seen_v1';

/// Whether the welcome pages have been seen (false on first launch).
final welcomeSeenProvider = FutureProvider<bool>((ref) async {
  final prefs = await SharedPreferences.getInstance();
  return prefs.getBool(welcomeSeenKey) ?? false;
});

class WelcomeScreen extends ConsumerStatefulWidget {
  const WelcomeScreen({super.key});

  @override
  ConsumerState<WelcomeScreen> createState() => _WelcomeScreenState();
}

class _WelcomeScreenState extends ConsumerState<WelcomeScreen> {
  final _pages = PageController();
  int _page = 0;

  static const _count = 3;

  @override
  void dispose() {
    _pages.dispose();
    super.dispose();
  }

  Future<void> _done() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(welcomeSeenKey, true);
    ref.invalidate(welcomeSeenProvider);
    if (mounted) context.go(Routes.connect);
  }

  void _next() {
    if (_page == _count - 1) {
      _done();
    } else {
      _pages.nextPage(duration: AppMotion.slow, curve: AppMotion.curve);
    }
  }

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    return Scaffold(
      body: SafeArea(
        child: Column(
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(AppSpace.page, AppSpace.s, AppSpace.s, 0),
              child: Row(
                children: [
                  Icon(Icons.favorite_rounded, color: AppColors.heart, size: 20),
                  const SizedBox(width: AppSpace.s),
                  Text('ECG Card', style: context.text.titleSmall),
                  const Spacer(),
                  AnimatedOpacity(
                    opacity: _page < _count - 1 ? 1 : 0,
                    duration: AppMotion.fast,
                    child: TextButton(
                      onPressed: _page < _count - 1 ? _done : null,
                      style: TextButton.styleFrom(foregroundColor: tones.inkSoft),
                      child: const Text('Skip'),
                    ),
                  ),
                ],
              ),
            ),
            Expanded(
              child: PageView(
                controller: _pages,
                onPageChanged: (p) => setState(() => _page = p),
                children: const [
                  _Page(
                    art: _HeartArt(),
                    title: 'Hello! Meet your\nECG Card',
                    body:
                        'A little card, handmade for you. Rest your thumbs on it and it listens to '
                        'your heartbeat, then shows it here, gently.',
                  ),
                  _Page(
                    art: ThumbsIllustration(width: 300),
                    title: 'How to hold it',
                    body: null,
                    tips: [
                      (Icons.touch_app_rounded, 'One thumb on each of the two round pads'),
                      (Icons.spa_rounded, 'Light touch, shoulders relaxed'),
                      (Icons.self_improvement_rounded, 'Sit still and breathe normally'),
                    ],
                  ),
                  _Page(
                    art: _PrivacyArt(),
                    title: 'Yours, and only yours',
                    body:
                        'Everything stays on your phone. No account, no cloud, nothing shared '
                        'unless you choose to share it.',
                  ),
                ],
              ),
            ),
            _Dots(count: _count, index: _page),
            Padding(
              padding: const EdgeInsets.fromLTRB(
                AppSpace.page,
                AppSpace.l,
                AppSpace.page,
                AppSpace.l,
              ),
              child: PrimaryButton(
                label: _page == _count - 1 ? 'Find my card' : 'Next',
                icon: _page == _count - 1 ? Icons.bluetooth_searching_rounded : null,
                onPressed: _next,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _Page extends StatelessWidget {
  const _Page({required this.art, required this.title, required this.body, this.tips = const []});
  final Widget art;
  final String title;
  final String? body;
  final List<(IconData, String)> tips;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, c) => SingleChildScrollView(
      padding: const EdgeInsets.symmetric(horizontal: AppSpace.page + 8),
      child: ConstrainedBox(
        constraints: BoxConstraints(minHeight: c.maxHeight),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            SizedBox(
              height: math.min(300, c.maxHeight * 0.48),
              child: Center(child: art),
            ),
            const SizedBox(height: AppSpace.l),
            Text(title, textAlign: TextAlign.center, style: context.text.headlineLarge),
            const SizedBox(height: AppSpace.m),
            if (body != null)
              Text(
                body!,
                textAlign: TextAlign.center,
                style: context.text.bodyLarge?.copyWith(color: context.tones.inkSoft),
              ),
            for (final (icon, text) in tips)
              Padding(
                padding: const EdgeInsets.only(top: AppSpace.s + 2),
                child: Row(
                  children: [
                    Container(
                      width: 36,
                      height: 36,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        color: context.tones.soft(AppColors.breath, 0.16),
                      ),
                      child: Icon(icon, size: 19, color: AppColors.breath),
                    ),
                    const SizedBox(width: AppSpace.m - 2),
                    Expanded(child: Text(text, style: context.text.bodyLarge)),
                  ],
                ),
              ),
          ],
        ),
      ),
    ),
  );
}

class _Dots extends StatelessWidget {
  const _Dots({required this.count, required this.index});
  final int count, index;

  @override
  Widget build(BuildContext context) => Row(
    mainAxisAlignment: MainAxisAlignment.center,
    children: [
      for (var i = 0; i < count; i++)
        AnimatedContainer(
          duration: AppMotion.medium,
          curve: AppMotion.curve,
          margin: const EdgeInsets.symmetric(horizontal: 4),
          width: i == index ? 24 : 8,
          height: 8,
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(AppRadius.pill),
            color: i == index ? context.tones.primary : context.tones.line,
          ),
        ),
    ],
  );
}

/// Soft concentric glow with a big gradient heart and a heartbeat line.
class _HeartArt extends StatelessWidget {
  const _HeartArt();

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    return SizedBox.square(
      dimension: 260,
      child: Stack(
        alignment: Alignment.center,
        children: [
          for (final (d, a) in [(260.0, 0.06), (200.0, 0.10), (144.0, 0.16)])
            Container(
              width: d,
              height: d,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: tones.soft(AppColors.heart, a * (context.isDark ? 1.6 : 1)),
              ),
            ),
          ShaderMask(
            shaderCallback: (r) => AppGradients.heart.createShader(r),
            child: const Icon(Icons.favorite_rounded, size: 112, color: Colors.white),
          ),
          CustomPaint(size: const Size(220, 60), painter: _BeatLine(tones.card)),
        ],
      ),
    );
  }
}

class _BeatLine extends CustomPainter {
  _BeatLine(this.color);
  final Color color;

  @override
  void paint(Canvas canvas, Size s) {
    final xs = [0.0, 0.34, 0.40, 0.46, 0.52, 0.58, 0.64, 1.0];
    final ys = [0.55, 0.55, 0.40, 0.95, 0.05, 0.70, 0.55, 0.55];
    final p = Path();
    for (var i = 0; i < xs.length; i++) {
      final o = Offset(s.width * xs[i], s.height * ys[i]);
      i == 0 ? p.moveTo(o.dx, o.dy) : p.lineTo(o.dx, o.dy);
    }
    final paint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 4.5
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    // Only the middle shows across the heart; the ends fade into the glow.
    paint.shader = LinearGradient(
      colors: [color.withValues(alpha: 0), color, color, color.withValues(alpha: 0)],
      stops: const [0.12, 0.3, 0.7, 0.88],
    ).createShader(Offset.zero & s);
    canvas.drawPath(p, paint);
  }

  @override
  bool shouldRepaint(_BeatLine old) => old.color != color;
}

class _PrivacyArt extends StatelessWidget {
  const _PrivacyArt();

  @override
  Widget build(BuildContext context) {
    final tones = context.tones;
    return SizedBox.square(
      dimension: 260,
      child: Stack(
        alignment: Alignment.center,
        children: [
          for (final (d, a) in [(260.0, 0.06), (200.0, 0.10)])
            Container(
              width: d,
              height: d,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: tones.soft(AppColors.breath, a * (context.isDark ? 1.6 : 1)),
              ),
            ),
          // A phone holding a heart, with a little lock badge.
          Container(
            width: 104,
            height: 168,
            decoration: BoxDecoration(
              color: tones.card,
              borderRadius: BorderRadius.circular(24),
              border: Border.all(color: tones.line, width: 2),
              boxShadow: [
                BoxShadow(color: tones.shadow, blurRadius: 20, offset: const Offset(0, 8)),
              ],
            ),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                const Icon(Icons.favorite_rounded, size: 40, color: AppColors.heart),
                const SizedBox(height: 10),
                for (final w in [56.0, 40.0])
                  Container(
                    width: w,
                    height: 6,
                    margin: const EdgeInsets.only(top: 6),
                    decoration: BoxDecoration(
                      color: tones.line,
                      borderRadius: BorderRadius.circular(3),
                    ),
                  ),
              ],
            ),
          ),
          Positioned(
            right: 62,
            bottom: 52,
            child: Container(
              width: 56,
              height: 56,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                gradient: AppGradients.breath,
                border: Border.all(color: tones.card, width: 4),
              ),
              child: const Icon(Icons.lock_rounded, color: Colors.white, size: 24),
            ),
          ),
        ],
      ),
    );
  }
}
