// Keeps the screen on while a session measures (check-in, breathing). The
// session controller turns it on when a session begins and off when it
// finishes, is cancelled, fails or is disposed. Under flutter test the
// provider is a no-op, so no platform channel is ever called.
import 'dart:async';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import '../device/env_stub.dart' if (dart.library.io) '../device/env_io.dart';

abstract class WakeLock {
  Future<void> enable();
  Future<void> disable();
}

/// Does nothing (tests, previews).
class NoWakeLock implements WakeLock {
  const NoWakeLock();
  @override
  Future<void> enable() async {}
  @override
  Future<void> disable() async {}
}

/// wakelock_plus. Best effort: a platform without wake locks is not an error.
class PlatformWakeLock implements WakeLock {
  const PlatformWakeLock();

  @override
  Future<void> enable() async {
    try {
      await WakelockPlus.enable();
    } catch (_) {}
  }

  @override
  Future<void> disable() async {
    try {
      await WakelockPlus.disable();
    } catch (_) {}
  }
}

final wakeLockProvider = Provider<WakeLock>(
  (ref) => runningInFlutterTest ? const NoWakeLock() : const PlatformWakeLock(),
);
