# Quick start — Flutter

The `medut` Flutter package (`bindings/flutter/`) is one Dart API over two
backends:

- **Android:** a thin Pigeon layer over the Kotlin SDK
  (`bindings/kotlin/medut`), which owns the SIP stack. It bundles the
  prebuilt `libmedut.so` for `arm64-v8a`, `armeabi-v7a` and `x86_64`, plus
  a small JNI layer.
- **iOS:** a thin Pigeon layer over MedutKit, the Swift SDK
  (`bindings/flutter/ios/MedutKit`, also published as the `bindings/swift`
  package). It vendors the prebuilt `Medut.xcframework` (device arm64 plus
  simulator arm64/x86_64).
- **Linux, macOS, Windows:** the Dart side drives the C library directly over
  `dart:ffi` (`lib/src/ffi_backend.dart`). The app bundles
  `medut.so` / `medut.dylib` / `medut.dll` itself, or passes
  `Medut.start(libPath: ...)`.

On mobile, native code can start the stack at app launch or from a push, and
receive and answer calls with no Flutter engine running. Dart's
`Medut.start()` then attaches to the stack that is already up. The methods,
events and errors are the same on every platform; what only a phone has
(Telecom/CallKit, push, remembered accounts, call-audio routes) is a
documented no-op or `UNSUPPORTED` error on desktop — see
[Desktop](#desktop).

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
build compiles the Kotlin SDK from `bindings/kotlin/medut`, and the iOS pod
compiles MedutKit from the sources under `bindings/flutter/ios`. A git
dependency (or a full checkout) has both.

**Pin a full SHA before you release.** Without `ref:` the dependency floats
on the default branch, native libraries included. After moving between refs,
run `flutter clean` before `flutter run`: Gradle caches native build output.

**Android:**
- The SDK's manifest already declares the permissions (`INTERNET`,
  `RECORD_AUDIO`, `MODIFY_AUDIO_SETTINGS`, `ACCESS_NETWORK_STATE`, and what
  Telecom and the call notification need).
- Request the microphone permission at runtime before the first call, for
  example with `permission_handler`.
- `minSdkVersion` must be at least 24; Flutter 3.44 or later (the plugin uses
  Flutter's built-in Kotlin).
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

and, for PushKit, the Push Notifications capability (the `aps-environment`
entitlement) on the Runner target.

`Medut.xcframework` must exist at `bindings/flutter/ios/Frameworks/`. Build
it on a Mac with `scripts/build-ios.sh`, or fetch the CI build with
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

Every `MedutConfig` field is optional, and anything left `null` keeps the
C library's default (`medut_config_init()`). On Android and iOS the native
side then:
- sets the state directory (the app's cache dir),
- drives network handover from the OS connectivity callback,
- activates call audio around calls, unless you pass a different
  `audioSession` mode.

The stack is process-wide. While it runs, `Medut.start()` returns the
running instance and does **not** apply the config and options you pass (a
debug-console warning says so when they differ); `shutdown()` first to start
with new ones.

### When native code owns the stack

The intended mobile setup starts the stack natively, so the push → incoming
call → answer path never needs Dart:

- **Android:** `MedutAndroid.start(context, config, options)` in
  `Application.onCreate`.
- **iOS:** `Medut.shared.start(config, options:)` in
  `application(_:didFinishLaunchingWithOptions:)`.

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
`CallStateEvent.endedLocally` tells a call declined from the system UI (or a
headset, or a watch) from one the far end ended.

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
   (`AccountConfig.pushProvider` / `pushToken` / `pushParam`) and hand your
   `FirebaseMessagingService` to the SDK:

   ```kotlin
   class PushService : FirebaseMessagingService() {
       override fun onMessageReceived(message: RemoteMessage) {
           MedutPush.wake(this, message.data)   // blocks up to 10 s
       }
       override fun onNewToken(token: String) {
           MedutPush.onNewToken(token)          // live and remembered accounts
       }
   }
   ```

   See the [Kotlin README](../../bindings/kotlin/README.md#push-wake-up-fcm)
   and [Push](../guides/push.md) for the payload and the server side.

With Telecom on, the system rings; when Flutter comes up, `Medut.start()`
attaches (`sdk.reattached`) and finds the account in `sdk.accounts` and the
call in `sdk.calls`. The example app does all three
(`example/android/.../MedutApplication.kt`), and its debug build has a
`FakePushReceiver` that stands in for FCM:
`adb shell am broadcast -n dev.medut.medut/.FakePushReceiver --es type call`.

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
(`MedutPush.updatePushToken(hex:)`).

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
`account.setReliableProvisional(ReliableProvisionalMode.required)` sets
100rel/PRACK per account.

Two settings above are worth being deliberate about:

- **`mediaEnc`.** A WSS/WebRTC gateway offers `UDP/TLS/RTP/SAVPF` and will
  not negotiate against the default unencrypted `RTP/AVP`. The SDK reports
  that mismatch as a media-profile problem.
- **`turnServer`.** `iceEnabled` with STUN alone is enough for an ordinary
  NAT. It is not enough on a mobile carrier NAT that maps one local port to a
  different public IP per destination: the reflexive address STUN reports is
  not the one your PBX sees. See
  [NAT traversal](../guides/nat_traversal.md#carrier-grade-nat-when-stun-is-not-enough).

`account.destroy()` is idempotent: destroying an account that is already gone
does nothing.

## 4 — Calls

Every action is a `Future`, and failures throw `MedutException`. Its
`code` is the SDK error name — `CALL_ENDED`, `NOT_RUNNING`,
`ACCOUNT_DESTROYED`, `AUTHENTICATION`, `TIMEOUT`, `INVALID_ARGUMENT`, ... —
the same on every platform, and `rawValue` the C return value
(`sdk.strerror(rawValue)` has the C library's text for it).

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
re-INVITEs active calls. Progress arrives as `NetworkEvent` stages. On
mobile the native side feeds connectivity changes to the SDK by itself; on
desktop the C library polls the interfaces (`netMonitorIntervalSeconds`,
default 10) and `sdk.networkChanged()` reports a change at once.

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

Routes are a phone concept: on desktop the list is empty and selecting does
nothing; pick devices with `listOutputDevices()` / `setOutputDevice()`.

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
- **Desktop:** only the switch; feeding the device
  (`medut_audio_external_push/_pull`) is the app's own native code.

On mobile the device opens only once call audio is active. The realtime loop
stays native on purpose, because a GC pause on the capture path is a dropped
frame. `appOwnedAudioStatus()` and `appOwnedAudioFormat()` report what it is
doing. See [App-owned audio device](../api/media.md#app-owned-audio-device)
for the full contract.

## 7 — Logging & tracing

The SDK never writes to stdout/stderr. Everything arrives as events:
- `LogEvent`: log lines, at or below `MedutConfig.logLevel` (0 = errors
  .. 3 = debug).
- `SipTraceEvent`: raw SIP messages. Enable `MedutConfig.traceSip`.
- `SdpNegotiationEvent`: enable `traceSdpDiff`.

`LogEvent` and `RegistrarWarningEvent` belong to no account; they arrive on
`sdk.events` and on every `account.events`. For Wireshark captures, use
`sdk.pcapStart('/path/trace.pcap')`.

## Desktop

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
final sdk = await Medut.start(
  libPath: '/opt/myapp/lib/medut.so',   // or leave null for the search path
);
```

On Linux and Windows the bundled core is used when `libPath` is left null.
A macOS app ships the `medut.dylib` that `scripts/build-macos.sh` builds.

What differs from mobile, predictably:

| Feature | Desktop behaviour |
|---|---|
| `start(telecom:, audioSession:)` | ignored |
| `start(monitorNetwork: false)` | sets `netMonitorIntervalSeconds` to 0 unless the config sets it |
| `createAccount(remember: true)` | `remember` ignored; `rememberedAccounts()` is empty, `forgetAccount()` a no-op (nothing but Dart starts the stack, so there is nothing to replay) |
| `listAudioRoutes()` / `setAudioRoute()` | empty list; `setSpeakerphone` / `setExternalRouting` are no-ops; `audioRoutes` never emits |
| `notifyCallKitAudioActive()` | no-op |
| `appOwnedAudioStatus()` | empty map; `appOwnedAudioErrors` never emits |
| `Medut.pushWake()` | throws `UNSUPPORTED` |
| `Medut.pushNewToken()` | sets the token on the running instance's accounts |
| `MedutConfig.logLevel` | any int reaches C (`-1` = no log events); mobile clamps to -1..3 |

`sdk.detach()` leaves the C stack running in the process, and the next
`Medut.start()` reattaches to it (`sdk.reattached`) and adopts its accounts
and calls, as on mobile.

## Example app

`bindings/flutter/example/` is a runnable softphone (Android, iOS):

```bash
cd bindings/flutter/example
flutter run
```

It has three tabs:
- **Account:** all five transports, codec ordering, retry controls, and
  "remember this account" for the native account store.
- **Call:** dial, answer, hold, mute, DTMF, transfer, speaker.
- **Diagnostics:** live stats, quality alerts, handover timeline, in-app log
  console.

## Maintainers

```bash
# After editing pigeons/messages.dart (Dart, Kotlin and Swift glue):
cd bindings/flutter && dart run pigeon --input pigeons/messages.dart

# After changing include/medut.h (desktop FFI bindings; adjust the clang
# include path in ffigen.yaml to your machine):
cd bindings/flutter && dart run ffigen --config ffigen.yaml

# Android core: builds all ABIs and refreshes the stripped copies the Kotlin
# SDK packages (bindings/kotlin/medut/src/main/jniLibs):
ANDROID_NDK=$HOME/Android/Sdk/ndk/<ver> ./scripts/build-android.sh

# iOS core (macOS + Xcode): builds the xcframework into
# bindings/flutter/ios/Frameworks:
./scripts/build-ios.sh

# Linux core, for the desktop backend and its tests:
./scripts/build-linux.sh
```

The prebuilt cores are tracked in git, and a rebuild reaches consumers only
once it is committed and pushed: apps pin a git SHA. Commit the refreshed
binaries in the same commit as the C change that motivated them.

Tests:
- **Dart, mobile backend:** `bindings/flutter/test/medut_test.dart`, with
  the native side faked at the Pigeon channels.
- **Dart, desktop backend:** `bindings/flutter/test/medut_smoke_test.dart`
  against `dist/linux/x86_64/medut.so` (skipped when it is not built), and
  `test/e2e_asterisk_test.dart` against a local Asterisk (skipped when none
  listens on 127.0.0.1:8089).
- **Native SDKs:** tested by their own suites, `bindings/kotlin/desktop-test`
  and `bindings/swift/Tests`.
