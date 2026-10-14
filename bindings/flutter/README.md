# Medut

SIP softphone SDK for Flutter on Android and iOS: a thin Pigeon layer over
the native Medut SDKs, which own a self-contained SIP stack (baresip/libre)
for the whole process lifetime.

Drop it into any Flutter app. It covers:
- registration over UDP/TCP/TLS/WS/WSS,
- calls, hold, transfer and DTMF,
- SIP MESSAGE and presence,
- push (RFC 8599),
- network handover,
- RTCP/MOS media statistics.

Everything app-specific is configuration you pass in: AOR, credentials,
server URL, codecs, `User-Agent`. The package ships no UI and imposes no call
flow.

| | |
|---|---|
| **Android** | `minSdk` 24; arm64-v8a, armeabi-v7a and x86_64. Built on the Kotlin SDK (`bindings/kotlin`), which handles audio mode and focus, call-audio routes, network callbacks, and an optional app-owned audio device. |
| **iOS** | 13.0+, device and simulator. Built on MedutKit (`bindings/swift`), which handles `AVAudioSession`, routes, `NWPathMonitor`, and the optional VoiceProcessingIO app-owned device. |

Because native code owns the stack, it can start the stack at launch or from
a push, and receive and answer calls with no Flutter engine running.
`Medut.start()` attaches to that running stack (`sdk.reattached`).

## Install

```yaml
dependencies:
  medut:
    git:
      url: https://github.com/Ibrahimgamal99/Medut-SDK.git
      path: bindings/flutter
      ref: <full-40-char-sha>
```

**Android:**
- The SDK declares `INTERNET`, `RECORD_AUDIO`, `MODIFY_AUDIO_SETTINGS` and
  `ACCESS_NETWORK_STATE`.
- Your app must still request the microphone permission at runtime (for
  example with `permission_handler`).
- The plugin compiles a small JNI layer. AGP installs the NDK and CMake it
  needs automatically.

**iOS** needs these entries in your app's `Info.plist`:

```xml
<key>NSMicrophoneUsageDescription</key>
<string>Microphone access is required for voice calls.</string>
<key>UIBackgroundModes</key>
<array><string>audio</string><string>voip</string></array>
```

The pod compiles MedutKit (the Swift SDK, whose sources live under
`bindings/flutter/ios`) and vendors
`bindings/flutter/ios/Frameworks/Medut.xcframework`. Produce that framework
with `scripts/build-ios.sh` on macOS, fetch it with
`scripts/fetch-prebuilt.sh`, or run the `ios-xcframework` workflow with
`commit=true`. If it is absent, `pod install` fails with a clear message.

## Use

```dart
import 'package:medut/medut.dart';

final sdk = await Medut.start(
  config: const MedutConfig(
    statsIntervalMs: 5000,      // MediaStatsEvent every 5 s
    userAgent: 'MyApp/2.1',     // your app's SIP User-Agent
  ),
);

final account = await sdk.createAccount(
  'alice@pbx.example.com',
  'secret',
  config: const AccountConfig(
    serverUrl: 'wss://pbx.example.com:8089/ws',  // WS/WSS need a URL
    mediaEnc: MediaEncryption.dtlsSrtp,
    audioCodecs: ['opus', 'ulaw', 'alaw'],
  ),
);
await account.register();

account.events.listen((ev) {
  if (ev is RegStateEvent && ev.state == RegState.registered) {
    account.call('bob@pbx.example.com');
  } else if (ev is IncomingCallEvent) {
    ev.call.answer();          // or ev.call.reject(statusCode: 486)
  }
});
```

Behaviour worth knowing:
- **Async actions.** Every action is a `Future`, and failures throw
  `MedutException` (`code`: `CALL_ENDED`, `NOT_RUNNING`, `AUTHENTICATION`,
  ...).
- **Cached state.** `call.state` and `account.regState` follow the events.
- **Config defaults.** `MedutConfig` fields left `null` keep the C library's
  defaults.
- **Telecom (Android).** `Medut.start(telecom: true)` registers calls with
  the system: system call UI, Bluetooth / Auto / Wear, incoming calls that
  ring with no Flutter engine up. Request the notification permission on
  API 33+.
- **Push wake-up** is native: start the stack from `Application.onCreate`
  and call `MedutPush.wake()` from your FCM service
  ([Kotlin README](../kotlin/README.md#push-wake-up-fcm)). Register with
  `createAccount(remember: true)` so that start finds the account; the
  example app's `MedutApplication` shows the whole arrangement.

See [`example/`](example/) for a runnable app and
[`docs/quickstart/flutter.md`](https://github.com/Ibrahimgamal99/Medut-SDK/blob/main/docs/quickstart/flutter.md)
for the full walkthrough: CallKit/Telecom, transfer, presence, recording,
handover and stats.

## Development

The Dart, Kotlin and Swift glue is generated from `pigeons/messages.dart`:

```sh
dart run pigeon --input pigeons/messages.dart
flutter test
```

## License

BSD 3-Clause — see [LICENSE](LICENSE). The native libraries statically link
baresip, libre and Opus (BSD-3-Clause) plus OpenSSL (Apache-2.0).
