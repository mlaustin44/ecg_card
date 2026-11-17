// Data / session providers (owned by the analysis + data layer).
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../data/sembast_session_store.dart';
import '../data/session_record.dart';
import '../data/session_store.dart';
import 'device_providers.dart';
import 'live_session.dart';
import 'session_controller.dart';
import 'wake_lock.dart';

/// The session store: sembast (a file on iOS / Android, IndexedDB on the web),
/// opened lazily on first use. Tests override it with InMemorySessionStore
/// (lib/data/memory_session_store.dart).
final sessionStoreProvider = Provider<SessionStore>((ref) {
  final store = SembastSessionStore();
  ref.onDispose(store.close);
  return store;
});

/// All saved sessions, newest first.
final sessionsProvider = StreamProvider<List<SessionRecord>>(
  (ref) => ref.watch(sessionStoreProvider).watchAll(),
);

/// The live session controller (one at a time), bound to the current card:
/// the card is looked up when a session begins, so switching cards between
/// sessions needs no rebuild (and a reconnect does not drop a session).
final liveSessionProvider = Provider<LiveSessionApi>((ref) {
  final store = ref.watch(sessionStoreProvider);
  final c = SessionController(
    card: () => ref.read(currentCardProvider),
    store: store,
    wakeLock: ref.watch(wakeLockProvider),
  );
  ref.onDispose(c.dispose);
  return c;
});
