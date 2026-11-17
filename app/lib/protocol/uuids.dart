// GATT UUIDs (fw/include/link_fmt.h, BLE section). Pure Dart.
//
// All UUIDs are full 128-bit, lower-case strings (the form universal_ble
// reports and compares).

/// ECG Card service, base d578xxxx-8a28-493a-ad8d-6b741ba09ea7.
abstract final class CardUuids {
  static const String service = 'd5780001-8a28-493a-ad8d-6b741ba09ea7';

  /// notify: 1..n STREAM_ECG_BLOCK payloads (25 B each).
  static const String ecg = 'd5780002-8a28-493a-ad8d-6b741ba09ea7';

  /// notify: 1..n STREAM_PPG_BLOCK payloads (15 B each).
  static const String ppg = 'd5780003-8a28-493a-ad8d-6b741ba09ea7';

  /// notify: one STREAM_EVENT payload.
  static const String events = 'd5780004-8a28-493a-ad8d-6b741ba09ea7';

  /// read, notify: LINK_STATUS (20 B).
  static const String status = 'd5780005-8a28-493a-ad8d-6b741ba09ea7';

  /// write with response: LINK_CMD payload (1..16 B).
  static const String control = 'd5780006-8a28-493a-ad8d-6b741ba09ea7';

  /// notify: device log text (UTF-8 chunks of at most 32 B). Optional
  /// (radio firmware before the device log has none).
  static const String log = 'd5780007-8a28-493a-ad8d-6b741ba09ea7';
}

/// Bluetooth SIG standard services and characteristics the card also offers.
abstract final class StdUuids {
  static const String heartRateService = '0000180d-0000-1000-8000-00805f9b34fb';
  static const String heartRateMeasurement = '00002a37-0000-1000-8000-00805f9b34fb';
  static const String batteryService = '0000180f-0000-1000-8000-00805f9b34fb';
  static const String batteryLevel = '00002a19-0000-1000-8000-00805f9b34fb';
  static const String deviceInfoService = '0000180a-0000-1000-8000-00805f9b34fb';
  static const String manufacturerName = '00002a29-0000-1000-8000-00805f9b34fb';
  static const String modelNumber = '00002a24-0000-1000-8000-00805f9b34fb';
  static const String serialNumber = '00002a25-0000-1000-8000-00805f9b34fb';
  static const String hardwareRevision = '00002a27-0000-1000-8000-00805f9b34fb';
  static const String firmwareRevision = '00002a26-0000-1000-8000-00805f9b34fb';
  static const String softwareRevision = '00002a28-0000-1000-8000-00805f9b34fb';
}

/// Advertised device name prefix ("ECG Card XXXX").
const String cardNamePrefix = 'ECG Card';
