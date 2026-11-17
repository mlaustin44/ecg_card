// Your card: name, connection, battery, firmware; using the card as a
// heart-rate sensor for other apps; how to hold it; Bluetooth help.
// Long-press the firmware version to open the Developer screen.
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

import '../../app.dart';
import '../../device/card_link.dart';
import '../../protocol/link_status.dart';
import '../../state/device_providers.dart';
import '../theme.dart';
import '../widgets/breath_card_art.dart';
import '../widgets/breath_kit.dart';
import '../widgets/primary_button.dart';
import '../widgets/section_card.dart';
import '../widgets/thumbs_illustration.dart';

class CardScreen extends ConsumerWidget {
  const CardScreen({super.key});

  void _back(BuildContext context) {
    if (context.canPop()) {
      context.pop();
    } else {
      context.go(Routes.home);
    }
  }

  Future<void> _forget(BuildContext context, WidgetRef ref, CardLink card) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Forget this card?'),
        content: Text('${card.name} will be disconnected. You can find it again any time.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Keep it')),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Forget')),
        ],
      ),
    );
    if (ok != true) return;
    try {
      await ref.read(currentCardProvider.notifier).forget();
    } catch (_) {
      // Forgetting is best effort; the connect screen starts fresh either way.
    }
    if (context.mounted) context.go(Routes.connect);
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final t = BreathTones.of(context);
    final card = ref.watch(currentCardProvider);
    final link =
        ref.watch(linkStateProvider).value ?? card?.currentLinkState ?? LinkState.disconnected;
    return Scaffold(
      backgroundColor: t.bg,
      body: SafeArea(
        child: Column(
          children: [
            BreathHeader(title: 'Your card', onBack: () => _back(context)),
            Expanded(
              child: StreamBuilder<CardStatus>(
                stream: card?.status,
                initialData: card?.lastStatus,
                builder: (context, snap) => ListView(
                  padding: const EdgeInsets.fromLTRB(
                    AppSpace.m,
                    AppSpace.s,
                    AppSpace.m,
                    AppSpace.xl,
                  ),
                  children: [
                    _hero(context, t, card, link),
                    if (card != null) ...[
                      const SizedBox(height: AppSpace.m),
                      _facts(context, t, link, snap.data),
                      const SizedBox(height: AppSpace.m),
                      BreathCard(
                        padding: const EdgeInsets.symmetric(
                          horizontal: AppSpace.s + 4,
                          vertical: AppSpace.xs,
                        ),
                        child: Column(
                          children: [
                            NavRow(
                              icon: Icons.bluetooth_searching_rounded,
                              color: t.indigo,
                              title: 'Find a different card',
                              onTap: () => context.go(Routes.connect),
                            ),
                            NavRow(
                              icon: Icons.link_off_rounded,
                              color: t.heart,
                              title: 'Forget this card',
                              trailing: const SizedBox.shrink(),
                              onTap: () => _forget(context, ref, card),
                            ),
                          ],
                        ),
                      ),
                    ],
                    const BreathSectionLabel('Use your card as a heart-rate sensor'),
                    _hrMonitor(t),
                    const BreathSectionLabel('How to hold it'),
                    _howToHold(t),
                    const BreathSectionLabel('Bluetooth help'),
                    _help(t),
                  ],
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _hero(BuildContext context, BreathTones t, CardLink? card, LinkState link) {
    final (label, color) = switch (link) {
      LinkState.connected => ('Connected', t.breath),
      LinkState.connecting => ('Connecting…', AppColors.sun),
      LinkState.disconnected => ('Not connected', t.inkFaint),
    };
    return BreathCard(
      padding: const EdgeInsets.fromLTRB(AppSpace.m, AppSpace.l, AppSpace.m, AppSpace.l),
      child: Column(
        children: [
          BreathCardArt(width: 200, connected: link == LinkState.connected),
          const SizedBox(height: AppSpace.m),
          Text(
            card?.name ?? 'No card yet',
            style: TextStyle(fontSize: 22, fontWeight: FontWeight.w600, color: t.ink),
          ),
          const SizedBox(height: AppSpace.s),
          if (card != null)
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
              decoration: BoxDecoration(
                color: color.withValues(alpha: 0.14),
                borderRadius: BorderRadius.circular(AppRadius.pill),
              ),
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Container(
                    width: 8,
                    height: 8,
                    decoration: BoxDecoration(color: color, shape: BoxShape.circle),
                  ),
                  const SizedBox(width: 6),
                  Text(
                    label,
                    style: TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: color),
                  ),
                ],
              ),
            )
          else ...[
            Text(
              "Let's find the card you were given.",
              style: TextStyle(fontSize: 15, color: t.inkSoft),
            ),
            const SizedBox(height: AppSpace.m),
            PrimaryButton(
              label: 'Find your card',
              expand: false,
              onPressed: () => context.go(Routes.connect),
            ),
          ],
        ],
      ),
    );
  }

  Widget _facts(BuildContext context, BreathTones t, LinkState link, CardStatus? s) {
    final pct = s?.battPct;
    final battery = s == null
        ? '–'
        : pct == null
        ? 'Unknown'
        : s.charging
        ? '$pct% · charging'
        : '$pct%';
    final battIcon = s?.charging ?? false
        ? Icons.battery_charging_full_rounded
        : pct == null
        ? Icons.battery_unknown_rounded
        : pct > 80
        ? Icons.battery_full_rounded
        : pct > 50
        ? Icons.battery_5_bar_rounded
        : pct > 20
        ? Icons.battery_3_bar_rounded
        : Icons.battery_1_bar_rounded;
    return BreathCard(
      padding: const EdgeInsets.symmetric(vertical: AppSpace.xs),
      child: Column(
        children: [
          _FactRow(
            icon: battIcon,
            color: (s?.battLow ?? false) ? t.heart : t.breath,
            label: 'Battery',
            value: battery,
          ),
          Divider(height: 1, indent: 64, color: t.track),
          _FactRow(
            icon: Icons.memory_rounded,
            color: t.indigo,
            label: 'Firmware',
            value: s?.firmware ?? '–',
            onLongPress: () => context.push(Routes.developer),
          ),
        ],
      ),
    );
  }

  Widget _hrMonitor(BreathTones t) => BreathCard(
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          'Your card can stream your heartbeat to other apps, like Elite HRV, Kubios, '
          'Zwift or a sports watch.',
          style: TextStyle(fontSize: 15, color: t.ink, height: 1.4),
        ),
        const SizedBox(height: AppSpace.m),
        const _Step(
          n: 1,
          text: 'On the card, press the right button until you see the “HR monitor” page.',
        ),
        const _Step(n: 2, text: 'Press the left button to start.'),
        const _Step(
          n: 3,
          text: 'In the other app, look for a heart-rate sensor and pick your card.',
        ),
        const SizedBox(height: AppSpace.s),
        _Note(
          icon: Icons.link_rounded,
          text:
              'One connection at a time: while another app is connected, this app can’t connect. '
              'Stop the HR monitor on the card to come back.',
        ),
        _Note(
          icon: Icons.touch_app_rounded,
          text: 'Keep both thumbs resting on the pads. The card then sends every beat-to-beat interval.',
        ),
        _Note(
          icon: Icons.self_improvement_rounded,
          text: 'It’s made for resting and breathing sessions, not for workouts.',
        ),
      ],
    ),
  );

  Widget _howToHold(BreathTones t) => BreathCard(
    child: Column(
      children: [
        const Padding(
          padding: EdgeInsets.symmetric(vertical: AppSpace.s),
          child: ThumbsIllustration(touching: true, animate: false, width: 260),
        ),
        const SizedBox(height: AppSpace.s),
        const _Note(
          icon: Icons.back_hand_rounded,
          text: 'Hold the card in both hands, one thumb resting on each gold pad.',
        ),
        const _Note(
          icon: Icons.chair_rounded,
          text: 'Sit back and rest your forearms on a table or your lap.',
        ),
        const _Note(icon: Icons.spa_rounded, text: 'A light touch is plenty. No need to press.'),
        const _Note(
          icon: Icons.hourglass_bottom_rounded,
          text: 'Stay still for the first few seconds while the card settles.',
        ),
      ],
    ),
  );

  Widget _help(BreathTones t) => BreathCard(
    padding: const EdgeInsets.symmetric(vertical: AppSpace.xs),
    child: const Column(
      children: [
        _Help(
          title: 'My card doesn’t show up',
          body:
              'Wake the card with a press of either button. Make sure Bluetooth is on for your '
              'phone and in the card’s Settings page, and keep the card within a couple of metres.',
        ),
        _Help(
          title: 'It’s connected to another app',
          body:
              'Only one app can talk to the card at a time. Stop the HR monitor on the card, or '
              'close the other app, then come back here.',
        ),
        _Help(
          title: 'It keeps disconnecting',
          body:
              'Charge the card for a while and keep your phone close. Walls and pockets can get in '
              'the way of Bluetooth.',
        ),
        _Help(
          title: 'Using a computer',
          body: 'On a computer, use Chrome or Edge. Other browsers can’t talk to Bluetooth devices yet.',
        ),
      ],
    ),
  );
}

class _FactRow extends StatelessWidget {
  const _FactRow({
    required this.icon,
    required this.color,
    required this.label,
    required this.value,
    this.onLongPress,
  });
  final IconData icon;
  final Color color;
  final String label, value;
  final VoidCallback? onLongPress;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return InkWell(
      onLongPress: onLongPress,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: AppSpace.m, vertical: 12),
        child: Row(
          children: [
            IconWell(icon: icon, color: color, size: 36),
            const SizedBox(width: 12),
            Text(label, style: TextStyle(fontSize: 15, color: t.ink)),
            const Spacer(),
            Text(
              value,
              style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: t.inkSoft),
            ),
          ],
        ),
      ),
    );
  }
}

class _Step extends StatelessWidget {
  const _Step({required this.n, required this.text});
  final int n;
  final String text;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 26,
            height: 26,
            alignment: Alignment.center,
            decoration: BoxDecoration(color: t.indigo, shape: BoxShape.circle),
            child: Text(
              '$n',
              style: const TextStyle(
                fontSize: 13,
                fontWeight: FontWeight.w700,
                color: Colors.white,
              ),
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Padding(
              padding: const EdgeInsets.only(top: 3),
              child: Text(text, style: TextStyle(fontSize: 15, color: t.ink, height: 1.35)),
            ),
          ),
        ],
      ),
    );
  }
}

class _Note extends StatelessWidget {
  const _Note({required this.icon, required this.text});
  final IconData icon;
  final String text;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return Padding(
      padding: const EdgeInsets.only(top: AppSpace.s),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.only(top: 1),
            child: Icon(icon, size: 20, color: t.breath),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Text(text, style: TextStyle(fontSize: 14, color: t.inkSoft, height: 1.4)),
          ),
        ],
      ),
    );
  }
}

class _Help extends StatelessWidget {
  const _Help({required this.title, required this.body});
  final String title, body;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    return ExpansionTile(
      tilePadding: const EdgeInsets.symmetric(horizontal: AppSpace.m),
      childrenPadding: const EdgeInsets.fromLTRB(AppSpace.m, 0, AppSpace.m, AppSpace.m),
      shape: const Border(),
      collapsedShape: const Border(),
      iconColor: t.inkSoft,
      collapsedIconColor: t.inkFaint,
      title: Text(
        title,
        style: TextStyle(fontSize: 15, fontWeight: FontWeight.w500, color: t.ink),
      ),
      children: [
        Align(
          alignment: Alignment.centerLeft,
          child: Text(body, style: TextStyle(fontSize: 14, color: t.inkSoft, height: 1.4)),
        ),
      ],
    );
  }
}
