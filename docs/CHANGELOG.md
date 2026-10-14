# Changelog

All notable changes to Medut are documented here.

---

## [Unreleased]

### Changed

- **Mobile architecture.** The Kotlin (`bindings/kotlin`) and Swift
  (`bindings/swift`) SDKs own the stack for the life of the process; the
  Flutter plugin is a Pigeon layer over them instead of a `dart:ffi` binding,
  and is mobile only. Every Dart action is async and throws
  `MedutException`.
- **DTLS-SRTP recovery.** As the DTLS server, a handshake that is under way
  is no longer reset at the deadline; only a peer that stays silent reaches
  the client-role fallback. Fixes silent inbound calls against FreePBX over
  WSS, where the ClientHello landed just before the first deadline.
- **pcap** is documented as SIP-only; RTP, STUN and DTLS were never written
  to the file.

### Added

- **Kotlin SDK:** Telecom integration through a self-managed
  ConnectionService (system call UI, Telecom-owned audio and routes, a
  ringing call notification with Answer / Decline), `MedutPush.wake()` for
  FCM, a Keystore-encrypted account store restored at start, the media tap,
  the cellular network request for ICE multipath, logcat output.
- **Swift SDK:** CallKit integration through the SDK's own provider,
  `MedutPush.wake(payload:)` for PushKit (reports the call to CallKit before
  the INVITE arrives), a Keychain account store restored at start, unified
  log output.
- **Flutter:** `Medut.start(telecom:)`, `createAccount(remember:)`,
  `Account.destroy(forget:)`, `forgetAccount()`, `rememberedAccounts()`; the
  example app starts the stack natively on both platforms.
- **CI:** `ios-xcframework` workflow builds the xcframework, compiles
  MedutKit, builds the example app and can commit the framework back.
  `build-mobile` runs the Dart analyzer and tests on plugin changes.
- **Android plugin** migrated to Flutter's built-in Kotlin (minimum Flutter
  3.44).

---

## [1.0.0] — 2026-10-12

Initial release of Medut, a general-purpose SIP client SDK with a flat C ABI
(`include/medut.h`) and C++, Python and Flutter/Dart bindings.

### Added

- **Signalling:** registration, calls (invite, answer, hangup, hold/resume,
  DTMF), blind and attended transfer (REFER), SIP MESSAGE, presence
  (PUBLISH, SUBSCRIBE/NOTIFY, BLF, MWI), PRACK/100rel, session timers,
  per-account and per-call custom headers, any number of accounts per stack.
- **Transports:** UDP, TCP, TLS, WS and WSS, with DNS SRV resolution and
  failover and server certificate verification.
- **Media security:** SRTP-SDES and DTLS-SRTP.
- **NAT traversal:** ICE with STUN and TURN over UDP, TCP and TLS; ICE server
  lists in the browser URL grammar, relay-only transport policy, automatic ICE
  restart, and experimental multipath gathering.
- **Mobility:** network handover (Wi-Fi ↔ 4G/5G) with transport rebind,
  re-REGISTER, call re-INVITE with media verification, and a `RECONNECTING`
  registration state.
- **Degraded links:** media-stall alerts, RTP timeout, SIP OPTIONS keepalive,
  adaptive Opus bitrate, Opus FEC.
- **Push notifications:** RFC 8599 Contact URI parameters (APNs, FCM) and
  REGISTER-only custom headers.
- **Audio:** Opus, G.711, G.722, G.726; PCM tap, WAV recording, mute, device
  enumeration and hot-switch, gain, AEC (suppressor or WebRTC full-duplex),
  NS, AGC, and app-owned audio devices.
- **Observability:** SIP trace, SDP diff, pcap capture, RTCP/MOS statistics,
  quality alerts, jitter buffer and audio level reporting.
- **Platforms:** Linux, macOS, Windows, Android and iOS.
