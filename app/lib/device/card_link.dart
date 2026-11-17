// The device interface the rest of the app uses. Implementations:
// ble_card.dart (universal_ble) and fake_card.dart (synthetic / replay).
import '../protocol/commands.dart';
import '../protocol/link_status.dart';
import '../protocol/stream_fmt.dart';

enum LinkState { disconnected, connecting, connected }

class FoundCard {
  const FoundCard({required this.id, required this.name, this.rssi});
  final String id;
  final String name; // "ECG Card XXXX"
  final int? rssi;
}

abstract class CardScanner {
  /// Cards advertising the ECG Card service; the list grows as more are seen.
  Stream<List<FoundCard>> scan();
  Future<void> stop();

  /// A link to a card found by [scan] (or remembered by id); not yet connected.
  CardLink linkTo(FoundCard card);
}

abstract class CardLink {
  String get id;
  String get name;

  Stream<LinkState> get linkState;
  LinkState get currentLinkState;

  /// Status notifications (and the initial read after connecting).
  Stream<CardStatus> get status;
  CardStatus? get lastStatus;

  /// ECG / PPG blocks, already split from notifications.
  Stream<EcgBlock> get ecg;
  Stream<PpgBlock> get ppg;
  Stream<CardEvent> get events;

  /// Device log text (Developer screen).
  Stream<String> get log;

  /// Connects, subscribes to everything, reads Status, sets the card's clock.
  Future<void> connect();
  Future<void> disconnect();

  /// Each completes on the matching CMD_RESULT event (CmdResult.timeout after 3 s).
  Future<CmdResult> start(CardMode mode);
  Future<CmdResult> stop();
  Future<CmdResult> setSetting(int item, int value);
  Future<CmdResult> setTime(DateTime now);
}
