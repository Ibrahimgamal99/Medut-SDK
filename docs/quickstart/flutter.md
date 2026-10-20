# Quick start — Flutter

The `medut` Flutter package (`bindings/flutter/`) is a thin Pigeon layer over
the native SDKs, which own the SIP stack:

- **Android:** the Kotlin SDK (`bindings/kotlin`). It bundles the prebuilt
  `libmedut.so` for `arm64-v8a`, `armeabi-v7a` and `x86_64`, plus a small JNI
  layer.
- **iOS:** MedutKit (`bindings/swift`). It vendors the prebuilt
  `Medut.xcframework` (device arm64 plus simulator arm64/x86_64).

Native code can start the stack at app launch or from a push, and receive and
answer calls with no Flutter engine running. Dart's `Medut.start()` then
attaches to the stack that is already up. On Linux, macOS and Windows the
same Dart API drives the C library over `dart:ffi`; see
[Desktop](#desktop-linux-macos-windows).

## 1 — Add the package

```yaml
dependencies:
  medut:
    git:
      url: https://github.com/Ibrahimgamal99/Medut-SDK.git
      path: bindings/flutter
      ref: <full-40-char-sha>   # pin for anything you ship
```

You need `path:` because the plugin lives in a subdirectory. The Android
build compiles the Kotlin SDK from `bindings/kotlin`, and the iOS pod
compiles MedutKit from the sources under `bindings/flutter/ios` (the Swift
package in `bindings/swift` points at the same files). A git dependency (or
a full checkout) has both.

**Pin a full SHA before you release.** Without `ref:` the dependency floats
on the default branch, native libraries included. After moving between refs,
run `flutter clean` before `flutter run`: Gradle caches native build output.

**Android:**
- The SDK's manifest already declares the permissions (`INTERNET`,
  `RECORD_AUDIO`, `MODIFY_AUDIO_SETTINGS`, `ACCESS_NETWORK_STATE`).
- Request the microphone permission at runtime before the first call, for
  example with `permission_handler`.
- `minSdkVersion` must be at least 24.
- The plugin compiles a small JNI layer, so the build needs the Android NDK
  and CMake 3.22.1. AGP installs both automatically when the SDK licenses are
  accepted.

**iOS** (13.0 or later) needs these entries in your app's `Info.plist`:

```xml
<key>NSMicrophoneUsageDescription</key>
<string>Microphone access is required for voice calls.</string>
<key>UIBackgroundModes</key>
<array><string>audio</string><string>voip</string></array>
```

`Medut.xcframework` must exist at `bindings/flutter/ios/Frameworks/`. Build it on a
Mac with `scripts/build-ios.sh`, or fetch the CI build with
`scripts/fetch-prebuilt.sh`. If it is missing, `pod install` fails with a
clear error.

## 2 — Start the SDK

```dart
import 'package:medut/medut.dart';

final sdk = await Medut.start(
  config: const MedutConfig(
    statsIntervalMs: 5000,        // MediaStatsEvent every 5 s
    mosAlertThreshold: 3.5,       // QualityAlertEvent when MOS drops
  ),
);
```

Every `MedutConfig` field is optional, and anything left `null` keeps the C
library's default. The native side then:
- sets the state directory,
- drives network handover from the OS connectivity callback,
- activates call audio around calls, unless you pass a different
  `audioSession` mode.

### When native code owns the stack

The intended setup starts the stack natively, so the push → incoming call →
answer path never needs Dart:

- **Android:** `MedutAndroid.start(context)` in `Application.onCreate` or in
  your FCM service.
- **iOS:** `Medut.shared.start(...)` in the app delegate or the PushKit
  handler.

When Flutter comes up later, `Medut.start()` attaches instead of starting:

```dart
final sdk = await Medut.start();
if (sdk.reattached) {
  // Native code started the stack: start()'s config was not used, and its
  // accounts and calls are already here.
  for (final a in sdk.accounts) print('${a.aor} is ${a.regState}');
  for (final c in sdk.calls) {
    if (c.state == CallState.ringing) showIncomingCallUi(c);
  }
}
```

`sdk.detach()` stops listening and leaves the stack running.
`sdk.shutdown()` stops the stack itself.

### Telecom and CallKit

```dart
final sdk = await Medut.start(telecom: true);
```

Every call is then registered with the system call framework: on Android the
Kotlin SDK's self-managed ConnectionService (API 26+), on iOS MedutKit's
CallKit provider. Calls show in the system call UI (and on Bluetooth, Auto,
Wear, the lock screen); a cellular call and a SIP call never fight for
audio; and an incoming call rings with no Flutter engine up. On Android that
is the built-in call notification, whose full-screen intent opens the app;
on iOS it is the native CallKit screen. Nothing else in the Dart API
changes. On Android request the notification permission on API 33+.

With `Medut.start(telecom: true)` the native side forwards the framework's
audio activation itself, so `notifyCallKitAudioActive` is not needed.

### Your own CallKit or Telecom integration

When a platform call framework owns call audio, tell the SDK so it does not
activate audio itself:

```dart
final sdk = await Medut.start(audioSession: AudioSessionMode.callFramework);

// Forward the framework's activation:
await sdk.notifyCallKitAudioActive(true);   // CallKit didActivate / Telecom active
await sdk.notifyCallKitAudioActive(false);  // didDeactivate / inactive
```

`CXProvider` must be the only thing that activates the `AVAudioSession`, and
Apple requires it to happen in `provider(_:didActivateAudioSession:)`. Answer
the SIP call from there, not from `CXAnswerCallAction`, so media opens against
an active session.

### Push wake-up

Push handling is native: there is no Dart isolate to wait for when the
process is started by a push. On Android:

1. Register with `createAccount(uri, password, remember: true)`. The Kotlin
   SDK keeps the config in an encrypted store on the device.
2. Start the stack from `Application.onCreate` with
   `MedutAndroid.start(this, config, AndroidOptions(telecom = TelecomOptions()))`.
   It re-creates and registers the remembered accounts before any UI exists.
3. Give the account its RFC 8599 push parameters
   (`AccountConfig.pushProvider` / `pushToken` / `pushParam`,
   `account.setPushToken()` on refresh) and call `MedutPush.wake(this)` from
   your `FirebaseMessagingService` — see the
   [Kotlin README](../../bindings/kotlin/README.md#push-wake-up-fcm).

With Telecom on, the system rings; when Flutter comes up, `Medut.start()`
attaches (`sdk.reattached`) and finds the account in `sdk.accounts` and the
call in `sdk.calls`. The example app does all three
(`example/android/.../MedutApplication.kt`), and its debug build has a
`FakePushReceiver` that stands in for FCM:
`adb shell am broadcast -n dev.medut.medut/.FakePushReceiver`.

Apps already on `firebase_messaging` can opt into the same path from Dart:

```dart
@pragma('vm:entry-point')
Future<void> onBackgroundMessage(RemoteMessage message) async {
  // Runs MedutPush.wake on a background thread; true when the INVITE came.
  await Medut.pushWake(message.data);
}

FirebaseMessaging.instance.onTokenRefresh.listen(Medut.pushNewToken);
```

`Medut.pushWake` needs no `Medut` instance in that isolate, but the stack
must already be running (from `Application.onCreate`), or it throws
`NOT_RUNNING`. A native `FirebaseMessagingService` is still the better
choice: it needs no Flutter engine at all.

On iOS the same three steps are `createAccount(remember: true)`,
`Medut.shared.start(config, options:)` with `StartOptions.callKit` in
`didFinishLaunching`, and `MedutPush.wake(payload:completion:)` from the
`PKPushRegistryDelegate`, with `voip` in `UIBackgroundModes` — see the
[Swift README](../../bindings/swift/README.md#push-wake-up-pushkit) and the
example's `AppDelegate.swift`. `wake` reports the call to CallKit from the
push payload before the INVITE arrives, as iOS requires, which is why
`Medut.pushWake` throws `UNSUPPORTED` on iOS: PushKit has to reach the SDK
natively. `Medut.pushNewToken(hex)` works on iOS too
(`MedutPush.updatePushToken(hex:)`). Apple kills background sockets, and push
is the sanctioned wake-up.

When the app returns to the foreground, `account.refreshRegistration()` makes
an account reachable again the way a push does: the REGISTER is re-sent in
place (same Call-ID and Contact), which is what an RFC 8599 server waits for
before releasing a held INVITE.

## 3 — Register an account (any transport)

```dart
// UDP / TCP / TLS — host comes from the AOR:
final account = await sdk.createAccount('alice@pbx.example.com', 'secret',
    config: const AccountConfig(transport: Transport.tls));

// WS / WSS — a full server URL is required (the scheme picks the transport):
final account = await sdk.createAccount('alice@pbx.example.com', 'secret',
    config: const AccountConfig(
      serverUrl: 'wss://pbx.example.com:8089/ws',
      mediaEnc: MediaEncryption.dtlsSrtp,   // required by WebRTC gateways
      iceEnabled: true,
      stunServer: 'stun:stun.example.com:3478',
      // TURN is not optional on a carrier NAT — see the note below.
      turnServer: 'turn:turn.example.com:3478',
      turnUser: 'user', turnPass: 'pass',
      audioCodecs: ['opus', 'ulaw', 'alaw'],  // ordered preference
    ));

await account.register();
account.events.listen((ev) {
  if (ev is RegStateEvent) print('reg: ${ev.state}');
});
```

`AccountConfig` also covers auth user, display name, outbound proxy, push
tokens (RFC 8599), DTMF mode, and per-account codec overrides.

Two settings above are worth being deliberate about:

- **`mediaEnc`.** A WSS/WebRTC gateway offers `UDP/TLS/RTP/SAVPF` and will
  not negotiate against the default unencrypted `RTP/AVP`. The SDK reports
  that mismatch as a media-profile problem.
- **`turnServer`.** `iceEnabled` with STUN alone is enough for an ordinary
  NAT. It is not enough on a mobile carrier NAT that maps one local port to a
  different public IP per destination: the reflexive address STUN reports is
  not the one your PBX sees. See
  [NAT traversal](../guides/nat_traversal.md#carrier-grade-nat-when-stun-is-not-enough).

## 4 — Calls

Every action is a `Future`, and failures throw `MedutException`. Its `code`
is the SDK error name, such as `CALL_ENDED`, `NOT_RUNNING` or
`AUTHENTICATION`.

```dart
final call = await account.call('bob@pbx.example.com');
await call.hold();   await call.resume();
await call.mute();   await call.sendDtmf('5');
await call.transfer('carol@pbx.example.com');        // blind
await call.attendedTransfer(otherCall);               // REFER w/ Replaces
final stats = await call.getStats();                  // on-demand MediaStats
await call.hangup();

sdk.events.listen((ev) {
  switch (ev) {
    case IncomingCallEvent e:  e.call.answer();
    case CallStateEvent e:     print('${e.state} ${e.reason ?? ''}');
    case MediaStatsEvent e:    print('MOS ${e.stats.mosLq}');
    case QualityAlertEvent e:  print('alert: ${e.issue} ${e.value}');
    default: break;
  }
});
```

A call is the same Dart object for its whole life, and `call.state` follows
its events. Ended calls leave `sdk.calls`, but the object you hold keeps
working: `info()` still answers with the state and duration.

`account.call(target)` accepts several forms:
- a full SIP URI,
- a bare extension (`'120'`),
- a phone number (`'+15551234'`),
- any other legal SIP user part.

The target is only completed with a scheme and the account domain. See
[Dial targets](../api/calls.md#dial-targets).

## 5 — Reconnection & network handover

Registration retries automatically with exponential backoff
(`MedutConfig.regRetry*`, or per account `setRetryPolicy`). On a network
change (Wi-Fi ↔ cellular) the SDK re-binds transports, re-REGISTERs and
re-INVITEs active calls. Progress arrives as `NetworkEvent` stages, and the
native side feeds connectivity changes to the SDK by itself.

While the SDK recovers, the account reports `RegState.reconnecting`.
`RegState.failed` is reserved for what the SDK has given up on: bad
credentials, an exhausted retry budget, or a retry the app cancelled.

```dart
await account.setRetryPolicy(initialMs: 2000, maxMs: 60000, backoff: 2.0);
await account.retryNow();     // skip the current backoff
await account.cancelRetry();  // stop retrying

sdk.events.listen((ev) {
  if (ev is RegStateEvent && ev.state == RegState.reconnecting) {
    print(ev.retryAttempt > 0
        ? 'reconnecting — attempt ${ev.retryAttempt} in ${ev.retryDelayMs} ms'
        : 'reconnecting…');
  }
  if (ev is NetworkEvent && ev.stage == NetworkStage.callMigrated) {
    print('audio recovered after ${ev.elapsedMs} ms');
  }
});
```

## 6 — Audio

```dart
final routes = await sdk.listAudioRoutes();   // earpiece / speaker / BT / wired
await sdk.setAudioRoute('bluetooth');
await sdk.setSpeakerphone(true);
sdk.audioRoutes.listen((routes) => print(routes)); // headset plugged, BT up, ...

await sdk.setMicGainDb(6);  await sdk.setSpeakerGainDb(-3);
```

Opus can be tuned through `MedutConfig.opus`
(`OpusConfig(bitrate: 32000, fec: true, dtx: true, ...)`).

### Taking over the microphone and speaker

By default the SDK owns the audio device, and that is the tested path. Turn
it around when the platform and the SDK's driver disagree: Bluetooth routing
that will not follow, CallKit owning the session, or a call that comes up
one-way.

```dart
final sdk = await Medut.start(
  config: const MedutConfig(appOwnedAudio: true),
);

// or at runtime, including mid-call:
await sdk.useAppOwnedAudio(true);

sdk.appOwnedAudioErrors.listen((e) {
  // mic-permission | unsupported-rate | device-open | capture-dead | ...
  // Nothing else reports these: the SDK is no longer holding the device.
  print(e);
});
```

The engine is part of the native SDKs:
- **Android:** `AppOwnedAudioEngine` captures and plays with `AudioRecord` and
  `AudioTrack` through `VOICE_COMMUNICATION`.
- **iOS:** `MedutAudioEngine` uses a `VoiceProcessingIO` AudioUnit.

Either way, the device opens only once call audio is active. The realtime
loop stays native on purpose, because a GC pause on the capture path is a
dropped frame. `appOwnedAudioStatus()` and `appOwnedAudioFormat()` report what
it is doing. See [App-owned audio device](../api/media.md#app-owned-audio-device)
for the full contract.

## 7 — Logging & tracing

The SDK never writes to stdout/stderr. Everything arrives as events:
- `LogEvent`: log lines.
- `SipTraceEvent`: raw SIP messages. Enable `MedutConfig.traceSip`.
- `SdpNegotiationEvent`: enable `traceSdpDiff`.

For Wireshark captures, use `sdk.pcapStart('/path/trace.pcap')`.

## Desktop (Linux, macOS, Windows)

The same Dart API runs on desktop over `dart:ffi`: `lib/src/ffi_backend.dart`
implements the plugin's host API by calling the C library directly, so
`Medut.start`, accounts, calls, events and errors behave as on mobile.

On Linux and Windows the plugin ships the prebuilt core, `linux/medut.so`
(x86_64, glibc 2.34+, needs OpenSSL 3 and PulseAudio) and
`windows/medut.dll` (x64, OpenSSL and zlib linked in, needs the MSVC
runtime). `flutter build linux` / `flutter build windows` bundle it with the
app, so `Medut.start()` needs no path. To refresh them after a core change:

- Linux: `scripts/build-linux.sh` rebuilds `linux/medut.so`; commit it.
- Windows: run the **Windows DLL** workflow
  (`gh workflow run windows-dll.yml -f commit=true`), which builds the DLL,
  runs `test/medut_smoke_test.dart` against it and commits it. Locally,
  `scripts/build-windows.ps1` refreshes `windows/medut.dll` too.

On macOS, build `medut.dylib` with `scripts/build-macos.sh` and ship it on the
library search path. Any platform can load the library from a given path:

```dart
final sdk = await Medut.start(libPath: '/opt/myapp/medut.so');
```

Mobile-only features do nothing there: `telecom` and `audioSession` are
ignored, `remember` is ignored and `rememberedAccounts()` is empty, and the
audio route list is empty.

## Example app

`bindings/flutter/example/` is a runnable softphone:

```bash
cd bindings/flutter/example
flutter run
```

It has three tabs:
- **Account:** all five transports, codec ordering, retry controls.
- **Call:** dial, answer, hold, mute, DTMF, transfer, speaker.
- **Diagnostics:** live stats, quality alerts, handover timeline, in-app log
  console.

## Maintainers

```bash
# After editing pigeons/messages.dart:
cd bindings/flutter && dart run pigeon --input pigeons/messages.dart

# Android core: builds all ABIs and refreshes the stripped copies the Kotlin
# SDK packages (bindings/kotlin/medut/src/main/jniLibs):
ANDROID_NDK=$HOME/Android/Sdk/ndk/<ver> ./scripts/build-android.sh

# iOS core (macOS + Xcode): builds the xcframework into bindings/flutter/ios/Frameworks:
./scripts/build-ios.sh
```

The prebuilt cores are tracked in git, and a rebuild reaches consumers only
once it is committed and pushed: apps pin a git SHA. Commit the refreshed
binaries in the same commit as the C change that motivated them.

Tests:
- **Dart:** `bindings/flutter/test/medut_test.dart`, with the native side
  faked at the Pigeon channels.
- **Native SDKs:** tested by their own suites, `bindings/kotlin/desktop-test`
  and `bindings/swift/Tests`.
