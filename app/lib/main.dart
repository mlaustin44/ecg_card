import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:material_ui/material_ui.dart';

import 'app.dart';
import 'device/ble_card.dart';
import 'device/fake_card.dart';
import 'state/device_providers.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final overrides = [
    // ---- device layer: the fake card when this platform has no Bluetooth
    if (!useFakeCardByDefault && !await bleSupported())
      cardScannerProvider.overrideWithValue(FakeScanner()),
    // ---- data layer overrides go here (e.g. sessionStoreProvider)
  ];
  runApp(ProviderScope(overrides: overrides, child: const EcgCardApp()));
}
