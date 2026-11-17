// Hand-written edge cases for the protocol layer.
import 'dart:typed_data';

import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';
import 'package:ecg_card_app/protocol/units.dart';
import 'package:ecg_card_app/protocol/uuids.dart';
import 'package:flutter_test/flutter_test.dart';

Uint8List b(List<int> v) => Uint8List.fromList(v);

void main() {
  group('block splitting', () {
    test('empty and short notifications give no blocks', () {
      expect(parseEcgNotification(Uint8List(0)), isEmpty);
      expect(parseEcgNotification(Uint8List(24)), isEmpty);
      expect(parsePpgNotification(Uint8List(14)), isEmpty);
    });

    test('ECG little-endian fields at the right offsets', () {
      final d = Uint8List(25 * 2 + 3);
      final bd = ByteData.sublistView(d);
      bd.setUint32(0, 0x01020304, Endian.little);
      d[4] = 0x05;
      for (var i = 0; i < 10; i++) {
        bd.setInt16(5 + 2 * i, -i * 1000, Endian.little);
      }
      bd.setUint32(25, 0x01020304 + 10, Endian.little);
      bd.setInt16(25 + 5 + 18, 32767, Endian.little);
      final blocks = parseEcgNotification(d);
      expect(blocks.length, 2);
      expect(blocks[0].start, 0x01020304);
      expect(blocks[0].gainHi && blocks[0].leadOff && !blocks[0].settling, isTrue);
      expect(blocks[0].samples[9], -9000);
      expect(blocks[1].start, 0x0102030e);
      expect(blocks[1].samples[9], 32767);
    });

    test('works on a view into a larger buffer', () {
      final big = Uint8List(40);
      big[3] = 0xAA; // u32 start of the block at offset 3 is 0xAA
      final view = Uint8List.sublistView(big, 3, 28);
      expect(parseEcgNotification(view).single.start, 0xAA);
    });

    test('PPG frame order: coarse0, fine0, coarse1, fine1', () {
      final d = Uint8List(15);
      final bd = ByteData.sublistView(d);
      bd.setUint16(4, 600, Endian.little);
      d[6] = 0x06;
      bd.setInt16(7, 1, Endian.little);
      bd.setInt16(9, 2, Endian.little);
      bd.setInt16(11, 3, Endian.little);
      bd.setInt16(13, -4, Endian.little);
      final p = parsePpgNotification(d).single;
      expect(p.coarse, [1, 3]);
      expect(p.fine, [2, -4]);
      expect(p.ledCode, 600);
      expect(p.noFinger, isTrue);
      expect([p.disturbed(0), p.disturbed(1)], [false, true]);
    });
  });

  group('events', () {
    test('empty payload is an UnknownEvent with kind -1', () {
      final e = parseEvent(Uint8List(0));
      expect(e, isA<UnknownEvent>());
      expect((e as UnknownEvent).kind, -1);
    });

    test('longer payloads of known kinds parse the known prefix', () {
      final e = parseEvent(b([6, 1, 0, 99, 99]));
      expect(e, isA<CmdResultEvent>());
      expect((e as CmdResultEvent).result, 0);
    });

    test('STATE of 8..10 bytes uses default gains', () {
      final e = parseEvent(b([4, 1, 50, 0, 8, 82, 0, 1, 2])) as StateEvent;
      expect((e.gainLo, e.gainHi), (558, 1098));
      expect(e.midCode, 2048);
    });

    test('BUTTON long only for kind 1', () {
      expect((parseEvent(b([5, 1, 1])) as ButtonEvent).long, isTrue);
      expect((parseEvent(b([5, 1, 2])) as ButtonEvent).long, isFalse);
    });

    test('LEAD contact', () {
      expect((parseEvent(b([3, 0])) as LeadEvent).contact, isFalse);
      expect((parseEvent(b([3, 1])) as LeadEvent).contact, isTrue);
    });
  });

  group('status', () {
    test('too short throws FormatException', () {
      expect(() => CardStatus.parse(Uint8List(19)), throwsFormatException);
    });

    test('unknown mode and meas values are tolerated', () {
      final d = Uint8List(20);
      d[1] = 9;
      d[2] = 7;
      final s = CardStatus.parse(d);
      expect(s.mode, CardMode.idle);
      expect(s.meas, MeasState.none);
      expect(s.battPct, 0);
      expect(s.battMv, isNull);
    });

    test('extra trailing bytes are ignored', () {
      final d = Uint8List(24);
      d[1] = 4;
      expect(CardStatus.parse(d).mode, CardMode.hrMonitor);
    });
  });

  group('commands', () {
    test('modes map to LINK_MODE_*', () {
      expect(Commands.start(CardMode.ecg), [1, 1]);
      expect(Commands.start(CardMode.hrMonitor), [1, 4]);
    });

    test('setTime uses UTC seconds and the DateTime offset', () {
      final utc = DateTime.utc(2026, 11, 17, 12, 0, 5, 900);
      final c = Commands.setTime(utc);
      final bd = ByteData.sublistView(c);
      expect(c[0], 4);
      expect(c.length, 7);
      expect(bd.getUint32(1, Endian.little), utc.millisecondsSinceEpoch ~/ 1000);
      expect(bd.getInt16(5, Endian.little), 0);
      final local = DateTime.now();
      expect(
        ByteData.sublistView(Commands.setTime(local)).getInt16(5, Endian.little),
        local.timeZoneOffset.inMinutes,
      );
    });

    test('result codes', () {
      expect(cmdResultOf(0), CmdResult.ok);
      expect(cmdResultOf(3), CmdResult.refusedUsb);
      expect(cmdResultOf(6), CmdResult.fault);
      expect(cmdResultOf(42), CmdResult.fault);
    });
  });

  group('units', () {
    test('805.7 / (8 * gain) per stream unit', () {
      expect(ecgUvPerUnit(1098), closeTo(805.7 / 8784, 1e-12));
      expect(ecgUvPerUnit(558), closeTo(805.7 / 4464, 1e-12));
    });

    test('scaler follows block flag and STATE gains', () {
      final sc = EcgScaler();
      final blk = EcgBlock(start: 0, flags: 1, samples: Int16List.fromList(List.filled(10, 8784)));
      expect(sc.toUv(blk)[0], closeTo(805.7, 1e-3));
      sc.onEvent(
        const StateEvent(
          mode: 1,
          notchHz: 50,
          midCode: 2048,
          battPct: 80,
          flags: 0,
          gainLo: 500,
          gainHi: 1000,
        ),
      );
      expect(sc.toUv(blk)[0], closeTo(8784 * 805.7 / 8000, 1e-2));
      final lo = EcgBlock(start: 0, flags: 0, samples: Int16List.fromList(List.filled(10, 4000)));
      expect(sc.toUv(lo)[0], closeTo(805.7, 1e-3));
      // nonsense gains from a STATE fall back to the as-built ones
      sc.onEvent(
        const StateEvent(
          mode: 1,
          notchHz: 50,
          midCode: 2048,
          battPct: 80,
          flags: 0,
          gainLo: 0,
          gainHi: 0,
        ),
      );
      expect(sc.gains, const EcgGains());
    });

    test('units round trip', () {
      expect(ecgUnitsFromUv(1200, 1098), (1200 / ecgUvPerUnit(1098)).round());
      expect(ecgUnitsFromUv(1e9, 1098), 32767);
      expect(
        ecgBlockToUv(
          EcgBlock(
            start: 0,
            flags: 1,
            samples: Int16List.fromList([ecgUnitsFromUv(1200, 1098), 0, 0, 0, 0, 0, 0, 0, 0, 0]),
          ),
        )[0],
        closeTo(1200, 0.1),
      );
    });

    test('ecgSeconds', () {
      expect(ecgSeconds(500), closeTo(0.99, 1e-12));
      expect(ecgHz, 500);
    });
  });

  test('uuids', () {
    expect(CardUuids.service, 'd5780001-8a28-493a-ad8d-6b741ba09ea7');
    expect(CardUuids.log, 'd5780007-8a28-493a-ad8d-6b741ba09ea7');
    expect(StdUuids.heartRateMeasurement, '00002a37-0000-1000-8000-00805f9b34fb');
  });
}
