# Quick start — Flutter / Dart

The `EchoSDK` Flutter package (`bindings/flutter/`) is a hybrid plugin:

- **Dart FFI** drives the native SIP/media core (`libechosdk`).
- On **Android** the plugin ships prebuilt `libechosdk.so` for
  `arm64-v8a`, `armeabi-v7a` and `x86_64` inside its `jniLibs`, plus a small
  Kotlin shim (`EchoSDKPlugin`) that provides the app cache dir, voice-call
  audio focus, speakerphone routing, and network-change callbacks.
- On **iOS** the plugin vendors a prebuilt dynamic `EchoSDK.xcframework`
  (device arm64 + simulator arm64/x86_64) plus a Swift shim
  (`EchoSDKPlugin`) for audio-session activation, speakerphone routing, and
  `NWPathMonitor` network-change callbacks. Capture uses Apple's
  VoiceProcessingIO audio unit (hardware echo cancellation).
- **Desktop** (Linux/Windows/macOS) works as a plain FFI binding — the app
  bundles the native library itself (or passes `EchoSDK(libPath: ...)`).

## 1 — Add the package

```yaml
dependencies:
  echo_sdk:
    git:
      url: https://github.com/NawyRE/echo-sdk.git
      path: bindings/flutter
      ref: <full-40-char-sha>   # pin for anything you ship
  # or, from a checkout:
  # echo_sdk:
  #   path: path/to/EchoSDK/bindings/flutter
```

`path:` is required either way — the plugin lives in a subdirectory, not at
the repo root.

**Pin a full SHA before you release.** Without `ref:` the dependency floats on
the default branch, and with `path:` it floats on whatever native library that
branch happened to carry. A `path:` checkout floats even harder: it tracks your
working tree, so the app ships whatever `.so` is on your disk at build time —
convenient while the two repos move together, wrong for a release. If the repo
is private, CI needs an SSH deploy key or token; pub fetches git dependencies
with a plain `git clone` and has no auth layer of its own.

Gradle caches the plugin's build output, so a dependency change is not always
enough to re-package the native library. After moving between refs (or after a
local rebuild) run `flutter clean` before `flutter run`, and confirm what
actually shipped:

```bash
# The two hashes must match; if they differ, the APK carries a stale library.
unzip -p build/app/outputs/flutter-apk/app-release.apk \
  lib/arm64-v8a/libechosdk.so | md5sum
md5sum bindings/flutter/android/src/main/jniLibs/arm64-v8a/libechosdk.so
```

Android needs nothing else — the plugin already carries the native libraries
and manifest permissions (`INTERNET`, `RECORD_AUDIO`,
`MODIFY_AUDIO_SETTINGS`, `ACCESS_NETWORK_STATE`). Your app must still
*request* the microphone permission at runtime (e.g. with
`permission_handler`) before the first call. `minSdkVersion` must be >= 24.

iOS (>= 13.0) additionally needs, in your app's `Info.plist`:

```xml
<key>NSMicrophoneUsageDescription</key>
<string>Microphone access is required for voice calls.</string>
<key>UIBackgroundModes</key>
<array><string>audio</string></array>
```

For production VoIP apps, add PushKit + CallKit in your app (Apple kills
background sockets; the push→CallKit→re-register flow is the sanctioned
model — pair it with `account.setPushToken()` / RFC 8599, or your own
server-side push).

With CallKit, hand it the audio session:

```dart
final sdk = await EchoSDK.start(
  config: const EchoSDKConfig(platformAudioActivate: false),
  manageAudioSession: false,   // CXProvider owns activation
);
```

`CXProvider` must be the only thing that activates the `AVAudioSession`
(Apple requires it in `provider(_:didActivateAudioSession:)`). Left at the
default, starting the SDK — at launch, or on a PushKit wake while CallKit is
still reporting the call — seizes the exclusive PlayAndRecord route out from
under CallKit. The category and mode are still configured, so audio works as
soon as CallKit activates the session.

The vendored xcframework must exist at
`bindings/flutter/ios/Frameworks/EchoSDK.xcframework` — it is built by CI
(`.github/workflows/build-mobile.yml`) or on any Mac with
`scripts/build-ios.sh`; `pod install` fails with a clear error when it is
missing.

## 2 — Start the SDK

```dart
import 'package:echo_sdk/echo_sdk.dart';

final sdk = await EchoSDK.start(
  config: const EchoSDKConfig(
    statsIntervalMs: 5000,        // MediaStatsEvent every 5 s
    mosAlertThreshold: 3.5,       // QualityAlertEvent when MOS drops
    lossAlertThreshold: 5.0,
    jitterAlertThreshold: 40.0,
  ),
);
```

`EchoSDK.start()` is required on Android: it injects the app cache dir as
the SDK's `tmp_dir`, switches network monitoring from polling to
ConnectivityManager callbacks, and manages audio focus around calls. On
desktop it behaves like the plain constructor.

The native stack is a **process-wide singleton** — constructing a second
`EchoSDK` from an isolate that already has one throws until you call
`shutdown()`.

### Background isolates (push wakeups)

An Android headless engine destroys its Dart isolate when its task ends, while
the process — and the SIP stack inside it, still registered — stays alive. The
next push runs your start-up code in a *new* isolate against that live stack, so
`EchoSDK.start()` reattaches to it instead of failing:

```dart
final sdk = await EchoSDK.start(config: cfg);

if (sdk.reattached) {
  // The stack was already up: `cfg` was NOT applied (config only takes effect
  // at init), and the accounts/calls of the previous isolate are adopted.
  for (final a in sdk.accounts) print('${a.aor} is ${a.regState}');

  // An INVITE that arrived while no isolate was listening fired no event here —
  // but the call is live, so pick it up from state instead.
  for (final c in sdk.calls) {
    if (c.state == CallState.ringing) showIncomingCallUi(c);
  }
} else {
  final account = sdk.createAccount('alice@pbx.example.com', 'secret');
  account.register();
}

// When the background task is done: park delivery, leave the stack up and
// push-reachable for the next wakeup. Use shutdown() only to really stop.
sdk.detach();
```

Pass `reattachIfRunning: false` to get the hard failure instead.

## 3 — Register an account (any transport)

```dart
// UDP / TCP / TLS — host comes from the AOR:
final account = sdk.createAccount('alice@pbx.example.com', 'secret',
    config: const AccountConfig(transport: Transport.tls));

// WS / WSS — a full server URL is required (scheme picks the transport):
final account = sdk.createAccount('alice@pbx.example.com', 'secret',
    config: const AccountConfig(
      serverUrl: 'wss://pbx.example.com:8089/ws',
      mediaEnc: MediaEncryption.dtlsSrtp,   // required by WebRTC gateways
      iceEnabled: true,
      stunServer: 'stun:stun.example.com:3478',
      // TURN is not optional on a carrier NAT — see the note below.
      turnServer: 'turn:turn.example.com:3478',
      turnUser: 'user', turnPass: 'pass',
      audioCodecs: ['opus', 'ulaw', 'alaw'],  // ordered preference (also the default)
    ));

account.register();
account.events.listen((ev) {
  if (ev is RegStateEvent) print('reg: ${ev.state}');
});
```

`AccountConfig` also covers auth user, display name, outbound proxy, push
tokens (RFC 8599), DTMF mode, and per-account codec overrides.

Two settings above are worth being deliberate about:

- **`mediaEnc`** — a WSS/WebRTC gateway offers `UDP/TLS/RTP/SAVPF` and will not
  negotiate against the default unencrypted `RTP/AVP`. baresip reports that as
  *"no common audio or video codecs"*, which points at the codec list when the
  media profile is the problem.
- **`turnServer`** — `iceEnabled` with STUN alone is enough for an ordinary NAT.
  It is *not* enough on a mobile carrier NAT that maps one local port to a
  different public IP per destination: the reflexive address STUN reports is not
  the one your PBX sees, so the candidate you signalled is wrong and a strict
  peer drops the media. Symptoms are intermittent — it works whenever the two
  views coincide. See [NAT traversal](../guides/nat_traversal.md#carrier-grade-nat-when-stun-is-not-enough).

## 4 — Calls

```dart
final call = account.call('bob@pbx.example.com');
call.hangup();  call.hold();  call.resume();
call.mute();    call.sendDtmf('5');
call.transfer('carol@pbx.example.com');        // blind
call.attendedTransfer(otherCall);               // REFER w/ Replaces
final stats = call.getStats();                  // on-demand MediaStats

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

`account.call(target)` takes a full SIP URI, a bare extension (`'120'`), a phone
number (`'+15551234'`), or any other legal SIP user part — the target is only
completed with a scheme and the account domain, never validated as numeric, so a
UUID dials like anything else:

```dart
account.call('550e8400-e29b-41d4-a716-446655440000');
// → INVITE sip:550e8400-e29b-41d4-a716-446655440000@pbx.example.com
```

`transfer()` takes the same forms. See [Dial targets](../api/calls.md#dial-targets)
for the completion rule and the server-side caveats.

## 5 — Reconnection & network handover

Registration retries automatically with exponential backoff
(`EchoSDKConfig.regRetry*` or per-account `setRetryPolicy`). On a network
change (Wi-Fi ↔ cellular) the SDK re-binds transports, re-REGISTERs, and
re-INVITEs active calls; progress arrives as `NetworkEvent` stages.

Throughout any of that the account reports `RegState.reconnecting` — a bad link,
a lost one, a handover, a keepalive probe the proxy stopped answering — so a
status indicator has one state to render as "Reconnecting…". `RegState.failed`
is kept for what the SDK has given up on: bad credentials, an exhausted retry
budget, a retry the app cancelled.

```dart
account.setRetryPolicy(initialMs: 2000, maxMs: 60000, backoff: 2.0);
account.retryNow();     // skip the current backoff
account.cancelRetry();  // stop retrying

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

On Android the plugin feeds ConnectivityManager changes to the SDK
automatically. On desktop the built-in poller
(`netMonitorIntervalSeconds`) covers it, or call `sdk.networkChanged()`
from your own connectivity hook.

Calls that negotiated **ICE** recover best-effort only: baresip cannot
regenerate ICE credentials mid-call (`NetworkEvent.ice` is true for these).

## 6 — Audio

```dart
sdk.listInputDevices();  sdk.setInputDevice('...');   // desktop devices
sdk.setSpeakerphone(true);                            // Android routing
sdk.setAec(true);  sdk.setNs(true);  sdk.setAgc(true);
sdk.setMicGainDb(6);  sdk.setSpeakerGainDb(-3);
```

Android capture/playback uses baresip's OpenSLES module (works on every
supported API level). Opus can be tuned via `EchoSDKConfig.opus`
(`OpusConfig(bitrate: 32000, fec: true, dtx: true, ...)`).

### Taking over the microphone and speaker

The SDK owns the audio device by default, and that is the tested path. Turn it
around when the platform and the SDK's driver disagree — Bluetooth routing that
will not follow, CallKit owning the session, a call that comes up one-way:

```dart
final sdk = await EchoSDK.start(
  config: const EchoSDKConfig(appOwnedAudio: true),
);

// or at runtime, including mid-call:
await sdk.useAppOwnedAudio(true);

sdk.appOwnedAudioErrors.listen((e) {
  // mic-permission | unsupported-rate | device-open | capture-dead | ...
  // Nothing else reports these: the SDK is no longer holding the device.
  print(e);
});
```

The plugin ships a working capture/playback engine for both platforms, so most
apps need nothing further —
[`AppOwnedAudioEngine.kt`](../../bindings/flutter/android/src/main/kotlin/dev/echosdk/flutter/AppOwnedAudioEngine.kt)
(`AudioRecord`/`AudioTrack` on `VOICE_COMMUNICATION`) and
[`EchoSDKExternalAudio.m`](../../bindings/flutter/ios/Classes/EchoSDKExternalAudio.m)
(a `VoiceProcessingIO` AudioUnit).

**The realtime loop is native on purpose.** `push`/`pull` are reachable from
Dart FFI but deliberately absent from the `EchoSDK` API: they run on a
10-20 ms deadline, and a GC pause on the capture path is a dropped frame. Dart
flips the mode and reads the format; Kotlin and Objective-C move the samples.

To write your own loop instead of using the shipped one, call the C directly —
`echosdk_audio_external_push/pull/format` — from your own audio thread. On
Android the JNI entry points are already exported from `libechosdk.so` as
`dev.echosdk.ExternalAudio`, so no NDK build is needed:

```kotlin
val fmt = IntArray(3)
if (ExternalAudio.nativeFormat(fmt) == 0) {
    val (srate, ch, ptime) = Triple(fmt[0], fmt[1], fmt[2])
    // direct ByteBuffers only — a heap buffer has no address to hand C
    ExternalAudio.nativePush(captureBuf, samples)
    ExternalAudio.nativePull(playBuf, samples)
}
```

Two things to get right, both covered by the shipped engine:

- **Capture through the platform voice path.** The SDK's mobile echo canceller
  *is* the capture preset of the driver you just displaced, so it leaves with
  it. Use `MediaRecorder.AudioSource.VOICE_COMMUNICATION` under
  `MODE_IN_COMMUNICATION`, or `VoiceProcessingIO` — otherwise the call echoes.
- **Poll the format; don't wait for an event.** There is no "media is up"
  event. `CallState.established` is a SIP state and races the device, and a
  mid-call re-INVITE can change the codec with no state change at all.

CallKit hosts must also forward session activation, since they own it:

```dart
// in CXProviderDelegate
await sdk.notifyCallKitAudioActive(true);   // didActivate
await sdk.notifyCallKitAudioActive(false);  // didDeactivate
```

See [App-owned audio device](../api/media.md#app-owned-audio-device) for the
full contract.

## 7 — Logging & tracing

The SDK never writes to stdout/stderr. Everything arrives as events:
`LogEvent` (log lines), `SipTraceEvent` (raw SIP, enable
`EchoSDKConfig.traceSip`), `SdpNegotiationEvent` (enable `traceSdpDiff`),
plus `sdk.pcapStart('/path/trace.pcap')` for Wireshark captures.

## Example app

`bindings/flutter/example/` is a runnable softphone (Android):

```bash
cd bindings/flutter/example
flutter run
```

Three tabs: Account (all five transports, codec ordering, retry controls),
Call (dial/answer/hold/mute/DTMF/transfer/speaker), Diagnostics (live
stats, quality alerts, handover timeline, in-app log console).

## Rebuilding the native libraries (maintainers)

```bash
# Android — builds all ABIs, verifies symbols/alignment, refreshes the
# plugin's stripped jniLibs copies:
ANDROID_NDK=$HOME/Android/Sdk/ndk/<ver> \
ECHOSDK_BUILD_ROOT=$HOME/.cache/echosdk-build \
  ./scripts/build-android.sh

# iOS (macOS + Xcode only) — builds the dynamic xcframework (device +
# simulator), verifies exports, refreshes the plugin's vendored copy:
./scripts/build-ios.sh

# Or let CI do both: .github/workflows/build-mobile.yml builds Android on
# ubuntu and the iOS xcframework on macos, uploading both as artifacts.

# Regenerate FFI bindings after changing include/echosdk.h:
cd bindings/flutter && dart run ffigen --config ffigen.yaml
```

`ECHOSDK_BUILD_ROOT` should point at a native filesystem when the repo
lives on NTFS — incremental builds there silently reuse stale objects.

`build-android.sh` refreshes `bindings/flutter/android/src/main/jniLibs/`
itself, as its last step per ABI (and `build-ios.sh` likewise stages
`bindings/flutter/ios/Frameworks/EchoSDK.xcframework`) — there is no second
command to run. Those `.so` files are tracked in git, and **a rebuild
reaches consumers only once they are committed and pushed**: apps pin a git
SHA, so an uncommitted rebuild leaves every consumer on the previous library
while your own `path:` checkout quietly uses the new one. That split is
invisible from the Dart side and presents as the SDK behaving differently on
device than it does in the example app or the Python binding. Commit the
refreshed `jniLibs` in the same commit as the C change that motivated it, and
hand consumers the new SHA.

The desktop smoke test for the binding is
`bindings/flutter/test/echo_sdk_smoke_test.dart` (needs
`scripts/build-linux.sh` first); the C-side owned-events gate is
`test/owned_events_test.c`.
