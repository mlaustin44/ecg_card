import 'package:ecg_card_app/data/memory_session_store.dart';
import 'package:ecg_card_app/data/sembast_session_store.dart';
import 'package:ecg_card_app/data/session_store.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sembast/sembast_memory.dart';

import 'fixtures.dart';

void main() {
  final stores = <String, SessionStore Function()>{
    'InMemorySessionStore': InMemorySessionStore.new,
    'SembastSessionStore': () =>
        SembastSessionStore(open: () => newDatabaseFactoryMemory().openDatabase('t.db')),
  };
  stores.forEach((name, make) {
    group(name, () {
      test('save, get, delete; watchAll newest first', () async {
        final s = make();
        final seen = <List<String>>[];
        final sub = s.watchAll().listen((l) => seen.add([for (final r in l) r.id]));
        await pumpEventQueue();
        expect(seen.last, isEmpty);

        await s.save(sampleRecord(id: 'old', start: DateTime(2026, 11, 1)));
        await s.save(sampleRecord(id: 'new', start: DateTime(2026, 11, 17)));
        await pumpEventQueue();
        expect(seen.last, ['new', 'old']);

        expect((await s.get('old'))!.start, DateTime(2026, 11, 1));
        expect(await s.get('nope'), isNull);

        // Save again = update (e.g. a note added on the results screen).
        await s.save((await s.get('old'))!.copyWith(note: 'hello'));
        await pumpEventQueue();
        expect((await s.get('old'))!.note, 'hello');
        expect(seen.last, ['new', 'old']);

        await s.delete('new');
        await pumpEventQueue();
        expect(seen.last, ['old']);
        await sub.cancel();
      });

      test('watchAll emits the current list at once to a late listener', () async {
        final s = make();
        await s.save(sampleRecord(id: 'a'));
        final first = await s.watchAll().first;
        expect(first.map((r) => r.id), ['a']);
      });
    });
  });

  test('SembastSessionStore falls back to memory when the open fails', () async {
    final s = SembastSessionStore(open: () => Future.error(StateError('no documents dir')));
    await s.save(sampleRecord(id: 'z'));
    expect((await s.get('z'))!.id, 'z');
    expect(s.persistent, isFalse);
    await s.delete('z');
  });
}
