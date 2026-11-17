import 'dart:io' show Platform;

/// True under `flutter test`.
bool get runningInFlutterTest => Platform.environment.containsKey('FLUTTER_TEST');
