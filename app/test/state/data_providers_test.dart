import 'package:ecg_card_app/data/memory_session_store.dart';
import 'package:ecg_card_app/data/session_record.dart';
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/state/data_providers.dart';
import 'package:ecg_card_app/state/device_providers.dart';
import 'package:ecg_card_app/state/live_session.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';

import 'test_card.dart';

final _card = TestCard();

class _Current extends CurrentCard {
  @override
  CardLink? build() => _card;
}

void main() {
  test('liveSessionProvider uses the current card and the store', () async {
    final store = InMemorySessionStore();
    final c = ProviderContainer(
      overrides: [
        sessionStoreProvider.overrideWithValue(store),
        currentCardProvider.overrideWith(_Current.new),
      ],
    );
    addTearDown(c.dispose);
    final sessions = <List<SessionRecord>>[];
    c.listen(sessionsProvider, (_, next) {
      if (next.hasValue) sessions.add(next.value!);
    }, fireImmediately: true);

    final live = c.read(liveSessionProvider);
    await live.begin(SessionKind.checkIn);
    expect(_card.startCalls, 1);
    _card.meas(MeasState.running);
    expect(live.state.phase, LivePhase.running);
    _card.stream(25);
    final r = await live.finish();
    await pumpEventQueue();
    expect(sessions.last.single.id, r!.id);
  });
}
