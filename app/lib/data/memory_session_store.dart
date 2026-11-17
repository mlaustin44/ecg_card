// In-memory SessionStore (tests, previews). Nothing is persisted.
import 'dart:async';

import 'session_record.dart';
import 'session_store.dart';

class InMemorySessionStore implements SessionStore {
  InMemorySessionStore([Iterable<SessionRecord> initial = const []]) {
    for (final r in initial) {
      _byId[r.id] = r;
    }
  }

  final Map<String, SessionRecord> _byId = {};
  final StreamController<List<SessionRecord>> _changes = StreamController.broadcast();

  List<SessionRecord> _sorted() =>
      _byId.values.toList()..sort((a, b) => b.start.compareTo(a.start));

  @override
  Future<void> save(SessionRecord record) async {
    _byId[record.id] = record;
    _changes.add(_sorted());
  }

  @override
  Future<SessionRecord?> get(String id) async => _byId[id];

  @override
  Future<void> delete(String id) async {
    if (_byId.remove(id) != null) _changes.add(_sorted());
  }

  @override
  Stream<List<SessionRecord>> watchAll() => Stream.multi((c) {
    c.add(_sorted());
    final sub = _changes.stream.listen(c.add);
    c.onCancel = sub.cancel;
  });

  Future<void> close() => _changes.close();
}
