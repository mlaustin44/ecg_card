# ECG Card app: architecture

A Flutter app (iOS and Android first; web only where Web Bluetooth exists: Chrome/Edge
desktop and Android) for the people who receive an ECG Card. TestFlight / sideload only.
Tone: calm, warm, friendly (think Calm / Oura), never clinical. **No medical claims**:
no rhythm interpretation, no "normal / abnormal", no diagnosis words. HRV is shown as
a wellness number ("recovery", "calm"), the ECG strip as "your recording".

The engineering client stays `tools/ble_client.html`. This app hides all tuning
(gain, RLD, restore, notch) except behind a Developer screen.

## Layout

    lib/
      main.dart, app.dart          entry, MaterialApp.router, providers scope
      protocol/                    PURE DART (no Flutter imports): the card's BLE contract
        uuids.dart                 service / characteristic UUIDs
        stream_fmt.dart            EcgBlock, PpgBlock, CardEvent (sealed) parsers
        link_status.dart           CardStatus (Status characteristic, 20 bytes)
        commands.dart              Control writes: start/stop/set/setTime/getStatus
        units.dart                 stream units -> microvolts, sample rates
      device/
        card_link.dart             THE interface the UI uses (below)
        ble_card.dart              universal_ble implementation
        fake_card.dart             synthetic card (ECG with RSA, PPG, status) + replay
      analysis/                    PURE DART: RR series, HRV, breathing, summaries
      data/                        session records + local store
      state/                       Riverpod providers wiring device/analysis/data to UI
      ui/
        theme.dart                 colours, type, spacing (light + dark)
        widgets/                   trace painter, HR graph, big number, breathing orb
        screens/                   one file per screen
    test/                          mirrors lib/

Source of truth for the protocol (read these, do not guess):
`fw/include/stream_fmt.h`, `fw/include/link_fmt.h` (BLE section, LINK_STATUS,
LINK_CMD, LINK_MODE_*), `fw/include/settings.h` (setting items/enums),
`tools/ble_client.html` (a working JS client: parsing, units, gains),
`tools/ecgcard/codec.py` (Python reference parser; use it to generate test vectors).

BLE notifications carry raw payloads (no COBS / CRC): ECG and PPG notifications are n
whole blocks back to back (25 / 15 bytes each); Events = one STREAM_EVENT payload;
Status = 20-byte LINK_STATUS; Control = write-with-response LINK_CMD payload, its
result arrives on Events as a CMD_RESULT event. Standard services also exist: Heart
Rate 0x180D/0x2A37 (flags, bpm, RR), Battery 0x180F, Device Information 0x180A.

## Device interface (lib/device/card_link.dart)

```dart
enum CardMode { idle, ecg, ppg, both, hrMonitor }            // LINK_MODE_* 0..4
enum LinkState { disconnected, connecting, connected }

class FoundCard { final String id; final String name; final int? rssi; }

abstract class CardScanner {
  Stream<List<FoundCard>> scan();           // cards advertising the ECG Card service
  Future<void> stop();
}

abstract class CardLink {
  String get id; String get name;
  Stream<LinkState> get linkState;
  Stream<CardStatus> get status;            // Status notifications (+ initial read)
  Stream<EcgBlock> get ecg;                 // already split into blocks
  Stream<PpgBlock> get ppg;
  Stream<CardEvent> get events;
  Stream<String> get log;                   // Log characteristic text (developer screen)
  CardStatus? get lastStatus;
  Future<void> connect();
  Future<void> disconnect();
  Future<CmdResult> start(CardMode mode);   // completes on the CMD_RESULT event
  Future<CmdResult> stop();
  Future<CmdResult> setSetting(int item, int value);
  Future<CmdResult> setTime(DateTime now);
}
```

`FakeCard implements CardLink` (and a `FakeScanner`) so every screen runs without
hardware (`--dart-define=FAKE_CARD=true`, and always in tests and on platforms with no
BLE). It synthesises a realistic session: settling, then ECG (QRS ~1.2 mV, 500 Hz,
blocks every 20 ms), R_PEAK events whose RR follows respiratory sinus arrhythmia
(breathing ~6/min swings HR ~±8 bpm), STATE / status updates, and supports scripted
contact loss. It can also replay a `ble_client.html` "Save session" text file
(`## ECG index,uV,flags` section); such recordings are personal data and are never
committed.

## Analysis (lib/analysis, pure Dart)

- RR series from R_PEAK sample indices (500 Hz) or pulse feet; artifact filter
  (drop intervals outside 300..2000 ms or > 25 % from the local median).
- Fast HR (mean of last 2 RR), HR, RMSSD, SDNN, pNN50 over a window.
- Breathing: resonance / coherence score for HRV biofeedback (RR resampled at 4 Hz,
  power in the peak band of 0.04..0.26 Hz vs total), breathing rate from the RSA peak.
- SessionSummary: duration, mean/min/max HR, RMSSD, a 1..5 "calm" score mapped from
  RMSSD (age-agnostic, documented thresholds), coherence for breathing sessions.

## Data (lib/data)

`SessionRecord` (id, start time, kind: check-in / breathing / free, duration, RR list,
summary numbers, a 10 s ECG strip in microvolts at 250 Hz, optional note/tag) in a local
store that works on iOS, Android and web (sembast + sembast_web). Export: PDF of a
session (strip + numbers, worded as "a recording, not a diagnosis") and CSV.

## Screens (lib/ui/screens)

1. Welcome / onboarding (3 friendly pages, how to hold the card) -> find your card.
2. Connect: scan list with name "ECG Card XXXX", remembers the last card, reconnects.
3. Home: today's check-in card, last results, quick actions (Check-in, Breathe, History),
   card battery and connection pill.
4. Live session (check-in, 1 min default): guided "place thumbs" -> "settling" -> live
   ECG trace + big HR, timer ring; graceful when contact drops (no error tone).
5. Results: HR, calm score, RMSSD, a clean ECG strip, HR-over-time sparkline; save + note.
6. Breathe (headline feature): animated orb paces breathing (default 6/min, 5.5..7
   selectable, inhale/exhale ratio), live HR wave showing RSA in sync, coherence meter,
   session length 2/5/10 min, summary at the end.
7. History: list + calendar/week strip, trends (resting HR, calm score), detail view
   with export/share.
8. Card: battery, firmware versions, name, "use as a heart-rate sensor for other apps"
   explainer (HR monitor mode), Bluetooth / pairing help.
9. Developer (hidden: long-press the version number): raw status, log, settings items
   (gain/notch/RLD/LED/sweep/restore/view), raw trace.

## Conventions

- Riverpod for state; go_router for navigation.
- Pure-Dart layers (protocol, analysis) have no Flutter imports and full unit tests.
- Widget tests use FakeCard. Golden/screenshot tests optional.
- `flutter analyze` clean, `flutter test` green, `dart format` applied.
