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

This stages `Medut.xcframework` (with the header and module map Swift needs)
in `bindings/flutter/ios/Frameworks`, where it is committed;
`scripts/fetch-prebuilt.sh` fetches the CI build instead, and the
`ios-xcframework` workflow can commit one for you. `Sources/` and
`Frameworks/` here are symlinks into `bindings/flutter/ios`: the Flutter
plugin's pod compiles the same files, and CocoaPods cannot see through a
symlink, so the real directories live on its side. Then add the package by
local path in Xcode, or run the tests on a simulator:

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
  unit that starts only once the session is active. With your own CallKit,
  forward `provider(_:didActivate:)` / `didDeactivate` to
  `callKitDidActivate()` / `callKitDidDeactivate()`; the SDK's own CallKit
  integration does that itself.
- **Logging.** With `StartOptions.logToSystemLog` (default) the stack's log
  and call events go to the unified log, subsystem `dev.medut`, so a process
  woken by a push with no UI attached still shows up in Console.

## CallKit

```swift
var options = StartOptions()
options.callKit = CallKitOptions()          // name, icon, ringtone optional
try Medut.shared.start(MedutConfig(), options: options)
```

Every call is then also a CallKit call: it shows in the native call UI and
on the lock screen, a cellular call and a SIP call never fight for audio,
and CallKit owns the audio session (`AudioSessionPolicy.callKit` is set for
you). Nothing else changes: `account.call()` places a call and
`call.answer()` answers one, and the SDK routes them through CallKit's
actions so the system UI stays in step; the SIP answer goes out once CallKit
has activated the audio session. Answering, holding, muting and ending from
the system side drive the SIP call the same way.
`MedutCallKit.shared.onOutgoingCallRefused` reports an outgoing call CallKit
would not start (another call has the line).

## Push wake-up (PushKit)

The SDK carries no PushKit dependency. Give the server the RFC 8599 push
parameters through `AccountConfig(pushProvider: .apns, pushToken: hex,
pushParam: bundleID)`, keep the token fresh with `MedutPush.updatePushToken`,
and handle the push in your `PKPushRegistryDelegate`:

```swift
func pushRegistry(_ registry: PKPushRegistry, didUpdate credentials: PKPushCredentials,
                  for type: PKPushType) {
    MedutPush.updatePushToken(credentials.token)
}

func pushRegistry(_ registry: PKPushRegistry, didReceiveIncomingPushWith payload: PKPushPayload,
                  for type: PKPushType, completion: @escaping () -> Void) {
    // didFinishLaunching started the stack with CallKit and restored the accounts.
    MedutPush.wake(payload: payload.dictionaryPayload, completion: completion)
}
```

`wake` reports a call to CallKit for every push, before it returns (iOS 13+
ends an app that does not, and stops delivering VoIP pushes to one that keeps
missing). For a call push it reports the call at once,
using the caller from the payload (`caller_name` / `caller_uri`, configurable
through `MedutPush.payloadKeys`), makes every account reachable again, and
waits for the INVITE, which then takes over that CallKit call: the caller
shown is updated from the SIP From header and an Answer the user already
tapped is carried out. No INVITE within the wait ends the call as
unanswered. `MedutPush.reachAccounts()` is the same recovery without the
wait, for a foreground return: each account refreshes its registration in
place (`Account.refreshRegistration()`), which is what RFC 8599 servers wait
for before releasing a held INVITE.

The payload fields (`MedutPush.payloadKeys`, same defaults as the Kotlin
SDK):

| Key | Meaning |
|---|---|
| `type` | `"call"` (default when absent) or `"cancel"` |
| `call_id` | SIP Call-ID of the INVITE; the INVITE with that Call-ID takes over the CallKit call, and a cancel ends that one |
| `caller_uri`, `caller_name` | shown until the INVITE arrives |
| `medut` | optional nested dictionary holding the keys above |

A cancel push ends the CallKit call its `call_id` names (without one, the
oldest waiting one) as ended by the caller. When a push can lead to no call
(the stack is not running, it runs without `StartOptions.callKit`, a cancel
with nothing to cancel, an unknown `type`) `wake` still reports a call and
ends it at once, to keep iOS delivering pushes. Start the stack in
`didFinishLaunching` so a push finds it running. On
`pushRegistry(_:didInvalidatePushTokenFor:)` call
`MedutPush.clearPushToken()`.

## Accounts across restarts

A push starts the process with no UI, and the stack must register before
the INVITE arrives, so the account configs have to be on the native side.
`Medut.shared.accountStore` keeps them in the Keychain (accessible after the
first unlock):

```swift
let account = try Medut.shared.createAccount(cfg)
Medut.shared.accountStore.save(cfg)        // survives the process
try account.register()

Medut.shared.accountStore.remove(cfg.uri)   // on logout
```

`Medut.shared.start` re-creates and registers every saved account on a
fresh start (`StartOptions.restoreAccounts`, on by default), so
`didFinishLaunching` is all a cold start needs. A Flutter app gets the same
with `createAccount(remember: true)`. The stack's own `MedutConfig` is not
stored: pass it at launch, it is app policy rather than user data.

The Flutter plugin (`bindings/flutter`) is a Pigeon layer over this package:
its pod compiles MedutKit from here and vendors the same xcframework. CallKit
reaches Dart as `Medut.start(telecom: true)`; push handling and the media tap
are native-only, since they have no UI and no Dart isolate to wait for.
