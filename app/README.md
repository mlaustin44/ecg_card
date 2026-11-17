# ECG Card app

Flutter app for people who received an ECG Card: guided check-ins, breathing with your
heart (HRV biofeedback), history, export. iOS and Android; web where Web Bluetooth
exists (Chrome / Edge on desktop and Android). Design and layer contracts:
`ARCHITECTURE.md`. The engineering client remains `tools/ble_client.html`.

## Setup

Flutter stable (3.50+); `flutter pub get` in this folder.

## Run

    flutter run                                  # phone over USB, real card
    flutter run --dart-define=FAKE_CARD=true     # simulated card (no hardware)
    flutter run -d chrome                        # web: Web Bluetooth (Chrome / Edge)

## Test

    flutter analyze && flutter test
    flutter test --update-goldens test/ui        # after an intended visual change

Protocol test vectors come from `tools/ecgcard/codec.py`:
`../.venv/bin/python -I tool/make_vectors.py` (from the repository root:
`.venv/bin/python -I app/tool/make_vectors.py`).

## Release

- Android: `flutter build apk --release` (sideload) or `flutter build appbundle`.
- iOS / TestFlight: on a Mac with Xcode, set the team and bundle id in
  `ios/Runner.xcworkspace`, then `flutter build ipa` and upload with Transporter.
- Web: `flutter build web`; serve over HTTPS (Web Bluetooth needs a secure origin).
