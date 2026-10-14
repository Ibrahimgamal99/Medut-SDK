# Changelog

## Unreleased

The plugin is now a Pigeon layer over the native SDKs (Kotlin on Android,
MedutKit on iOS), which own the stack for the process lifetime.  Breaking:

- **Mobile only.** Desktop (Linux/Windows/macOS) is no longer supported by
  the plugin; use the C++ or Python bindings there.
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

## 1.0.0

Initial release of the Flutter plugin for Medut: accounts, calls, transfer,
messaging, presence, ICE server configuration, network handover, audio routing,
app-owned audio on Android and iOS, and media statistics, over `dart:ffi` with
bundled native libraries for Android and iOS.
