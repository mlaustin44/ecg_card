// Results of a session (also the history detail): heart rate, calm score,
// the recording strip, heart rate over time, a note, share and delete.
import 'dart:convert';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';
import 'package:material_ui/material_ui.dart';
import 'package:share_plus/share_plus.dart';

import '../../app.dart';
import '../../data/export.dart';
import '../../data/session_record.dart';
import '../../state/data_providers.dart';
import '../theme.dart';
import '../widgets/calm_score.dart';
import '../widgets/empty_state.dart';
import '../widgets/format.dart';
import '../widgets/hr_sparkline.dart';
import '../widgets/primary_button.dart';
import '../widgets/section_card.dart';
import '../widgets/strip_view.dart';

class ResultsScreen extends ConsumerWidget {
  const ResultsScreen({super.key, required this.sessionId});
  final String sessionId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final sessions = ref.watch(sessionsProvider);
    final record = sessions.value?.where((r) => r.id == sessionId).firstOrNull;
    final Widget body;
    if (record != null) {
      body = _Results(record: record);
    } else if (sessions.hasValue || sessions.hasError) {
      body = Center(
        child: EmptyState(
          icon: Icons.search_off_rounded,
          title: "This recording isn't here",
          message: 'It may have been deleted.',
          action: SoftButton(label: 'Back home', onPressed: () => context.go(Routes.home)),
        ),
      );
    } else {
      body = const Center(child: CircularProgressIndicator());
    }
    return Scaffold(
      appBar: AppBar(
        leading: BackButton(
          onPressed: () => context.canPop() ? context.pop() : context.go(Routes.home),
        ),
        title: record == null
            ? null
            : Text(record.kind == SessionKind.breathing ? 'Breathing session' : 'Check-in'),
        actions: [
          if (record != null) ...[
            IconButton(
              tooltip: 'Share',
              icon: const Icon(Icons.ios_share_rounded),
              onPressed: () => _share(context, record),
            ),
            IconButton(
              tooltip: 'Delete',
              icon: const Icon(Icons.delete_outline_rounded),
              onPressed: () => _delete(context, ref, record),
            ),
          ],
        ],
      ),
      body: body,
    );
  }

  Future<void> _delete(BuildContext context, WidgetRef ref, SessionRecord r) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Delete this recording?'),
        content: const Text("It will be removed from this phone. This can't be undone."),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Keep it')),
          TextButton(
            onPressed: () => Navigator.pop(c, true),
            style: TextButton.styleFrom(foregroundColor: Theme.of(c).colorScheme.error),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
    if (ok != true || !context.mounted) return;
    await ref.read(sessionStoreProvider).delete(r.id);
    if (!context.mounted) return;
    context.canPop() ? context.pop() : context.go(Routes.home);
  }

  Future<void> _share(BuildContext context, SessionRecord r) async {
    final choice = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(AppSpace.m, 0, AppSpace.m, AppSpace.m),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(AppSpace.s, 0, AppSpace.s, AppSpace.s),
                child: Text('Share this recording', style: c.text.titleLarge),
              ),
              NavRow(
                icon: Icons.picture_as_pdf_rounded,
                color: AppColors.heart,
                title: 'A one-page PDF',
                subtitle: 'Your recording and numbers, ready to print or send',
                onTap: () => Navigator.pop(c, 'pdf'),
              ),
              NavRow(
                icon: Icons.table_chart_rounded,
                color: AppColors.breath,
                title: 'Spreadsheet (CSV)',
                subtitle: 'Every beat, for the curious',
                onTap: () => Navigator.pop(c, 'csv'),
              ),
            ],
          ),
        ),
      ),
    );
    if (choice == null || !context.mounted) return;
    final box = context.findRenderObject() as RenderBox?;
    final origin = box == null ? null : box.localToGlobal(Offset.zero) & box.size;
    try {
      final name = exportBaseName(r);
      final XFile file;
      if (choice == 'pdf') {
        final bytes = await sessionPdf(r);
        file = XFile.fromData(bytes, mimeType: 'application/pdf', name: '$name.pdf');
      } else {
        final csv = sessionCsv(r);
        file = XFile.fromData(utf8.encode(csv), mimeType: 'text/csv', name: '$name.csv');
      }
      await SharePlus.instance.share(
        ShareParams(
          files: [file],
          fileNameOverrides: [file.name],
          subject: 'My ECG Card recording',
          sharePositionOrigin: origin,
        ),
      );
    } catch (_) {
      if (context.mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text("Couldn't share right now. Please try again.")),
        );
      }
    }
  }
}

class _Results extends ConsumerWidget {
  const _Results({required this.record});
  final SessionRecord record;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final r = record;
    final now = ref.watch(clockProvider)();
    final stats = r.stats;
    final tones = context.tones;
    final breathing = r.kind == SessionKind.breathing;
    return ListView(
      padding: const EdgeInsets.fromLTRB(AppSpace.page, AppSpace.s, AppSpace.page, AppSpace.xl),
      children: [
        Text(friendlyWhen(r.start, now).toUpperCase(), style: AppText.eyebrow(context)),
        const SizedBox(height: AppSpace.xs + 2),
        Text(_headline(r), style: context.text.headlineMedium),
        const SizedBox(height: AppSpace.l),

        // Heart rate + calm.
        Hero(
          tag: 'session-${r.id}',
          child: SectionCard(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text('HEART RATE', style: AppText.eyebrow(context)),
                          const SizedBox(height: AppSpace.s),
                          Row(
                            crossAxisAlignment: CrossAxisAlignment.baseline,
                            textBaseline: TextBaseline.alphabetic,
                            children: [
                              Text(
                                stats == null ? '--' : '${stats.meanHr.round()}',
                                style: AppText.numeral(56, color: tones.ink),
                              ),
                              const SizedBox(width: 6),
                              Text(
                                'bpm',
                                style: context.text.titleMedium?.copyWith(color: tones.inkSoft),
                              ),
                            ],
                          ),
                          if (stats != null) ...[
                            const SizedBox(height: AppSpace.xs),
                            Text(
                              'from ${stats.minHr.round()} to ${stats.maxHr.round()} as you breathed',
                              style: context.text.bodySmall,
                            ),
                          ],
                        ],
                      ),
                    ),
                    Container(
                      width: 52,
                      height: 52,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        color: tones.soft(AppColors.heart, 0.14),
                      ),
                      child: const Icon(Icons.favorite_rounded, color: AppColors.heart, size: 26),
                    ),
                  ],
                ),
                if (r.calmScore != null) ...[
                  const SizedBox(height: AppSpace.l - 4),
                  Divider(color: tones.line),
                  const SizedBox(height: AppSpace.l - 4),
                  Text('CALM SCORE', style: AppText.eyebrow(context)),
                  const SizedBox(height: AppSpace.s),
                  Row(
                    crossAxisAlignment: CrossAxisAlignment.center,
                    children: [
                      Expanded(
                        child: Text(
                          calmWord(r.calmScore!),
                          style: context.text.headlineSmall?.copyWith(color: AppColors.breath),
                        ),
                      ),
                      CalmDots(score: r.calmScore!, dot: 18),
                    ],
                  ),
                  const SizedBox(height: AppSpace.s),
                  Text(calmLine(r.calmScore!), style: context.text.bodyMedium),
                ],
              ],
            ),
          ),
        ),

        if (breathing && r.coherence != null) ...[
          const SizedBox(height: AppSpace.m),
          SectionCard(
            title: 'In sync',
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  crossAxisAlignment: CrossAxisAlignment.baseline,
                  textBaseline: TextBaseline.alphabetic,
                  children: [
                    Text(
                      '${(r.coherence! * 100).round()}',
                      style: AppText.numeral(40, color: AppColors.breath),
                    ),
                    Text('%', style: context.text.titleMedium?.copyWith(color: AppColors.breath)),
                  ],
                ),
                const SizedBox(height: AppSpace.s),
                ClipRRect(
                  borderRadius: BorderRadius.circular(AppRadius.pill),
                  child: LinearProgressIndicator(
                    value: r.coherence!.clamp(0, 1),
                    minHeight: 8,
                    color: AppColors.breath,
                  ),
                ),
                const SizedBox(height: AppSpace.s + 2),
                Text(
                  'How closely your heartbeat followed your breathing.',
                  style: context.text.bodySmall,
                ),
              ],
            ),
          ),
        ],

        if (r.stripUv.length > 10) ...[
          const SizedBox(height: AppSpace.m),
          SectionCard(
            title: 'Your recording',
            trailing: Text('swipe to see more', style: context.text.labelSmall),
            padding: const EdgeInsets.fromLTRB(
              AppSpace.m + 4,
              AppSpace.m + 4,
              AppSpace.m + 4,
              AppSpace.m + 4,
            ),
            child: StripView(samples: r.stripUv, hz: r.stripHz, pxPerSec: 90, height: 150),
          ),
        ],

        if (r.rr.length > 3) ...[
          const SizedBox(height: AppSpace.m),
          SectionCard(
            title: 'Heart rate over ${durationWords(r.duration)}',
            child: HrSparkline(rr: r.rr, height: 92),
          ),
        ],

        const SizedBox(height: AppSpace.m),
        _NoteCard(record: r),

        if (stats != null) ...[const SizedBox(height: AppSpace.m), _Curious(record: r)],

        const SizedBox(height: AppSpace.l),
        Text(
          'A personal recording for curiosity and calm,\nnot a medical device or a diagnosis.',
          textAlign: TextAlign.center,
          style: context.text.bodySmall?.copyWith(color: tones.inkSoft.withValues(alpha: 0.8)),
        ),
      ],
    );
  }

  static String _headline(SessionRecord r) {
    if (r.kind == SessionKind.breathing) return 'A breathing session, well done';
    return switch (r.calmScore) {
      null => 'Here is your check-in',
      >= 4 => 'A calm check-in',
      3 => 'A steady check-in',
      _ => 'A lively check-in',
    };
  }
}

class _Curious extends StatelessWidget {
  const _Curious({required this.record});
  final SessionRecord record;

  @override
  Widget build(BuildContext context) {
    final s = record.stats!;
    final tones = context.tones;
    Widget item(String value, String unit, String label) => Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Text(
                value,
                style: AppText.numeral(24, color: tones.ink, weight: FontWeight.w700),
              ),
              const SizedBox(width: 3),
              Text(unit, style: context.text.labelSmall),
            ],
          ),
          const SizedBox(height: 4),
          Text(label, style: context.text.bodySmall),
        ],
      ),
    );
    return SectionCard(
      color: tones.soft(AppColors.indigo, context.isDark ? 0.10 : 0.05),
      title: 'For the curious',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              item(s.rmssdMs.round().toString(), 'ms', 'RMSSD'),
              item(s.sdnnMs.round().toString(), 'ms', 'SDNN'),
              item('${s.beats}', '', 'beats'),
            ],
          ),
          const SizedBox(height: AppSpace.m),
          Text(
            'RMSSD is how much the gap between heartbeats changes from one beat to the next. '
            'It tends to be higher when you are rested and relaxed, and varies a lot from person '
            'to person, so compare yourself with yourself.',
            style: context.text.bodySmall,
          ),
        ],
      ),
    );
  }
}

class _NoteCard extends ConsumerStatefulWidget {
  const _NoteCard({required this.record});
  final SessionRecord record;

  @override
  ConsumerState<_NoteCard> createState() => _NoteCardState();
}

class _NoteCardState extends ConsumerState<_NoteCard> {
  late final _text = TextEditingController(text: widget.record.note ?? '');
  bool _saving = false;

  bool get _dirty => _text.text.trim() != (widget.record.note ?? '').trim();

  @override
  void initState() {
    super.initState();
    _text.addListener(() => setState(() {}));
  }

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    setState(() => _saving = true);
    try {
      await ref.read(sessionStoreProvider).save(widget.record.copyWith(note: _text.text.trim()));
      if (mounted) {
        FocusScope.of(context).unfocus();
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Note saved')));
      }
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  @override
  Widget build(BuildContext context) => SectionCard(
    title: 'A note for yourself',
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.end,
      children: [
        TextField(
          controller: _text,
          minLines: 2,
          maxLines: 5,
          textCapitalization: TextCapitalization.sentences,
          style: context.text.bodyLarge,
          decoration: const InputDecoration(
            hintText: 'How are you feeling? After coffee, a walk, a good sleep…',
          ),
        ),
        AnimatedSize(
          duration: AppMotion.medium,
          curve: AppMotion.curve,
          child: _dirty
              ? Padding(
                  padding: const EdgeInsets.only(top: AppSpace.s + 4),
                  child: SoftButton(
                    label: _saving ? 'Saving…' : 'Save note',
                    icon: Icons.check_rounded,
                    onPressed: _saving ? null : _save,
                  ),
                )
              : const SizedBox(width: double.infinity),
        ),
      ],
    ),
  );
}
