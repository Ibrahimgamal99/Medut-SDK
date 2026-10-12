# Changelog

All notable changes to Medut are documented here.

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
