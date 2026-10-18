# Medut SDK

**A general-purpose SIP client SDK.** Register against any standards-compliant
SIP server, place and answer calls, and let the SDK handle SIP parsing, RTP
jitter buffers, NAT traversal, SRTP key exchange, and network handover.

Nothing about it is tied to one PBX, one app, or one call flow: it ships no UI,
imposes no state machine, and takes everything app-specific — AOR, credentials,
server URL, codecs, `User-Agent` — as configuration. Softphone, contact-centre
agent, embedded intercom, door station, or a headless call bot are all the same
API.

One shared library, one header (`include/medut.h`), full thread safety — call
any API from any thread. Built on [baresip](https://github.com/baresip/baresip)
and [libre](https://github.com/baresip/re), with the SIP/RTP internals kept out
of your way.

```python
import medut as sdk

acc = sdk.create_account("alice@pbx.example.com", "secret",
                         transport="wss", media_enc="dtls_srtp",
                         ice_enabled=True, audio_codecs=["opus", "pcmu"])
acc.register()

@sdk.on("registered")
def _(ev):
    sdk.call("bob@pbx.example.com")

@sdk.on("incoming_call")
def _(ev):
    print(f"call from {ev.from_uri}")
    ev.call.answer()

sdk.run()   # blocks, dispatching events until stop() or Ctrl-C
```

The same flow in C++ and Dart is in [Quick start](#quick-start) below.

---

## Why Medut SDK

| | |
|---|---|
| **Server-agnostic** | Plain RFC 3261 signalling with no vendor extensions in the hot path. If it speaks SIP, it works — Asterisk, FreeSWITCH, Kamailio, OpenSIPS, 3CX, or a carrier SBC. |
| **Every transport, including WebSocket** | UDP, TCP, TLS, WS and WSS in the same build. RFC 7118 over one connection, so a client behind a reverse proxy or a browser-facing gateway needs no separate stack. |
| **Carrier-grade media out of the box** | ICE/STUN/TURN, DTLS-SRTP and SDES, RTCP with MOS scoring, adaptive jitter buffer, AEC/NS/AGC — not add-ons you assemble yourself. |
| **Survives real networks** | Wi-Fi ↔ 5G handover with media verification, media-stall and RTP-timeout detection, OPTIONS keepalive, DNS SRV failover, adaptive Opus bitrate, and a `RECONNECTING` state that models recovery instead of dropping you to `FAILED`. |
| **One flat C ABI** | 80 functions in one header, opaque handles, append-only structs, no callbacks into internal types. Easy to bind from anything — every binding (C++, Python, Kotlin, Swift, Flutter) is thin. |
| **Thread-safe by construction** | Every public call is safe from any thread; events arrive on a dedicated dispatch thread and you may re-enter the API from inside a handler. |
| **Self-contained binaries** | TLS, zlib and audio deps are statically linked. No system OpenSSL, no runtime package hunt, no version skew between dev and production. |
| **Debuggable in the field** | SIP trace, SDP diff, pcap capture, per-call stats and quality alerts are part of the API, so a customer-site failure is a log you can read rather than a repro you have to build. |
| **Permissive licence** | BSD 3-Clause, statically linkable into closed-source products. No LGPL relinking obligation, no dual-licence fee. |

---

## Features

| Category | Features |
|---|---|
| **Transport** | UDP · TCP · TLS · WS · WSS |
| **Crypto** | SRTP-SDES · DTLS-SRTP |
| **NAT** | ICE · STUN · TURN |
| **Calls** | Invite · Answer · Hangup · Hold/Resume · DTMF |
| **Transfer** | Blind (REFER) · Attended (REFER w/ Replaces) · Accept/reject an incoming REFER (RFC 3515 NOTIFY) |
| **Messaging** | SIP MESSAGE send/receive |
| **Presence** | PUBLISH · SUBSCRIBE/NOTIFY · BLF · MWI |
| **Reliability** | PRACK / 100rel (RFC 3262) |
| **Mobility** | Network handover (Wi-Fi ↔ 4G/5G) — transport rebind · re-REGISTER · call re-INVITE with media verification · `RECONNECTING` registration state for the whole recovery |
| **Degraded links** | Media-stall alerts · RTP timeout · SIP OPTIONS keepalive · DNS SRV failover · adaptive Opus bitrate |
| **Push notifications** | RFC 8599 Contact URI params (APNs · FCM) · REGISTER-only custom headers · push wake-up that re-registers and rings before the INVITE lands (Kotlin, Swift) |
| **System call UI** | Telecom self-managed ConnectionService (Android) · CallKit provider (iOS) · answer, hold, mute and hang up from the system side · incoming calls ring with the app closed |
| **Accounts across restarts** | Keystore-encrypted store (Android) · Keychain (iOS) · restored and registered at every start, including one triggered by a push |
| **Custom headers** | Per-account and per-dialog (call) SIP headers |
| **Media** | Opus · G.711 · G.722 · G.726 · PCM tap · WAV recording · TX/RX mute · device enumerate + hot-switch · mic/speaker gain · AEC (half-duplex or WebRTC full-duplex) · NS · AGC · app-owned audio device |
| **Observability** | SIP trace · SDP diff · SIP pcap · RTCP/MOS stats · quality alerts · jitter buffer · audio level · bandwidth · logcat / unified log output from the mobile SDKs |
| **Multi-account** | Any number of accounts per stack |

---

## Standards and interoperability

Implemented per RFC, not per vendor:

| | |
|---|---|
| **Signalling** | RFC 3261 (SIP) · 3262 (100rel/PRACK) · 3263 (SRV resolution and failover) · 3515 (REFER) · 4028 (session timers) · 7118 (SIP over WebSocket) · 8599 (push Contact params) |
| **Media** | RFC 3550 (RTP/RTCP) · 4566 (SDP) · 4568 (SDES) · 4733 (DTMF events) · 5761 (RTCP mux) · 5763/5764 (DTLS-SRTP) |
| **NAT** | RFC 8445 (ICE) with STUN and TURN |

Asterisk is the primary development target, and
[`test/pbx_compat.c`](test/pbx_compat.c) is a live-server harness with
registration, call, DTMF, hold, transfer and MESSAGE scenarios plus
per-flavour cases for **3CX** (attended transfer), **FreeSWITCH** (strict
100rel) and **Kamailio** (presence/BLF). Server-side configuration for the
common deployments is in [TLS and WSS](docs/guides/tls_wss.md) and
[WebRTC browser interop](docs/guides/webrtc_browser.md).

---

## Platforms

| Platform | Shared library | Static archive | TLS backend |
|---|---|---|---|
| Linux x86_64 | `dist/linux/x86_64/medut.so` | `medut.a` | OpenSSL (embedded) |
| macOS universal | `dist/macos/universal/medut.dylib` | `medut.a` | OpenSSL (embedded) |
| Windows x64 | `dist\windows\x64\medut.dll` | `bare.lib` | OpenSSL (embedded via vcpkg) |
| Android arm64-v8a · armeabi-v7a · x86_64 | `dist/android/<ABI>/medut.so` | `medut.a` | OpenSSL (embedded, built from source) |
| iOS device + simulator | `dist/ios/Medut.xcframework` | — | OpenSSL (embedded, built from source) |

Shared libraries are **fully self-contained** — OpenSSL, zlib, and the audio
dependencies are baked in, so nothing extra is needed at runtime.

## Language bindings

| Language | Location | Style |
|---|---|---|
| C | `include/medut.h` | the entire API surface |
| C++ | `bindings/cpp/medut.hpp` | header-only RAII wrapper |
| Python | `bindings/python/` | cffi + event decorators |
| Kotlin (Android) | `bindings/kotlin/` | JNI layer, listener API; Telecom, FCM wake-up, encrypted account store |
| Swift (iOS) | `bindings/swift/` | Swift package, async event stream; CallKit, PushKit wake-up, Keychain account store |
| Flutter | `bindings/flutter/` | Pigeon over the Kotlin and Swift SDKs on mobile; `dart:ffi` on desktop |

On mobile the native SDK owns the stack for the life of the process: started
at launch or by a push, it registers the remembered accounts and rings an
incoming call through the system call UI with no app UI up. The Flutter
plugin attaches to that running stack.

---

## Getting the source

```bash
git clone https://github.com/Ibrahimgamal99/Medut-SDK.git
cd Medut-SDK
```

`third_party/` is not in git. The platform build scripts fetch libre,
baresip and Opus at the pinned revisions (and OpenSSL for Android and iOS)
through [`scripts/fetch-third-party.sh`](scripts/fetch-third-party.sh) on
first use, and move an existing checkout to the pin when it changes.

---

## Quick start

### Python

```bash
bash bindings/python/build.sh
# builds the SDK if needed, then pip-installs the package — no LD_LIBRARY_PATH

python bindings/python/examples/quickstart.py bindings/python/examples/account.json
```

Full API walkthrough: [docs/quickstart/python.md](docs/quickstart/python.md).

### C / C++

**Linux / macOS**

```bash
bash scripts/build-linux.sh          # or scripts/build-macos.sh
bash bindings/cpp/build.sh           # builds the examples
./bindings/cpp/build/quickstart bindings/cpp/examples/account.json
```

**Windows (PowerShell)** — needs Visual Studio 2022, CMake in `PATH`, and
[vcpkg](https://vcpkg.io) with `vcpkg install openssl zlib:x64-windows-static-md`:

```powershell
.\scripts\build-windows.ps1          # dist\windows\x64\medut.dll + bare.lib
cd bindings\cpp
.\build-examples.ps1
.\build\Release\quickstart.exe examples\account.json
```

The C++ wrapper in `bindings/cpp/medut.hpp`:

```cpp
#include "medut.hpp"

Medut::SDK sdk;
sdk.on_event([](const medut_event_t& ev) {
    if (ev.type == MEDUT_EV_REG_STATE &&
        ev.u.reg.state == MEDUT_REG_REGISTERED)
        puts("registered");
    if (ev.type == MEDUT_EV_INCOMING_CALL)
        Medut::Call(ev.u.incoming.call).answer();
});
sdk.init();

auto acct = sdk.create_account("alice@pbx.example.com", "secret",
                               MEDUT_TRANSPORT_TLS, {MEDUT_CODEC_OPUS});
acct.register_account();
auto call = acct.call("bob@pbx.example.com");
```

Plain C, manual compile, and Windows linking: [docs/quickstart/c.md](docs/quickstart/c.md).

### Flutter / Dart

```yaml
dependencies:
  medut:
    git:
      url: https://github.com/Ibrahimgamal99/Medut-SDK.git
      path: bindings/flutter
```

```dart
// telecom: calls go through Telecom / CallKit, so they ring with the app closed.
final sdk = await Medut.start(
    telecom: true, config: const MedutConfig(statsIntervalMs: 5000));

// remember: the native SDK keeps the account and registers it on every
// start, including one triggered by a push.
final account = await sdk.createAccount('alice@pbx.example.com', 'secret',
    remember: true,
    config: const AccountConfig(
      serverUrl: 'wss://pbx.example.com:8089/ws',
      mediaEnc: MediaEncryption.dtlsSrtp,
      audioCodecs: ['opus', 'ulaw'],
    ));
await account.register();

account.events.listen((ev) {
  if (ev is IncomingCallEvent) ev.call.answer();
});
```

Platform setup (permissions, `Info.plist`, the native start at launch, push):
[bindings/flutter/README.md](bindings/flutter/README.md) ·
[docs/quickstart/flutter.md](docs/quickstart/flutter.md).  Native apps use the
SDKs directly: [Kotlin](bindings/kotlin/README.md) ·
[Swift](bindings/swift/README.md).

---

## Account configuration

The examples take a JSON account file. `server_url`, `outbound_proxy`,
`server_host`, `server_port` and `auth_user` are derived from `uri` + `transport`
unless you set them (default ports: udp/tcp 5060, tls 5061, ws 8088, wss 8089).

```json
{
  "enabled":      true,
  "uri":          "100@pbx.example.com",
  "password":     "secret",
  "display_name": "Extension 100",
  "transport":    "wss",
  "media_enc":    "dtls_srtp",
  "ice_enabled":  true,
  "rtcp_mux":     true,
  "stun_server":  "stun:stun.l.google.com:19302",
  "verify_tls":   false,
  "audio_codec":  "opus"
}
```

Every field, including the handover and degraded-link tuning knobs:
[docs/api/config.md](docs/api/config.md).

---

## Debugging

The SDK is silent by default. For verbose init/shutdown traces
(`[bsdk] step N: ...`):

```bash
MEDUT_DEBUG_INIT=1 ./quickstart account.json        # Linux / macOS
```

```powershell
$env:MEDUT_DEBUG_INIT=1; .\quickstart.exe account.json
```

For runtime SIP/media verbosity, raise `medut_config_t.log_level`
(0=err, 1=warn, 2=info, 3=debug). To read the signalling itself, subscribe to
SIP trace events — see [docs/guides/debugging.md](docs/guides/debugging.md).

---

## Documentation

Everything lives in [`docs/`](docs/index.md):

**Quick starts** — [C/C++](docs/quickstart/c.md) ·
[Python](docs/quickstart/python.md) · [Kotlin](bindings/kotlin/README.md) ·
[Swift](bindings/swift/README.md) · [Flutter](docs/quickstart/flutter.md)

**Guides** — [Complete SDK usage, all languages](docs/guides/complete_example.md) ·
[NAT traversal](docs/guides/nat_traversal.md) ·
[Network handover](docs/guides/network_handover.md) ·
[Degraded links](docs/guides/degraded_links.md) ·
[Push notifications](docs/guides/push.md) ·
[TLS and WSS](docs/guides/tls_wss.md) ·
[Multiple accounts](docs/guides/multi_account.md) ·
[WebRTC browser interop](docs/guides/webrtc_browser.md) ·
[Debugging SIP](docs/guides/debugging.md)

**API reference** — [Overview](docs/api/overview.md) ·
[Configuration](docs/api/config.md) · [Accounts](docs/api/accounts.md) ·
[Calls](docs/api/calls.md) · [Media](docs/api/media.md) ·
[Observability](docs/api/observability.md) · [Events](docs/api/events.md)

[Changelog](docs/CHANGELOG.md)

---

## License

Medut SDK is released under the [BSD 3-Clause License](LICENSE).

The shipped libraries statically link baresip, libre and Opus (all BSD-3-Clause)
and OpenSSL (Apache-2.0), which is built from source on Android and iOS. Each
dependency's own licence text is in `third_party/<name>/` after
[`scripts/fetch-third-party.sh`](scripts/fetch-third-party.sh) runs.
