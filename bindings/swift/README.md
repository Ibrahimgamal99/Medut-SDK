# MedutKit — Swift binding

Swift API for the Medut SIP SDK on iOS. It wraps the same C core as the
Flutter, Python and C++ bindings (`include/medut.h`), and is written for apps
where native code owns the stack for the whole process lifetime: started at
launch or from a PushKit wake, with no Flutter engine involved in placing or
answering calls.

## Build

The native core is a binary target. On macOS:

```sh
scripts/build-ios.sh
```

This stages `Frameworks/Medut.xcframework` (with the header and module map
Swift needs) next to `Package.swift`, where it is committed;
`scripts/fetch-prebuilt.sh` fetches the CI build instead. Then add the package by local path in
Xcode, or run the tests on a simulator:

```sh
cd bindings/swift
xcodebuild test -scheme MedutKit -destination 'platform=iOS Simulator,name=iPhone 16'
```

## Use

```swift
import MedutKit

var options = StartOptions()
options.audioSession = .callKit          // CXProvider owns the AVAudioSession

try Medut.shared.start(MedutConfig(), options: options)

var cfg = AccountConfig(uri: "alice@pbx.example.com", password: "secret")
cfg.serverURL = "wss://pbx.example.com/ws"
cfg.mediaEncryption = .dtlsSRTP
cfg.iceEnabled = true
cfg.iceServers = [ICEServer(url: "stun:stun.l.google.com:19302")]
cfg.pushProvider = .apns
cfg.pushToken = voipTokenHex
cfg.pushParam = Bundle.main.bundleIdentifier

let account = try Medut.shared.createAccount(cfg)
try account.register()

Medut.shared.addEventHandler { event in
    switch event {
    case .registration(let e):
        print("registration: \(e.state)")
    case .incomingCall(let e):
        print("call from \(e.fromURI)")   // report it to CallKit, answer later
    case .callState(let e) where e.state.isTerminal:
        print("call ended: \(e.reason ?? "")")
    default:
        break
    }
}

let call = try account.call("sip:bob@pbx.example.com")
```

Or consume events as an async sequence:

```swift
for await event in Medut.shared.events() { ... }
```

## Design notes

- **One stack per process.** `Medut.shared` is the only instance. `start`
  takes over a stack something else in the process already started
  (`reattached` is then true) instead of failing.
- **Thread-safe.** Call any method from any thread. Events are decoded on
  the SDK's event thread and delivered on the queue you subscribe with (main by
  default), in order.
- **Memory-safe handles.** `Account` and `Call` methods never pass a freed
  pointer to C:
  - **After shutdown or a restart**, handles from an earlier stack throw
    `.notRunning`.
  - **After `destroy()`**, an account throws `.accountDestroyed`.
  - **After a call ends**, it throws `.callEnded`, and the object stays safe to
    hold. It keeps its native wrapper until you release it.
- **Defaults come from C.** `MedutConfig()` reads `medut_config_init()`, so
  this binding never keeps a second copy of the defaults.
- **Audio session.**
  - `.automatic` activates the `AVAudioSession` around calls, for apps without
    CallKit.
  - `.callKit` leaves activation to `CXProvider`. Answer from
    `provider(_:didActivate:)`.
  - `.manual` leaves activation to the app.

  `MedutAudioSession.shared` lists and selects routes in all three modes.
- **Network handover.** An `NWPathMonitor` reports only real path changes
  (same filter as the Flutter plugin). Turn it off with
  `StartOptions.monitorNetwork = false` to drive `Medut.shared.network.changed()`
  yourself.
- **App-owned audio.** `MedutAudioSession.shared.useAppOwnedAudio(true)`
  hands the microphone and speaker to `MedutAudioEngine`, a VoiceProcessingIO
  unit that starts only once the session is active. Under CallKit, forward
  `provider(_:didActivate:)` / `didDeactivate` to `callKitDidActivate()` /
  `callKitDidDeactivate()`.

The Flutter plugin (`bindings/flutter`) is a Pigeon layer over this package:
its pod compiles MedutKit from here and vendors the same xcframework.

Not yet in this package: CallKit/PushKit integration.
