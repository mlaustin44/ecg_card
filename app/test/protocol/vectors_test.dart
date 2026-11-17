// Parsers against vectors from the Python reference codec
// (app/tool/make_vectors.py -> test/fixtures/protocol_vectors.json).
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:ecg_card_app/protocol/commands.dart';
import 'package:ecg_card_app/protocol/link_status.dart';
import 'package:ecg_card_app/protocol/stream_fmt.dart';
import 'package:flutter_test/flutter_test.dart';

Uint8List hex(String h) => Uint8List.fromList([
  for (var i = 0; i < h.length; i += 2) int.parse(h.substring(i, i + 2), radix: 16),
]);

String toHex(Uint8List b) => b.map((v) => v.toRadixString(16).padLeft(2, '0')).join();

void main() {
  final v = jsonDecode(
    File('test/fixtures/protocol_vectors.json').readAsStringSync(),
  ) as Map<String, dynamic>;
  List<Map<String, dynamic>> list(String k) => (v[k] as List).cast<Map<String, dynamic>>();

  group('ECG notifications', () {
    for (final (i, t) in list('ecg').indexed) {
      test(
        'vector $i (${(t['blocks'] as List).length} blocks${t['note'] != null ? ', ${t['note']}' : ''})',
        () {
          final blocks = parseEcgNotification(hex(t['hex'] as String));
          final want = (t['blocks'] as List).cast<Map<String, dynamic>>();
          expect(blocks.length, want.length);
          for (var k = 0; k < want.length; k++) {
            expect(blocks[k].start, want[k]['start']);
            expect(blocks[k].flags, want[k]['flags']);
            expect(blocks[k].samples.toList(), (want[k]['samples'] as List).cast<int>());
            expect(blocks[k].gainHi, want[k]['gain_hi']);
            expect(blocks[k].settling, want[k]['settling']);
            expect(blocks[k].leadOff, want[k]['lead_off']);
          }
        },
      );
    }
  });

  group('PPG notifications', () {
    for (final (i, t) in list('ppg').indexed) {
      test('vector $i', () {
        final blocks = parsePpgNotification(hex(t['hex'] as String));
        final want = (t['blocks'] as List).cast<Map<String, dynamic>>();
        expect(blocks.length, want.length);
        for (var k = 0; k < want.length; k++) {
          expect(blocks[k].start, want[k]['start']);
          expect(blocks[k].ledCode, want[k]['led_code']);
          expect(blocks[k].flags, want[k]['flags']);
          expect(blocks[k].coarse, (want[k]['coarse'] as List).cast<int>());
          expect(blocks[k].fine, (want[k]['fine'] as List).cast<int>());
          expect(blocks[k].noFinger, want[k]['no_finger']);
        }
      });
    }
  });

  group('Events', () {
    for (final (i, t) in list('events').indexed) {
      test('vector $i ${t['type']}${t['note'] != null ? ' (${t['note']})' : ''}', () {
        final e = parseEvent(hex(t['hex'] as String));
        switch (t['type']) {
          case 'r_peak':
            e as RPeakEvent;
            expect((e.sampleIndex, e.hrBpm), (t['sample_index'], t['hr_bpm']));
          case 'pulse':
            e as PulseEvent;
            expect(
              (e.footIndex, e.pulseBpm, e.patMs, e.piPermille),
              (t['foot_index'], t['pulse_bpm'], t['pat_ms'], t['pi_permille']),
            );
          case 'lead':
            expect((e as LeadEvent).contact, t['contact']);
          case 'state':
            e as StateEvent;
            expect(
              (e.mode, e.notchHz, e.midCode, e.battPct, e.flags, e.gainLo, e.gainHi),
              (
                t['mode'],
                t['notch_hz'],
                t['mid_code'],
                t['batt_pct'],
                t['flags'],
                t['gain_lo'],
                t['gain_hi'],
              ),
            );
          case 'button':
            e as ButtonEvent;
            expect((e.button, e.long), (t['button'], t['long']));
          case 'cmd_result':
            e as CmdResultEvent;
            expect((e.cmd, e.result), (t['cmd'], t['result']));
          case 'unknown':
            e as UnknownEvent;
            expect(e.kind, t['kind']);
            expect(toHex(e.payload), t['hex']);
          default:
            fail('unknown vector type ${t['type']}');
        }
      });
    }
  });

  group('Status', () {
    for (final (i, t) in list('status').indexed) {
      test('vector $i', () {
        final raw = hex(t['hex'] as String);
        final s = CardStatus.parse(raw);
        expect(s.proto, t['proto']);
        expect(s.mode.index, t['mode']);
        expect(s.meas, t['meas'] == 255 ? MeasState.none : MeasState.values[t['meas'] as int]);
        expect(s.battPct, t['batt_pct'] == 255 ? null : t['batt_pct']);
        expect(s.flags, t['flags']);
        expect(
          (s.gain, s.notch, s.rld, s.ledLimit, s.sweep),
          (t['gain'], t['notch'], t['rld'], t['led_limit'], t['sweep']),
        );
        expect((s.notchHz, s.hrBpm), (t['notch_hz'], t['hr_bpm']));
        expect(s.battMv, t['batt_mv'] == 0 ? null : t['batt_mv']);
        expect(s.firmware, '${t['fw_major']}.${t['fw_minor']}.${t['fw_patch']}');
        expect(s.uptimeMin, t['uptime_min']);
        final f = t['flags'] as int;
        expect(
          [s.usb, s.charging, s.chargeFault, s.noContact, s.battLow, s.gainHi, s.timeValid],
          [for (var b = 0; b < 7; b++) f & (1 << b) != 0],
        );
        expect(toHex(s.encode()), t['hex'], reason: 'encode round trip');
      });
    }
  });

  group('Commands', () {
    for (final (i, t) in list('commands').indexed) {
      test('vector $i ${t['cmd']}', () {
        final Uint8List b = switch (t['cmd']) {
          'start' => Commands.start(CardMode.values[t['mode'] as int]),
          'stop' => Commands.stop(),
          'get_status' => Commands.getStatus(),
          'set' => Commands.set(t['item'] as int, t['value'] as int),
          'set_time' => Commands.setTimeRaw(
            t['unix_seconds'] as int,
            t['utc_offset_minutes'] as int,
          ),
          _ => throw StateError('cmd'),
        };
        expect(toHex(b), t['hex']);
      });
    }
  });
}
