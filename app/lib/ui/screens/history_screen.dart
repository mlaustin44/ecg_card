// History: a week strip, 30-day trends and the list of saved sessions.
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:intl/intl.dart';
import 'package:material_ui/material_ui.dart';

import '../../app.dart';
import '../../data/session_record.dart';
import '../../state/data_providers.dart';
import '../theme.dart';
import '../widgets/breath_history_empty.dart';
import '../widgets/breath_history_tile.dart';
import '../widgets/breath_history_trend.dart';
import '../widgets/breath_history_week.dart';
import '../widgets/breath_kit.dart';
import '../widgets/format.dart';
import '../widgets/section_card.dart';

/// Per-day trend values over the last [days] days (pure; tested).
class BreathTrends {
  BreathTrends.from(List<SessionRecord> all, DateTime today, {this.days = 30}) {
    final first = breathDay(today).subtract(Duration(days: days - 1));
    final hrByDay = <int, List<double>>{};
    final hrAnyByDay = <int, List<double>>{};
    final calmByDay = <int, List<int>>{};
    for (final r in all) {
      final d = breathDay(r.start).difference(first).inDays;
      if (d < 0 || d >= days) continue;
      final hr = r.stats?.meanHr;
      if (hr != null) {
        (hrAnyByDay[d] ??= []).add(hr);
        if (r.kind == SessionKind.checkIn) (hrByDay[d] ??= []).add(hr);
      }
      if (r.calmScore != null) (calmByDay[d] ??= []).add(r.calmScore!);
    }
    // Resting heart rate comes from check-ins; fall back to any session.
    final src = hrByDay.isNotEmpty ? hrByDay : hrAnyByDay;
    restingHr = [for (final e in src.entries) BreathDayValue(e.key, _mean(e.value))]
      ..sort((a, b) => a.day.compareTo(b.day));
    calm = [
      for (final e in calmByDay.entries)
        BreathDayValue(e.key, _mean(e.value.map((v) => v.toDouble()))),
    ]..sort((a, b) => a.day.compareTo(b.day));
  }

  final int days;
  late final List<BreathDayValue> restingHr;
  late final List<BreathDayValue> calm;

  /// Mean of the last 7 days that have a value (the headline number).
  static double? recent(List<BreathDayValue> v, int days) {
    final last = [
      for (final p in v)
        if (p.day >= days - 7) p.value,
    ];
    return last.isEmpty ? (v.isEmpty ? null : v.last.value) : _mean(last);
  }

  static double _mean(Iterable<double> v) => v.reduce((a, b) => a + b) / v.length;
}

class HistoryScreen extends ConsumerStatefulWidget {
  const HistoryScreen({super.key});

  @override
  ConsumerState<HistoryScreen> createState() => _HistoryScreenState();
}

class _HistoryScreenState extends ConsumerState<HistoryScreen> {
  int _weekBack = 0;
  DateTime? _selected;

  DateTime get _today => breathDay(ref.read(clockProvider)());

  void _back() {
    if (context.canPop()) {
      context.pop();
    } else {
      context.go(Routes.home);
    }
  }

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    final sessions = ref.watch(sessionsProvider);
    return Scaffold(
      backgroundColor: t.bg,
      body: SafeArea(
        child: Column(
          children: [
            BreathHeader(title: 'History', onBack: _back),
            Expanded(
              child: switch (sessions) {
                AsyncData(:final value) when value.isEmpty => Center(
                  child: SingleChildScrollView(
                    child: BreathHistoryEmpty(
                      onCheckIn: () => context.go(Routes.checkIn),
                      onBreathe: () => context.go(Routes.breathe),
                    ),
                  ),
                ),
                AsyncData(:final value) => _content(t, value),
                AsyncError() => Center(
                  child: Padding(
                    padding: const EdgeInsets.all(AppSpace.l),
                    child: Text(
                      "We couldn't open your history just now.",
                      textAlign: TextAlign.center,
                      style: TextStyle(fontSize: 15, color: t.inkSoft),
                    ),
                  ),
                ),
                _ => Center(child: CircularProgressIndicator(color: t.breath, strokeWidth: 2.5)),
              },
            ),
          ],
        ),
      ),
    );
  }

  Widget _content(BreathTones t, List<SessionRecord> all) {
    final today = _today;
    // A rolling week ending today (or 7, 14, … days earlier).
    final weekStart = today.subtract(Duration(days: 6 + 7 * _weekBack));
    final kindsByDay = <DateTime, Set<SessionKind>>{};
    for (final r in all) {
      (kindsByDay[breathDay(r.start)] ??= {}).add(r.kind);
    }
    final trends = BreathTrends.from(all, today);
    final hr = BreathTrends.recent(trends.restingHr, trends.days);
    final calm = BreathTrends.recent(trends.calm, trends.days);

    final shown = _selected == null
        ? all
        : [
            for (final r in all)
              if (breathDay(r.start) == _selected) r,
          ];
    final groups = <DateTime, List<SessionRecord>>{};
    for (final r in shown) {
      (groups[breathDay(r.start)] ??= []).add(r);
    }

    return ListView(
      padding: const EdgeInsets.fromLTRB(AppSpace.m, AppSpace.s, AppSpace.m, AppSpace.xl),
      children: [
        BreathWeekStrip(
          weekStart: weekStart,
          today: today,
          kindsByDay: kindsByDay,
          selected: _selected,
          onSelect: (d) => setState(() => _selected = _selected == d ? null : d),
          onPrev: () => setState(() => _weekBack++),
          onNext: _weekBack == 0 ? null : () => setState(() => _weekBack--),
        ),
        const BreathSectionLabel('Last 30 days'),
        IntrinsicHeight(
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Expanded(
                child: BreathTrendCard(
                  title: 'Resting heart rate',
                  value: hr?.round().toString(),
                  unit: 'bpm',
                  points: trends.restingHr,
                  color: t.heart,
                  icon: Icons.favorite_rounded,
                ),
              ),
              const SizedBox(width: AppSpace.s + 4),
              Expanded(
                child: BreathTrendCard(
                  title: 'Calm',
                  value: calm?.toStringAsFixed(1),
                  unit: 'of 5',
                  points: trends.calm,
                  color: t.breath,
                  icon: Icons.spa_rounded,
                  style: BreathTrendStyle.bars,
                ),
              ),
            ],
          ),
        ),
        Row(
          children: [
            Expanded(
              child: BreathSectionLabel(
                _selected == null ? 'Sessions' : DateFormat('EEEE d MMMM').format(_selected!),
              ),
            ),
            if (_selected != null)
              Padding(
                padding: const EdgeInsets.only(top: AppSpace.m),
                child: TextButton(
                  onPressed: () => setState(() => _selected = null),
                  style: TextButton.styleFrom(foregroundColor: t.indigo),
                  child: const Text('Show all'),
                ),
              ),
          ],
        ),
        if (shown.isEmpty)
          BreathCard(
            child: Row(
              children: [
                const IconWell(icon: Icons.wb_sunny_rounded, color: AppColors.sun, size: 40),
                const SizedBox(width: 12),
                Expanded(
                  child: Text(
                    'A quiet day. Nothing recorded.',
                    style: TextStyle(fontSize: 15, color: t.inkSoft),
                  ),
                ),
              ],
            ),
          ),
        for (final e in groups.entries) ...[
          if (_selected == null)
            Padding(
              padding: const EdgeInsets.fromLTRB(AppSpace.xs, AppSpace.s, AppSpace.xs, AppSpace.s),
              child: Text(
                _dayName(e.key, today),
                style: TextStyle(fontSize: 14, fontWeight: FontWeight.w600, color: t.inkSoft),
              ),
            ),
          for (final r in e.value)
            Padding(
              padding: const EdgeInsets.only(bottom: AppSpace.s),
              child: BreathSessionTile(record: r, onTap: () => context.push(Routes.session(r.id))),
            ),
        ],
      ],
    );
  }

  static String _dayName(DateTime d, DateTime today) {
    final diff = today.difference(d).inDays;
    if (diff == 0) return 'Today';
    if (diff == 1) return 'Yesterday';
    if (diff < 7) return DateFormat('EEEE').format(d);
    return DateFormat('EEE d MMM').format(d);
  }
}
