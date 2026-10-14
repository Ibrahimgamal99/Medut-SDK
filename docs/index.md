# Medut

A general-purpose SIP client SDK built on [baresip](https://github.com/baresip/baresip)
and [libre](https://github.com/baresip/re). It registers against any
standards-compliant SIP server, ships no UI, and imposes no call flow —
everything app-specific is configuration you pass in.

---

## What it does

| Capability | Details |
|---|---|
| **SIP** | INVITE, BYE, REGISTER, MESSAGE, SUBSCRIBE/NOTIFY, REFER (blind + attended, plus accept/reject of an incoming REFER), PRACK/100rel |
| **WebRTC media** | DTLS-SRTP, SDES, ICE, STUN, TURN, RTCP-mux |
| **Transports** | UDP, TCP, TLS, WebSocket, WSS |
| **Audio** | Opus, G.711 (PCMU/PCMA), G.722, G.726; AEC, NS, AGC; PCM tap, WAV recording, device hot-switch, app-owned audio device |
| **Mobility** | Wi-Fi ↔ 4G/5G handover with media verification, `RECONNECTING` registration state, RFC 8599 push |
| **Degraded links** | Media-stall alerts, RTP timeout, OPTIONS keepalive, SRV failover, adaptive Opus bitrate |
| **Observability** | RTCP stats, MOS (E-model + simplified, per direction), quality alerts, SIP trace, pcap, SDP diff |
| **Messaging** | SIP MESSAGE (in/out), MWI, presence (PUBLISH/SUBSCRIBE, BLF) |
| **Multi-account** | Any number of accounts per stack; every API is thread-safe |

---

## Platform support

| Platform | Architecture | TLS backend | Shared lib output |
|---|---|---|---|
| Linux | x86_64 | OpenSSL (embedded) | `dist/linux/x86_64/medut.so` |
| macOS | x86_64 + arm64 (universal) | OpenSSL (embedded) | `dist/macos/universal/medut.dylib` |
| Windows | x64 | OpenSSL (embedded via vcpkg) | `dist\windows\x64\medut.dll` |
| Android | arm64-v8a, armeabi-v7a, x86_64 | mbedTLS (embedded) | `dist/android/<ABI>/medut.so` |
| iOS | device + simulator | mbedTLS (embedded) | `dist/ios/Medut.xcframework` |

Each platform build script produces both the static archive (`medut.a` / `bare.lib`) and the shared library in one step. Shared libraries are **fully self-contained** — consumers need no extra packages at runtime.

---

## Language support

| Language | Binding | Location |
|---|---|---|
| C / C++ | Header-only RAII wrapper | `bindings/cpp/medut.hpp` |
| Python | cffi | `bindings/python/` |
| Kotlin (Android) | JNI | `bindings/kotlin/` |
| Swift (iOS) | Swift package over the C module | `bindings/swift/` |
| Flutter | Pigeon over the Kotlin and Swift SDKs | `bindings/flutter/` |

---

## Quick start by language

- [C / C++](quickstart/c.md)
- [Python](quickstart/python.md)
- [Flutter / Dart](quickstart/flutter.md)

---

## SDK usage guide

**[How to use the SDK — all languages (C/C++, Python, Flutter)](guides/complete_example.md)**

Covers every operation in one place: account setup, transport, STUN/TURN/ICE,
calls, hold, mute, transfer, stats, audio devices, and more — with code for
all three languages side by side.

---

## API reference

- [Overview & architecture](api/overview.md)
- [Configuration reference](api/config.md)
- [Accounts & registration](api/accounts.md)
- [Calls](api/calls.md)
- [Media & audio](api/media.md)
- [Observability](api/observability.md)
- [Events reference](api/events.md)

---

## Dial refusals

A PBX dialplan typically refuses a call the caller is not allowed to place with
**403 Forbidden**, which the SDK reports as `MEDUT_ERR_FORBIDDEN` on
`MEDUT_EV_CALL_STATE` / `FAILED`. It is not `MEDUT_ERR_AUTH`: the account
is registered and its credentials are fine, so the app should say something
like *"Not allowed to call"* and never a login prompt. (`MEDUT_ERR_AUTH` is
still what 401 and 407 produce, and what a registration rejected with 403
produces — there the account itself was refused.)

403 carries no room for an explanation the user can read, so the reason travels
separately. **Today it does not travel over SIP at all**: the dialplan emits a
`dial_blocked` event, which reaches the app over the backend's own websocket,
outside the SDK. An app on the SDK therefore has the refusal but not yet its reason,
and should show the generic text until the websocket event arrives.

**Planned:** the dialplan also sends a SIP MESSAGE from
`sip:echo-dial-blocked@<pbx>`, addressed to the device that dialled so that a
web and a mobile client on one extension do not receive each other's messages.
That needs nothing new from the SDK when it lands — it arrives as
`MEDUT_EV_MESSAGE`, with `from_uri` naming the sender and `body` holding the
text — so an app can be written against it now:

- On `FAILED` with `MEDUT_ERR_FORBIDDEN`, show the generic refusal.
- On a message from `echo-dial-blocked@…`, show its body as the reason,
  replacing the generic text if the failure has already been shown, or keeping
  it ready for the failure that is about to arrive. The two are independent and
  either can arrive first, so neither may wait on the other.

Matching only on the user part (`echo-dial-blocked`) keeps the check working
across the PBX hostnames a deployment uses.

---

## Guides

- [NAT traversal (ICE / STUN / TURN)](guides/nat_traversal.md)
- [Network handover (Wi-Fi ↔ 4G/5G)](guides/network_handover.md)
- [Degraded links (bad signal, slow uplink, dead paths)](guides/degraded_links.md)
- [TLS and WSS setup](guides/tls_wss.md)
- [Multiple accounts](guides/multi_account.md)
- [WebRTC browser interop](guides/webrtc_browser.md)
- [Debugging SIP](guides/debugging.md)
