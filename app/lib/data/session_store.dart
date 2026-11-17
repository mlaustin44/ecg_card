// Local session storage (contract). SembastSessionStore (iOS, Android, web)
// and InMemorySessionStore (tests) are implemented by the data layer.
import 'session_record.dart';

abstract class SessionStore {
  Future<void> save(SessionRecord record);
  Future<SessionRecord?> get(String id);
  Future<void> delete(String id);

  /// Newest first; emits the current list at once and after every change.
  Stream<List<SessionRecord>> watchAll();
}
