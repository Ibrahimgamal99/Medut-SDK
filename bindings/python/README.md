# Medut SDK

There was no production-ready VoIP SDK for Python — if you wanted SIP calling, you had to wrap C libraries yourself or glue together low-level tools. Medut SDK fixes that.

Register an account, place and receive calls, handle encryption and NAT traversal, monitor call quality — all from Python. Python bindings for [Medut SDK](https://github.com/Ibrahimgamal99/Medut-SDK), built on [baresip](https://github.com/baresip/baresip) + [libre](https://github.com/baresip/re).

```bash
pip install medut
```
---

## Features

| Category | |
|---|---|
| Transport | UDP · TCP · TLS · WS · WSS |
| Crypto | SRTP-SDES · DTLS-SRTP |
| NAT | ICE · STUN · TURN |
| Calls | Invite · Answer · Hangup · Hold/Resume · DTMF · Blind/Attended transfer |
| Messaging | SIP MESSAGE · Presence · BLF · MWI |
| Audio | AEC · NS · AGC · gain · mute · device hot-switch · PCM tap · WAV recording |
| Stats | RTCP/MOS · jitter · bandwidth · SIP trace · pcap |
| Multi-account | ✓ — unlimited accounts per stack |
| Thread-safe | ✓ — call any API from any thread |

---

## Requirements

Wheels are published for Linux (x86_64, aarch64) and Windows (x64, ARM64), Python 3.9+.

**Linux** — glibc 2.17 or newer (CentOS 7 and later) and one runtime library:

```bash
sudo apt install libpulse0                            # Ubuntu/Debian
sudo dnf install pulseaudio-libs                      # Fedora/RHEL
sudo pacman -S libpulse                               # Arch
```

> `import medut` prints the exact install command if it is missing. OpenSSL and opus are statically embedded.  
> `libwebrtc-audio-processing-1` is optional — enables `AEC_WEBRTC` full-duplex mode; the SDK works without it.

**Windows** — only the Visual C++ Redistributable 2015–2022 ([x64](https://aka.ms/vs/17/release/vc_redist.x64.exe), [ARM64](https://aka.ms/vs/17/release/vc_redist.arm64.exe)) required. OpenSSL, zlib, and opus are statically embedded.
---

## Quick start

```python
import medut as sdk

# Receive calls
sdk.configure(log_level=1)
acc = sdk.create_account("alice@pbx.example.com", "secret")
acc.register()

@sdk.on("incoming_call")
def _(ev):
    ev.call.answer()

@sdk.on("ended")
def _(ev):
    sdk.stop()

sdk.run()
```

```python
# Make a call
sdk.configure(log_level=1)
acc = sdk.create_account("alice@pbx.example.com", "secret")
acc.register()

@sdk.on("registered")
def _(ev):
    sdk.call("bob@pbx.example.com")

@sdk.on("ended")
def _(ev):
    sdk.stop()

sdk.run()
```

**WSS + DTLS-SRTP + ICE:**
```python
sdk.configure(verify_server=True)
acc = sdk.create_account("alice@pbx.example.com", "secret",
    transport="wss", server_url="wss://pbx.example.com/ws",
    media_enc="dtls_srtp", ice_enabled=True,
    stun_server="stun:stun.l.google.com:19302")
```

---

## Audio

```python
sdk.set_aec(True)
sdk.set_ns(True)
sdk.set_agc(True)
sdk.set_mic_gain(6.0)       # dB, range −20 to +20
sdk.set_speaker_gain(-3.0)
sdk.set_jitter_buffer(20, 200)
```

| AEC mode | Quality | Requires |
|---|---|---|
| Suppressor *(default, mode=1)* | Half-duplex | nothing |
| WebRTC *(mode=2)* | Full-duplex | `libwebrtc-audio-processing-1` |

```python
# WebRTC full-duplex AEC — configure before first account (desktop only, opt-in build)
sdk.configure(aec_mode=2)   # 2 = WEBRTC; requires cmake -DMEDUT_WITH_WEBRTC_AEC=ON
```

---

## `sdk.create_account` parameters

| Parameter | Type | Example |
|---|---|---|
| `transport` | str | `"udp"` `"tcp"` `"tls"` `"ws"` `"wss"` |
| `media_enc` | str | `"none"` `"sdes"` `"dtls_srtp"` |
| `ice_enabled` | bool | |
| `stun_server` | str | `"stun:stun.l.google.com:19302"` |
| `turn_server` | str | `"turn:turn.example.com:3478"` |
| `server_url` | str | WebSocket URL — overrides host/port |
| `audio_codecs` | list[str] | `["opus", "ulaw", "alaw"]` |
| `extra_headers` | dict | Added to every REGISTER/INVITE |

`sdk.configure()` accepts global options (applies before first account):

| Key | Example |
|---|---|
| `log_level` | `0`–`3` |
| `stats_interval_ms` | `5000` |
| `verify_server` | `False` to skip TLS verification |
| `transport` | `"wss"` |

---

## Events — `@sdk.on(name)` names

| Name | When | Sub-names (also available) |
|---|---|---|
| `reg_state` | Registration changed | `registering` · `registered` · `unregistered` · `reconnecting` · `reg_failed` |
| `incoming_call` | INVITE received | — |
| `call_state` | Call changed | `calling` · `ringing` · `established` · `held` · `ended` · `cancelled` · `call_failed` |
| `dtmf` | Digit received | — |
| `media_stats` | Periodic RTCP/MOS report | — |
| `transfer_request` | REFER received | — |
| `message` | SIP MESSAGE received | — |
| `presence_state` | Buddy presence changed | — |
| `mwi` | Voicemail notification | — |
| `quality_alert` | MOS/loss/jitter/RTT threshold crossed | — |
| `sip_trace` | Raw SIP message *(enable with `trace_sip=True`)* | — |
| `*` | Every event (wildcard) | — |

```python
@sdk.on("registered")
def _(ev):
    print("registered!")

@sdk.on("incoming_call")
def _(ev):
    print(f"call from {ev.from_uri}")
    ev.call.answer()

@sdk.on("media_stats")
def _(ev):
    print(f"MOS={ev.mos_lq:.2f}  RTT={ev.rtt_ms:.0f}ms  loss={ev.loss_pct:.1f}%")
```

---