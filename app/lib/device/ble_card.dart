// The real card over Bluetooth LE (universal_ble).
//
// Connect sequence: connect -> discover -> MTU 247 (Android; iOS / web
// negotiate on their own) -> connection priority high (Android) -> subscribe
// ECG, PPG, Events, Status, Log (optional) -> read Status -> SET_TIME.
// A link that drops without disconnect() being called is re-established with
// backoff (linkState: disconnected -> connecting -> connected).
//
// Web: scanning opens the browser's device chooser (one card per scan, needs a
// user gesture), and a device can only be connected in the session in which it
// was chosen, so there is no reconnect-by-id across page loads.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:universal_ble/universal_ble.dart';

import '../protocol/commands.dart';
import '../protocol/link_status.dart';
import '../protocol/stream_fmt.dart';
import '../protocol/uuids.dart';
import 'card_link.dart';

/// Services the app uses (web needs them as optionalServices to access them).
const List<String> cardServices = [
  CardUuids.service,
  StdUuids.heartRateService,
  StdUuids.batteryService,
  StdUuids.deviceInfoService,
];

/// A scan could not run (Bluetooth off, permission denied, unsupported).
class CardScanException implements Exception {
  const CardScanException(this.message, [this.cause]);
  final String message;
  final Object? cause;
  @override
  String toString() => 'CardScanException: $message';
}

/// True when this platform has a usable Bluetooth stack for universal_ble
/// (false e.g. in a browser without Web Bluetooth).
Future<bool> bleSupported() async {
  try {
    final s = await UniversalBle.getBluetoothAvailabilityState();
    return s != AvailabilityState.unsupported;
  } catch (_) {
    return false;
  }
}

class BleScanner implements CardScanner {
  StreamController<List<FoundCard>>? _ctl;
  StreamSubscription<BleDevice>? _sub;
  final _links = <String, BleCard>{};

  static bool _isCard(BleDevice d) =>
      d.services.contains(CardUuids.service) || (d.name ?? '').startsWith(cardNamePrefix);

  @override
  Stream<List<FoundCard>> scan() {
    unawaited(stop());
    final found = <String, FoundCard>{};
    late final StreamController<List<FoundCard>> ctl;
    void add(BleDevice d) {
      if (!_isCard(d)) return;
      final name = (d.name?.isNotEmpty ?? false) ? d.name! : cardNamePrefix;
      found[d.deviceId] = FoundCard(id: d.deviceId, name: name, rssi: d.rssi);
      if (!ctl.isClosed) ctl.add(List.unmodifiable(found.values));
    }

    ctl = StreamController<List<FoundCard>>(
      onListen: () async {
        ctl.add(const []);
        _sub = UniversalBle.scanStream.listen(add);
        try {
          final state = await UniversalBle.getBluetoothAvailabilityState();
          if (state == AvailabilityState.unsupported) {
            throw const CardScanException('Bluetooth is not available on this device');
          }
          if (state == AvailabilityState.poweredOff) {
            throw const CardScanException('Bluetooth is off');
          }
          if (state == AvailabilityState.unauthorized) {
            throw const CardScanException('Bluetooth permission is needed to find your card');
          }
          // Cards already connected to this phone (e.g. by a sports app) do not advertise.
          if (!kIsWeb) {
            try {
              for (final d in await UniversalBle.getSystemDevices(
                withServices: [CardUuids.service],
              )) {
                if ((d.name ?? '').startsWith(cardNamePrefix) ||
                    d.services.contains(CardUuids.service)) {
                  add(d);
                }
              }
            } catch (_) {
              /* optional */
            }
          }
          await UniversalBle.startScan(
            scanFilter: ScanFilter(withServices: [CardUuids.service]),
            platformConfig: PlatformConfig(
              web: WebOptions(optionalServices: cardServices),
              android: AndroidOptions(requestLocationPermission: false),
            ),
          );
        } on CardScanException catch (e) {
          if (!ctl.isClosed) ctl.addError(e);
        } catch (e) {
          // web: the user closed the chooser without picking a card
          if (kIsWeb) {
            await stop();
          } else if (!ctl.isClosed) {
            ctl.addError(CardScanException('Could not scan for cards', e));
          }
        }
      },
      onCancel: stop,
    );
    _ctl = ctl;
    return ctl.stream;
  }

  @override
  Future<void> stop() async {
    final sub = _sub, ctl = _ctl;
    _sub = null;
    _ctl = null;
    await sub?.cancel();
    try {
      if (!kIsWeb || await UniversalBle.isScanning()) {
        await UniversalBle.stopScan();
      }
    } catch (_) {}
    if (ctl != null && !ctl.isClosed) await ctl.close();
  }

  @override
  CardLink linkTo(FoundCard card) => _links.putIfAbsent(card.id, () => BleCard(card.id, card.name));
}

class BleCard implements CardLink {
  BleCard(this.id, this.name);

  @override
  final String id;
  @override
  final String name;

  static const _cmdTimeout = Duration(seconds: 3);
  static const _connectTimeout = Duration(seconds: 15);

  final _linkCtl = StreamController<LinkState>.broadcast();
  final _statusCtl = StreamController<CardStatus>.broadcast();
  final _ecgCtl = StreamController<EcgBlock>.broadcast();
  final _ppgCtl = StreamController<PpgBlock>.broadcast();
  final _eventCtl = StreamController<CardEvent>.broadcast();
  final _logCtl = StreamController<String>.broadcast();

  LinkState _link = LinkState.disconnected;
  CardStatus? _lastStatus;
  bool _wanted = false; // the app wants the link (connect() called, no disconnect())
  bool _connecting = false;
  int _generation = 0; // bumps on every (re)connect attempt / teardown
  final _subs = <StreamSubscription<dynamic>>[];
  StreamSubscription<bool>? _connSub;
  Timer? _retry;
  int _retryCount = 0;
  bool _hasLog = false;
  final _logBuf = BytesBuilder(copy: false);

  /// Commands are sent one at a time; each waits for its CMD_RESULT.
  Future<void> _cmdChain = Future.value();
  _Waiter? _waiter;

  /// Negotiated ATT MTU (null where the platform does not tell).
  int? mtu;

  /// Device Information strings read after connecting (manufacturer, model,
  /// serial, firmware, software, hardware), where present.
  final Map<String, String> deviceInfo = {};

  @override
  Stream<LinkState> get linkState => _linkCtl.stream;
  @override
  LinkState get currentLinkState => _link;
  @override
  Stream<CardStatus> get status => _statusCtl.stream;
  @override
  CardStatus? get lastStatus => _lastStatus;
  @override
  Stream<EcgBlock> get ecg => _ecgCtl.stream;
  @override
  Stream<PpgBlock> get ppg => _ppgCtl.stream;
  @override
  Stream<CardEvent> get events => _eventCtl.stream;
  @override
  Stream<String> get log => _logCtl.stream;

  void _setLink(LinkState s) {
    if (s == _link) return;
    _link = s;
    if (!_linkCtl.isClosed) _linkCtl.add(s);
  }

  // ---------------------------------------------------------------- connect

  Future<void>? _pending;

  /// Throws when the first attempt fails (no background retries for it); a link
  /// that was up and drops is retried until [disconnect].
  @override
  Future<void> connect() {
    if (_link == LinkState.connected) return Future.value();
    final p = _pending;
    if (p != null) return p;
    _wanted = true;
    _retry?.cancel();
    _retryCount = 0;
    return _pending = _attempt().whenComplete(() => _pending = null);
  }

  Future<void> _attempt() async {
    final gen = ++_generation;
    _connecting = true;
    _setLink(LinkState.connecting);
    try {
      await _teardownSubs();
      await UniversalBle.connect(id, timeout: _connectTimeout);
      _check(gen);
      _connSub = UniversalBle.connectionStream(id).listen((up) {
        if (!up && gen == _generation) _onLinkLost();
      });
      await UniversalBle.discoverServices(id);
      _check(gen);
      if (!kIsWeb) {
        try {
          mtu = await UniversalBle.requestMtu(id, 247);
        } catch (_) {
          /* best effort */
        }
        if (defaultTargetPlatform == TargetPlatform.android) {
          try {
            await UniversalBle.requestConnectionPriority(id, BleConnectionPriority.highPerformance);
          } catch (_) {}
        }
      }
      _check(gen);
      await _subscribeAll(gen);
      _check(gen);
      try {
        final raw = await UniversalBle.read(id, CardUuids.service, CardUuids.status);
        _onStatus(raw);
      } catch (_) {
        /* a notification follows within 5 s */
      }
      _connecting = false;
      _retryCount = 0;
      _setLink(LinkState.connected);
      unawaited(setTime(DateTime.now()));
      unawaited(_readDeviceInfo(gen));
    } catch (e) {
      _connecting = false;
      if (gen != _generation) {
        return; // superseded by disconnect() or a newer attempt
      }
      await _teardownSubs();
      try {
        await UniversalBle.disconnect(id);
      } catch (_) {}
      _setLink(LinkState.disconnected);
      rethrow;
    }
  }

  void _check(int gen) {
    if (gen != _generation || !_wanted) throw StateError('connect cancelled');
  }

  Future<void> _subscribeAll(int gen) async {
    void listen(String char, void Function(Uint8List) on) {
      _subs.add(
        UniversalBle.characteristicValueStream(id, char).listen((v) {
          if (gen != _generation) return;
          try {
            on(v);
          } catch (e) {
            debugPrint('ECG Card: bad notification on $char: $e');
          }
        }),
      );
    }

    listen(CardUuids.ecg, (v) {
      for (final b in parseEcgNotification(v)) {
        _ecgCtl.add(b);
      }
    });
    listen(CardUuids.ppg, (v) {
      for (final b in parsePpgNotification(v)) {
        _ppgCtl.add(b);
      }
    });
    listen(CardUuids.events, (v) => _onEvent(parseEvent(v)));
    listen(CardUuids.status, _onStatus);
    listen(CardUuids.log, _onLog);

    for (final c in [CardUuids.ecg, CardUuids.ppg, CardUuids.events, CardUuids.status]) {
      await UniversalBle.subscribeNotifications(id, CardUuids.service, c);
    }
    // Log is optional (radio firmware before the device log has none)
    try {
      await UniversalBle.subscribeNotifications(id, CardUuids.service, CardUuids.log);
      _hasLog = true;
    } catch (_) {
      _hasLog = false;
    }
  }

  /// Whether the card has the Log characteristic (after connecting).
  bool get hasLog => _hasLog;

  Future<void> _readDeviceInfo(int gen) async {
    const keys = {
      'manufacturer': StdUuids.manufacturerName,
      'model': StdUuids.modelNumber,
      'serial': StdUuids.serialNumber,
      'hardware': StdUuids.hardwareRevision,
      'firmware': StdUuids.firmwareRevision,
      'software': StdUuids.softwareRevision,
    };
    for (final MapEntry(:key, :value) in keys.entries) {
      if (gen != _generation) return;
      try {
        final raw = await UniversalBle.read(id, StdUuids.deviceInfoService, value);
        deviceInfo[key] = utf8.decode(raw, allowMalformed: true).replaceAll('\u0000', '').trim();
      } catch (_) {
        /* absent */
      }
    }
  }

  void _onStatus(Uint8List raw) {
    if (raw.length < CardStatus.bytes) return;
    final s = CardStatus.parse(raw);
    _lastStatus = s;
    _statusCtl.add(s);
  }

  void _onEvent(CardEvent e) {
    _eventCtl.add(e);
    if (e is CmdResultEvent) {
      final w = _waiter;
      if (w != null && w.cmd == e.cmd && !w.done.isCompleted) {
        w.done.complete(cmdResultOf(e.result));
      }
    }
  }

  void _onLog(Uint8List chunk) {
    var start = 0;
    for (var i = 0; i < chunk.length; i++) {
      if (chunk[i] == 0x0A) {
        _logBuf.add(Uint8List.sublistView(chunk, start, i));
        _emitLogLine();
        start = i + 1;
      }
    }
    if (start < chunk.length) _logBuf.add(Uint8List.sublistView(chunk, start));
    if (_logBuf.length > 1024) _emitLogLine();
  }

  void _emitLogLine() {
    final line = utf8.decode(_logBuf.takeBytes(), allowMalformed: true);
    _logCtl.add(line.endsWith('\r') ? line.substring(0, line.length - 1) : line);
  }

  // ---------------------------------------------------------------- link loss

  void _onLinkLost() {
    if (_link == LinkState.disconnected && !_connecting) return;
    _generation++;
    _connecting = false;
    _failWaiter();
    unawaited(_teardownSubs());
    _setLink(LinkState.disconnected);
    if (_wanted) _scheduleRetry();
  }

  void _scheduleRetry() {
    _retry?.cancel();
    final s = [1, 2, 4, 8, 15][_retryCount.clamp(0, 4)];
    _retryCount++;
    _retry = Timer(Duration(seconds: s), () async {
      if (!_wanted || _link != LinkState.disconnected) return;
      try {
        await _attempt();
      } catch (_) {
        if (_wanted) _scheduleRetry();
      }
    });
  }

  @override
  Future<void> disconnect() async {
    _wanted = false;
    _retry?.cancel();
    _generation++;
    _connecting = false;
    _failWaiter();
    await _teardownSubs();
    try {
      await UniversalBle.disconnect(id);
    } catch (_) {}
    _setLink(LinkState.disconnected);
  }

  Future<void> _teardownSubs() async {
    final subs = List.of(_subs);
    _subs.clear();
    for (final s in subs) {
      await s.cancel();
    }
    await _connSub?.cancel();
    _connSub = null;
    _logBuf.clear();
  }

  /// Disconnects and closes the streams.
  Future<void> dispose() async {
    await disconnect();
    for (final c in [_linkCtl, _statusCtl, _ecgCtl, _ppgCtl, _eventCtl, _logCtl]) {
      await c.close();
    }
  }

  // ---------------------------------------------------------------- commands

  @override
  Future<CmdResult> start(CardMode mode) => _send(Commands.start(mode));
  @override
  Future<CmdResult> stop() => _send(Commands.stop());
  @override
  Future<CmdResult> setSetting(int item, int value) => _send(Commands.set(item, value));
  @override
  Future<CmdResult> setTime(DateTime now) => _send(Commands.setTime(now));

  /// Asks the card to send Status now (it arrives on [status]).
  Future<CmdResult> requestStatus() => _send(Commands.getStatus());

  Future<CmdResult> _send(Uint8List bytes) {
    final result = _cmdChain.then((_) => _sendNow(bytes));
    _cmdChain = result.then((_) {}, onError: (_) {});
    return result;
  }

  Future<CmdResult> _sendNow(Uint8List bytes) async {
    if (_link != LinkState.connected) return CmdResult.timeout;
    final w = _Waiter(bytes[0]);
    _waiter = w;
    try {
      await UniversalBle.write(id, CardUuids.service, CardUuids.control, bytes);
      return await w.done.future.timeout(_cmdTimeout, onTimeout: () => CmdResult.timeout);
    } catch (_) {
      return CmdResult.timeout;
    } finally {
      if (identical(_waiter, w)) _waiter = null;
    }
  }

  void _failWaiter() {
    final w = _waiter;
    if (w != null && !w.done.isCompleted) w.done.complete(CmdResult.timeout);
  }
}

class _Waiter {
  _Waiter(this.cmd);
  final int cmd;
  final done = Completer<CmdResult>();
}
