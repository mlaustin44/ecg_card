// The 1-minute check-in: place thumbs → settling → live trace, heart rate and
// a timer ring → results. Contact loss pauses the timer with a gentle nudge.
import 'dart:async';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

import '../../app.dart';
import '../../data/session_record.dart';
import '../../state/data_providers.dart';
import '../../state/live_session.dart';
import '../theme.dart';
import '../widgets/ecg_trace_view.dart';
import '../widgets/empty_state.dart';
import '../widgets/format.dart';
import '../widgets/heart_rate_number.dart';
import '../widgets/primary_button.dart';
import '../widgets/section_card.dart';
import '../widgets/thumbs_illustration.dart';
import '../widgets/timer_ring.dart';

const checkInLength = Duration(seconds: 60);

class CheckInScreen extends ConsumerStatefulWidget {
  const CheckInScreen({super.key});

  @override
  ConsumerState<CheckInScreen> createState() => _CheckInScreenState();
}

/// What the screen shows, derived from the live session.
enum _View { starting, place, settling, live, short, error }

class _CheckInScreenState extends ConsumerState<CheckInScreen> {
  late final LiveSessionApi _api = ref.read(liveSessionProvider);
  StreamSubscription<LiveSession>? _sub;

  /// A session left over from before (finished, failed) is not shown: the
  /// screen starts fresh and begin() takes it from there.
  late LiveSession _s = switch (_api.state.phase) {
    LivePhase.idle ||
    LivePhase.finished ||
    LivePhase.error => const LiveSession(phase: LivePhase.starting),
    _ => _api.state,
  };
  bool _navigated = false;

  @override
  void initState() {
    super.initState();
    _sub = _api.changes.listen(_onChange);
    WidgetsBinding.instance.addPostFrameCallback((_) => _begin());
  }

  void _begin() {
    final p = _api.state.phase;
    if (p == LivePhase.idle || p == LivePhase.finished || p == LivePhase.error) {
      _api.begin(SessionKind.checkIn, target: checkInLength);
    }
  }

  void _onChange(LiveSession s) {
    if (!mounted) return;
    setState(() => _s = s);
    final record = s.record;
    if (s.phase == LivePhase.finished && s.saved && record != null && !_navigated) {
      _navigated = true;
      context.pushReplacement(Routes.session(record.id));
    }
  }

  @override
  void dispose() {
    _sub?.cancel();
    final p = _api.state.phase;
    if (p != LivePhase.idle && p != LivePhase.finished) _api.cancel();
    super.dispose();
  }

  _View get _view => switch (_s.phase) {
    LivePhase.idle || LivePhase.starting => _View.starting,
    LivePhase.waitContact => _s.elapsed > Duration.zero ? _View.live : _View.place,
    LivePhase.settling => _s.elapsed > Duration.zero ? _View.live : _View.settling,
    LivePhase.running => _View.live,
    LivePhase.finished => _s.saved ? _View.live : _View.short,
    LivePhase.error => _View.error,
  };

  Future<void> _close() async {
    final measuring = _s.phase == LivePhase.running || _s.elapsed > const Duration(seconds: 10);
    if (measuring && _s.phase != LivePhase.finished) {
      final stop = await showDialog<bool>(
        context: context,
        builder: (c) => AlertDialog(
          title: const Text('Stop this check-in?'),
          content: const Text("What's been measured so far won't be kept."),
          actions: [
            TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Keep going')),
            TextButton(onPressed: () => Navigator.pop(c, true), child: const Text('Stop')),
          ],
        ),
      );
      if (stop != true || !mounted) return;
    }
    await _api.cancel();
    if (!mounted) return;
    context.canPop() ? context.pop() : context.go(Routes.home);
  }

  @override
  Widget build(BuildContext context) {
    final view = _view;
    final Widget body = switch (view) {
      _View.starting => const _Starting(key: ValueKey('starting')),
      _View.place => const _Place(key: ValueKey('place')),
      _View.settling => _Settling(key: const ValueKey('settling'), progress: _s.settleProgress),
      _View.live => _Live(key: const ValueKey('live'), s: _s, ecg: _api),
      _View.short => _TooShort(key: const ValueKey('short'), onAgain: _again, onHome: _home),
      _View.error => _Problem(
        key: const ValueKey('error'),
        message: _s.error ?? 'Something got in the way. Let\'s try that again.',
        onAgain: _again,
        onHome: _home,
      ),
    };
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) _close();
      },
      child: Scaffold(
        appBar: AppBar(
          leading: IconButton(
            tooltip: 'Close',
            icon: const Icon(Icons.close_rounded),
            onPressed: _close,
          ),
          title: const Text('Check-in'),
        ),
        body: SafeArea(
          top: false,
          child: AnimatedSwitcher(
            duration: AppMotion.medium,
            switchInCurve: AppMotion.curve,
            switchOutCurve: Curves.easeIn,
            transitionBuilder: (child, a) => FadeTransition(
              opacity: a,
              child: SlideTransition(
                position: Tween(begin: const Offset(0, 0.03), end: Offset.zero).animate(a),
                child: child,
              ),
            ),
            child: body,
          ),
        ),
      ),
    );
  }

  void _again() => _api.begin(SessionKind.checkIn, target: checkInLength);

  void _home() => context.canPop() ? context.pop() : context.go(Routes.home);
}

class _Centered extends StatelessWidget {
  const _Centered({required this.children});
  final List<Widget> children;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (_, c) => SingleChildScrollView(
      padding: const EdgeInsets.symmetric(horizontal: AppSpace.page + 8),
      child: ConstrainedBox(
        constraints: BoxConstraints(minHeight: c.maxHeight),
        child: Column(mainAxisAlignment: MainAxisAlignment.center, children: children),
      ),
    ),
  );
}

class _Starting extends StatelessWidget {
  const _Starting({super.key});

  @override
  Widget build(BuildContext context) => _Centered(
    children: [
      const ThumbsIllustration(width: 280, animate: false),
      const SizedBox(height: AppSpace.xl),
      Text('Waking up your card…', style: context.text.headlineSmall),
      const SizedBox(height: AppSpace.m),
      SizedBox(
        width: 120,
        child: ClipRRect(
          borderRadius: BorderRadius.circular(AppRadius.pill),
          child: const LinearProgressIndicator(minHeight: 6),
        ),
      ),
      const SizedBox(height: 80),
    ],
  );
}

class _Place extends StatelessWidget {
  const _Place({super.key});

  @override
  Widget build(BuildContext context) => _Centered(
    children: [
      const ThumbsIllustration(width: 300),
      const SizedBox(height: AppSpace.xl),
      Text(
        'Place both thumbs\non the pads',
        textAlign: TextAlign.center,
        style: context.text.headlineMedium,
      ),
      const SizedBox(height: AppSpace.m - 4),
      Text(
        'Hold the card flat, one thumb on each round pad. A light touch is plenty. Let your shoulders drop.',
        textAlign: TextAlign.center,
        style: context.text.bodyLarge?.copyWith(color: context.tones.inkSoft),
      ),
      const SizedBox(height: 80),
    ],
  );
}

class _Settling extends StatelessWidget {
  const _Settling({super.key, required this.progress});
  final double progress;

  @override
  Widget build(BuildContext context) => _Centered(
    children: [
      const ThumbsIllustration(width: 300, touching: true),
      const SizedBox(height: AppSpace.xl),
      Text('Lovely. Hold still…', textAlign: TextAlign.center, style: context.text.headlineMedium),
      const SizedBox(height: AppSpace.m - 4),
      Text(
        'The card is settling in to your signal. Breathe normally.',
        textAlign: TextAlign.center,
        style: context.text.bodyLarge?.copyWith(color: context.tones.inkSoft),
      ),
      const SizedBox(height: AppSpace.l),
      SizedBox(
        width: 200,
        child: ClipRRect(
          borderRadius: BorderRadius.circular(AppRadius.pill),
          child: TweenAnimationBuilder<double>(
            tween: Tween(end: progress.clamp(0, 1)),
            duration: AppMotion.medium,
            builder: (_, v, _) =>
                LinearProgressIndicator(value: v, minHeight: 8, color: AppColors.breath),
          ),
        ),
      ),
      const SizedBox(height: 80),
    ],
  );
}

class _Live extends StatelessWidget {
  const _Live({super.key, required this.s, required this.ecg});
  final LiveSession s;
  final LiveSessionApi ecg;

  @override
  Widget build(BuildContext context) {
    final target = s.target ?? checkInLength;
    final progress = s.elapsed.inMilliseconds / target.inMilliseconds;
    final left = target - s.elapsed;
    final linkLost = s.linkLost;
    final lost =
        !linkLost &&
        (s.phase == LivePhase.waitContact ||
            (s.status?.noContact ?? false) && s.phase == LivePhase.running);
    final resettling = !linkLost && s.phase == LivePhase.settling;
    final paused = linkLost || lost || resettling;
    final tones = context.tones;
    return LayoutBuilder(
      builder: (context, c) {
        final ring = (c.maxHeight * 0.40).clamp(200.0, 270.0);
        return Padding(
          padding: const EdgeInsets.fromLTRB(AppSpace.page, AppSpace.s, AppSpace.page, AppSpace.l),
          child: Column(
            children: [
              const Spacer(),
              TimerRing(
                progress: progress,
                size: ring,
                paused: paused,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    HeartRateNumber(
                      bpm: s.hrBpm ?? s.fastBpm,
                      beats: s.rr.length,
                      size: ring * 0.27,
                    ),
                    const SizedBox(height: AppSpace.s + 2),
                    AnimatedSwitcher(
                      duration: AppMotion.fast,
                      child: Text(
                        paused ? 'paused' : '${clockText(left)} left',
                        key: ValueKey(paused),
                        style: context.text.titleSmall?.copyWith(color: tones.inkSoft),
                      ),
                    ),
                  ],
                ),
              ),
              const Spacer(),
              AnimatedSwitcher(
                duration: AppMotion.medium,
                child: paused
                    ? _Nudge(
                        key: ValueKey(linkLost ? 'link' : lost),
                        icon: linkLost
                            ? Icons.bluetooth_searching_rounded
                            : lost
                            ? Icons.touch_app_rounded
                            : Icons.hourglass_top_rounded,
                        text: linkLost
                            ? 'Reconnecting to your card… keep it close by.'
                            : lost
                            ? 'Lost touch. Rest your thumbs back on the pads.'
                            : 'Nice, hold still a moment while it settles again.',
                      )
                    : Text(
                        s.rr.length < 3
                            ? 'Finding your rhythm…'
                            : 'Breathe normally and stay relaxed.',
                        key: ValueKey(s.rr.length < 3),
                        style: context.text.bodyLarge?.copyWith(color: tones.inkSoft),
                      ),
              ),
              const SizedBox(height: AppSpace.l),
              AnimatedOpacity(
                opacity: paused ? 0.35 : 1,
                duration: AppMotion.medium,
                child: SectionCard(
                  padding: const EdgeInsets.symmetric(vertical: AppSpace.m),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: AppSpace.m + 4),
                        child: Text('YOUR HEARTBEAT', style: AppText.eyebrow(context)),
                      ),
                      const SizedBox(height: AppSpace.s),
                      SizedBox(
                        height: 132,
                        width: double.infinity,
                        child: EcgTraceView(samples: ecg.ecgUv),
                      ),
                    ],
                  ),
                ),
              ),
              const Spacer(),
            ],
          ),
        );
      },
    );
  }
}

class _Nudge extends StatelessWidget {
  const _Nudge({super.key, required this.icon, required this.text});
  final IconData icon;
  final String text;

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: AppSpace.m, vertical: AppSpace.s + 4),
    decoration: BoxDecoration(
      color: context.tones.soft(AppColors.sun, context.isDark ? 0.18 : 0.22),
      borderRadius: BorderRadius.circular(AppRadius.small + 4),
    ),
    child: Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(icon, size: 22, color: const Color(0xFFC98A1E)),
        const SizedBox(width: AppSpace.s + 4),
        Flexible(
          child: Text(text, style: context.text.titleSmall?.copyWith(color: context.tones.ink)),
        ),
      ],
    ),
  );
}

class _TooShort extends StatelessWidget {
  const _TooShort({super.key, required this.onAgain, required this.onHome});
  final VoidCallback onAgain, onHome;

  @override
  Widget build(BuildContext context) => _Centered(
    children: [
      EmptyState(
        icon: Icons.hourglass_bottom_rounded,
        color: AppColors.sun,
        title: 'That one was a little short',
        message: 'A check-in needs about 20 seconds of steady touch to keep. Shall we try again?',
        action: Column(
          children: [
            PrimaryButton(label: 'Try again', icon: Icons.refresh_rounded, onPressed: onAgain),
            const SizedBox(height: AppSpace.s),
            TextButton(onPressed: onHome, child: const Text('Back home')),
          ],
        ),
      ),
    ],
  );
}

class _Problem extends StatelessWidget {
  const _Problem({super.key, required this.message, required this.onAgain, required this.onHome});
  final String message;
  final VoidCallback onAgain, onHome;

  @override
  Widget build(BuildContext context) => _Centered(
    children: [
      EmptyState(
        icon: Icons.wb_twilight_rounded,
        color: AppColors.sun,
        title: "Let's try that again",
        message: message,
        action: Column(
          children: [
            PrimaryButton(label: 'Try again', icon: Icons.refresh_rounded, onPressed: onAgain),
            const SizedBox(height: AppSpace.s),
            TextButton(onPressed: onHome, child: const Text('Back home')),
          ],
        ),
      ),
    ],
  );
}
