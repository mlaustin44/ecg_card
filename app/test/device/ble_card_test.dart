// BleCard / BleScanner against a simulated universal_ble platform.

import 'package:ecg_card_app/device/ble_card.dart';
import 'package:ecg_card_app/device/card_link.dart';
import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';
import 'package:ecg_card_app/protocol/uuids.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:universal_ble/universal_ble.dart';

import 'sim_ble_platform.dart';

Future<void> settle([int ms = 20]) => Future<void>.delayed(Duration(milliseconds: ms));

void main() {
  late SimBlePlatform sim;
  setUp(() {
    sim = SimBlePlatform();
    UniversalBle.setInstance(sim);
  });

  test('scanner lists only cards', () async {
    final s = BleScanner();
    final lists = await s.scan().take(2).toList();
    expect(lists.first, isEmpty);
    expect(lists.last.single.name, 'ECG Card 3F2A');
    expect(lists.last.single.id, sim.deviceId);
    await s.stop();
  });

  test('connect: subscribes, reads Status, sets the time', () async {
    final card = BleCard(sim.deviceId, 'ECG Card 3F2A');
    final links = <LinkState>[];
    card.linkState.listen(links.add);
    final statuses = <CardStatus>[];
    card.status.listen(statuses.add);
    await card.connect();
    await settle();
    expect(card.currentLinkState, LinkState.connected);
    expect(links, [LinkState.connecting, LinkState.connected]);
    expect(
      sim.subscribed,
      containsAll([
        CardUuids.ecg,
        CardUuids.ppg,
        CardUuids.events,
        CardUuids.status,
        CardUuids.log,
      ]),
    );
    expect(card.hasLog, isTrue);
    expect(sim.mtuRequested, 247);
    expect(card.lastStatus!.battPct, 82);
    await settle();
    expect(sim.writes.first[0], CmdId.setTime);
    expect(sim.writes.first.length, 7);
    await card.dispose();
  });

  test('notifications are parsed into blocks, events, status and log lines', () async {
    final card = BleCard(sim.deviceId, 'c');
    await card.connect();
    final ecg = <EcgBlock>[], ppg = <PpgBlock>[], ev = <CardEvent>[], log = <String>[];
    card.ecg.listen(ecg.add);
    card.ppg.listen(ppg.add);
    card.events.listen(ev.add);
    card.log.listen(log.add);
    sim.notify(CardUuids.ecg, [...List.filled(25, 0)..[0] = 10, ...List.filled(25, 0)..[0] = 20]);
    sim.notify(CardUuids.ppg, List.filled(15, 0));
    sim.notify(CardUuids.events, [1, 0x10, 0x27, 0, 0, 64, 0]);
    sim.notify(CardUuids.log, 'hello wo'.codeUnits);
    sim.notify(CardUuids.log, 'rld\nnext'.codeUnits);
    sim.notify(CardUuids.log, ' line\n'.codeUnits);
    await settle();
    expect(ecg.map((b) => b.start), [10, 20]);
    expect(ppg.length, 1);
    expect(ev.whereType<RPeakEvent>().single.hrBpm, 64);
    expect(log, ['hello world', 'next line']);
    await card.dispose();
  });

  test('commands complete on the matching CMD_RESULT', () async {
    final card = BleCard(sim.deviceId, 'c');
    await card.connect();
    await settle();
    sim.cmdResult = 5;
    expect(await card.start(CardMode.ecg), CmdResult.busy);
    sim.cmdResult = 0;
    expect(await card.setSetting(SettingItem.sweep, 2), CmdResult.ok);
    expect(sim.writes.last, [3, 4, 2]);
    // a result for another command does not complete it
    sim.cmdResult = null;
    final f = card.stop();
    await settle();
    sim.notify(CardUuids.events, [6, CmdId.start, 0]);
    await settle();
    sim.notify(CardUuids.events, [6, CmdId.stop, 0]);
    expect(await f, CmdResult.ok);
    await card.dispose();
  });

  test('a command without a result times out after 3 s', () async {
    final card = BleCard(sim.deviceId, 'c');
    await card.connect();
    await settle();
    sim.cmdResult = null;
    final sw = Stopwatch()..start();
    expect(await card.stop(), CmdResult.timeout);
    expect(sw.elapsedMilliseconds, inInclusiveRange(2900, 4000));
    await card.dispose();
  });

  test('not connected: commands give timeout without writing', () async {
    final card = BleCard(sim.deviceId, 'c');
    expect(await card.start(CardMode.ecg), CmdResult.timeout);
    expect(sim.writes, isEmpty);
  });

  test('a dropped link is re-established', () async {
    final card = BleCard(sim.deviceId, 'c');
    final links = <LinkState>[];
    card.linkState.listen(links.add);
    await card.connect();
    await settle();
    links.clear();
    sim.dropLink();
    await settle(1500);
    expect(links, [LinkState.disconnected, LinkState.connecting, LinkState.connected]);
    expect(sim.connects, 2);
    // notifications flow again on the new link
    final ev = <CardEvent>[];
    card.events.listen(ev.add);
    sim.notify(CardUuids.events, [3, 1]);
    await settle();
    expect(ev.single, isA<LeadEvent>());
    await card.disconnect();
    expect(card.currentLinkState, LinkState.disconnected);
    sim.dropLink();
    await settle(1200);
    expect(sim.connects, 2, reason: 'no reconnect after disconnect()');
  });

  test('a failed first connect throws and leaves the link disconnected', () async {
    sim.failConnects = 1;
    final card = BleCard(sim.deviceId, 'c');
    await expectLater(card.connect(), throwsA(anything));
    expect(card.currentLinkState, LinkState.disconnected);
    await card.connect();
    expect(card.currentLinkState, LinkState.connected);
    await card.dispose();
  });

  test('card without the Log characteristic', () async {
    sim = SimBlePlatform(hasLog: false);
    UniversalBle.setInstance(sim);
    final card = BleCard(sim.deviceId, 'c');
    await card.connect();
    expect(card.hasLog, isFalse);
    expect(card.currentLinkState, LinkState.connected);
    await card.dispose();
  });
}
