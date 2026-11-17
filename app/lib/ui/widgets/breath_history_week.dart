// History: a week strip of days, with soft dots on days that have sessions.
import 'package:intl/intl.dart';
import 'package:material_ui/material_ui.dart';

import '../../data/session_record.dart';
import '../theme.dart';
import 'breath_kit.dart';

DateTime breathDay(DateTime t) => DateTime(t.year, t.month, t.day);

Color breathKindColor(BreathTones t, SessionKind k) => switch (k) {
  SessionKind.checkIn => t.heart,
  SessionKind.breathing => t.breath,
  SessionKind.free => t.indigo,
};

IconData breathKindIcon(SessionKind k) => switch (k) {
  SessionKind.checkIn => Icons.favorite_rounded,
  SessionKind.breathing => Icons.air_rounded,
  SessionKind.free => Icons.timeline_rounded,
};

String breathKindName(SessionKind k) => switch (k) {
  SessionKind.checkIn => 'Check-in',
  SessionKind.breathing => 'Breathing',
  SessionKind.free => 'Recording',
};

class BreathWeekStrip extends StatelessWidget {
  const BreathWeekStrip({
    super.key,
    required this.weekStart,
    required this.today,
    required this.kindsByDay,
    required this.selected,
    required this.onSelect,
    required this.onPrev,
    this.onNext,
  });

  final DateTime weekStart;
  final DateTime today;
  final Map<DateTime, Set<SessionKind>> kindsByDay;
  final DateTime? selected;
  final ValueChanged<DateTime> onSelect;
  final VoidCallback onPrev;
  final VoidCallback? onNext; // null on the current week

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    final end = weekStart.add(const Duration(days: 6));
    final title = weekStart.month == end.month
        ? DateFormat('MMMM y').format(weekStart)
        : '${DateFormat('MMM').format(weekStart)} – ${DateFormat('MMM y').format(end)}';
    return BreathCard(
      padding: const EdgeInsets.fromLTRB(AppSpace.s, AppSpace.s, AppSpace.s, AppSpace.m),
      child: Column(
        children: [
          Row(
            children: [
              IconButton(
                tooltip: 'Previous week',
                onPressed: onPrev,
                icon: Icon(Icons.chevron_left_rounded, color: t.inkSoft),
              ),
              Expanded(
                child: Text(
                  title,
                  textAlign: TextAlign.center,
                  style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: t.ink),
                ),
              ),
              IconButton(
                tooltip: 'Next week',
                onPressed: onNext,
                icon: Icon(
                  Icons.chevron_right_rounded,
                  color: onNext == null ? t.inkFaint.withValues(alpha: 0.4) : t.inkSoft,
                ),
              ),
            ],
          ),
          const SizedBox(height: AppSpace.xs),
          Row(
            children: [
              for (var i = 0; i < 7; i++)
                Expanded(child: _day(context, t, weekStart.add(Duration(days: i)))),
            ],
          ),
        ],
      ),
    );
  }

  Widget _day(BuildContext context, BreathTones t, DateTime d) {
    final isToday = d == today;
    final isSel = d == selected;
    final future = d.isAfter(today);
    final kinds = kindsByDay[d] ?? const <SessionKind>{};
    final fg = isSel ? Colors.white : (future ? t.inkFaint.withValues(alpha: 0.5) : t.ink);
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: future ? null : () => onSelect(d),
      child: Column(
        children: [
          Text(
            DateFormat('E').format(d).substring(0, 1),
            style: TextStyle(fontSize: 12, color: t.inkFaint, fontWeight: FontWeight.w500),
          ),
          const SizedBox(height: 6),
          AnimatedContainer(
            duration: const Duration(milliseconds: 200),
            width: 38,
            height: 38,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: isSel
                  ? t.indigo
                  : (kinds.isNotEmpty ? t.indigo.withValues(alpha: 0.08) : null),
              border: isToday && !isSel ? Border.all(color: t.indigo, width: 1.5) : null,
            ),
            child: Text(
              '${d.day}',
              style: TextStyle(
                fontSize: 15,
                fontWeight: isToday || isSel ? FontWeight.w700 : FontWeight.w500,
                color: fg,
              ),
            ),
          ),
          const SizedBox(height: 6),
          SizedBox(
            height: 6,
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                for (final k in SessionKind.values)
                  if (kinds.contains(k))
                    Container(
                      width: 6,
                      height: 6,
                      margin: const EdgeInsets.symmetric(horizontal: 1.5),
                      decoration: BoxDecoration(
                        color: breathKindColor(t, k),
                        shape: BoxShape.circle,
                      ),
                    ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
