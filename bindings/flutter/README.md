# Medut

SIP softphone SDK for Flutter — a self-contained native stack (baresip/libre)
driven over `dart:ffi`, with no dependency on any particular host app.

Drop it into any Flutter app: registration over UDP/TCP/TLS/WS/WSS, calls,
hold/transfer/DTMF, SIP MESSAGE, presence, push (RFC 8599), network handover,
and RTCP/MOS media statistics. Everything app-specific — AOR, credentials,
server URL, codecs, `User-Agent` — is configuration you pass in; the package
ships no UI and imposes no call flow.

| | |
|---|---|
| **Android** | arm64-v8a · armeabi-v7a · x86_64 · `minSdk` 24. Prebuilt `libmedut.so` ships in the package; Kotlin shim handles cache dir, audio focus, speakerphone, network callbacks, and an app-owned audio device (`AudioRecord`/`AudioTrack` on `VOICE_COMMUNICATION`). |
| **iOS** | 13.0+, device + simulator. Vendors `Medut.xcframework`; Swift shim handles `AVAudioSession`, speaker routing, `NWPathMonitor`. Capture uses VoiceProcessingIO (hardware AEC), also for the optional app-owned audio device. |
| **Linux / macOS / Windows** | Works as a plain FFI binding — the app bundles the native library itself, or passes `Medut(libPath: ...)`. |

## Install

```yaml
dependencies:
  medut:
    git:
      url: https://github.com/Ibrahimgamal99/Medut-SDK.git
      path: bindings/flutter
```

**Android** needs nothing further — the plugin carries the native libraries and
declares `INTERNET`, `RECORD_AUDIO`, `MODIFY_AUDIO_SETTINGS` and
`ACCESS_NETWORK_STATE`. Your app must still *request* the microphone permission
at runtime (e.g. `permission_handler`) before the first call.

**iOS** needs, in your app's `Info.plist`:

```xml
<key>NSMicrophoneUsageDescription</key>
<string>Microphone access is required for voice calls.</string>
<key>UIBackgroundModes</key>
<array><string>audio</string></array>
```

The vendored `bindings/flutter/ios/Frameworks/Medut.xcframework` is produced by
`scripts/build-ios.sh` on macOS or by the `build-mobile` CI workflow; `pod install`
fails with a clear message when it is absent. For production VoIP, add PushKit +
CallKit in your app and pair them with `account.setPushToken()`.

## Use

```dart
import 'package:medut/medut.dart';

final sdk = await Medut.start(
  config: const MedutConfig(
    statsIntervalMs: 5000,      // MediaStatsEvent every 5 s
    mosAlertThreshold: 3.5,     // QualityAlertEvent when MOS drops
    userAgent: 'MyApp/2.1',     // your app's SIP User-Agent
  ),
);

final account = sdk.createAccount(
  'alice@pbx.example.com',
  'secret',
  config: const AccountConfig(
    serverUrl: 'wss://pbx.example.com:8089/ws',  // WS/WSS need a URL
    mediaEnc: MediaEncryption.dtlsSrtp,
    audioCodecs: ['opus', 'ulaw', 'alaw'],
  ),
);
account.register();

account.events.listen((ev) {
  if (ev is RegStateEvent && ev.state == RegState.registered) {
    account.call('bob@pbx.example.com');
  } else if (ev is IncomingCallEvent) {
    ev.call.answer();          // or ev.call.reject(486)
  }
});
```

`Medut.start()` is what makes the package portable across apps: it asks the
platform shim for the app's own cache directory (baresip's `tmp_dir`), switches
network monitoring to OS callbacks, and manages the audio session around calls.
The native stack is a **process-wide singleton** — a second `Medut` in a
process that already has one throws unless you `shutdown()` first, and
`Medut.start()` reattaches (`sdk.reattached`) when a background isolate wakes
up against a live stack.

See [`example/`](example/) for a runnable app and
[`docs/quickstart/flutter.md`](https://github.com/Ibrahimgamal99/Medut-SDK/blob/main/docs/quickstart/flutter.md)
for the full API walkthrough (transfer, presence, recording, handover, stats).

## License

BSD 3-Clause — see [LICENSE](LICENSE). The native libraries bundled in this
package statically link baresip, libre and Opus (BSD-3-Clause) plus Mbed TLS
(Apache-2.0).
