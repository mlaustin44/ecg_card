// SessionStore on sembast: a file in the app documents directory on iOS /
// Android (sembast_io), IndexedDB on the web (sembast_web).
import 'dart:async';

import 'package:sembast/sembast.dart';
import 'package:sembast/sembast_memory.dart' show databaseFactoryMemory;

import 'sembast_open_io.dart' if (dart.library.js_interop) 'sembast_open_web.dart' as platform;
import 'session_record.dart';
import 'session_store.dart';

class SembastSessionStore implements SessionStore {
  /// [open] defaults to the platform database; it is called on first use.
  /// If opening fails (no documents directory, e.g. in a widget test), the
  /// store falls back to an in-memory database and [persistent] is false.
  SembastSessionStore({Future<Database> Function()? open})
    : _open = open ?? platform.openSessionDatabase;

  final Future<Database> Function() _open;
  Future<Database>? _db;
  bool _persistent = true;

  static final _store = stringMapStoreFactory.store('sessions');

  /// False when the store fell back to memory (sessions are not kept).
  bool get persistent => _persistent;

  Future<Database> get _database => _db ??= _openOrFallback();

  Future<Database> _openOrFallback() async {
    try {
      return await _open();
    } catch (_) {
      _persistent = false;
      return databaseFactoryMemory.openDatabase('ecg_card_sessions_fallback.db');
    }
  }

  @override
  Future<void> save(SessionRecord record) async {
    final db = await _database;
    await _store.record(record.id).put(db, record.toJson());
  }

  @override
  Future<SessionRecord?> get(String id) async {
    final db = await _database;
    final v = await _store.record(id).get(db);
    return v == null ? null : _decode(v);
  }

  @override
  Future<void> delete(String id) async {
    final db = await _database;
    await _store.record(id).delete(db);
  }

  @override
  Stream<List<SessionRecord>> watchAll() async* {
    final db = await _database;
    final q = _store.query(finder: Finder(sortOrders: [SortOrder('start', false)]));
    yield* q.onSnapshots(db).map((snaps) => [for (final s in snaps) ?_tryDecode(s.value)]);
  }

  Future<void> close() async {
    final f = _db;
    _db = null;
    if (f != null) await (await f).close();
  }

  static SessionRecord _decode(Map<String, Object?> v) =>
      SessionRecord.fromJson(Map<String, Object?>.of(v));

  /// A record that cannot be read (a newer or damaged format) is skipped
  /// rather than breaking the whole history list.
  static SessionRecord? _tryDecode(Map<String, Object?> v) {
    try {
      return _decode(v);
    } catch (_) {
      return null;
    }
  }
}
