# Changelog

## 1.1.0

Defaults changed (see **Changed** below) and `MediaStats` grew, hence the minor
bump.

### Upgrading

- Native code compiled against the 1.0.x `echosdk.h` must be rebuilt:
  `echosdk_call_get_stats()` fills the stats struct at its new, larger size,
  so a caller built against the old header has memory written past the end of
  its struct. Dart code is unaffected — the FFI bindings in this package match
  the libraries it ships.

### Fixed

- **iOS app-owned audio ran the SDK's noise gate, AGC and echo suppressor on
  top of VoiceProcessingIO.** `setAec(true)` counted as asking for the
  suppressor, and the SDK took an app-owned device for a raw microphone. Only
  `setAecMode(suppressor)` asks for it now, and the shipped engines (iOS and
  Android) tell the SDK their capture runs through the platform voice path, so
  its NS and AGC stay off. On an iPhone with NS, AGC and AEC all on, the TX
  chain log line shows all three off. Requires the refreshed `jniLibs` and
  xcframework in this release.

- **Accounts created with the default codec list offered no Opus**, so calls
  went out G.711, since the SDK began registering Opus mono. Requires the
  refreshed native libraries.

- **Audio after packet loss.** An isolated lost packet is rebuilt from the next
  packet's FEC instead of sometimes being skipped; a packet that arrives after
  its slot was already concealed is dropped instead of being spliced into the
  concealment and adding its length to the delay for good; concealment of a
  stopped stream lasts 1 s at every ptime (it was 3 s at 60 ms); and a mid-call
  codec change no longer logs a decode error on every audio callback until the
  new codec's first packet. Requires the refreshed native libraries.

- **Opus settings carried across a re-init:** one `EchoSDK.init` with mono, a
  low complexity or a pinned `opusExpectedLossPct` left them in place for
  every later init.

- **With two calls up, the echo of one far end was not ducked on the other
  call.**

- **`MediaStats.fecActive` read true on a congested link with
  `opusExpectedLossPct` pinned**, where the encoder codes no FEC below the
  bitrate floor.

- **An out-of-range or NaN `aecSuppressionLevel` at init** made the
  suppressor output NaN audio.

- A call whose media came up slowly was diagnosed as stalled, "repaired" with
  an ICE restart, and left permanently silent. The stall clock counted the ICE
  connectivity checks and the DTLS-SRTP handshake — everything that must finish
  before the first packet can arrive — as silence, so any handshake slower than
  `mediaStallMs` produced an alert on the very tick audio became able to flow.
  The repair then restarted ICE even though nothing had moved, which against
  Asterisk/FreePBX makes it rebuild its RTP instance and reject the new
  connectivity checks, leaving the stream pinned to the PBX's private address
  for the rest of the call. The clock now starts when media can actually carry
  RTP, a stall on an unchanged address gets a plain re-offer instead of an ICE
  restart, and a restart that fails keeps the stream on the pair that was
  already working. Requires the refreshed `jniLibs` in this release.

- One incoming SIP MESSAGE, presence NOTIFY, MWI NOTIFY or REFER used to stop
  the event stream permanently. Four native event producers skipped the queue
  length accounting, which underflowed a `size_t` counter and made every
  producer believe the queue was full from then on — so a Dart app subscribed to
  presence or BLF stopped receiving *any* event, including call and registration
  state, seconds after start-up. Requires the refreshed `jniLibs` in this
  release.

### Added

- Android logs `AcousticEchoCanceler.isAvailable()` and
  `NoiseSuppressor.isAvailable()` (tag `EchoSDK`) as each call's audio comes
  up, to collect field data on which phones expose them. Nothing acts on it
  yet.

- `NetworkStage.callIceFailed` — ICE finished its connectivity checks with no
  usable candidate pair, so the media path was never validated. Distinct from
  `callIceStale`, where the candidates could not be re-gathered at all. The
  call is not ended and audio often survives on the previously validated pair,
  so treat it as "the new path was never proven" and let the `mediaStall`
  quality alert say whether audio actually stopped.

- `Call.transferAccept()` / `Call.transferReject()` — answer an incoming
  `TransferRequestEvent`. `transferAccept()` returns the new `Call` placed to
  the transfer target and keeps it linked to the original, so the far end gets
  the SIP NOTIFY that says the transfer worked; `transferReject()` refuses it
  and leaves the call up. Answer every `TransferRequestEvent` with exactly one
  of them — do **not** hang up and dial the URI yourself, which breaks the REFER
  subscription and leaves the transferor waiting.
  `TransferRequestEvent` gains `autoFollowed` (always false today).

- `Call.info()` returns a `CallInfo`: peer URI and display name, local and
  contact URIs, Call-ID, diverter URI, direction, remote-hold state, last SIP
  status, `duration`, `setupDuration`, line number, transport and state. The
  complement to `Call.stats()`, which stays the per-tick media numbers. Safe to
  call at any point, including after the call has ended.


- `RegState.reconnecting` — a registration the SDK is recovering by itself no
  longer reports `RegState.failed`. It covers a retry armed after a timeout or
  5xx (with `retryAttempt` / `retryDelayMs`), a keepalive probe the proxy stopped
  answering, and a network handover or lost link, and it holds for the whole
  recovery instead of flipping back to `registering` per attempt.
  `RegState.failed` now means the SDK has given up: bad credentials, an exhausted
  retry budget, or a `cancelRetry()`. Apps that render only `failed` should add a
  `reconnecting` case — see the status banner in `example/lib/main.dart`.

### Fixed

- Wi-Fi ↔ cellular handover left an ICE call with dead audio. The re-INVITE
  re-advertised the network the call had just left (the ICE module owns the
  media-level `c=` line, and nothing re-gathered), so the PBX kept sending RTP to
  an address that was gone and dropped what arrived from the new one. ICE calls
  are now migrated with a real RFC 8445 §9 ICE restart — new credentials, a fresh
  gather on the new interface — and emit the ordinary `callMigrating` →
  `callMigrated` sequence, with the re-INVITE arriving up to
  `iceGatheringTimeoutMs` after `callMigrating`. `NetworkStage.callIceStale` and
  `EchoSDKConfig.netIceHandover` now apply only to the calls a restart could not
  be performed for.

- In-dialog requests on an incoming call never reached the server, so hanging up
  put no BYE on the wire and the ICE re-offer never arrived. The PBX sent no
  Record-Route, so libre routed to a Contact naming an internal hostname that
  resolves nowhere, and the lookup failed asynchronously after the request had
  already been accepted. In-dialog requests now follow the WebSocket flow the
  registration established (RFC 7118 §B.2). Applies on every platform: the fix
  moved from a GNU-ld `--wrap` interposition (Linux/Android only) into the
  patched libre sources, so iOS carries it too.

- A media-encryption mismatch was reported as "no common audio or video codecs",
  which points at the codec list when the codecs are fine and the media profile
  is the problem. Now named correctly when the account offers no encryption.

- Hanging up an answered incoming call sent no BYE — the app reported the call
  ended while the caller was still connected, and their eventual hangup came
  back as `481 Call Does Not Exist`. Fixed in libre's session layer itself
  (the ACK now retires every reply record it matches), so it applies on every
  platform — the earlier `--wrap`-based fix reached Linux/Android only.

- Incoming calls had no audio when ICE was on. ICE nominated a peer-reflexive
  candidate, which is never signalled, so media left from an address the server
  had not been told about and Asterisk dropped it — the call connected and both
  sides heard silence. The SDK now re-offers when the address ICE settles on is
  not the one that was advertised.

- `EchoSDKConfig`'s ICE toggle had no STUN server to go with it in the example
  app, which is the configuration that triggers the above. Added STUN and TURN
  fields, shown when ICE is enabled, and defaulted `mediaEnc` to
  `MediaEncryption.dtlsSrtp` — a WSS-facing PBX rejects an unencrypted account.
  TURN is not optional on a carrier NAT that hands out a different public IP per
  destination; STUN alone cannot describe that and the re-offer cannot rescue it.

- `MediaStats.micLevelDbov` / `audioLevelDbov` now start as `NaN` rather than
  `-127.0`. Both values were previously `-127.0`, which is also the reading for
  genuine silence, so a dead microphone and an unmeasured one were
  indistinguishable.

- Outgoing calls could never send their INVITE when `iceEnabled` was set. The
  native stack defers the INVITE until ICE candidate gathering reports
  complete, nothing bounded that wait, and one path never reported at all — so
  a call sat in `CallState.calling` forever with no SIP message on the wire and
  no event, while `Account.call()` had already returned a `Call`. The new
  `EchoSDKConfig.iceGatheringTimeoutMs` (default 2000; `-1` keeps the SDK
  default, `0` waits indefinitely) releases the offer with whatever candidates
  exist when it expires, the same way dart-sip-ua's `ice_gathering_timeout`
  and pjsua's `PJSUA_ICE_TRANSPORT_INIT_TIMEOUT` do.

### Changed

- On an app-owned device the SDK's echo suppressor runs only when asked for
  with `setAecMode(AecMode.suppressor)`, on desktop too; it used to come on
  there by itself whenever AEC was on.

- The example app now shows **In call · M:SS** under the peer name, ticking
  once a second, and `On hold · M:SS` while held. It previously showed only the
  raw call-state name. The clock is wall-time, started on
  `CallState.established`: `MediaStats.callDurationMs` only advances once per
  `statsIntervalMs` and is not emitted at all when stats are off, so a timer
  built on it moves in five-second jumps or not at all.

### Added

- Degraded-link handling on `EchoSDKConfig` — for the failure handover cannot
  see, where the address stays put and the link itself goes bad:
  `mediaStallMs`, `rtpTimeoutSeconds`, `adaptiveBitrate` with its
  `adaptMin/MaxBitrate`, `adaptLossDown/UpPct` and `adaptRecoverTicks` bounds,
  `opusExpectedLossPct`, `keepaliveReregister`, `dnsSrvFailover`,
  `regRetryJitter`, `sipTimerBMs`, `sipTimerFMs` and `netIceHandover`.
- `QualityIssue.mediaStall` — no inbound RTP for `mediaStallMs` while the call
  is neither held nor mid-handover. Non-fatal; fires again with
  `recovering = true` when packets resume.
- `NetworkStage.callIceStale` — this call negotiated ICE, so its candidates are
  stale and cannot be re-gathered mid-call. Emitted before the handover
  re-INVITE so the UI can say something useful while the attempt is in flight.
- `IceHandover` enum for `netIceHandover`: `bestEffort` (default) or `failFast`.
- `Call.setRtpTimeout()`, `Call.setBitrate()`, `Account.keepaliveNow()`,
  `EchoSDK.setAdaptiveBitrate()`.

- `TransferFailedEvent` — an outgoing REFER was refused. Previously this arrived
  as `CallStateEvent(CallState.failed)`, which is terminal: the Dart layer
  dropped the call handle while the call was still established natively, so a
  blind transfer to a busy or unknown extension destroyed the live call instead
  of reporting the refusal. The call is now left alone and the app can resume
  the parked caller. There is no success event — an accepted REFER closes the
  leg and arrives as `CallState.ended`.

### Changed

- `statsIntervalMs` now defaults to 2000 (was 0) and the three quality-alert
  thresholds to 3.5 / 5.0 / 40.0. These are written to the native config
  unconditionally, so leaving them at 0 disabled RTCP accounting outright —
  which also disabled quality alerts, media-stall detection and adaptive
  bitrate. Set `statsIntervalMs: 0` explicitly to opt out.

## 1.0.0

First release of the Flutter/Dart binding.

- FFI binding over the full `echosdk.h` surface: accounts, calls, hold/resume,
  DTMF, blind + attended transfer, SIP MESSAGE, presence/BLF/MWI, custom
  headers, push tokens (RFC 8599).
- Transports: UDP · TCP · TLS · WS · WSS, with SRTP-SDES and DTLS-SRTP.
- Reconnection and network handover (Wi-Fi ↔ cellular) driven by OS
  connectivity callbacks.
- Media: codec selection and Opus tuning, PCM tap, WAV recording, mute, device
  enumeration and hot-switch, gain, AEC.
- Observability: RTCP/MOS media statistics and quality alerts, SIP trace.
- Android: prebuilt `libechosdk.so` for arm64-v8a, armeabi-v7a and x86_64 plus
  a Kotlin shim (cache dir, audio focus, speakerphone, ConnectivityManager).
- iOS: vendored dynamic `EchoSDK.xcframework` plus a Swift shim
  (`AVAudioSession`, speaker routing, `NWPathMonitor`), VoiceProcessingIO
  capture with hardware echo cancellation.
- Desktop (Linux/macOS/Windows) usable as a plain FFI binding.
