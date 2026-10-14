# Media & audio

## Codecs

Set in `medut_config_t`:

```c
cfg.audio_codecs[0]  = MEDUT_CODEC_OPUS;
cfg.audio_codecs[1]  = MEDUT_CODEC_PCMU;
cfg.audio_codec_count = 2;
```

| Codec | Enum | Notes |
|---|---|---|
| Opus | `MEDUT_CODEC_OPUS` | Wideband / fullband, preferred for WebRTC |
| G.711 µ-law | `MEDUT_CODEC_PCMU` | 8 kHz, universal PSTN compatibility |
| G.711 A-law | `MEDUT_CODEC_PCMA` | 8 kHz, European PSTN |

These three are the only codecs compiled into the library, on every platform.
Leaving `audio_codec_count` at 0 offers all three, Opus first — see
[Default codec list](#default-codec-list).

`MEDUT_CODEC_G722` and `MEDUT_CODEC_G726_32` still exist as enum
constants for ABI compatibility, but no module provides them; selecting one
has no effect beyond a warning.

### By name

Codecs can also be listed by name, which reaches any codec a loaded baresip
module registers. Names are matched case-insensitively; aliases: `opus`,
`ulaw`/`g711u`/`pcmu`, `alaw`/`g711a`/`pcma`. An unrecognized name is passed
through to baresip as-is.

```c
strcpy(cfg.audio_codec_names[0], "ulaw");
strcpy(cfg.audio_codec_names[1], "opus");
cfg.audio_codec_name_count = 2;
```

Both lists exist on `medut_config_t` (global) and
`medut_account_config_t` (per account). Precedence, highest first:

1. account `audio_codec_names`
2. account `audio_codecs`
3. global `audio_codec_names`
4. global `audio_codecs`

### Default codec list

When all four are empty — the default — the SDK offers a fixed list that is
identical on desktop and mobile:

```
opus/48000/2, PCMU/8000/1, PCMA/8000/1
```

This is pinned in `src/account.c` rather than left to baresip, which would
otherwise offer whatever codecs the linked modules happen to register. If a
build ever ships without Opus or G.711, the SDK logs `codec list "…" matched
no loaded codec` at account setup instead of silently offering something else.

In Flutter both levels are `List<String>` and use the name form:
`MedutConfig(audioCodecs: ['ulaw', 'opus'])` globally,
`AccountConfig(audioCodecs: [...])` per account.

---

## Opus tuning

Fine-tune the Opus encoder at init time via `cfg.opus` (`medut_opus_config_t`):

```c
cfg.opus.bitrate    = 32000;  // 0 = auto/VBR (default)
cfg.opus.complexity = 5;      // 0–10 CPU trade-off; -1 = opus default (9)
cfg.opus.cbr        = false;  // constant bitrate; false = VBR (default)
cfg.opus.dtx        = true;   // discontinuous transmission (silence suppression)
cfg.opus.fec        = true;   // in-band forward error correction
cfg.opus.stereo     = false;  // stereo output; false = mono (default)
```

All fields default to 0 / false / -1 (Opus encoder defaults). Only set what you need.

---

## Audio processing

Enable at init time via `medut_config_t`:

```c
cfg.aec_mode              = MEDUT_AEC_SUPPRESSOR;  // built-in half-duplex gate
cfg.aec_suppression_level = 1.0f;                    // 0.0–1.0; 1.0 = maximum
cfg.ns                    = true;   // noise suppression (WebRTC NS, or the built-in gate)
cfg.agc                   = true;   // automatic gain control (normalise to −20 dBFS)
```

AEC mode options:

| Value | `aec_mode` int (Python) | Description |
|---|---|---|
| `MEDUT_AEC_OFF` | `0` | No echo cancellation |
| `MEDUT_AEC_SUPPRESSOR` | `1` | Built-in half-duplex gate (default) |
| `MEDUT_AEC_WEBRTC` | `2` | WebRTC AEC3 (desktop Linux only; requires `webrtc-audio-processing` 1.x) |

Toggle at runtime without re-dialling. Every toggle reaches calls that are
already up, on their next audio frame:

```c
medut_set_aec_mode(MEDUT_AEC_OFF);        // disable AEC
medut_set_aec_mode(MEDUT_AEC_SUPPRESSOR); // re-enable
medut_set_aec_suppression_level(0.5f);      // softer suppression
medut_set_ns(false);
medut_set_agc(true);
```

**Python**

```python
import medut as sdk

# ── Configure before the first create_account() ──────────────────────────────
sdk.configure(
    aec_mode              = 1,    # 1 = suppressor (default); 2 = WebRTC full-duplex
    aec_suppression_level = 1.0,  # 0.0 = no suppression, 1.0 = maximum (default)
    ns                    = True, # noise suppression
    agc                   = True, # automatic gain control
)

# ── Toggle at runtime (any time, from any thread) ────────────────────────────
sdk.set_aec(True)                      # enable (uses mode set at configure())
sdk.set_aec(False)                     # disable

sdk.set_aec_mode(0)                    # 0 = off
sdk.set_aec_mode(1)                    # 1 = suppressor (restore default)
sdk.set_aec_mode(2)                    # 2 = WebRTC full-duplex (desktop only, opt-in build)

sdk.set_aec_suppression_level(0.6)     # tune aggressiveness (suppressor only)
                                        # 0.0 = TX passes through freely
                                        # 1.0 = maximum ducking (default)

sdk.set_ns(True)                       # noise suppression on/off
sdk.set_agc(True)                      # auto gain control on/off

sdk.set_mic_gain(6.0)                  # TX gain dB [-20, +20]; 0 = unity (bypass)
sdk.set_speaker_gain(-3.0)             # RX gain dB [-20, +20]; 0 = unity (bypass)
```

### AEC mode comparison

| | Suppressor (mode=1, default) | WebRTC (mode=2, opt-in) |
|--|--|--|
| Duplex | Half — ducks mic when far-end is loud | Full — both sides speak simultaneously |
| Double-talk | One side goes quiet | Both parties heard |
| CPU | Negligible | Moderate |
| Platform | Desktop | Desktop Linux only |
| Build | None | `cmake -DMEDUT_WITH_WEBRTC_AEC=ON` + `webrtc-audio-processing` 1.x headers (on by default when found) |
| Noise suppression (`ns`) | Built-in gate: pauses −20 dB | WebRTC NS: also under speech |

> **WebRTC AEC** must be selected at init time via `sdk.configure(aec_mode=2)`. Only `off ↔ init_mode` transitions are valid at runtime — switching between SUPPRESSOR and WEBRTC returns an error. The library is loaded at runtime (`libwebrtc-audio-processing-1.so.N`, the version the SDK was built against); when it is missing, init falls back to the suppressor and the log says `aec=suppressor (webrtc unavailable)`. webrtc-audio-processing 2.x has a different API and is not supported.
>
> **Which filters touch the mic** is logged as one line at init and after every change, e.g. `Medut: TX chain [set_ns]: mic_gain=unity/bypass ns=off (platform voice path) agc=off aec=off (platform canceller)`. Start there when the far end cannot hear a caller.
>
> **Mobile (Android / iOS):** full-duplex AEC is handled by the OS audio driver — Android captures through the `VOICE_COMMUNICATION` recording preset, iOS through `VoiceProcessingIO`. The echo is gone before the SDK sees a sample, so the software suppressor is **not** run there even when `aec_mode` is left at `SUPPRESSOR`: ducking the mic another 16.5 dB would only half-duplex a call the hardware had already made full-duplex. No SDK flag needed, and setting one changes nothing. The same holds for `ns` and `agc`: the voice path already does both, and the SDK's own stay off there. They all come back when the app owns the device (`medut_audio_use_external`; the suppressor only when asked for with `set_aec_mode(SUPPRESSOR)`, NS and AGC unless the app reports its own voice path with `medut_audio_external_set_voice_path`), and on an Android device that refuses the `VOICE_COMMUNICATION` preset — the SDK reads the preset back from each new recorder.

---

## App-owned audio device

By default the SDK opens the platform's capture and playback devices itself.
An app that would rather own them — because it already runs its own audio
engine, needs the mic for something else at the same time, or wants the OS
integration its platform team has already built — can take them over:

```c
medut_audio_use_external(true);   /* SDK stops touching the hardware */
```

From then on the app supplies and consumes PCM. Nothing else about the call
changes; SIP, ICE, SRTP, codecs and the jitter buffer stay with the SDK.

```c
uint32_t srate, ptime;
uint8_t  ch;
medut_audio_external_format(&srate, &ch, &ptime);   /* e.g. 8000, 1, 20 */

/* capture thread — this is what the far end hears */
medut_audio_external_push(mic_pcm, nsamp);

/* playback thread — this is what the local user hears */
medut_audio_external_pull(spk_pcm, nsamp);
```

PCM is S16LE interleaved. Any buffer size is accepted — the stack re-frames to
the call's ptime internally. `pull()` always fills the buffer completely,
writing silence when no call is up, so it can feed the speaker unconditionally.
Call both from the app's own audio threads, never from inside a Medut event
callback.

`medut_audio_use_external(false)` gives the devices back. Both directions
take effect immediately, including on a call already in progress.

Python:

```python
sdk.use_external_audio(True)

fmt = sdk.external_audio_format()        # (8000, 1, 20) or None before media
if fmt:
    srate, ch, ptime = fmt
    nsamp = srate * ch * ptime // 1000

    sdk.external_audio_push(mic_pcm)     # bytes / bytearray / array('h') / numpy
    spk_pcm = sdk.external_audio_pull(nsamp)   # always exactly nsamp samples
```

Dart:

```dart
sdk.useAppOwnedAudio(true);

final fmt = sdk.appOwnedAudioFormat;     // null until the call has media
if (fmt != null) {
  print('${fmt.sampleRate}Hz ${fmt.channels}ch, ${fmt.samplesPerFrame}/frame');
}
```

A complete working example is
[`bindings/python/examples/external_audio.py`](../../bindings/python/examples/external_audio.py):
it pushes a synthesised tone as the microphone and writes the far end to a WAV,
so the whole feature can be verified on a desktop with no audio hardware.

### Full API surface

C (`include/medut.h`):

| | |
|---|---|
| `medut_audio_use_external(bool)` | take/return the device; live, incl. mid-call |
| `medut_audio_external_push(pcm, nsamp)` | captured mic audio → far end |
| `medut_audio_external_pull(pcm, nsamp)` | far end → speaker; always fills |
| `medut_audio_external_format(&srate, &ch, &ptime)` | negotiated format, `ENODEV` until media |
| `medut_audio_external_is_active()` | is a call using the app-owned device |
| `medut_audio_external_set_voice_path(bool)` | capture runs through the platform voice path; SDK NS/AGC stay off |

Flutter (`Medut`):

| | |
|---|---|
| `Future<void> useAppOwnedAudio(bool)` | throws `StateError` if the stack is down |
| `ExternalAudioFormat? appOwnedAudioFormat` | null until the call has media; `.samplesPerFrame` sizes your buffers |
| `bool appOwnedAudioActive` | false between calls even with the mode on |
| `Future<Map> appOwnedAudioStatus()` | native engine diagnostics — see below |
| `Stream<AppOwnedAudioError> appOwnedAudioErrors` | **handle this** — see below |
| `Future<void> notifyCallKitAudioActive(bool)` | iOS + CallKit only |

C++ (`SDK`): `use_external_audio`, `audio_push`, `audio_pull`,
`audio_format(AudioFormat&)`, `audio_external_active`. `audio_push`/`audio_pull`
deliberately do not throw — `ENODEV` is the normal state between calls and they
run on a realtime thread.

Python: `use_external_audio`, `external_audio_push` (any S16LE buffer),
`external_audio_pull`, `external_audio_format`, `external_audio_active`.

### Handle the error stream

Once the app owns the device the SDK is not holding it and **cannot see it
fail**. Nothing else reports these, and unhandled they present as a call that
connects with silence in one or both directions:

```dart
sdk.appOwnedAudioErrors.listen((e) => log('$e'));
```

| Code | Meaning |
|---|---|
| `mic-permission` | `RECORD_AUDIO` not granted — `AudioRecord` would not initialise |
| `unsupported-rate` | the device will not open the negotiated rate |
| `device-open` | the platform refused to build the capture/playback device |
| `capture-dead` / `playback-dead` | the stream died twice in a row and was given up on |
| `unavailable` | the native library did not load |

`appOwnedAudioStatus()` returns the native engine's own view, which is how you
tell "the device opened" from "audio is actually moving": `armed`, `paused`,
`running`, `available`, `sampleRate`, `channels`, `ptimeMs`, `pushFrames`,
`pullFrames`, `pushErrors`, `capturePeak`, `nonSilentFrames`, `lastError`.
A healthy 20 ms loop advances `pushFrames`/`pullFrames` by ~50 per second.

### Who calls push and pull

On mobile, **the realtime loop belongs in the native layer** — Kotlin over JNI,
Swift calling the C directly — not in Dart. A garbage-collection pause on the
capture path is a dropped frame, and there is no way to schedule around it. The
Dart API deliberately exposes only the mode switch and the format; there is no
`push`/`pull` on `Medut` for that reason.

Whatever runs the loop must capture through the platform's voice path —
Android `MediaRecorder.AudioSource.VOICE_COMMUNICATION` with
`AudioManager.MODE_IN_COMMUNICATION`, iOS `VoiceProcessingIO` — see the echo
note below.

Then say so: call `medut_audio_external_set_voice_path(true)` once capture is
running and `false` when it stops (from the device's start and stop, never the
audio callback). With it the SDK keeps its own NS and AGC off, as it does on its
own mobile drivers — stacked on the platform's they only cost speech. Without
it the SDK assumes a raw microphone. The shipped engines (`AppOwnedAudioEngine`
in the Kotlin SDK, `MedutAudioEngine` in MedutKit) report it; on an iPhone with NS, AGC
and AEC all on, the TX chain line then reads
`ns=off (app-owned voice path) agc=off (app-owned voice path) aec=off (app-owned voice path)`.

### Capture channel count (Android)

Opus commonly negotiates 48 kHz **stereo**, but no phone has a stereo
voice-communication capture path. Asking `AudioRecord` for `CHANNEL_IN_STEREO`
does not fail — it returns an initialised recorder that reads without error and
delivers audio far too quiet to hear, which is indistinguishable from a working
call by any frame-level check. Capture mono and up-mix to the negotiated channel
count before `push()`; the shipped engine does this.

### Quiet rooms read as digital silence

The `VOICE_COMMUNICATION` path runs the platform noise suppressor, which emits
*exact* digital silence rather than a noise floor when it decides there is no
speech. So `micLevelDbov` in the media stats legitimately reads `-127` (the
all-zero sentinel) for most frames of a quiet call, and the level is a 1 Hz
snapshot of a single frame. Neither is evidence of a broken capture path — count
frames that carried signal instead if you need to tell a gated microphone from a
dead one.

### Lifecycle

The device opens when the call gets media, not when it is answered, so
`medut_audio_external_format()` returns `ENODEV` until then. There is **no
"media is up" event** to wait on: `CALL_ESTABLISHED` is a SIP state and races
the device by a few milliseconds, and a mid-call re-INVITE can renegotiate the
codec with no call-state change at all. Poll the format — it reports both that
the device opened and that its format changed — and stop the loops when the
call ends.

`medut_audio_use_external()` is **not sticky across a restart**: `medut_init()`
re-derives the device from the platform, so an app that shuts the stack down and
brings it back up has to ask again.

### One device, one call

baresip opens a device per call, but the app has one microphone and one speaker.
The most recently opened call owns them; a second concurrent call gets silence
rather than a share of the mic, which is the honest outcome — the app cannot
capture twice. Ending the newer call hands the devices back to the one still up.
Mixing two calls together is conferencing, and out of scope here.

> **Echo cancellation follows the device.** The SDK's AEC on mobile *is* the
> platform capture path — Android's `VOICE_COMMUNICATION` preset, iOS's
> `VoiceProcessingIO` — and those belong to the drivers this displaces. An app
> that takes the device over owns AEC with it: capture through the equivalent
> voice path on its own side, or expect echo. See
> [AEC mode comparison](#aec-mode-comparison).
>
> The SDK's own half-duplex suppressor does become *available* again while the
> app owns the device, since the hardware canceller is out of the path — but it
> stays **off** unless asked for with
> `medut_set_aec_mode(MEDUT_AEC_SUPPRESSOR)`, on every platform, desktop
> included. `medut_set_aec(true)` is not that request. Switching it on
> automatically would silently duck the TX of an app that is already
> cancelling properly. It is forced back off when the platform device returns,
> so the two never stack.

---

## iOS audio session / CallKit

On iOS the SDK configures the shared `AVAudioSession` during `medut_init()`:
category `PlayAndRecord`, mode `VoiceChat` (hardware AEC + earpiece routing),
options `AllowBluetooth | AllowBluetoothA2DP`. Whether it also *activates* that
session is a config decision:

| `platform_audio_activate` | Behavior | Use for |
|---|---|---|
| `true` (default) | Category + mode + `setActive:YES` at init | Apps that own audio outright, no CallKit |
| `false` | Category + mode only; activation left to the app | **CallKit apps** |

CallKit apps must set it to `false`. `CXProvider` owns the session, and Apple
requires activation to happen only in
`provider(_:didActivateAudioSession:)` — activating anywhere else takes the
exclusive `PlayAndRecord` route out from under CallKit. "Anywhere else" includes
starting the SDK at app launch, and a PushKit wake that starts the SDK while
CallKit is still reporting the incoming call. Nothing else changes: the category
is in place, so audio works the moment CallKit activates the session.

```c
cfg.platform_audio_activate = false;   /* CXProvider owns activation */
```

```swift
// MedutKit: .callKit sets platform_audio_activate = false and keeps the SDK
// from toggling activation around calls.
var options = StartOptions()
options.audioSession = .callKit
try Medut.shared.start(MedutConfig(), options: options)
```

```dart
// Flutter: the same through the plugin.
final sdk = await Medut.start(audioSession: AudioSessionMode.callFramework);
```

Deactivation is symmetric — a CallKit app lets `didDeactivateAudioSession` do
it. Every non-iOS platform ignores this field.

Ordering follows from the same rule: the SDK opens its audio streams when the
call reaches ESTABLISHED, so answer the SIP call from
`provider(_:didActivateAudioSession:)` (or after it has fired) rather than
directly in `CXAnswerCallAction` — otherwise the streams open against a session
CallKit has not activated yet.

---

## Jitter buffer

Configure at init time:

```c
cfg.jitter_buffer_min_ms = 20;
cfg.jitter_buffer_max_ms = 150;
cfg.jbuf_type = MEDUT_JBUF_ADAPTIVE;   // default
// cfg.jbuf_type = MEDUT_JBUF_FIXED;   // constant depth at min_ms
```

Adjust at runtime (takes effect on new calls):

```c
medut_set_jitter_buffer(20, 200);                    // resize adaptive buffer
medut_set_jitter_buffer_type(MEDUT_JBUF_FIXED);    // switch to fixed depth
medut_set_jitter_buffer_type(MEDUT_JBUF_ADAPTIVE); // back to adaptive
```

Both bounds default to baresip's built-in values when left at zero.

---

## Per-call DSCP / QoS

Override the RTP DSCP marking for a specific established call:

```c
// EF (46) — Expedited Forwarding, lowest latency
medut_call_set_dscp_rtp(call, 46);
```

The global RTP and SIP DSCP values are set at init time via `cfg.dscp_rtp` and `cfg.dscp_sip`.

---

## Mute / unmute

```c
medut_audio_mute(call, true);     // mute microphone (TX)
medut_audio_mute(call, false);    // unmute microphone

medut_audio_mute_rx(call, true);  // silence speaker (RX)
medut_audio_mute_rx(call, false); // unmute speaker

// Query current TX mute state (no network round-trip)
bool muted = medut_audio_is_muted(call);
```

`medut_audio_mute` stops encoding and sending microphone audio.
`medut_audio_mute_rx` disables the RTP receiver — the remote continues to send but audio is not decoded or played.

---

## Audio device enumeration

```c
medut_audio_device_t devs[32];

int n = medut_audio_list_input_devices(devs, 32);
for (int i = 0; i < n; i++)
    printf("[%d] %s%s\n", i, devs[i].name,
           devs[i].is_default ? "  *default*" : "");

n = medut_audio_list_output_devices(devs, 32);
for (int i = 0; i < n; i++)
    printf("[%d] %s%s\n", i, devs[i].name,
           devs[i].is_default ? "  *default*" : "");
```

`medut_audio_device_t` fields:

| Field | Type | Description |
|---|---|---|
| `name` | `char[128]` | Device identifier — pass to `set_input/output_device` |
| `description` | `char[256]` | Human-readable label (may be empty) |
| `is_default` | `bool` | Platform default device |

Returns the number of entries written (≥ 0). Returns 0 if the audio module has not finished enumeration yet — call again after a short delay or after receiving the first `MEDUT_EV_LOG` message.

---

## Audio device selection

```c
medut_audio_set_input_device("Plantronics Headset");    // microphone
medut_audio_set_output_device("Plantronics Headset");   // speaker
medut_audio_set_input_device(NULL);                     // platform default
```

Device names come from `medut_audio_list_input/output_devices()`. The change takes effect immediately on all active calls — no re-dial required.

---

## PCM media tap

Install a callback that receives every decoded audio frame:

```c
void my_tap(medut_call_handle_t call,
            medut_media_dir_t   dir,   // RX or TX
            const int16_t        *pcm,
            size_t                samples,
            uint32_t              sample_rate,
            uint8_t               channels,
            uint64_t              timestamp_us,
            void                 *userdata)
{
    // copy PCM data if needed; this runs on the audio thread
    memcpy(my_buf, pcm, samples * channels * sizeof(int16_t));
    my_buf_ready = true;
}

medut_call_set_media_tap(call, my_tap, my_userdata);
medut_call_set_media_tap(call, NULL, NULL);   // remove tap
```

**Rules:**
- Called from the **audio thread** — must return immediately.
- Copy PCM data to your own buffer; don't hold a reference to `pcm` past the callback.
- `dir == MEDUT_MEDIA_DIR_RX` = decoded received audio (what you hear).
- `dir == MEDUT_MEDIA_DIR_TX` = microphone audio before encoding (what you send).

---

## Audio recording

Record a call's audio to a single WAV file (PCM S16LE). Both the received (RX) and sent (TX) audio are clip-summed into one stream — you hear both sides of the conversation.

```c
medut_call_record_start(call, "/tmp/call.wav");

// Stop and finalize the WAV header
medut_call_record_stop(call);
```

**Output format:** PCM S16LE WAV. Sample rate and channel count are taken from the first audio frame (e.g. 48 kHz/2ch for Opus, 8 kHz/1ch for G.711).

**Typical usage — start on answer, stop on hangup:**

```c
case MEDUT_EV_CALL_STATE:
    if (ev->u.call_state.state == MEDUT_CALL_ESTABLISHED)
        medut_call_record_start(ev->u.call_state.call, "/tmp/call.wav");
    if (ev->u.call_state.state == MEDUT_CALL_ENDED)
        medut_call_record_stop(ev->u.call_state.call);
```

**Notes:**
- Returns `EALREADY` if recording is already active.
- Always call `record_stop` before hangup to get a correctly finalized WAV header. If the call is destroyed first, the file is closed but sizes in the header will be wrong.
- Recording is independent of the PCM media tap — both can be active simultaneously.

---

## Media encryption

| Enum | Description |
|---|---|
| `MEDUT_MEDIA_ENC_NONE` | No encryption (RTP) |
| `MEDUT_MEDIA_ENC_SDES` | SRTP with SDP crypto attributes (RFC 4568) |
| `MEDUT_MEDIA_ENC_DTLS_SRTP` | DTLS-SRTP (RFC 5764) — required for WebRTC |

Set globally in `medut_config_t.media_enc` or per-account in `medut_account_config_t.media_enc`.
