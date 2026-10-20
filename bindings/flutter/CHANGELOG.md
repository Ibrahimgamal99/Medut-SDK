# Changelog

## Unreleased

Added:

- `Account.refreshRegistration()`: re-send the REGISTER in place (same
  Call-ID and Contact), as a push wake-up does.
- `Account.setReliableProvisional(ReliableProvisionalMode)` for 100rel/PRACK.
- `Medut.pushWake(data)` and `Medut.pushNewToken(token)`, static, for apps on
  `firebase_messaging`: Android runs `MedutPush.wake` on a background thread;
  iOS `pushWake` throws `UNSUPPORTED` (PushKit must reach the SDK natively).
- `Medut.strerror(code)`, `MedutConfig.copyWith`, and
  `SdpNegotiationEvent.rejectedCodecs` / `warnings`.
- Log and registrar-warning events, which belong to no account, also reach
  every `Account.events`.
- Concurrent `Medut.start()` calls share one instance.
- The example app runs on desktop too (no Telecom, CallKit or runtime
  permissions there).
- **Desktop (Linux, macOS, Windows) over `dart:ffi`.** `lib/src/ffi_backend.dart`
  implements the plugin's host API on the C library, so the same Dart API runs
  on desktop. `Medut.start(libPath:)` loads the library from a given path;
  mobile-only features (Telecom/CallKit, remembered accounts, audio routes)
  do nothing there. Tested by `test/medut_smoke_test.dart` against the
  shipped core (skipped when it is not there).
- **The plugin ships the Linux and Windows cores.** `linux/medut.so` and
  `windows/medut.dll` are committed and declared as FFI plugins, so Flutter
  bundles them with the app and `Medut.start()` needs no `libPath` there.
  `scripts/build-linux.sh` refreshes the Linux one; the manual
  `windows-dll.yml` workflow builds, tests and (with `commit=true`) commits
  the Windows one.

Fixed:

- The audio routes stream keeps working after `shutdown()` then `start()`.
- An event that lacks its call (or its stats) arrives as `UnknownEvent`
  instead of failing a null check and stopping the event stream.
- `AudioSessionMode` crosses as explicit raw values (0, 1, 2), not enum
  positions.
- `logLevel` above 3 is clamped instead of refused by the native side.
- `Medut.start()` on a running SDK warns in debug builds when the config or
  options differ from the running ones (they are still ignored).
- `Account.destroy()` twice is a no-op; the call finalizer stays quiet after
  `detach()` / `shutdown()`.

The plugin is now a Pigeon layer over the native SDKs (Kotlin on Android,
MedutKit on iOS), which own the stack for the process lifetime.  Breaking:

- **Desktop goes through `dart:ffi`.** On Linux, macOS and Windows the same
  Dart API drives the C library directly (see Added above).
- **Async.** Actions return `Future`s and throw `MedutException`;
  `createAccount`, `Account.call`, `Call.info`, `Call.getStats`,
  `Call.isHeld` / `isMuted` / `iceState` included.
- **Config defaults.** `MedutConfig` fields are nullable, and null keeps the
  C default (the old `0` / `-1` sentinels are gone).  New fields for session
  timers, MOS method and handover tuning.
- **Start options.** `Medut.start(audioSession:, monitorNetwork:)` replaces
  `manageAudioSession` / `libPath` / `reattachIfRunning`; `reattached` now
  means the native side had already started the stack.
- **Accounts and calls are handles by id.** `Account.regState` and
  `Call.state` are cached from events (`Account.refreshRegState()` reads it
  now).  An ended `Call` stays usable for as long as it is held.
- **App-owned audio** lives in the native SDKs; `appOwnedAudioActive()` and
  `appOwnedAudioFormat()` are async.
- **Telecom and CallKit.** `Medut.start(telecom: true)` registers every
  call with the system: the Kotlin SDK's self-managed ConnectionService on
  Android, MedutKit's CallKit provider on iOS. System call UI, no fight with
  cellular calls, and incoming calls that ring with no Flutter engine up.
- **Built-in Kotlin.** The Android plugin no longer applies the Kotlin
  Gradle Plugin itself. Minimum Flutter 3.44 / Dart 3.12.
- **Remembered accounts.** `createAccount(remember: true)` hands the config
  to the native account store (Keystore-encrypted file on Android, Keychain
  on iOS); a process started at launch or by a push registers it before any
  Dart runs, and `Medut.start()` then attaches and finds it in `accounts`.
  `Account.destroy(forget: true)`, `forgetAccount()` and
  `rememberedAccounts()` manage the store.
- **`CallStateEvent.endedLocally`**: this side ended the call, including a
  Decline from the system call UI, a Bluetooth headset or a watch, so call
  history can tell a declined call from a missed one.

## 1.0.0

Initial release of the Flutter plugin for Medut: accounts, calls, transfer,
messaging, presence, ICE server configuration, network handover, audio routing,
app-owned audio on Android and iOS, and media statistics, over `dart:ffi` with
bundled native libraries for Android and iOS.
