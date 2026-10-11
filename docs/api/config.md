# Configuration reference

Always start with `echosdk_config_init(&cfg)` to zero-fill and set `version`/`struct_size`.

## echosdk_config_t

### Forward-compat guard

| Field | Type | Default | Description |
|---|---|---|---|
| `version` | `uint32_t` | 1 | Set by `echosdk_config_init`. Do not change. |
| `struct_size` | `size_t` | `sizeof(echosdk_config_t)` | Set by `echosdk_config_init`. Do not change. |

### Transport

| Field | Type | Default | Description |
|---|---|---|---|
| `transport` | `echosdk_transport_t` | `UDP` | Default transport: UDP, TCP, TLS, WS, WSS |
| `local_ip` | `const char *` | NULL | Bind IP. NULL = auto-select. |
| `local_port` | `uint16_t` | 0 | Local SIP port. 0 = OS-assigned. |
| `bind_interface` | `const char *` | NULL | Interface name e.g. `"wlan0"`. NULL = any. |
| `prefer_ipv6` | `bool` | false | Prefer IPv6 addresses for ICE candidates. |
| `sip_domain` | `const char *` | NULL | AOR domain e.g. `"pbx.example.com"`. |

### Server address (pick one form)

**Simple form** (UDP/TCP/TLS, default ports):
```
transport   = ECHOSDK_TRANSPORT_TLS
server_host = "pbx.example.com"
server_port = 0   → uses 5061
```

**URL form** (required for WS/WSS):
```
server_url = "wss://pbx.example.com:8089/ws"
```

| Field | Type | Default | Description |
|---|---|---|---|
| `server_url` | `const char *` | NULL | Full URL. Overrides transport/server_host/server_port. |
| `server_host` | `const char *` | NULL | Hostname or IP (simple form). |
| `server_port` | `uint16_t` | 0 | 0 = transport default (5060/5061). |
| `outbound_proxy` | `const char *` | NULL | Override outbound proxy (NULL = auto from server info). |

### TLS / WSS

| Field | Type | Default | Description |
|---|---|---|---|
| `ca_cert_path` | `const char *` | NULL | CA bundle path. NULL = platform trust store ([see below](../guides/tls_wss.md#certificate-trust-store)). |
| `client_cert` | `const char *` | NULL | Client certificate PEM path. |
| `client_key` | `const char *` | NULL | Client private key PEM path. |
| `verify_server` | `bool` | true | Verify server certificate. |
| `sni_hostname` | `const char *` | NULL | SNI override (for reverse proxy). NULL = derive from URL. |
| `user_agent` | `const char *` | NULL | `User-Agent` header value. |

### WebSocket-specific

| Field | Type | Default | Description |
|---|---|---|---|
| `ws_origin` | `const char *` | NULL | `Origin` header. NULL = auto from URL. |
| `ws_extra_headers` | `const char **` | NULL | NULL-terminated extra header strings. |
| `ws_keepalive_ms` | `uint32_t` | 0 | WebSocket ping interval. 0 = libre default (15 s); 20000–30000 is the useful range behind proxies that idle-close. |

### NAT

| Field | Type | Default | Description |
|---|---|---|---|
| `stun_server` | `const char *` | NULL | e.g. `"stun:stun.l.google.com:19302"` |
| `turn_server` | `const char *` | NULL | e.g. `"turn:turn.example.com:3478"` |
| `turn_user` | `const char *` | NULL | TURN username |
| `turn_pass` | `const char *` | NULL | TURN password |
| `ice_enabled` | `bool` | false | Enable ICE |
| `rtcp_mux` | `bool` | true | Multiplex RTCP on the RTP port (RFC 5761) |
| `ice_gathering_timeout_ms` | `uint32_t` | 2000 | Deadline for ICE candidate gathering — on an outgoing call, and on the re-gather of a handover ICE restart. 0 waits indefinitely on dial; the restart falls back to 3 s |
| `ice_servers` / `ice_server_count` | `const echosdk_ice_server_t *` / `uint32_t` | NULL / 0 | Every STUN/TURN server to gather from, in preference order (`{url, user, pass}`; URL grammar `stun:` / `stuns:` / `turn:` / `turns:` with optional `?transport=udp\|tcp`). Empty = fold in `stun_server` / `turn_server` above, TURN first. Resolved and asked on every gather, including the re-gather of an ICE restart |
| `ice_transport_policy` | `echosdk_ice_transport_policy_t` | `ALL` | `RELAY` offers and accepts relayed candidates only; a call with no usable TURN server then fails fast instead of leaking a direct path |
| `ice_multipath` | `bool` | false | Gather on every interface the platform can pin a socket to (cellular next to Wi-Fi, a VPN next to wired; at most two extra), so a handover offers candidates that already exist. Android needs the plugin's cellular request (`EchoSDKConfig.iceMultipath` does both) |

With ICE enabled the INVITE is **not** sent when the call is placed — it is sent
once the ICE stack reports that candidate gathering is done. Nothing in that
stack bounds how long that takes, and one path never reports at all, so without
a deadline an outgoing call can sit in `CALLING` forever: no SIP message on the
wire, no event, and `echosdk_call_invite()` already returned success.

`ice_gathering_timeout_ms` is that bound. On expiry the offer is released with
whatever candidates were gathered by then — the same choice JsSIP, SIP.js and
dart-sip-ua make (they cap the identical wait at 0.5–5 s), and the same one
pjsua makes with `PJSUA_ICE_TRANSPORT_INIT_TIMEOUT`. Gathering is not
cancelled: a completion arriving afterwards re-offers the fuller candidate set
in a re-INVITE, and a *failure* arriving afterwards is dropped rather than
ending a call whose offer is already on the wire.

The same bound applies to the re-gather of the ICE restart that migrates a call
on network handover, where it decides how long the call stays without audio
before an offer goes out. There a configured 0 does *not* mean "wait for ever" —
the call is already live and silent, so a 3 s bound applies instead.

Distinct from `sip_timer_b_ms`, which bounds the INVITE transaction *after* the
request is sent. This one bounds the window before it exists.

### Media

| Field | Type | Default | Description |
|---|---|---|---|
| `media_enc` | `echosdk_media_enc_t` | `NONE` | `NONE`, `SDES`, `DTLS_SRTP` |

A WSS or WebRTC-facing PBX offers `UDP/TLS/RTP/SAVPF` and will not
negotiate against an account offering plain `RTP/AVP`. baresip reports the
mismatch as *"no common audio or video codecs"*, which sends you looking at
the codec list — the codecs are usually fine and the media profile is the
problem. Set `media_enc` to `DTLS_SRTP` for those deployments; the SDK
rewrites the message when the account offers no encryption, but the setting
is what fixes it.
| `audio_codecs[8]` | `echosdk_codec_t[]` | `[OPUS]` | Preference-ordered codec list |
| `audio_codec_count` | `int` | 1 | Number of codecs in `audio_codecs` |
| `audio_codec_names[8][32]` | `char[][]` | empty | Preference-ordered codec list by name; wins over `audio_codecs` |
| `audio_codec_name_count` | `int` | 0 | Number of names in `audio_codec_names`; 0 = use `audio_codecs` |
| `dscp_sip` | `uint8_t` | 0 | DSCP for SIP (0=OS default, 24=AF31) |
| `dscp_rtp` | `uint8_t` | 0 | DSCP for RTP (0=OS default, 46=EF) |
| `enable_video` | `bool` | false | Reserved for future video support; no effect today |

### Audio processing

| Field | Type | Default | Description |
|---|---|---|---|
| `aec_mode` | `echosdk_aec_mode_t` | `SUPPRESSOR` | `OFF`, `SUPPRESSOR`, `WEBRTC` |
| `aec_suppression_level` | `float` | 1.0 | 0.0–1.0; 1.0 = strongest suppression |
| `ns` | `bool` | false | Noise suppression |
| `agc` | `bool` | false | Automatic gain control |
| `mic_gain_db` | `float` | 0.0 | Microphone gain in dB (−20 to +20) |
| `speaker_gain_db` | `float` | 0.0 | Speaker gain in dB (−20 to +20) |
| `platform_audio_activate` | `bool` | true | iOS: activate the AVAudioSession during init. **Set false in CallKit apps** — see [media.md](media.md#ios-audio-session--callkit) |

There is deliberately **no** config field for the app-owned audio device: it is a
runtime switch (`echosdk_audio_use_external()`), so it can be flipped mid-call
and does not survive a restart. Flutter exposes `EchoSDKConfig.appOwnedAudio`
as convenience only — it is applied straight after `init()`, not marshalled into
`echosdk_config_t`. See [App-owned audio device](media.md#app-owned-audio-device).

### Opus encoder

| Field | Type | Default | Description |
|---|---|---|---|
| `opus.bitrate` | `int` | 32000 | Target bitrate bps (VBR). 32 kbps is where speech quality levels off; 0 lets Opus pick (~50 kbps mono) |
| `opus.complexity` | `int` | −1 | 0–10 CPU/quality trade-off (−1 = opus default: 9) |
| `opus.cbr` | `bool` | false | Constant bitrate (false = VBR) |
| `opus.dtx` | `bool` | false | Discontinuous transmission: the encoder sends nothing in silence (packets of 2 bytes or less are dropped, the RTP timestamp keeps advancing, the first packet after the gap carries the marker) and the offer says `usedtx=1`. Our own switch — it does not wait for the peer's `usedtx` |
| `opus.fec` | `bool` | true | In-band forward error correction: ask the peer for it, and send our own sized by `opus_expected_loss_pct`. `false` turns it off both ways |
| `opus.stereo` | `bool` | false | Send and ask for stereo (false = mono both ways) |

### Jitter buffer

| Field | Type | Default | Description |
|---|---|---|---|
| `jitter_buffer_min_ms` | `uint32_t` | 0 | Minimum depth ms (0 = 40 ms; fixed: 100 ms, the depth it holds) |
| `jitter_buffer_max_ms` | `uint32_t` | 0 | Maximum depth ms (0 = 400 ms; fixed: 200 ms) |
| `jbuf_type` | `echosdk_jbuf_type_t` | `ADAPTIVE` | `ADAPTIVE` or `FIXED` depth |

### Registration

| Field | Type | Default | Description |
|---|---|---|---|
| `reg_expires` | `uint32_t` | 3600 | REGISTER Expires (seconds) |
| `reg_refresh_pct` | `uint32_t` | 75 | Refresh at N% of expires |
| `keepalive_interval` | `uint32_t` | 30000 | SIP OPTIONS probe period in ms; 0 = off. Refreshes the UDP NAT binding and detects a black-holed path — see [Degraded links](../guides/degraded_links.md) |
| `keepalive_reregister` | `bool` | true | Re-REGISTER immediately when a probe fails |
| `reg_retry_initial_ms` | `uint32_t` | 2000 | Initial retry delay |
| `reg_retry_max_ms` | `uint32_t` | 300000 | Max retry delay (5 min) |
| `reg_retry_backoff` | `float` | 2.0 | Exponential backoff multiplier |
| `reg_retry_max_attempts` | `uint32_t` | 0 | 0 = retry forever |
| `reg_retry_jitter` | `float` | 0.2 | Randomise each delay by ±this fraction, so a fleet returning from an outage does not hit the registrar in one burst. 0 disables |
| `dns_srv_failover` | `bool` | true | Advance one RFC 3263 SRV target per failed attempt instead of re-sending to the same dead proxy. Ignored when `outbound_proxy` is pinned, or for IP literals and WS/WSS URLs |

### SIP timers (RFC 3261 §17)

T1 and T2 are compile-time constants inside libre's transaction layer
(`SIP_T1` / `SIP_T2`); the fields exist for completeness and setting them has
no effect. Timers B and F are enforced by the SDK instead, which is what makes
them useful — a request that gets no response at all is otherwise bounded only
by libre's fixed 64·T1 = 32 s.

| Field | Type | Default | Description |
|---|---|---|---|
| `sip_t1_ms` | `uint32_t` | 500 | Informational only |
| `sip_t2_ms` | `uint32_t` | 4000 | Informational only |
| `sip_timer_b_ms` | `uint32_t` | 32000 | An outgoing call still in `CALLING` after this long is cancelled with 408 → `CALL_FAILED` / `ECHOSDK_ERR_TIMEOUT`. 8000–12000 to fail fast on mobile; 0 disables. Only `CALLING` is watched — a call that reached `RINGING` has proven the path works |
| `sip_timer_f_ms` | `uint32_t` | 32000 | A REGISTER with no answer after this long reports `ECHOSDK_ERR_TIMEOUT` and hands over to the retry policy; 0 disables |

### Session timers (RFC 4028)

| Field | Type | Default |
|---|---|---|
| `session_timer_enabled` | `bool` | true |
| `session_expires_s` | `uint32_t` | 1800 |
| `session_min_se_s` | `uint32_t` | 90 |

### Quality & observability

| Field | Type | Default | Description |
|---|---|---|---|
| `stats_interval_ms` | `uint32_t` | 2000 | Poll period for `ECHOSDK_EV_MEDIA_STATS`; 0 disables. Also the master switch for RTCP accounting, so with 0 the loss/jitter/RTT/MOS fields read back as zero everywhere and quality alerts, media-stall detection and adaptive bitrate are all inert. Python: `call.poll_stats(interval=)` overrides this per-call |
| `mos_method` | `echosdk_mos_method_t` | `EMODEL` | `EMODEL` or `SIMPLIFIED` |
| `mos_alert_threshold` | `float` | 3.5 | Fire `QUALITY_ALERT` when `mos_lq` drops below this; 0 disables |
| `loss_alert_threshold` | `float` | 5.0 | Fire when `loss_pct` exceeds this; 0 disables |
| `jitter_alert_threshold` | `float` | 40.0 | Fire when `jitter_ms` exceeds this; 0 disables |
| `trace_sip` | `bool` | false | Emit `ECHOSDK_EV_SIP_TRACE` per message |
| `trace_sdp_diff` | `bool` | false | Emit `ECHOSDK_EV_SDP_NEGOTIATION` |
| `pcap_path` | `const char *` | NULL | Path for live pcap capture |

### Network handover (Wi-Fi ↔ 4G/5G)

Full guide: [Network handover](../guides/network_handover.md).

| Field | Type | Default | Description |
|---|---|---|---|
| `net_monitor_interval_s` | `uint32_t` | 10 | Interface poll period in seconds; 0 = off. Set 0 on mobile and call `echosdk_network_changed()` from the OS callback |
| `net_settle_ms` | `uint32_t` | 1500 | Debounce — how long the address set must stay stable before acting |
| `net_reinvite_calls` | `bool` | true | Re-INVITE active calls onto the new local address |
| `net_verify_ms` | `uint32_t` | 4000 | Wait for RTP on the new path before retrying; 0 disables the media check |
| `net_max_attempts` | `uint32_t` | 6 | Retry ceiling for the rebind and for each call migration |
| `net_hangup_on_migration_failure` | `bool` | false | End calls whose media could not be migrated |
| `net_ice_handover` | `echosdk_ice_handover_t` | `BEST_EFFORT` | Applies only to an ICE call that could **not** be re-gathered (ICE calls are normally migrated with a full ICE restart): `BEST_EFFORT` runs the full retry budget, `FAIL_FAST` gives up after one attempt — see [Network handover](../guides/network_handover.md#ice-calls) |

### Degraded links

Handover above covers a changed local address. These cover the other failure:
the address stays put and the link itself goes bad. Full guide:
[Degraded links](../guides/degraded_links.md).

| Field | Type | Default | Description |
|---|---|---|---|
| `media_stall_ms` | `uint32_t` | 4000 | Fire `QUALITY_ALERT` with issue `MEDIA_STALL` after this long with no inbound RTP; fires again with `recovering` when it resumes. Non-fatal; 0 disables. Requires `stats_interval_ms` > 0 |
| `rtp_timeout_s` | `uint32_t` | 0 | **End** the call after this long with no inbound RTP (baresip `avt.rtp_timeout`). 0 = never; 30–60 when wanted. Only sendrecv streams are checked, so a held call is never torn down |
| `adaptive_bitrate` | `bool` | true | Step the audio encoder bitrate down under peer-reported loss and back up on recovery, via the codec's encoder-update path — no re-INVITE, no audio gap. Opus only; a fixed-rate codec has nothing to vary |
| `adapt_min_bitrate` | `uint32_t` | 12000 | Adaptation floor, bps. Raised to 20000 while Opus FEC is in use (below it libopus cannot code FEC), unless the loss turns out to be congestion |
| `adapt_max_bitrate` | `uint32_t` | 32000 | Adaptation ceiling, bps |
| `adapt_loss_down_pct` | `float` | 5.0 | Halve the bitrate above this loss % |
| `adapt_loss_up_pct` | `float` | 1.0 | Raise it (+25%) below this loss % |
| `adapt_recover_ticks` | `uint32_t` | 5 | Consecutive clean stats ticks required before a step up |
| `opus_expected_loss_pct` | `uint32_t` | 0 | Opus in-band FEC (LBRR) redundancy, percent. 0 = switched with the link: on at an RTCP report of 4%+ loss or two in a row at 2%+, off after 10 s under 1%, and 5–25% while on. Non-zero pins it. Received FEC is used either way |

### Logging and event delivery

| Field | Type | Default | Description |
|---|---|---|---|
| `log_level` | `int` | 0 | 0=err, 1=warn, 2=info, 3=debug |
| `event_cb` | `echosdk_event_cb_t` | required | Your event callback |
| `event_userdata` | `void *` | NULL | Passed back to `event_cb` |
| `deliver_owned_events` | `bool` | false | Event ownership mode — see below |

**Event ownership.** With `deliver_owned_events = false` (the default) the event
passed to `event_cb` is **borrowed**: it is valid only for the duration of the
callback, and anything you keep must be copied out before you return.

With `deliver_owned_events = true` the callback receives a heap-owned clone and
you **must** call `echosdk_event_release(ev)` exactly once per delivered event —
from any thread, at any time after delivery. Use this for bindings that dispatch
events asynchronously (Dart's `NativeCallable.listener` runs the handler on the
isolate's event loop, after the C call has already returned). Releasing an event
that was not delivered in owned mode is undefined behaviour.

`echosdk_set_event_handler(cb, userdata, deliver_owned_events)` re-points event
delivery at a new consumer — or parks it with `NULL` — under the same contract,
without tearing the stack down.

### Runtime paths

| Field | Type | Default | Description |
|---|---|---|---|
| `tmp_dir` | `const char *` | NULL | Directory for baresip's temporary files. NULL = `$TMPDIR`, or `/tmp` on Linux. **Android must set this** to the app cache dir (`context.getCacheDir().getAbsolutePath()`): `/tmp` does not exist there and `$TMPDIR` is not reliably set from native code. |

---

## echosdk_account_config_t

| Field | Type | Required | Description |
|---|---|---|---|
| `uri` | `const char *` | Yes | `"user@host"` or `"sip:user@host"` |
| `password` | `const char *` | Yes | Digest auth password |
| `transport` | `echosdk_transport_t` | No | Overrides global default |
| `server_host` | `const char *` | No | Overrides host from `uri` |
| `server_port` | `uint16_t` | No | 0 = derive from transport |
| `server_url` | `const char *` | No | Full URL (WS/WSS) |
| `auth_user` | `const char *` | No | NULL = user part of `uri` |
| `display_name` | `const char *` | No | SIP display name |
| `media_enc` | `echosdk_media_enc_t` | No | Per-account media encryption override |
| `ice_enabled` | `bool` | No | Per-account ICE override |
| `stun_server` | `const char *` | No | Per-account STUN override |
| `turn_server` | `const char *` | No | Per-account TURN override |
| `turn_user` / `turn_pass` | `const char *` | No | TURN credentials |
| `ice_servers` / `ice_server_count` / `ice_transport_policy` | see global | No | Per-account server list and policy; empty = the global list, then the legacy fields. Replaceable at runtime with `echosdk_account_set_ice_servers()` |
| `rtcp_mux` | `bool` | No | RTCP-mux override; only read when `rtcp_mux_set` is true |
| `rtcp_mux_set` | `bool` | No | false = inherit the global `rtcp_mux`; true = use the field above |
| `outbound` | `const char *` | No | Outbound route override; NULL = auto-derived from server |
| `outbound_proxy` | `const char *` | No | NULL = auto-derived from server |
| `verify_tls` | `bool` | No | false = skip TLS cert check |
| `push_provider` | `echosdk_push_provider_t` | No | `NONE`, `APNS`, `FCM` — see [Accounts → Push notifications](accounts.md#push-notifications) |
| `push_token` | `const char *` | No | Device token (APNs hex, FCM registration token) |
| `push_param` | `const char *` | No | `pn-param` value (APNs topic / FCM sender id) |
| `audio_codecs[8]` | `echosdk_codec_t[]` | No | Per-account preference-ordered codec list |
| `audio_codec_count` | `int` | No | Number of entries in `audio_codecs` |
| `audio_codec_names[8][32]` | `char[][]` | No | Codec list by name; wins over `audio_codecs` |
| `audio_codec_name_count` | `int` | No | Number of entries in `audio_codec_names`; 0 = use `audio_codecs` |
| `dtmf_mode` | `echosdk_dtmf_mode_t` | No | `RFC4733` (default), `SIP_INFO`, `AUTO` |
