// Developer (hidden: long-press the firmware version on the Card screen):
// raw status, the device log, settings items and mode commands.
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';

import '../../app.dart';
import '../../device/card_link.dart';
import '../../protocol/commands.dart';
import '../../protocol/link_status.dart';
import '../../state/device_providers.dart';
import '../theme.dart';
import '../widgets/breath_kit.dart';

/// One settings item (fw/include/settings.h) and its value names.
class DevSetting {
  const DevSetting(this.item, this.name, this.values, {this.fallback = 0});
  final int item;
  final String name;
  final List<String> values;

  /// SETTINGS_DEFAULTS value, shown until the card reports or we set one.
  final int fallback;

  String label(int? v) => v == null || v < 0 || v >= values.length ? '?' : values[v];
}

const devSettings = [
  DevSetting(SettingItem.gain, 'Gain', ['Auto', '558x', '1098x']),
  DevSetting(SettingItem.notch, 'Notch', ['Auto', '50 Hz', '60 Hz']),
  DevSetting(SettingItem.rld, 'RLD', ['Both', 'Left', 'Right', 'Off']),
  DevSetting(SettingItem.ledLimit, 'LED limit', ['5 mA', '10 mA', '15 mA'], fallback: 2),
  DevSetting(SettingItem.sweep, 'Sweep', ['1.5 s', '3 s', '6 s'], fallback: 1),
  DevSetting(SettingItem.ble, 'Bluetooth', ['Off', 'On'], fallback: 1),
  DevSetting(SettingItem.restore, 'Restore', ['Fast', 'Slow', 'Off'], fallback: 2),
  DevSetting(SettingItem.view, 'Live view', ['Trace', '+HR graph']),
];

/// Current value of [s] from the card's status, when the status carries it.
int? devStatusValue(CardStatus? st, int item) {
  if (st == null) return null;
  return switch (item) {
    SettingItem.gain => st.gain,
    SettingItem.notch => st.notch,
    SettingItem.rld => st.rld,
    SettingItem.ledLimit => st.ledLimit,
    SettingItem.sweep => st.sweep,
    _ => null,
  };
}

class DeveloperScreen extends ConsumerStatefulWidget {
  const DeveloperScreen({super.key});

  @override
  ConsumerState<DeveloperScreen> createState() => _DeveloperScreenState();
}

class _DeveloperScreenState extends ConsumerState<DeveloperScreen> {
  static const _maxLog = 400;

  CardLink? _card;
  StreamSubscription<String>? _logSub;
  final List<String> _log = [];
  final Map<int, int> _setLocally = {};
  final Map<int, String> _itemResult = {};
  String? _cmdResult;
  bool _busy = false;

  @override
  void initState() {
    super.initState();
    ref.listenManual<CardLink?>(
      currentCardProvider,
      (_, card) => _attach(card),
      fireImmediately: true,
    );
  }

  void _attach(CardLink? card) {
    if (identical(card, _card)) return;
    _logSub?.cancel();
    _card = card;
    _logSub = card?.log.listen((line) {
      if (!mounted) return;
      setState(() {
        _log.addAll(line.trimRight().split('\n'));
        if (_log.length > _maxLog) _log.removeRange(0, _log.length - _maxLog);
      });
    });
  }

  @override
  void dispose() {
    _logSub?.cancel();
    super.dispose();
  }

  Future<void> _run(String what, Future<CmdResult> Function(CardLink) f) async {
    final card = _card;
    if (card == null || _busy) return;
    setState(() => _busy = true);
    final sw = Stopwatch()..start();
    String res;
    try {
      res = (await f(card)).name;
    } catch (e) {
      res = 'error: $e';
    }
    if (!mounted) return;
    setState(() {
      _busy = false;
      _cmdResult = '$what → $res (${sw.elapsedMilliseconds} ms)';
    });
  }

  Future<void> _cycle(DevSetting s, int? current) async {
    final card = _card;
    if (card == null) return;
    final next = ((current ?? s.fallback) + 1) % s.values.length;
    setState(() => _itemResult[s.item] = '…');
    String res;
    try {
      final r = await card.setSetting(s.item, next);
      res = r.name;
      if (r == CmdResult.ok) _setLocally[s.item] = next;
    } catch (e) {
      res = 'error';
    }
    if (!mounted) return;
    setState(() => _itemResult[s.item] = '${s.label(next)}: $res');
  }

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    final card = ref.watch(currentCardProvider);
    final link =
        ref.watch(linkStateProvider).value ?? card?.currentLinkState ?? LinkState.disconnected;
    final mono = TextStyle(fontFamily: 'monospace', fontSize: 12, color: t.ink, height: 1.35);
    return Scaffold(
      backgroundColor: t.bg,
      appBar: AppBar(
        title: const Text('Developer'),
        backgroundColor: t.bg,
        surfaceTintColor: Colors.transparent,
        leading: IconButton(
          icon: const Icon(Icons.arrow_back_rounded),
          onPressed: () => context.canPop() ? context.pop() : context.go(Routes.card),
        ),
      ),
      body: StreamBuilder<CardStatus>(
        stream: card?.status,
        initialData: card?.lastStatus,
        builder: (context, snap) {
          final st = snap.data;
          return ListView(
            padding: const EdgeInsets.fromLTRB(AppSpace.m, 0, AppSpace.m, AppSpace.xl),
            children: [
              _section(t, 'Link'),
              BreathCard(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _kv(mono, t, 'name', card?.name ?? '(none)'),
                    _kv(mono, t, 'id', card?.id ?? '–'),
                    _kv(mono, t, 'link', link.name),
                    const SizedBox(height: AppSpace.s),
                    Wrap(
                      spacing: AppSpace.s,
                      runSpacing: AppSpace.s,
                      children: [
                        _btn('Connect', card == null ? null : () => card.connect()),
                        _btn('Disconnect', card == null ? null : () => card.disconnect()),
                        _btn(
                          'Set time',
                          card == null
                              ? null
                              : () => _run('setTime', (c) => c.setTime(DateTime.now())),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
              _section(t, 'Mode'),
              BreathCard(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Wrap(
                      spacing: AppSpace.s,
                      runSpacing: AppSpace.s,
                      children: [
                        for (final m in [
                          CardMode.ecg,
                          CardMode.ppg,
                          CardMode.both,
                          CardMode.hrMonitor,
                        ])
                          _btn(
                            _modeLabel(m),
                            card == null ? null : () => _run('start(${m.name})', (c) => c.start(m)),
                          ),
                        _btn(
                          'Stop',
                          card == null ? null : () => _run('stop', (c) => c.stop()),
                          tonal: true,
                        ),
                      ],
                    ),
                    const SizedBox(height: AppSpace.s),
                    Text(
                      _cmdResult ?? 'No command sent yet',
                      style: mono.copyWith(color: t.inkSoft),
                    ),
                  ],
                ),
              ),
              _section(t, 'Status'),
              BreathCard(
                child: st == null ? Text('No status yet', style: mono) : _status(mono, t, st),
              ),
              _section(t, 'Settings'),
              BreathCard(
                padding: const EdgeInsets.symmetric(vertical: AppSpace.xs),
                child: Column(
                  children: [
                    for (final s in devSettings) _settingRow(t, mono, s, st, card != null),
                  ],
                ),
              ),
              Row(
                children: [
                  Expanded(child: _section(t, 'Log (${_log.length})')),
                  IconButton(
                    tooltip: 'Copy log',
                    icon: Icon(Icons.copy_rounded, size: 18, color: t.inkSoft),
                    onPressed: _log.isEmpty
                        ? null
                        : () => Clipboard.setData(ClipboardData(text: _log.join('\n'))),
                  ),
                  IconButton(
                    tooltip: 'Clear log',
                    icon: Icon(Icons.delete_outline_rounded, size: 18, color: t.inkSoft),
                    onPressed: _log.isEmpty ? null : () => setState(_log.clear),
                  ),
                ],
              ),
              BreathCard(
                color: t.dark ? const Color(0xFF0C0D13) : const Color(0xFFF2EFEA),
                child: SizedBox(
                  height: 220,
                  child: _log.isEmpty
                      ? Text('Nothing logged yet', style: mono.copyWith(color: t.inkFaint))
                      : ListView.builder(
                          reverse: true,
                          itemCount: _log.length,
                          itemBuilder: (_, i) => Text(_log[_log.length - 1 - i], style: mono),
                        ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }

  Widget _section(BreathTones t, String s) => BreathSectionLabel(s);

  static String _modeLabel(CardMode m) => switch (m) {
    CardMode.ecg => 'ECG',
    CardMode.ppg => 'PPG',
    CardMode.both => 'ECG + PPG',
    CardMode.hrMonitor => 'HR monitor',
    CardMode.idle => 'Idle',
  };

  Widget _kv(TextStyle mono, BreathTones t, String k, String v) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 1.5),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          width: 92,
          child: Text(k, style: mono.copyWith(color: t.inkFaint)),
        ),
        Expanded(child: Text(v, style: mono)),
      ],
    ),
  );

  Widget _status(TextStyle mono, BreathTones t, CardStatus s) {
    final flags = [
      if (s.usb) 'usb',
      if (s.charging) 'charging',
      if (s.chargeFault) 'chargeFault',
      if (s.noContact) 'noContact',
      if (s.battLow) 'battLow',
      if (s.gainHi) 'gainHi',
      if (s.timeValid) 'timeValid',
    ];
    String setting(int item, int v) => devSettings.firstWhere((d) => d.item == item).label(v);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _kv(mono, t, 'proto', '${s.proto}'),
        _kv(mono, t, 'mode', s.mode.name),
        _kv(mono, t, 'meas', s.meas.name),
        _kv(mono, t, 'battery', '${s.battPct ?? '?'} %  ${s.battMv ?? '?'} mV'),
        _kv(
          mono,
          t,
          'flags',
          '0x${s.flags.toRadixString(16).padLeft(2, '0')} ${flags.isEmpty ? '' : flags.join(' ')}',
        ),
        _kv(mono, t, 'gain', '${s.gain} (${setting(SettingItem.gain, s.gain)})'),
        _kv(
          mono,
          t,
          'notch',
          '${s.notch} (${setting(SettingItem.notch, s.notch)}) now ${s.notchHz} Hz',
        ),
        _kv(mono, t, 'rld', '${s.rld} (${setting(SettingItem.rld, s.rld)})'),
        _kv(mono, t, 'ledLimit', '${s.ledLimit} (${setting(SettingItem.ledLimit, s.ledLimit)})'),
        _kv(mono, t, 'sweep', '${s.sweep} (${setting(SettingItem.sweep, s.sweep)})'),
        _kv(mono, t, 'hrBpm', s.hrBpm == 0 ? 'n/a' : '${s.hrBpm}'),
        _kv(mono, t, 'firmware', s.firmware),
        _kv(mono, t, 'uptime', '${s.uptimeMin} min'),
      ],
    );
  }

  Widget _settingRow(BreathTones t, TextStyle mono, DevSetting s, CardStatus? st, bool enabled) {
    final fromStatus = devStatusValue(st, s.item);
    final current = fromStatus ?? _setLocally[s.item];
    final res = _itemResult[s.item];
    return Padding(
      padding: const EdgeInsets.fromLTRB(AppSpace.m, 6, AppSpace.s, 6),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(s.name, style: TextStyle(fontSize: 15, color: t.ink)),
                if (res != null) Text(res, style: mono.copyWith(color: t.inkFaint, fontSize: 11)),
              ],
            ),
          ),
          Text(
            current == null ? '${s.label(s.fallback)}?' : s.label(current),
            style: TextStyle(fontSize: 14, fontWeight: FontWeight.w600, color: t.inkSoft),
          ),
          const SizedBox(width: AppSpace.s),
          IconButton(
            tooltip: 'Next ${s.name.toLowerCase()} value',
            onPressed: enabled ? () => _cycle(s, current) : null,
            icon: Icon(
              Icons.sync_alt_rounded,
              size: 20,
              color: enabled ? AppColors.indigo : t.inkFaint,
            ),
          ),
        ],
      ),
    );
  }

  static final _compact = ButtonStyle(
    minimumSize: const WidgetStatePropertyAll(Size(0, 38)),
    padding: const WidgetStatePropertyAll(EdgeInsets.symmetric(horizontal: 16)),
    visualDensity: VisualDensity.compact,
    textStyle: const WidgetStatePropertyAll(
      TextStyle(fontFamily: AppText.family, fontSize: 14, fontWeight: FontWeight.w700),
    ),
  );

  Widget _btn(String label, VoidCallback? onPressed, {bool tonal = false}) => tonal
      ? FilledButton.tonal(onPressed: onPressed, style: _compact, child: Text(label))
      : OutlinedButton(onPressed: onPressed, style: _compact, child: Text(label));
}
