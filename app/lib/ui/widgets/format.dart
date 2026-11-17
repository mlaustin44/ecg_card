// Friendly date / time / duration text, and an overridable clock (goldens).
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:intl/intl.dart';

/// "Now" for greetings and relative dates; tests override it.
final clockProvider = Provider<DateTime Function()>((ref) => DateTime.now);

String greetingFor(DateTime t) => switch (t.hour) {
  >= 5 && < 12 => 'Good morning',
  >= 12 && < 18 => 'Good afternoon',
  >= 18 && < 23 => 'Good evening',
  _ => 'Hello, night owl',
};

/// "Today, 8:42 AM" / "Yesterday, 9:10 PM" / "Mon 16 Nov, 7:30 AM".
String friendlyWhen(DateTime t, DateTime now) {
  final day = DateTime(t.year, t.month, t.day);
  final today = DateTime(now.year, now.month, now.day);
  final diff = today.difference(day).inDays;
  final time = DateFormat.jm().format(t);
  if (diff == 0) return 'Today, $time';
  if (diff == 1) return 'Yesterday, $time';
  if (t.year == now.year) return '${DateFormat('EEE d MMM').format(t)}, $time';
  return '${DateFormat('d MMM y').format(t)}, $time';
}

/// "Tuesday 17 November".
String longDate(DateTime t) => DateFormat('EEEE d MMMM').format(t);

/// "0:42" style clock.
String clockText(Duration d) {
  final s = d.inSeconds.clamp(0, 359999);
  return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
}

/// "1 min", "45 s", "2 min 30 s".
String durationWords(Duration d) {
  final s = d.inSeconds;
  if (s < 60) return '$s s';
  final m = s ~/ 60, r = s % 60;
  return r == 0 || m >= 10 ? '$m min' : '$m min $r s';
}
