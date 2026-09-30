# Events reference

All events are delivered via the `event_cb` you set in `echosdk_config_t`. The callback is called from the **event dispatch thread** (not `re_main`) so you may call EchoSDK APIs from inside it.

Event payloads are in the `ev->u` union — access the member matching `ev->type`.

## Python

Events are dispatched via `@sdk.on(name)` decorators. Every handler receives a single event object with a `.type` string and type-specific fields:

```python
import echo_sdk as sdk

@sdk.on("registered")
def _(ev):
    print("registered")

@sdk.on("reg_failed")
def _(ev):
    print(f"registration failed: {ev.error_str}  retry in {ev.retry_delay_ms} ms")

@sdk.on("incoming_call")
def _(ev):
    ev.call.answer()

@sdk.on("established")
def _(ev):
    ev.call.poll_stats(interval=5.0, on_update=lambda s: print(s.mos_lq, s.rtt_ms))

@sdk.on("ended")
def _(ev):
    sdk.stop()

@sdk.on("media_stats")
def _(ev):
    print(f"MOS={ev.mos_lq:.2f}  RTT={ev.rtt_ms:.0f}ms  loss={ev.loss_pct:.1f}%")

@sdk.on("sip_trace")
def _(ev):
    print(">>>" if ev.direction == "tx" else "<<<", ev.raw_message)
```

Valid event names for `@sdk.on()`:

| Name | Umbrella / sub-state |
|---|---|
| `reg_state` | Umbrella — fires for any reg change |
| `registering` · `registered` · `unregistered` · `reg_failed` | Sub-states of `reg_state` |
| `call_state` | Umbrella — fires for any call change |
| `calling` · `ringing` · `established` · `held` · `ended` · `cancelled` · `call_failed` | Sub-states of `call_state` |
| `incoming_call` · `dtmf` · `sdp_negotiation` · `sip_trace` | Direct events |
| `media_stats` · `log` · `registrar_warning` | Direct events |
| `transfer_request` · `transfer_failed` · `mwi` · `message` · `presence_state` · `quality_alert` | Direct events |
| `*` | Wildcard — every event |

A sub-state handler and its umbrella handler both fire for the same event. An unknown name raises `ValueError` at decoration time (typo protection).

State fields use strings instead of integer constants:

| Event | `.state` values |
|---|---|
| `reg_state` | `"unregistered"` `"registering"` `"registered"` `"failed"` `"unregistering"` |
| `call_state` | `"calling"` `"ringing"` `"established"` `"held"` `"ended"` `"cancelled"` `"failed"` |
| `presence_state` | `.status`: `"unknown"` `"open"` `"closed"` `"busy"` |
| `quality_alert` | `.issue`: `"mos"` `"loss"` `"jitter"` `"rtt"` `"media_stall"` |
| `sip_trace` | `.direction`: `"tx"` or `"rx"` |

---

## ECHOSDK_EV_LOG
**`ev->u.log`** — `echosdk_ev_log_t`

Emitted for every internal log message at or below `cfg.log_level`.

| Field | Type | Description |
|---|---|---|
| `message` | `const char *` | Log text |

---

## ECHOSDK_EV_REG_STATE
**`ev->u.reg`** — `echosdk_ev_reg_state_t`

Fires on every registration state change.

| Field | Type | Description |
|---|---|---|
| `account` | handle | The account that changed state |
| `state` | `echosdk_reg_state_t` | `UNREGISTERED`, `REGISTERING`, `REGISTERED`, `RECONNECTING`, `FAILED`, `UNREGISTERING` |
| `error` | `echosdk_error_t` | `ECHOSDK_OK` when `REGISTERED` |
| `error_str` | `const char *` | Human-readable error; NULL on OK |
| `retry_attempt` | `uint32_t` | How many retries so far |
| `retry_delay_ms` | `uint32_t` | Next retry in this many ms |

`RECONNECTING` vs `FAILED` is the difference between a status line and an error
dialog:

| State | Meaning | What the app does |
|---|---|---|
| `RECONNECTING` | The registration is down and the SDK is getting it back itself — a retry armed after a timeout or 5xx, a keepalive probe the proxy stopped answering, a network handover (Wi-Fi ↔ cellular, VPN, dock). | Show "Reconnecting…". Nothing else. |
| `FAILED` | The SDK has stopped trying: `ECHOSDK_ERR_AUTH`, an exhausted `reg_retry_max_attempts`, or a retry the app cancelled. | Surface it — new credentials, or `echosdk_account_retry_now()`. |

A recovery reports `RECONNECTING` once and stays there for every attempt it
makes, ending at `REGISTERED` or `FAILED`; it does not flip back to
`REGISTERING` per attempt. `retry_attempt` / `retry_delay_ms` are non-zero on
the event that announces an armed retry and zero on the rest, so
`"reconnecting — attempt %u in %.1f s"` renders only when there is a countdown
to render.

---

## ECHOSDK_EV_INCOMING_CALL
**`ev->u.incoming`** — `echosdk_ev_incoming_call_t`

Fires when a new INVITE is received. Call `echosdk_call_answer()` or `echosdk_call_hangup()`.

| Field | Type | Description |
|---|---|---|
| `account` | handle | Receiving account |
| `call` | handle | The incoming call |
| `from_uri` | `const char *` | Caller SIP URI |
| `display_name` | `const char *` | Caller display name (may be NULL) |

---

## ECHOSDK_EV_CALL_STATE
**`ev->u.call_state`** — `echosdk_ev_call_state_t`

Fires on every call state transition.

| State | Meaning |
|---|---|
| `CALLING` | INVITE sent, waiting for response |
| `RINGING` | 180 Ringing received |
| `ESTABLISHED` | 200 OK / ACK exchanged; media flowing |
| `HELD` | Call on hold (re-INVITE sendonly) |
| `ENDED` | BYE sent or received |
| `CANCELLED` | CANCEL sent before answer |
| `FAILED` | Error response (4xx, 5xx, 6xx) |

> **A remote hangup is `ENDED`, not `FAILED`.** libre reports a peer-initiated
> termination by passing `ECONNRESET` to its session close handler — its BYE
> path answers 200 OK and then terminates the session with that errno — so a
> normal hangup reaches the SDK looking exactly like a dead socket
> (`"Connection reset by peer [104]"`). The SDK therefore treats a transport
> error on a call that had reached `ESTABLISHED` as a remote hangup and reports
> `ENDED` with `reason = "Remote hangup"` and `error = ECHOSDK_OK`.
>
> A consequence worth knowing: a genuine mid-call transport failure is also
> reported as `ENDED`. The two are indistinguishable at this layer, and the call
> is over either way — the transport itself is covered by `REG_STATE` and
> `ECHOSDK_EV_NETWORK`, which a plain hangup never touches.
>
> Errors *before* answer are unaffected: 486, 603 and every other 4xx/5xx/6xx
> still arrive as `FAILED` with the SIP reason phrase intact.

| Field | Type | Description |
|---|---|---|
| `account` | handle | Owning account |
| `call` | handle | The call |
| `state` | `echosdk_call_state_t` | New state |
| `error` | `echosdk_error_t` | Error code when FAILED |
| `reason` | `const char *` | SIP reason phrase, `"Remote hangup"` for a peer BYE, or NULL |

---

## ECHOSDK_EV_CALL_DTMF
**`ev->u.dtmf`** — `echosdk_ev_call_dtmf_t`

DTMF digit received via RFC 4733.

| Field | Type | Description |
|---|---|---|
| `call` | handle | Call that received the digit |
| `digit` | `char` | `'0'`–`'9'`, `'*'`, `'#'`, `'A'`–`'D'` |

---

## ECHOSDK_EV_SDP_NEGOTIATION
**`ev->u.sdp`** — `echosdk_ev_sdp_negotiation_t`

Emitted after SDP offer/answer exchange (when `cfg.trace_sdp_diff = true`).

| Field | Type | Description |
|---|---|---|
| `call` | handle | |
| `local_sdp` | `const char *` | Full local SDP |
| `remote_sdp` | `const char *` | Full remote SDP |
| `negotiated_codec` | `const char *` | e.g. `"opus/48000/2"` |
| `negotiated_crypto` | `const char *` | `"NONE"`, `"SDES"`, `"DTLS-SRTP"` |
| `rejected_codecs` | `const char * const *` | NULL-terminated array |
| `warnings` | `const char * const *` | NULL-terminated array |

---

## ECHOSDK_EV_SIP_TRACE
**`ev->u.sip_trace`** — `echosdk_ev_sip_trace_t`

Raw SIP message (when `cfg.trace_sip = true`).

| Field | Type | Description |
|---|---|---|
| `dir` | `echosdk_media_dir_t` | `TX` or `RX` |
| `transport` | `const char *` | `"UDP"`, `"TCP"`, `"TLS"`, `"WS"`, `"WSS"` |
| `remote_addr` | `const char *` | `"1.2.3.4:5060"` |
| `raw_message` | `const char *` | Full SIP message text |
| `timestamp_us` | `uint64_t` | Microseconds since boot |

---

## ECHOSDK_EV_MEDIA_STATS
**`ev->u.stats`** — `echosdk_ev_media_stats_t`

Periodic RTCP stats (rate set by `cfg.stats_interval_ms`). Also returned synchronously by `echosdk_call_get_stats()`. All RTCP-dependent fields are zero until the first RTCP exchange — packet counters and bandwidth are available immediately.

**Packet counters**

| Field | Type | Description |
|---|---|---|
| `call` | handle | |
| `packets_sent` | `uint32_t` | RTP packets sent |
| `packets_received` | `uint32_t` | RTP packets received |
| `packets_lost` | `uint32_t` | TX-side lost (remote reported we dropped) — cumulative for the call, per RFC 3550 |
| `packets_lost_rx` | `uint32_t` | RX-side lost (we didn't receive) — cumulative |
| `bytes_sent` | `uint32_t` | Total RTP bytes sent |
| `bytes_received` | `uint32_t` | Total RTP bytes received |
| `tx_errors` | `uint32_t` | RTP transmit errors |
| `rx_errors` | `uint32_t` | RTP receive errors |

**Loss**

| Field | Type | Description |
|---|---|---|
| `loss_pct` | `float` | TX-side loss % **over the last poll window**, not lifetime |
| `loss_pct_rx` | `float` | RX-side loss % over the last poll window |

**Delay and jitter**

| Field | Type | Description |
|---|---|---|
| `rtt_ms` | `float` | Round-trip time (ms), from the peer's RR (RFC 3550 LSR/DLSR). `0.0` = not measured yet, including for a whole call against a peer that sends no RTCP |
| `jitter_ms` | `float` | RX interarrival jitter we observe (ms) |
| `tx_jitter_ms` | `float` | TX jitter remote reports back (ms) |

**Jitter buffer**

| Field | Type | Description |
|---|---|---|
| `jitter_buffer_ms` | `uint32_t` | Adaptive buffer depth (ms) |
| `jitter_buffer_load` | `uint32_t` | Packets currently held in buffer |
| `late_packets` | `uint32_t` | Packets that arrived too late |
| `discarded_packets` | `uint32_t` | Packets discarded (overflow or flush) |
| `jitter_buffer_target_ms` | `uint32_t` | Buffer's current jitter estimate (ms) |
| `jitter_buffer_adaptive` | `bool` | True while the buffer is in adaptive mode |

**Packet loss concealment**

| Field | Type | Description |
|---|---|---|
| `plc_frames` | `uint32_t` | Frames lost at the jitter buffer (PLC trigger count) |
| `plc_ratio` | `float` | `plc_frames` / total RX frames (0.0–1.0) |
| `fec_active` | `bool` | Our encoder is sending Opus in-band FEC: on at 4% peer-reported loss (or 2% in two reports running), off after 10 s under 1%. False for G.711 or a peer without `useinbandfec=1` |
| `fec_level_pct` | `uint32_t` | Expected loss the FEC is sized for (5–25); 0 when off |

**Bandwidth**

| Field | Type | Description |
|---|---|---|
| `bandwidth_kbps_tx` | `uint32_t` | Current TX bitrate (kbps) |
| `bandwidth_kbps_rx` | `uint32_t` | Current RX bitrate (kbps) |
| `avg_bandwidth_kbps_tx` | `uint32_t` | Session-average TX bitrate (kbps) |
| `avg_bandwidth_kbps_rx` | `uint32_t` | Session-average RX bitrate (kbps) |

**MOS scores** — zero until RTCP available

| Field | Type | Description |
|---|---|---|
| `mos_lq` | `float` | TX-path listening quality (1.0–4.5) — no delay term |
| `mos_cq` | `float` | TX-path conversational quality — `mos_lq` minus the delay impairment, so always ≤ `mos_lq` |
| `mos_lq_rx` | `float` | RX-path listening quality (far end → you), scored on **effective** loss: network loss plus jitter-buffer discards |
| `mos_cq_rx` | `float` | RX-path conversational quality |
| `mos_method` | `echosdk_mos_method_t` | `EMODEL` or `SIMPLIFIED` |

See [Observability → MOS methods](observability.md#mos-methods) for the formulas.

**Codec**

| Field | Type | Description |
|---|---|---|
| `codec_name` | `const char *` | e.g. `"opus"`, `"PCMU"` |
| `codec_clock_rate` | `uint32_t` | RTP clock rate Hz (e.g. 48000) |
| `codec_sample_rate` | `uint32_t` | Audio sample rate Hz |
| `codec_channels` | `uint8_t` | 1 = mono, 2 = stereo |
| `payload_type` | `int` | RTP payload type (0–127) |

**Audio level**

| Field | Type | Description |
|---|---|---|
| `audio_level_dbov` | `float` | Speaker (RX) level in dBov (0 = max, –127 = silent); `NaN` if unavailable |
| `mic_level_dbov` | `float` | Microphone (TX) level in dBov; `NaN` if unavailable |

**Stream identity**

| Field | Type | Description |
|---|---|---|
| `ssrc_tx` | `uint32_t` | Our SSRC |
| `ssrc_rx` | `uint32_t` | Remote SSRC (0 if not yet received) |
| `remote_addr` | `char[64]` | Remote RTP address `"ip:port"` |

**Session history** — populated from the first stats tick

| Field | Type | Description |
|---|---|---|
| `mos_lq_min` | `float` | Worst `mos_lq` tick this call |
| `mos_lq_avg` | `float` | Session average `mos_lq`, averaged in the R-factor domain and converted once (MOS is non-linear in R) |
| `stats_tick` | `uint32_t` | Which poll this is (1-based) |
| `call_duration_ms` | `uint64_t` | Elapsed ms since `CALL_ESTABLISHED` |
| `is_final` | `bool` | True on the last stats event before teardown — the one to log |

---

## ECHOSDK_EV_REGISTRAR_WARNING
**`ev->u.reg_warn`** — `echosdk_ev_registrar_warning_t`

Non-fatal warning from the registrar (e.g. `Warning:` header).

| Field | Type |
|---|---|
| `message` | `const char *` |

---

## ECHOSDK_EV_TRANSFER_REQUEST
**`ev->u.transfer_req`** — `echosdk_ev_transfer_req_t`

Incoming REFER request — the peer is asking you to transfer this call.

**This event requires an answer.** A REFER creates an implicit subscription
(RFC 3515), and the transferor waits for a final `message/sipfrag` NOTIFY saying
what became of it. The SDK sends the `202 Accepted` and `100 Trying`, then hands
the decision to you. Call exactly one of `echosdk_call_transfer_accept()` or
`echosdk_call_transfer_reject()` on `call`; ignoring the event leaves the far end
waiting out its 60-second subscription.

Do not follow a transfer by hanging up and dialling `refer_to_uri` — the new call
is then unrelated to the REFER and the subscription is never answered. See
[Calls → Incoming transfer request](calls.md#incoming-transfer-request).

| Field | Type | Description |
|---|---|---|
| `account` | handle | |
| `call` | handle | Call that received the REFER |
| `refer_to_uri` | `const char *` | Transfer target URI |
| `has_replaces` | `bool` | true = attended transfer |
| `auto_followed` | `bool` | true when the SDK already followed the transfer itself. Always false today — placing a call is the app's decision — but check it before accepting so future auto-follow policy cannot make you place a second call |

---

## ECHOSDK_EV_TRANSFER_FAILED
**`ev->u.transfer_failed`** — `echosdk_ev_transfer_failed_t`

An *outgoing* REFER was refused. The call is **still established** — a refused
transfer is not a call failure, and the stack does not close the leg. Resume the
caller (they are usually on hold) and report the reason; do not hang up.

There is no matching success event: an accepted REFER hands the call to the
transfer target and closes our leg, which arrives as
`ECHOSDK_EV_CALL_STATE` / `ECHOSDK_CALL_ENDED`.

| Field | Type | Description |
|---|---|---|
| `account` | handle | |
| `call` | handle | The call we tried to transfer away |
| `reason` | `const char *` | Status line / cause, may be NULL |

---

## ECHOSDK_EV_MWI
**`ev->u.mwi`** — `echosdk_ev_mwi_t`

Voicemail notification (NOTIFY from MWI subscription).

| Field | Type |
|---|---|
| `account` | handle |
| `messages_waiting` | `bool` |
| `new_voice` | `uint32_t` |
| `old_voice` | `uint32_t` |
| `new_urgent` | `uint32_t` |
| `old_urgent` | `uint32_t` |
| `raw_body` | `const char *` |

---

## ECHOSDK_EV_MESSAGE
**`ev->u.msg`** — `echosdk_ev_message_t`

Incoming SIP MESSAGE (out-of-dialog instant message).

| Field | Type |
|---|---|
| `account` | handle |
| `from_uri` | `const char *` |
| `body` | `const char *` |
| `content_type` | `const char *` |

---

## ECHOSDK_EV_PRESENCE_STATE
**`ev->u.presence`** — `echosdk_ev_presence_state_t`

Buddy presence changed (received via SUBSCRIBE/NOTIFY).

| Field | Type |
|---|---|
| `account` | handle |
| `target_uri` | `const char *` |
| `status` | `echosdk_presence_status_t`: `UNKNOWN`, `OPEN`, `CLOSED`, `BUSY` |

## ECHOSDK_EV_NETWORK
**`ev->u.network`** — `echosdk_ev_network_t`

Progress of a network handover (Wi-Fi ↔ 4G/5G, VPN up/down, dock/undock).
See [Network handover](../guides/network_handover.md) for the full sequence.

| Field | Type |
|---|---|
| `event` | `echosdk_net_event_t` — see stage table below |
| `call` | handle — `CALL_*` stages only, else NULL |
| `account` | handle — `REREGISTERING` only, else NULL |
| `local_addr` | `const char *` — new local IP, `""` when unknown |
| `attempt` / `max_attempts` | `uint32_t` — retry counter, e.g. "3/6" |
| `elapsed_ms` | `uint32_t` — on `CALL_MIGRATED`, how long audio was interrupted |
| `ice` | `bool` — the call has a live ICE media-NAT; it is migrated with an ICE restart |
| `error` | `echosdk_error_t` — `ECHOSDK_OK` unless a `*_FAILED` stage |

Stages: `CHANGE_DETECTED`, `DOWN`, `UP`, `TRANSPORT_RESET`, `REREGISTERING`,
`CALL_MIGRATING`, `CALL_MIGRATE_ACCEPTED`, `CALL_MIGRATED`,
`CALL_MIGRATION_FAILED`, `CALL_DEFERRED`, `HANDOVER_FAILED`,
`CALL_ICE_STALE`, `CALL_ICE_FAILED`.

An ICE call is migrated with a full RFC 8445 §9 ICE restart, so it emits the
ordinary `CALL_MIGRATING` → `CALL_MIGRATE_ACCEPTED` → `CALL_MIGRATED` sequence —
with `CALL_MIGRATING` arriving up to `cfg.ice_gathering_timeout_ms` before the
re-INVITE itself, which is built from the re-gather.

`CALL_ICE_STALE` marks the exception: an ICE call the restart could not be
performed for, whose re-INVITE therefore carries the pre-handover candidates. It
is emitted once per call per handover, before that offer. The offer still
recovers a direct or still-valid TURN-relayed path; if media does not resume, the
remedy is to re-place the call. `cfg.net_ice_handover` decides whether to keep
retrying; see [Network handover](../guides/network_handover.md#ice-calls).

`CALL_ICE_FAILED` is the other ICE outcome, and a different one: the candidates
*were* gathered and offered and the peer answered, but every pair in the check
list failed, so nothing was nominated and the media path was never validated.
The usual causes are a peer that rejected the connectivity checks — Asterisk
answers `401 Unauthorized` for a while after it rebuilds its own ICE session
across a restart offer — or a path that stopped carrying UDP.

The call is not ended. Where a pair had been validated by an earlier ICE
generation the SDK holds the stream on it, so audio usually survives a failed
restart; the event says the *new* path was never proven, not that the call is
silent. Treat it as a prompt to check whether audio is actually flowing (the
`ECHOSDK_QUALITY_MEDIA_STALL` alert answers that) rather than as a reason to
hang up. It is emitted once per failed ICE generation, so a call that restarts
twice can emit it twice.

---

## ECHOSDK_EV_QUALITY_ALERT
**`ev->u.quality_alert`** — `echosdk_ev_quality_alert_t`

Fired when a quality metric crosses one of the configured thresholds, and
again when it crosses back. Edge-triggered, so a call that stays bad produces
one alert, not one per stats tick.

| Field | Type | Description |
|---|---|---|
| `call` | handle | The affected call |
| `issue` | `echosdk_quality_issue_t` | `MOS`, `LOSS`, `JITTER`, `RTT`, `MEDIA_STALL` |
| `value` | `float` | The metric that crossed |
| `threshold` | `float` | The threshold it crossed |
| `recovering` | `bool` | true = the value came back across |

`MEDIA_STALL` is the odd one out: it is not a metric threshold but the absence
of inbound RTP for `cfg.media_stall_ms` while the call is neither held nor
mid-handover — the link is up and the dialog is healthy, and no audio is
arriving. `value` is the stall duration in ms. It is deliberately non-fatal;
`cfg.rtp_timeout_s` is the setting that ends such a call instead. See
[Degraded links](../guides/degraded_links.md).

## `ECHOSDK_EV_ICE_STATE`

Emitted whenever the ICE liveness of a call's media path changes — the
agent's own verdict from its connectivity checks and RFC 7675 consent, not an
inference from RTP.  Payload `echosdk_ev_ice_state_t`:

| Field | Meaning |
|---|---|
| `call` | the call |
| `state`, `prev` | `echosdk_ice_state_t`: `NEW → GATHERING → CHECKING → CONNECTED → COMPLETED`; `DISCONNECTED` while the selected pair stops answering or receiving but checks continue; `FAILED` once every pair has timed out and nothing is left to gather |
| `generation` | ICE restarts so far (1 = the initial session) |
| `local_type`, `remote_type` | candidate types of the selected pair (`host`, `srflx`, `prflx`, `relay`) |
| `local_addr`, `remote_addr` | the selected pair as `ip:port`, `""` until one is selected |
| `rtt_ms` | STUN round trip on the selected pair |
| `error` | `ECHOSDK_ERR_TIMEOUT` on `FAILED`, else `ECHOSDK_OK` |

The same view is on every `ECHOSDK_EV_MEDIA_STATS` tick as `ice_state`,
`ice_local_type` / `ice_remote_type`, `ice_local_addr` / `ice_remote_addr`,
`ice_rtt_ms` and `ice_generation`, so a quality readout can show the path
without tracking the event stream.  On an ICE call `remote_addr` is the
selected remote candidate; on a plain-RTP call it is the signalled peer.

`FAILED` never ends the call: where a pair was validated earlier the stream is
still held on it and audio usually survives; redialling is the app's decision.
`echosdk_call_ice_state()` returns the current value from any thread.
