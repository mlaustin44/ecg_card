// A universal_ble platform double: one simulated ECG Card peripheral.
import 'dart:async';
import 'dart:typed_data';

import 'package:ecg_card_app/protocol/uuids.dart';
import 'package:universal_ble/universal_ble.dart';

class SimBlePlatform extends UniversalBlePlatform {
  SimBlePlatform({this.deviceId = 'aa:bb:cc:dd:3f:2a', this.hasLog = true});

  final String deviceId;
  final bool hasLog;

  bool connected = false;
  int connects = 0;
  int failConnects = 0; // the next n connects fail
  int? cmdResult = 0; // null: no CMD_RESULT (command lost)
  final writes = <Uint8List>[];
  final subscribed = <String>{};
  int mtuRequested = 0;
  Uint8List status = Uint8List(20)
    ..[0] = 1
    ..[2] = 0xFF
    ..[3] = 82;

  /// Push a notification on [char].
  void notify(String char, List<int> bytes) =>
      updateCharacteristicValue(deviceId, char, Uint8List.fromList(bytes), null);

  /// The link drops (out of range).
  void dropLink() {
    connected = false;
    updateConnection(deviceId, false, 'link lost');
  }

  @override
  Future<AvailabilityState> getBluetoothAvailabilityState() async => AvailabilityState.poweredOn;

  @override
  Future<void> startScan({ScanFilter? scanFilter, PlatformConfig? platformConfig}) async {
    scheduleMicrotask(() {
      updateScanResult(BleDevice(deviceId: 'other', name: 'Some Watch', rssi: -70));
      updateScanResult(
        BleDevice(
          deviceId: deviceId,
          name: 'ECG Card 3F2A',
          rssi: -55,
          services: [CardUuids.service],
        ),
      );
    });
  }

  @override
  Future<void> stopScan() async {}

  @override
  Future<bool> isScanning() async => false;

  @override
  Future<List<BleDevice>> getSystemDevices(List<String>? withServices) async => [];

  @override
  Future<void> connect(
    String deviceId, {
    Duration? connectionTimeout,
    bool autoConnect = false,
    ConnectionPlatformConfig? platformConfig,
  }) async {
    connects++;
    if (failConnects > 0) {
      failConnects--;
      scheduleMicrotask(() => updateConnection(deviceId, false, 'failed'));
      return;
    }
    connected = true;
    scheduleMicrotask(() => updateConnection(deviceId, true));
  }

  @override
  Future<void> disconnect(String deviceId) async {
    connected = false;
    scheduleMicrotask(() => updateConnection(deviceId, false));
  }

  @override
  Future<BleConnectionState> getConnectionState(String deviceId) async =>
      connected ? BleConnectionState.connected : BleConnectionState.disconnected;

  @override
  Future<List<BleService>> discoverServices(
    String deviceId,
    bool withDescriptors, {
    DiscoverServicesPlatformConfig? platformConfig,
  }) async {
    const n = [CharacteristicProperty.notify];
    return [
      BleService(CardUuids.service, [
        BleCharacteristic(CardUuids.ecg, n, []),
        BleCharacteristic(CardUuids.ppg, n, []),
        BleCharacteristic(CardUuids.events, n, []),
        BleCharacteristic(CardUuids.status, [CharacteristicProperty.read, ...n], []),
        BleCharacteristic(CardUuids.control, [CharacteristicProperty.write], []),
        if (hasLog) BleCharacteristic(CardUuids.log, n, []),
      ]),
    ];
  }

  @override
  Future<void> setNotifiable(
    String deviceId,
    String service,
    String characteristic,
    BleInputProperty bleInputProperty,
  ) async {
    if (characteristic == CardUuids.log && !hasLog) {
      throw Exception('no such characteristic');
    }
    subscribed.add(characteristic);
  }

  @override
  Future<Uint8List> readValue(
    String deviceId,
    String service,
    String characteristic, {
    Duration? timeout,
  }) async {
    if (characteristic == CardUuids.status) return status;
    throw Exception('not readable');
  }

  @override
  Future<void> writeValue(
    String deviceId,
    String service,
    String characteristic,
    Uint8List value,
    BleOutputProperty bleOutputProperty,
  ) async {
    writes.add(value);
    final r = cmdResult;
    if (characteristic == CardUuids.control && r != null) {
      Timer(const Duration(milliseconds: 5), () => notify(CardUuids.events, [6, value[0], r]));
    }
  }

  @override
  Future<int> requestMtu(String deviceId, int expectedMtu) async => mtuRequested = expectedMtu;

  @override
  Future<void> requestConnectionPriority(String deviceId, BleConnectionPriority priority) async {}

  @override
  Future<Uint8List> readDescriptorValue(
    String deviceId,
    String service,
    String characteristic,
    String descriptor, {
    Duration? timeout,
  }) => throw UnimplementedError();
  @override
  Future<void> writeDescriptorValue(
    String deviceId,
    String service,
    String characteristic,
    String descriptor,
    Uint8List value,
  ) => throw UnimplementedError();
  @override
  Future<int> readRssi(String deviceId) => throw UnimplementedError();
  @override
  Future<bool> isPaired(String deviceId) async => true;
  @override
  Future<bool> pair(String deviceId) async => true;
  @override
  Future<void> unpair(String deviceId) async {}
  @override
  Future<bool> enableBluetooth() async => true;
  @override
  Future<bool> disableBluetooth() async => true;
  @override
  Future<BleDevice> requestDevice({ScanFilter? scanFilter, PickerPlatformConfig? platformConfig}) =>
      throw UnimplementedError();
}
