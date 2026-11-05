# Medut SDK

SIP softphone SDK for Flutter: one Dart API over a self-contained SIP stack
(Medut's own, on libre). On Android and iOS it is a thin Pigeon layer over the native
Medut SDKs, which own the stack for the whole process lifetime; on Linux,
macOS and Windows the same API drives the C library over `dart:ffi`.

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
| **Android** | `minSdk` 24; arm64-v8a, armeabi-v7a and x86_64. Built on the Kotlin SDK (`bindings/kotlin/medut`): Telecom integration (API 26+), FCM wake-up, an encrypted account store, audio mode and focus, call-audio routes, network callbacks, and an optional app-owned audio device. |
| **iOS** | 13.0+, device and simulator. Built on MedutKit (`ios/MedutKit`, also the `bindings/swift` package): CallKit integration, PushKit wake-up, a Keychain account store, `AVAudioSession`, routes, `NWPathMonitor`, and the optional VoiceProcessingIO app-owned device. |
| **Linux / macOS / Windows** | `dart:ffi` straight to the C library (`lib/src/ffi_backend.dart`). The app bundles `medut.so` / `.dylib` / `.dll`, or passes `Medut.start(libPath: ...)`. Phone-only features are documented no-ops (see below). |

Because native code owns the stack on mobile, it can start the stack at launch
or from a push, and receive and answer calls with no Flutter engine running.
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

Flutter 3.44 or later.

**Android:**
- The SDK declares `INTERNET`, `RECORD_AUDIO`, `MODIFY_AUDIO_SETTINGS`,
  `ACCESS_NETWORK_STATE` and what Telecom and its call notification need.
- Your app must still request the microphone permission at runtime (for
  example with `permission_handler`), and the notification permission on
  API 33+ when it uses Telecom.
- The plugin compiles the Kotlin SDK and its small JNI layer from source. AGP
  installs the NDK and CMake it needs automatically.

**iOS** needs these entries in your app's `Info.plist`, plus the Push
Notifications capability (`aps-environment`) for PushKit:

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
`scripts/fetch-prebuilt.sh` (from a "Build mobile libraries" CI run, which also
commits it to its branch by default).
If it is absent, `pod install` fails with a clear message.

**Desktop:** on Linux and Windows the plugin bundles a prebuilt core for the
app's target (x64 or arm64), so nothing else is needed. On macOS build
`medut.dylib` with `scripts/build-macos.sh` and ship it with the app, or point
`Medut.start(libPath: ...)` at it. See [Desktop](#desktop-linux-macos-windows).

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
  ..., the same names on every platform).
- **Cached state.** `call.state` and `account.regState` follow the events.
- **Config defaults.** `MedutConfig` fields left `null` keep the C
  library's defaults.
- **One instance.** The stack is process-wide: while it runs,
  `Medut.start()` returns the running instance and ignores the config and
  options passed (a debug warning says so). `shutdown()` first to change them.
- **Telecom / CallKit.** `Medut.start(telecom: true)` registers calls with
  the system: system call UI, Bluetooth / Auto / Wear, incoming calls that
  ring with no Flutter engine up.

## Host-app setup for push and native start

The full arrangement, as the example app has it:

**Android** — start the stack in your `Application` and hand FCM to the SDK:

```kotlin
class MyApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        // Re-creates and registers the accounts Dart created with
        // remember: true, before any UI or Flutter engine exists.
        MedutAndroid.start(this, MedutConfig(),
            AndroidOptions(telecom = TelecomOptions()))
    }
}

class PushService : FirebaseMessagingService() {
    override fun onMessageReceived(message: RemoteMessage) {
        MedutPush.wake(this, message.data)     // rings via Telecom, re-registers
    }
    override fun onNewToken(token: String) = MedutPush.onNewToken(token)
}
```

Declare both in the manifest (`android:name=".MyApplication"` and the
service with the `com.google.firebase.MESSAGING_EVENT` intent filter). Apps
that only have `firebase_messaging` in Dart can call
`Medut.pushWake(message.data)` from the background handler and
`Medut.pushNewToken(token)` on refresh instead; the stack must still be
started in `Application.onCreate`. See the
[Kotlin README](../kotlin/README.md#push-wake-up-fcm).

**iOS** — start the stack in the app delegate and wire PushKit:

```swift
override func application(_ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
  var options = StartOptions()
  options.callKit = CallKitOptions()
  try? Medut.shared.start(MedutConfig(), options: options)
  let registry = PKPushRegistry(queue: .main)
  registry.delegate = self
  registry.desiredPushTypes = [.voIP]
  pushRegistry = registry
  return super.application(application, didFinishLaunchingWithOptions: launchOptions)
}

func pushRegistry(_ r: PKPushRegistry, didUpdate c: PKPushCredentials, for t: PKPushType) {
  MedutPush.updatePushToken(c.token)
}

func pushRegistry(_ r: PKPushRegistry, didReceiveIncomingPushWith p: PKPushPayload,
                  for t: PKPushType, completion: @escaping () -> Void) {
  MedutPush.wake(payload: p.dictionaryPayload, completion: completion)
}
```

(`import medut` gives the app delegate these types.) PushKit must reach
`MedutPush` natively — iOS requires the call reported to CallKit before the
handler returns — so `Medut.pushWake` is Android-only. See the
[Swift README](../swift/README.md#push-wake-up-pushkit).

Register accounts from Dart with `createAccount(..., remember: true)` so
those native starts find them; `account.destroy(forget: true)` and
`forgetAccount()` remove them again.

## Desktop (Linux, macOS, Windows)

The same Dart API runs on desktop over `dart:ffi`: `lib/src/ffi_backend.dart`
implements the plugin's host API by calling the C library directly, so
`Medut.start`, accounts, calls, events and errors behave as on mobile.

On Linux and Windows the plugin ships prebuilt cores for x64 and arm64:
`linux/<arch>/medut.so` (built on CentOS 7, so glibc 2.17+: RHEL/CentOS 7+,
Ubuntu 14.04+, Debian 8+; OpenSSL linked in, needs only PulseAudio's
`libpulse.so.0` and zlib) and `windows/<arch>/medut.dll` (OpenSSL and zlib
linked in, needs the MSVC runtime). `flutter build linux` /
`flutter build windows` bundle the one for the target, so `Medut.start()`
needs no path.

After a core change, refresh them with the **Desktop libraries** workflow
(`gh workflow run desktop-libs.yml -f commit=true`). It builds each one on a
native runner, checks its exports and dependencies and loads it (the Linux
ones on glibc 2.17), runs `test/medut_smoke_test.dart` against the x64 ones,
and commits them. Locally, `scripts/build-linux.sh` builds against the system
OpenSSL into `dist/` without touching the committed ones (test it with
`MEDUT_LIB=../../dist/linux/x86_64/medut.so flutter test`), and
`scripts/build-windows.ps1 [-Arch x64|arm64]` refreshes
`windows/<arch>/medut.dll`.

On macOS, build `medut.dylib` with `scripts/build-macos.sh` and ship it on the
library search path. Any platform can load the library from a given path:

```dart
final sdk = await Medut.start(libPath: '/opt/myapp/medut.so');
```

Phone-only features are predictable there: `telecom` and `audioSession` are
ignored, `remember` is ignored and `rememberedAccounts()` is empty, audio
routes are an empty list (selecting does nothing; use `setOutputDevice`),
`notifyCallKitAudioActive` is a no-op, and `Medut.pushWake` throws
`UNSUPPORTED`. `start(monitorNetwork: false)` turns the C library's interface
polling off. See
[docs/quickstart/flutter.md](https://github.com/Ibrahimgamal99/Medut-SDK/blob/main/docs/quickstart/flutter.md#desktop).

See [`example/`](example/) for a runnable app and
[`docs/quickstart/flutter.md`](https://github.com/Ibrahimgamal99/Medut-SDK/blob/main/docs/quickstart/flutter.md)
for the full walkthrough: CallKit/Telecom, transfer, presence, recording,
handover and stats.

## Development

The Dart, Kotlin and Swift glue is generated from `pigeons/messages.dart`, the
desktop FFI bindings from `include/medut.h`:

```sh
dart run pigeon --input pigeons/messages.dart
dart run ffigen --config ffigen.yaml     # adjust the clang include path first
flutter test                             # desktop tests need scripts/build-linux.sh
```

## License

BSD 3-Clause — see [LICENSE](LICENSE). The native libraries statically link
libre and Opus (BSD-3-Clause) plus OpenSSL (Apache-2.0); parts of the stack
are adapted from baresip (BSD-3-Clause) — see the SDK's THIRD_PARTY_NOTICES.
