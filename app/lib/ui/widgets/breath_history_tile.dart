// History: one saved session in the list.
import 'package:intl/intl.dart';
import 'package:material_ui/material_ui.dart';

import '../../data/session_record.dart';
import 'breath_history_week.dart';
import 'breath_kit.dart';
import 'calm_score.dart';
import 'format.dart';
import 'section_card.dart';

class BreathSessionTile extends StatelessWidget {
  const BreathSessionTile({super.key, required this.record, required this.onTap});
  final SessionRecord record;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final t = BreathTones.of(context);
    final r = record;
    final color = breathKindColor(t, r.kind);
    final hr = r.stats?.meanHr;
    final coh = r.coherence;
    return BreathCard(
      onTap: onTap,
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      child: Row(
        children: [
          IconWell(icon: breathKindIcon(r.kind), color: color, size: 42),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  breathKindName(r.kind),
                  style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: t.ink),
                ),
                const SizedBox(height: 2),
                Text(
                  '${DateFormat.jm().format(r.start)} · ${durationWords(r.duration)}',
                  style: TextStyle(fontSize: 13, color: t.inkSoft),
                ),
              ],
            ),
          ),
          Column(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              if (hr != null)
                Text.rich(
                  TextSpan(
                    children: [
                      TextSpan(text: '${hr.round()}'),
                      TextSpan(
                        text: ' bpm',
                        style: TextStyle(
                          fontSize: 12,
                          fontWeight: FontWeight.w400,
                          color: t.inkFaint,
                        ),
                      ),
                    ],
                  ),
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: t.ink),
                ),
              const SizedBox(height: 5),
              if (r.kind == SessionKind.breathing && coh != null)
                Text(
                  '${(coh * 100).round()}% sync',
                  style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: t.breath),
                )
              else if (r.calmScore != null)
                CalmDots(score: r.calmScore!, dot: 9, color: t.breath),
            ],
          ),
          const SizedBox(width: 4),
          Icon(Icons.chevron_right_rounded, size: 20, color: t.inkFaint),
        ],
      ),
    );
  }
}
