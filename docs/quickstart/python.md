# Quick start — Python

## Prerequisites

### Linux

One library is required that is not pre-installed on all distros:

```bash
# Ubuntu / Debian
sudo apt install libwebrtc-audio-processing-1

# Fedora / RHEL / CentOS
sudo dnf install webrtc-audio-processing

# Arch
sudo pacman -S webrtc-audio-processing
```

Everything else (`libssl`, `libz`, `libpulse`, …) is already on every desktop Linux.

> **Missing library at import time?** Medut raises `ImportError` with the exact
> install command for your distro — you don't need to remember the package name.

### Windows

No extra installs needed. OpenSSL, zlib, and opus are all statically embedded in
`medut.dll`. The only requirement is the **Visual C++ Redistributable (2015–2022)**,
which ships with Windows, Visual Studio, and most apps — it is almost never missing.

If you do hit a `vcruntime140.dll` error at import time, Medut will tell you:

```
winget install Microsoft.VCRedist.2022.x64
```

---

## One-command setup

### Linux / macOS

```bash
bash bindings/python/build.sh
```

This does everything in one step:
1. Builds the SDK if the `.so` is missing
2. Regenerates `_medut_clean.h` from `include/medut.h` so the binding stays in sync
3. Copies the `.so` into the package directory — no `LD_LIBRARY_PATH` needed
4. Installs the Python package (`pip install -e`)

Re-run `build.sh` whenever `include/medut.h` or the C source changes.

### Windows

```powershell
.\bindings\python\build.ps1
```

This does the same steps as `build.sh` but for Windows:
1. Builds `medut.dll` via `scripts\build-windows.ps1` if missing
2. Copies the DLL into the package directory
3. Installs the Python package (`pip install -e`)

**Prerequisites:** Visual Studio 2022, vcpkg with `VCPKG_ROOT` set, Python ≥ 3.9.

---

## Manual setup (alternative)

Build the shared library for your platform:

| Platform | Command | Output |
|---|---|---|
| Linux | `bash scripts/build-linux.sh` | `dist/linux/x86_64/medut.so` |
| macOS | `bash scripts/build-macos.sh` | `dist/macos/universal/medut.dylib` |
| Windows | `.\scripts\build-windows.ps1` | `dist\windows\x64\medut.dll` |
| Android | `bash scripts/build-android.sh` | `dist/android/<ABI>/medut.so` |

Then install the package:

```bash
# Linux / macOS
pip install bindings/python

# Windows
pip install bindings\python
```

The loader searches for the library in this order:

| Priority | Source |
|---|---|
| 1 | `MEDUT_LIB` env var — absolute path to the `.so`/`.dylib`/`.dll` |
| 2 | `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` directories |
| 3 | Package directory — the `.so` copied there by `build.sh` |
| 4 | Repo `dist/` directory — works from a source checkout |
| 5 | System library paths |

To pin a specific build:
```bash
export MEDUT_LIB=/abs/path/to/medut.so
# or
export LD_LIBRARY_PATH=/abs/path/to/dir:$LD_LIBRARY_PATH
```

---

## Register and wait for a call

```python
import medut as sdk

sdk.configure(log_level=1)
account = sdk.create_account("alice@pbx.example.com", "secret")
account.register()

@sdk.on("registered")
def _(ev):
    print("Registered!")

@sdk.on("incoming_call")
def _(ev):
    print(f"Incoming call from {ev.from_uri}")
    ev.call.answer()

@sdk.on("ended")
def _(ev):
    sdk.stop()

sdk.run()
```

---

## Make an outgoing call

```python
import medut as sdk

sdk.configure(log_level=1, stats_interval_ms=5000)
account = sdk.create_account("alice@pbx.example.com", "secret", transport="tls")
account.register()

@sdk.on("registered")
def _(ev):
    sdk.call("bob@pbx.example.com")

@sdk.on("media_stats")
def _(ev):
    print(f"MOS: {ev.mos_lq:.2f}  RTT: {ev.rtt_ms:.0f} ms")

@sdk.on("ended")
def _(ev):
    sdk.stop()

sdk.run()
```

---

## Custom TLS + ICE

```python
import medut as sdk

sdk.configure(log_level=2, verify_server=True)
account = sdk.create_account("alice@pbx.example.com", "secret",
    transport   = "wss",
    server_url  = "wss://pbx.example.com/ws",
    media_enc   = "dtls_srtp",   # a WSS gateway will not accept plain RTP
    ice_enabled = True,
    stun_server = "stun:stun.l.google.com:19302",
    # TURN is required on a carrier NAT that maps one local port to a different
    # public IP per destination — STUN alone reports an address the PBX never
    # sees, and the media is dropped. See guides/nat_traversal.md.
    turn_server = "turn:turn.example.com:3478",
    turn_user   = "user",
    turn_pass   = "pass",
)
account.register()
```

---

## Media stats

Stats fire automatically via the `media_stats` event when `stats_interval_ms > 0`.
For custom polling intervals, use `call.poll_stats()`:

```python
@sdk.on("established")
def _(ev):
    # Print stats every 2 s (independent of stats_interval_ms)
    ev.call.poll_stats(interval=2.0, on_update=lambda s: s.print())

@sdk.on("media_stats")
def _(ev):
    # Receive SDK-timed stats
    print(f"MOS={ev.mos_lq:.2f}  RTT={ev.rtt_ms:.0f}ms  loss={ev.loss_pct:.1f}%")
```

For a one-shot snapshot from any thread:
```python
stats = call.stats()                  # returns a new CallStats
call.fetch_stats(existing_stats)      # updates an existing CallStats in-place
```

---

## App-owned audio device

The SDK opens the microphone and speaker by default. To own them yourself —
feeding PCM in and taking it out, while SIP/ICE/SRTP/codecs/jitter stay with the
SDK:

```python
sdk.use_external_audio(True)

fmt = sdk.external_audio_format()          # (48000, 2, 20) or None before media
if fmt:
    srate, ch, ptime = fmt
    n = srate * ch * ptime // 1000
    sdk.external_audio_push(mic_pcm)       # bytes / array('h') / numpy int16
    spk = sdk.external_audio_pull(n)       # always exactly n samples
```

`examples/external_audio.py` is a complete runnable version: it pushes a
synthesised tone as the microphone and writes the far end to a WAV, so the whole
path can be verified on a desktop with no audio hardware. See
[App-owned audio device](../api/media.md#app-owned-audio-device) for the
contract, including that you own echo cancellation once you own the device.

---

## Runtime audio quality controls

```python
# Toggle filters on the fly at any time (module-level):
sdk.set_aec(True)
sdk.set_ns(False)
sdk.set_agc(True)

# Change jitter buffer bounds (takes effect on new calls):
sdk.set_jitter_buffer(20, 200)   # widen on a poor network

# Set per-call RTP DSCP on an established call:
call.set_dscp_rtp(46)  # EF — Expedited Forwarding
```

---

## Microphone and speaker gain

Manual dB gain control on the TX (mic) and RX (speaker) paths, independent of AGC.
Range: −20 to +20 dB. `0.0` = unity — fast-path bypass with no per-sample work.
Takes effect within one audio frame (~20 ms), safe to call from any thread.

```python
import medut as sdk

# Boost a quiet USB mic by 6 dB (≈ 2× amplitude):
sdk.set_mic_gain(6.0)

# Attenuate the speaker by 3 dB:
sdk.set_speaker_gain(-3.0)

# Back to unity (bypass):
sdk.set_mic_gain(0.0)
sdk.set_speaker_gain(0.0)
```

> **Interaction with AGC**: mic gain is applied *before* NS/AGC/AEC on the TX chain.
> AGC then normalises the boosted signal to −20 dBFS. If AGC is off, the raw dB boost
> reaches the encoder as-is.

---

## Echo cancellation

### Simple on/off (all platforms)

```python
sdk.set_aec(True)   # enable — uses the mode configured at init
sdk.set_aec(False)  # disable
```

The default mode is the half-duplex TX suppressor that attenuates the microphone when the far end is loud. It works on all platforms with zero external dependencies.

### Tuning the suppressor

```python
# 0.0 = no suppression (TX passes through freely)
# 1.0 = maximum suppression (default — −16.5 dB floor when RX is active)
sdk.set_aec_suppression_level(0.6)   # medium: less ducking on double-talk
sdk.set_aec_suppression_level(1.0)   # restore default
```

### Full-duplex WebRTC AEC (desktop, opt-in)

For true acoustic echo cancellation where both parties can speak simultaneously:

```python
# At init time — configure before first create_account():
sdk.configure(aec_mode=2)   # 2 = WEBRTC; requires MEDUT_WITH_WEBRTC_AEC=ON build

# At runtime — only off ↔ init_mode transitions are valid:
sdk.set_aec(False)   # pause
sdk.set_aec(True)    # resume WebRTC AEC
```

**Requires:**
1. Build with: `cmake -DMEDUT_WITH_WEBRTC_AEC=ON ...`
2. System library: `libwebrtc-audio-processing-1-dev` (Debian/Ubuntu) or equivalent
3. Desktop platform only — WebRTC AEC returns `ENOTSUP` on Android/iOS

> **Mobile**: on Android and iOS, full-duplex AEC is handled by the OS audio driver
> (AAudio `VOICE_COMMUNICATION` / `AVAudioSession .voiceChat`).
> No SDK configuration needed — the platform does it automatically.

### AEC mode comparison

| | Suppressor (default, mode=1) | WebRTC (advanced, mode=2) |
|--|--|--|
| **Duplex** | Half-duplex (ducks TX when RX is loud) | Full-duplex (cancels echo while both speak) |
| **Double-talk** | One side goes quiet | Both parties heard simultaneously |
| **CPU** | Negligible | Moderate |
| **Platform** | All | Desktop only |
| **Build flag** | None | `MEDUT_WITH_WEBRTC_AEC=ON` |
| **Best for** | Mobile / low-power / simple calls | Desktop softphone / professional voice |

---

## Module-level API

```python
import medut as sdk

sdk.configure(**kwargs)         # set global options before first account
sdk.create_account(uri, password, **kwargs)  # returns Account
sdk.call(target, account=None)  # place outbound call; auto-resolves single account
sdk.on(name)                    # decorator to register event handler
sdk.run()                       # block until sdk.stop() or Ctrl-C
sdk.stop()                      # signal run() to exit cleanly
sdk.version()                   # SDK version string
```

`sdk.create_account` accepts these string values:

| Kwarg | String values |
|---|---|
| `transport` | `"udp"` `"tcp"` `"tls"` `"ws"` `"wss"` |
| `media_enc` | `"none"` `"sdes"` `"dtls_srtp"` |
| `rel100` | `"disabled"` `"enabled"` `"required"` |
| `extra_headers` | `dict[str, str]` — calls `account.add_header()` for each pair |

`sdk.call(target)` accepts a full SIP URI, a bare extension (`"120"`), a phone number (`"+15551234"`), or any other legal SIP user part — a UUID (`"550e8400-e29b-41d4-a716-446655440000"`) dials fine, since the target is only completed with a scheme and the account domain, never validated as numeric. See [Dial targets](../api/calls.md#dial-targets). When only one account exists it is used automatically; pass `account=` when multiple accounts are registered.

---

## Event names for `@sdk.on()`

| Name | When fired |
|---|---|
| `"reg_state"` | Registration state changed (umbrella) |
| `"registering"` · `"registered"` · `"unregistered"` · `"reg_failed"` | Reg sub-states |
| `"call_state"` | Call state changed (umbrella) |
| `"calling"` · `"ringing"` · `"established"` · `"held"` · `"ended"` · `"cancelled"` · `"call_failed"` | Call sub-states |
| `"incoming_call"` | Incoming INVITE received |
| `"dtmf"` | DTMF digit received |
| `"sdp_negotiation"` | SDP offer/answer complete |
| `"media_stats"` | Periodic RTP stats |
| `"sip_trace"` | Raw SIP message (when `trace_sip=True`) |
| `"log"` | SDK log message |
| `"transfer_request"` | Incoming REFER |
| `"mwi"` | Voicemail notification |
| `"message"` | SIP MESSAGE received |
| `"presence_state"` | Buddy presence changed |
| `"quality_alert"` | Quality threshold crossed |
| `"registrar_warning"` | Non-fatal registrar warning |
| `"*"` | Every event (wildcard) |

State string values:

- `RegStateEvent.state`: `"unregistered"` `"registering"` `"registered"` `"failed"` `"unregistering"` `"reconnecting"`
  — `"reconnecting"` is a transient loss the SDK is recovering from itself (retry
  armed, dead keepalive path, network handover); `"failed"` is terminal
- `CallStateEvent.state`: `"calling"` `"ringing"` `"established"` `"held"` `"ended"` `"cancelled"` `"failed"`
- `SipTraceEvent.direction`: `"tx"` or `"rx"`
- `PresenceStateEvent.status`: `"unknown"` `"open"` `"closed"` `"busy"`
- `QualityAlertEvent.issue`: `"mos"` `"loss"` `"jitter"` `"rtt"`

---

## See also
- Full example: [bindings/python/examples/quickstart.py](../../bindings/python/examples/quickstart.py)
- Events: [events reference](../api/events.md)
- Media & audio API: [api/media.md](../api/media.md)
