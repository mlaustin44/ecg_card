// Home: greeting, card pill, Check in and Breathe, the latest result, links.
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

import '../../app.dart';
import '../../data/session_record.dart';
import '../../state/data_providers.dart';
import '../../state/device_providers.dart';
import '../theme.dart';
import '../widgets/calm_score.dart';
import '../widgets/connection_pill.dart';
import '../widgets/format.dart';
import '../widgets/section_card.dart';
import 'welcome_screen.dart';

class HomeScreen extends ConsumerWidget {
  const HomeScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // First launch: the welcome pages come first.
    final seen = ref.watch(welcomeSeenProvider);
    if (seen.value == false) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (context.mounted) context.go(Routes.welcome);
      });
    }
    if (seen.value != true) return const Scaffold(body: SizedBox.shrink());

    final now = ref.watch(clockProvider)();
    final card = ref.watch(currentCardProvider);
    final sessions = ref.watch(sessionsProvider);
    final latest = sessions.value?.firstOrNull;

    return Scaffold(
      body: SafeArea(
        bottom: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(AppSpace.page, AppSpace.m, AppSpace.page, AppSpace.xl),
          children: [
            Row(
              children: [
                const _Mark(),
                const Spacer(),
                CardConnectionPill(
                  onTap: () => context.push(card == null ? Routes.connect : Routes.card),
                ),
              ],
            ),
            const SizedBox(height: AppSpace.l),
            Text(longDate(now).toUpperCase(), style: AppText.eyebrow(context)),
            const SizedBox(height: AppSpace.xs + 2),
            Text(greetingFor(now), style: context.text.headlineLarge),
            const SizedBox(height: AppSpace.xs),
            Text(
              latest == null
                  ? 'Shall we take a first look at your heart?'
                  : 'How is your heart doing today?',
              style: context.text.bodyLarge?.copyWith(color: context.tones.inkSoft),
            ),
            const SizedBox(height: AppSpace.l),
            if (card == null) ...[
              _ConnectNudge(onTap: () => context.push(Routes.connect)),
              const SizedBox(height: AppSpace.m),
            ],
            _CheckInCard(onTap: () => context.push(card == null ? Routes.connect : Routes.checkIn)),
            const SizedBox(height: AppSpace.m),
            _BreatheCard(onTap: () => context.push(Routes.breathe)),
            const SizedBox(height: AppSpace.m),
            if (latest != null)
              _LatestCard(
                record: latest,
                now: now,
                onTap: () => context.push(Routes.session(latest.id)),
              )
            else if (sessions.hasValue)
              const _FirstTimeCard(),
            const SizedBox(height: AppSpace.m),
            SectionCard(
              padding: const EdgeInsets.symmetric(horizontal: AppSpace.m - 4, vertical: AppSpace.s),
              child: Column(
                children: [
                  NavRow(
                    icon: Icons.calendar_month_rounded,
                    color: context.tones.primary,
                    title: 'History',
                    subtitle: sessions.value == null || sessions.value!.isEmpty
                        ? 'Your recordings, over time'
                        : '${sessions.value!.length} ${sessions.value!.length == 1 ? 'recording' : 'recordings'} so far',
                    onTap: () => context.push(Routes.history),
                  ),
                  Divider(indent: 58, color: context.tones.line),
                  NavRow(
                    icon: Icons.credit_card_rounded,
                    color: AppColors.sun,
                    title: 'Your card',
                    subtitle: card == null ? 'Not set up yet' : 'Battery, name and help',
                    onTap: () => context.push(card == null ? Routes.connect : Routes.card),
                  ),
                ],
              ),
            ),
            const SizedBox(height: AppSpace.l),
            Text(
              'A wellness companion for curiosity and calm,\nnot a medical device.',
              textAlign: TextAlign.center,
              style: context.text.bodySmall?.copyWith(
                color: context.tones.inkSoft.withValues(alpha: 0.8),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _CheckInCard extends StatelessWidget {
  const _CheckInCard({required this.onTap});
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => SectionCard(
    gradient: AppGradients.checkIn,
    onTap: onTap,
    padding: EdgeInsets.zero,
    child: Stack(
      children: [
        // A soft heartbeat line drawn behind the text.
        Positioned.fill(child: CustomPaint(painter: _CardBeat())),
        Padding(
          padding: const EdgeInsets.all(AppSpace.l - 2),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  const IconWell(
                    icon: Icons.favorite_rounded,
                    color: Colors.white,
                    onGradient: true,
                    size: 48,
                  ),
                  const Spacer(),
                  _Chip(text: '1 minute'),
                ],
              ),
              const SizedBox(height: AppSpace.l + 8),
              Text('Check in', style: context.text.headlineMedium?.copyWith(color: Colors.white)),
              const SizedBox(height: AppSpace.xs),
              Text(
                'Rest your thumbs on the card and see\nhow your heart is doing right now.',
                style: context.text.bodyMedium?.copyWith(
                  color: Colors.white.withValues(alpha: 0.85),
                ),
              ),
              const SizedBox(height: AppSpace.m + 4),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 11),
                decoration: BoxDecoration(
                  color: Colors.white,
                  borderRadius: BorderRadius.circular(AppRadius.pill),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      'Start',
                      style: context.text.labelLarge?.copyWith(color: AppColors.indigo),
                    ),
                    const SizedBox(width: 6),
                    const Icon(Icons.arrow_forward_rounded, size: 18, color: AppColors.indigo),
                  ],
                ),
              ),
            ],
          ),
        ),
      ],
    ),
  );
}

class _CardBeat extends CustomPainter {
  @override
  void paint(Canvas canvas, Size s) {
    final y0 = s.height * 0.40;
    final xs = [0.38, 0.62, 0.66, 0.70, 0.74, 0.78, 0.82, 1.0];
    final ys = [0.0, 0.0, -0.10, 0.22, -0.42, 0.12, 0.0, 0.0];
    final p = Path()..moveTo(s.width * xs[0], y0);
    for (var i = 1; i < xs.length; i++) {
      p.lineTo(s.width * xs[i], y0 + ys[i] * s.height * 0.35);
    }
    canvas.drawPath(
      p,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 3
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round
        ..shader = LinearGradient(
          colors: [Colors.white.withValues(alpha: 0), Colors.white.withValues(alpha: 0.35)],
        ).createShader(Rect.fromLTWH(s.width * 0.38, 0, s.width * 0.62, s.height)),
    );
  }

  @override
  bool shouldRepaint(_CardBeat old) => false;
}

class _Chip extends StatelessWidget {
  const _Chip({required this.text});
  final String text;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
    decoration: BoxDecoration(
      color: Colors.white.withValues(alpha: 0.2),
      borderRadius: BorderRadius.circular(AppRadius.pill),
    ),
    child: Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        const Icon(Icons.schedule_rounded, size: 15, color: Colors.white),
        const SizedBox(width: 5),
        Text(text, style: context.text.labelMedium?.copyWith(color: Colors.white)),
      ],
    ),
  );
}

class _BreatheCard extends StatelessWidget {
  const _BreatheCard({required this.onTap});
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => SectionCard(
    gradient: AppGradients.breath,
    onTap: onTap,
    child: Row(
      children: [
        const IconWell(icon: Icons.air_rounded, color: Colors.white, onGradient: true, size: 48),
        const SizedBox(width: AppSpace.m),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('Breathe', style: context.text.titleLarge?.copyWith(color: Colors.white)),
              const SizedBox(height: 2),
              Text(
                'Slow, guided breathing in time with your heart',
                style: context.text.bodySmall?.copyWith(color: Colors.white.withValues(alpha: 0.9)),
              ),
            ],
          ),
        ),
        const SizedBox(width: AppSpace.s),
        const Icon(Icons.arrow_forward_rounded, color: Colors.white),
      ],
    ),
  );
}

class _ConnectNudge extends StatelessWidget {
  const _ConnectNudge({required this.onTap});
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => SectionCard(
    color: context.tones.soft(AppColors.sun, context.isDark ? 0.16 : 0.20),
    onTap: onTap,
    child: Row(
      children: [
        const IconWell(icon: Icons.bluetooth_searching_rounded, color: Color(0xFFD08A12), size: 48),
        const SizedBox(width: AppSpace.m),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text("Let's find your card", style: context.text.titleMedium),
              const SizedBox(height: 2),
              Text('Switch it on and we\'ll connect in a moment.', style: context.text.bodySmall),
            ],
          ),
        ),
        Icon(Icons.chevron_right_rounded, color: context.tones.inkSoft),
      ],
    ),
  );
}

class _LatestCard extends StatelessWidget {
  const _LatestCard({required this.record, required this.now, required this.onTap});
  final SessionRecord record;
  final DateTime now;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final hr = record.stats?.meanHr.round();
    final kind = record.kind == SessionKind.breathing ? 'Last breathing session' : 'Last check-in';
    return Hero(
      tag: 'session-${record.id}',
      child: SectionCard(
        onTap: onTap,
        title: kind,
        trailing: Text(friendlyWhen(record.start, now), style: context.text.labelSmall),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    crossAxisAlignment: CrossAxisAlignment.baseline,
                    textBaseline: TextBaseline.alphabetic,
                    children: [
                      Text(
                        hr?.toString() ?? '--',
                        style: AppText.numeral(44, color: context.tones.ink),
                      ),
                      const SizedBox(width: 6),
                      Text(
                        'bpm',
                        style: context.text.titleSmall?.copyWith(color: context.tones.inkSoft),
                      ),
                    ],
                  ),
                  const SizedBox(height: 2),
                  Text('average heart rate', style: context.text.bodySmall),
                ],
              ),
            ),
            if (record.calmScore != null)
              Column(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Text(
                    calmWord(record.calmScore!),
                    style: context.text.titleMedium?.copyWith(color: AppColors.breath),
                  ),
                  const SizedBox(height: AppSpace.s),
                  CalmDots(score: record.calmScore!, dot: 12),
                ],
              ),
          ],
        ),
      ),
    );
  }
}

class _FirstTimeCard extends StatelessWidget {
  const _FirstTimeCard();

  @override
  Widget build(BuildContext context) => SectionCard(
    child: Row(
      children: [
        IconWell(icon: Icons.auto_awesome_rounded, color: AppColors.sun, size: 44),
        const SizedBox(width: AppSpace.m),
        Expanded(
          child: Text(
            'Your first check-in will appear here, with your heart rate and calm score.',
            style: context.text.bodyMedium,
          ),
        ),
      ],
    ),
  );
}

/// The little app mark: a heart in a soft coral circle.
class _Mark extends StatelessWidget {
  const _Mark();

  @override
  Widget build(BuildContext context) => Container(
    width: 40,
    height: 40,
    decoration: const BoxDecoration(shape: BoxShape.circle, gradient: AppGradients.heart),
    child: const Icon(Icons.favorite_rounded, color: Colors.white, size: 20),
  );
}
