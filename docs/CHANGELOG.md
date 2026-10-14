# Changelog

All notable changes to Medut are documented here.

---

## [Unreleased]

### Fixed

- **RFC 8599 push params now reach the wire.** `pn-provider` / `pn-prid` /
  `pn-param` were built from the account config but never put in the
  REGISTER Contact, so no server had a token to push to (baresip job 11).
- **`medut_account_set_push_token()`** applies the new token: a REGISTER with
  it goes out at once, or once an in-flight one is answered, and over UDP,
  TCP and TLS the old token's binding is removed. A token too long for the
  Contact is refused and the old one stays. An account the app has not
  registered is no longer registered by it.
- **A push binding survives a re-created registration client** (handover,
  retry): the old client no longer un-REGISTERs a binding its successor
  carries on, which an RFC 8599 server saw as the device leaving (libre
  hunk R).
- **Push wake-up refreshes the registration** instead of sending an OPTIONS
  probe (Kotlin, Swift). RFC 8599 servers release a held INVITE only on the
  binding refresh.
- **A refreshed push token is saved** with the remembered accounts (Kotlin,
  Swift, Flutter), so a process a later push starts registers with it.
- **Kotlin in minified builds:** `MedutAndroid.start` failed with
  `NoSuchFieldError` once R8 renamed the enum and config types the JNI reads
  fields by. The consumer rules keep them.
- **Kotlin mute:** a Telecom route change no longer un-mutes a call the app
  muted itself.
- **Kotlin notification Answer** opens an activity directly, as Android 12+
  blocks one started from a notification's broadcast receiver; the ringtone
  loops (`FLAG_INSISTENT`).
- **Flutter plugin** forwards events and sets `onAppOwnedAudioError` only
  after Dart's `start()`, and chains to the host app's handler.

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

- **`medut_account_refresh_registration()`** (Kotlin
  `Account.refreshRegistration()`): re-sends the REGISTER from the client
  holding the binding (same Call-ID and Contact), as a push wake-up needs.
- **Push payload contract** (`docs/guides/push.md`): `type` (call / cancel),
  `call_id`, `caller_uri`, `caller_name`, `aor`, with configurable names.
  Kotlin: `MedutPush.wake(context, data)` rings through Telecom as soon as a
  call push arrives and binds the INVITE to the ring by Call-ID (an Answer
  tapped before it is carried out, a Decline rejects it); a cancel push or
  no INVITE in time ends the ring as missed. `MedutPush.onNewToken`,
  `PushPayload.parse`, `MedutTelecom.onShowPushIncomingCallUi`,
  `AccountStore.updatePushToken`.
- **Push guide** with the server contract and an OpenSIPS configuration.
- **Incoming-call filter** (`MedutTelecom.incomingCallFilter`,
  `MedutCallKit.incomingCallFilter`): reject before the system rings.
- **System-side state callbacks:** `MedutTelecom.onCallAudioStateChanged`;
  `MedutCallKit.onCallMutedChanged`, `onCallHeldChanged`.
- `MedutCallKit.update(options:)`; `MedutTelecom.answerPushRing` /
  `declinePushRing` and the ring key on `onShowPushIncomingCallUi`;
  `TelecomOptions.callActivity`; public `EXTRA_CALL_ID` / `ACTION_ANSWER` /
  `ACTION_DECLINE`.
- `AccountConfig.headers` / `registerHeaders`, saved by the account stores.
- **`endedLocally` on the call-state event** (Kotlin
  `MedutEvent.CallStateChanged`, Swift `CallStateEvent`, Flutter
  `CallStateEvent`): true when this side ended the call, including a Decline
  from the system call UI, a Bluetooth headset or a watch. An incoming call
  that ends unanswered with it set was declined, not missed.
- `MedutAndroid.setStartOnBoot()`: opt-in registration after a reboot.

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
