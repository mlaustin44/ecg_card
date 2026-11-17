// Find your card: scanning ripples, the cards nearby with signal strength,
// tap to connect (remembered by the device layer), friendly help.
import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

import '../../app.dart';
import '../../device/ble_card.dart' show CardScanException;
import '../../device/card_link.dart';
import '../../state/device_providers.dart';
import '../theme.dart';
import '../widgets/primary_button.dart';
import '../widgets/scanning_pulse.dart';
import '../widgets/section_card.dart';

/// Cards seen by the scanner while the Connect screen is open.
final scanResultsProvider = StreamProvider.autoDispose<List<FoundCard>>((ref) {
  final scanner = ref.watch(cardScannerProvider);
  ref.onDispose(scanner.stop);
  return scanner.scan();
});

class ConnectScreen extends ConsumerStatefulWidget {
  const ConnectScreen({super.key});

  @override
  ConsumerState<ConnectScreen> createState() => _ConnectScreenState();
}

class _ConnectScreenState extends ConsumerState<ConnectScreen> {
  String? _connectingId;
  String? _error;

  /// On the web the browser's device chooser needs a tap, so scanning starts
  /// from the "Find my card" button; elsewhere it starts on open.
  bool _scanning = !kIsWeb;

  Future<void> _connect(FoundCard card) async {
    setState(() {
      _connectingId = card.id;
      _error = null;
    });
    try {
      await ref.read(currentCardProvider.notifier).choose(card);
      if (!mounted) return;
      ScaffoldMessenger.of(context)
          .showSnackBar(SnackBar(content: Text('Connected to ${card.name}')));
      if (context.canPop()) {
        context.pop();
      } else {
        context.go(Routes.home);
      }
    } catch (_) {
      if (mounted) {
        setState(() {
          _error =
              "Couldn't connect to ${card.name}. Make sure it's on and close by, then tap it again.";
        });
      }
    } finally {
      if (mounted) setState(() => _connectingId = null);
    }
  }

  @override
  Widget build(BuildContext context) {
    final scan = _scanning
        ? ref.watch(scanResultsProvider)
        : const AsyncValue<List<FoundCard>>.data([]);
    final current = ref.watch(currentCardProvider);
    final cards = [...?scan.value]..sort((a, b) => (b.rssi ?? -200).compareTo(a.rssi ?? -200));
    final tones = context.tones;

    return Scaffold(
      appBar: AppBar(
        leading: context.canPop() ? const BackButton() : null,
        automaticallyImplyLeading: false,
      ),
      body: SafeArea(
        top: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(AppSpace.page, 0, AppSpace.page, AppSpace.xl),
          children: [
            Center(child: ScanningPulse(size: 190, active: _scanning && !scan.hasError)),
            const SizedBox(height: AppSpace.m),
            Text(
              "Let's find your card",
              textAlign: TextAlign.center,
              style: context.text.headlineMedium,
            ),
            const SizedBox(height: AppSpace.s),
            Text(
              'Switch your card on and keep it close by.',
              textAlign: TextAlign.center,
              style: context.text.bodyLarge?.copyWith(color: tones.inkSoft),
            ),
            const SizedBox(height: AppSpace.l),
            if (!_scanning)
              PrimaryButton(
                label: 'Find my card',
                icon: Icons.bluetooth_searching_rounded,
                onPressed: () => setState(() => _scanning = true),
              )
            else if (scan.hasError)
              _ScanProblem(error: scan.error, onRetry: () => ref.invalidate(scanResultsProvider))
            else
              SectionCard(
                padding: const EdgeInsets.fromLTRB(AppSpace.m, AppSpace.m, AppSpace.m, AppSpace.s),
                title: cards.isEmpty
                    ? 'Looking nearby…'
                    : cards.length == 1
                    ? 'Found 1 card'
                    : 'Found ${cards.length} cards',
                trailing: SizedBox(
                  width: 14,
                  height: 14,
                  child: CircularProgressIndicator(
                    strokeWidth: 2,
                    color: tones.inkSoft.withValues(alpha: 0.6),
                  ),
                ),
                child: AnimatedSize(
                  duration: AppMotion.medium,
                  curve: AppMotion.curve,
                  alignment: Alignment.topCenter,
                  child: Column(
                    children: [
                      if (cards.isEmpty)
                        Padding(
                          padding: const EdgeInsets.only(bottom: AppSpace.s + 4),
                          child: Row(
                            children: [
                              IconWell(
                                icon: Icons.credit_card_rounded,
                                color: tones.inkSoft,
                                size: 40,
                              ),
                              const SizedBox(width: AppSpace.m - 2),
                              Expanded(
                                child: Text(
                                  'Your card will show up here in a moment.',
                                  style: context.text.bodyMedium,
                                ),
                              ),
                            ],
                          ),
                        ),
                      for (final c in cards)
                        _CardRow(
                          card: c,
                          connecting: _connectingId == c.id,
                          isCurrent: current?.id == c.id,
                          onTap: _connectingId == null ? () => _connect(c) : null,
                        ),
                    ],
                  ),
                ),
              ),
            if (_error != null) ...[
              const SizedBox(height: AppSpace.m),
              SectionCard(
                color: tones.soft(AppColors.sun, 0.18),
                padding: const EdgeInsets.all(AppSpace.m),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Icon(Icons.info_outline_rounded, color: Color(0xFFC98A1E)),
                    const SizedBox(width: AppSpace.s + 4),
                    Expanded(
                      child: Text(
                        _error!,
                        style: context.text.bodyMedium?.copyWith(color: tones.ink),
                      ),
                    ),
                  ],
                ),
              ),
            ],
            const SizedBox(height: AppSpace.l),
            const _Help(),
          ],
        ),
      ),
    );
  }
}

class _CardRow extends StatelessWidget {
  const _CardRow({
    required this.card,
    required this.connecting,
    required this.isCurrent,
    this.onTap,
  });
  final FoundCard card;
  final bool connecting, isCurrent;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final bars = signalBars(card.rssi);
    final words = switch (bars) {
      >= 3 => 'Close by',
      2 => 'Nearby',
      1 => 'A little far',
      _ => 'Very faint',
    };
    return NavRow(
      icon: Icons.favorite_rounded,
      color: AppColors.heart,
      title: card.name,
      subtitle: connecting
          ? 'Connecting…'
          : isCurrent
          ? 'Your card · $words'
          : words,
      onTap: onTap,
      trailing: connecting
          ? const SizedBox(
              width: 22,
              height: 22,
              child: CircularProgressIndicator(strokeWidth: 2.5),
            )
          : Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                SignalBars(bars: bars),
                const SizedBox(width: AppSpace.s),
                Icon(
                  Icons.chevron_right_rounded,
                  color: context.tones.inkSoft.withValues(alpha: 0.7),
                ),
              ],
            ),
    );
  }
}

/// 0..4 bars from RSSI (dBm).
int signalBars(int? rssi) {
  if (rssi == null) return 2;
  if (rssi >= -60) return 4;
  if (rssi >= -70) return 3;
  if (rssi >= -82) return 2;
  if (rssi >= -92) return 1;
  return 0;
}

class SignalBars extends StatelessWidget {
  const SignalBars({super.key, required this.bars});
  final int bars;

  @override
  Widget build(BuildContext context) => Row(
    mainAxisSize: MainAxisSize.min,
    crossAxisAlignment: CrossAxisAlignment.end,
    children: [
      for (var i = 0; i < 4; i++)
        Container(
          width: 4,
          height: 6.0 + i * 3.5,
          margin: const EdgeInsets.only(left: 2),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(2),
            color: i < bars ? AppColors.breath : context.tones.line,
          ),
        ),
    ],
  );
}

/// Friendly title and help for a scan failure.
(String, String) scanProblemText(Object? error) {
  final m = error is CardScanException ? error.message.toLowerCase() : '';
  if (m.contains('off')) {
    return ('Bluetooth is switched off', 'Turn Bluetooth on in your settings, then try again.');
  }
  if (m.contains('permission')) {
    return (
      'Bluetooth permission needed',
      'Allow this app to find nearby devices in your phone settings, then try again.',
    );
  }
  if (m.contains('not available')) {
    return (
      "Bluetooth isn't available here",
      'This device or browser cannot talk to the card. On a computer, try Chrome or Edge.',
    );
  }
  return (
    "Couldn't look for cards",
    'Check that Bluetooth is on and that this app may find nearby devices, then try again.',
  );
}

class _ScanProblem extends StatelessWidget {
  const _ScanProblem({required this.error, required this.onRetry});
  final Object? error;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final (title, help) = scanProblemText(error);
    return SectionCard(
      child: Column(
        children: [
          IconWell(icon: Icons.bluetooth_disabled_rounded, color: AppColors.sun, size: 52),
          const SizedBox(height: AppSpace.m),
          Text(title, textAlign: TextAlign.center, style: context.text.titleMedium),
          const SizedBox(height: AppSpace.xs + 2),
          Text(help, textAlign: TextAlign.center, style: context.text.bodyMedium),
          const SizedBox(height: AppSpace.m),
          SoftButton(label: 'Try again', icon: Icons.refresh_rounded, onPressed: onRetry),
        ],
      ),
    );
  }
}

class _Help extends StatelessWidget {
  const _Help();

  static const _tips = [
    (
      Icons.power_settings_new_rounded,
      'Is it switched on?',
      'Press the button on the card; its little screen lights up.',
    ),
    (
      Icons.bluetooth_rounded,
      'Bluetooth allowed?',
      'Your phone may ask to let this app find nearby devices. Say yes.',
    ),
    (Icons.near_me_rounded, 'Bring it close', 'Keep the card within a step or two of your phone.'),
    (Icons.usb_off_rounded, 'Unplugged?', 'While charging the card rests. Unplug it to measure.'),
  ];

  @override
  Widget build(BuildContext context) => SectionCard(
    title: "Can't see your card?",
    child: Column(
      children: [
        for (final (i, t) in _tips.indexed) ...[
          if (i > 0) const SizedBox(height: AppSpace.m),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              IconWell(icon: t.$1, color: context.tones.primary, size: 36),
              const SizedBox(width: AppSpace.m - 2),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(t.$2, style: context.text.titleSmall),
                    const SizedBox(height: 2),
                    Text(t.$3, style: context.text.bodySmall),
                  ],
                ),
              ),
            ],
          ),
        ],
      ],
    ),
  );
}
