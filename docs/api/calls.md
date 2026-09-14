# Calls

## Outgoing call

```c
echosdk_call_handle_t call;
int rc = echosdk_call_invite(acct, "sip:bob@pbx.example.com", &call);
// fires: CALLING → RINGING → ESTABLISHED  (or FAILED)
```

### Dial targets

The second argument is an opaque dial string, not a phone number. The SDK only
completes it into a full SIP URI — it prepends `sip:` when there is no scheme,
and appends `@<account-domain>` (plus the port, when it is not 5060) when there
is no `@`. IP literals and anything already carrying `user@host` are left alone.
Nothing is validated as numeric and nothing is stripped, so any legal SIP user
part works:

```c
echosdk_call_invite(acct, "sip:bob@pbx.example.com", &call);   /* full URI   */
echosdk_call_invite(acct, "120", &call);                       /* extension  */
echosdk_call_invite(acct, "+15551234", &call);                 /* E.164      */

/* A UUID is a legal user part — hex digits and '-' are all RFC 3261
   `unreserved`, so no percent-encoding is needed. */
echosdk_call_invite(acct, "550e8400-e29b-41d4-a716-446655440000", &call);
/* → INVITE sip:550e8400-e29b-41d4-a716-446655440000@pbx.example.com */
```

```python
sdk.call("550e8400-e29b-41d4-a716-446655440000")
```

```dart
account.call('550e8400-e29b-41d4-a716-446655440000');
```

An account may also *register* under a UUID — the AOR is just
`echosdk_account_config_t.uri` — and an inbound call from such a peer reports it
verbatim in `from_uri`, so opaque identifiers work in both directions without a
phone number anywhere in the flow.

Two things to get right, both on the server side:

- **Your PBX has to route the identifier.** On Asterisk that means a PJSIP
  endpoint/AOR named by the UUID, or a dialplan pattern that matches it — the
  usual `exten => _X.` patterns match digits only and will never match a UUID.
- **SIP user parts are case-sensitive**, while UUIDs as identifiers are not.
  Pick one casing (lowercase by convention) and use it for both registration and
  dialling, or the registrar will not match the AOR.

## Incoming call

```c
// In your event callback:
case ECHOSDK_EV_INCOMING_CALL:
    printf("Incoming from %s\n", ev->u.incoming.from_uri);
    echosdk_call_answer(ev->u.incoming.call);
    break;
```

To reject without answering, use `echosdk_call_reject()` with the SIP status
code you want the caller to see:

```c
echosdk_call_reject(call, 486, "Busy Here");   // or 603 "Decline"
echosdk_call_reject(call, 0, NULL);            // 0 = default behaviour
```

`echosdk_call_hangup()` also terminates an unanswered call, but sends the
default response rather than a code you choose.

## Hang up / terminate

```c
echosdk_call_hangup(call);   // sends BYE; fires ECHOSDK_EV_CALL_STATE (ENDED)
```

## Hold and resume

```c
echosdk_call_hold(call);     // re-INVITE with sendonly
echosdk_call_resume(call);   // re-INVITE with sendrecv

// Query current state (reads local hold flag — no network round-trip)
bool held = echosdk_call_is_held(call);
```

## DTMF

```c
echosdk_call_send_dtmf(call, '5');   // digit per account dtmf_mode
// valid: '0'-'9', '*', '#', 'A'-'D'
```

DTMF mode is configured per-account via `echosdk_account_config_t.dtmf_mode`:

| Value | Description |
|---|---|
| `ECHOSDK_DTMF_RFC4733` | RFC 4733 RTP telephony-event (default) |
| `ECHOSDK_DTMF_SIP_INFO` | SIP INFO `application/dtmf-relay` — legacy gateways |
| `ECHOSDK_DTMF_AUTO` | Prefer RFC 4733, fall back to SIP INFO |

## Transfer

### Blind transfer (REFER)
```c
echosdk_call_transfer(call, "sip:carol@pbx.example.com");
```

### Attended transfer
```c
// call_a is the original call (to transfer away)
// call_b is the consultation call already established
echosdk_call_attended_transfer(call_a, call_b);
// embeds Replaces header from call_b's dialog
```

### Incoming transfer request

When the remote side sends a REFER to your UA, the SDK fires
`ECHOSDK_EV_TRANSFER_REQUEST` and waits for your decision.

A REFER creates an implicit subscription (RFC 3515 §2.4.4): the transferor is
owed a final `message/sipfrag` NOTIFY telling it whether the reference
succeeded, and until that arrives it has no idea whether to hang up or recover.
The SDK answers the `202 Accepted` and the `100 Trying` for you, then stops —
whether to follow a transfer is policy, not transport. **Answer every
`ECHOSDK_EV_TRANSFER_REQUEST` with exactly one of `echosdk_call_transfer_accept()`
or `echosdk_call_transfer_reject()`.** Ignoring it leaves the far end waiting
out the 60-second subscription.

```c
case ECHOSDK_EV_TRANSFER_REQUEST: {
    echosdk_call_handle_t call = ev->u.transfer_req.call;
    const char *uri  = ev->u.transfer_req.refer_to_uri;
    bool attended    = ev->u.transfer_req.has_replaces;

    if (user_accepted(uri)) {
        echosdk_call_handle_t moved = NULL;
        if (echosdk_call_transfer_accept(call, &moved) == ECHOSDK_OK) {
            /* `moved` is the new call to the target. The original stays up —
             * hang it up when the new one is established, or keep both and
             * let the user pick. */
        }
    }
    else {
        echosdk_call_transfer_reject(call, 603, "Declined");
    }
    break;
}
```

> **Do not implement this by hanging up and dialling the URI.** That is the
> obvious-looking approach and it is wrong: the new call is then unrelated to
> the REFER, so the subscription is never answered and the transferor never
> learns the transfer worked. `echosdk_call_transfer_accept()` keeps the two
> linked, which is what lets the SDK report the outcome for you — `200 OK` when
> the new call is established, or the failure status if it is not.

Both are also available in the bindings:

```python
@sdk.on("transfer_request")
def _(ev):
    new_call = ev.call.transfer_accept()   # or ev.call.transfer_reject(603)
```

```dart
final moved = ev.call.transferAccept();    // or ev.call.transferReject()
```

## Call information

`echosdk_call_get_info()` returns the call's identity and timing — as opposed to
`echosdk_call_get_stats()`, which is the per-tick media numbers. It is safe to
call at any point in the call's life, including after it has ended.

```c
echosdk_call_info_t info;
if (echosdk_call_get_info(call, &info) == ECHOSDK_OK) {
    printf("%s %s (%s) up %llu ms\n",
           info.is_outgoing ? "to" : "from",
           info.peer_uri, info.peer_display_name,
           (unsigned long long)info.duration_ms);
}
```

| Field | Notes |
|---|---|
| `peer_uri`, `peer_display_name` | far end AoR and From display-name |
| `local_uri`, `contact_uri` | our AoR, far end Contact |
| `call_id` | SIP Call-ID |
| `diverter_uri` | Diversion / History-Info when the call was forwarded to us. Not Referred-By — a transferred call carries no diverter |
| `is_outgoing` | we placed it |
| `is_remote_hold` | the **peer** put us on hold. Local hold is `echosdk_call_is_held()`; the two are independent |
| `sip_status` | last SIP status; 0 while the call is up |
| `duration_ms` | since ESTABLISHED; 0 before that |
| `setup_duration_ms` | INVITE → answer, in whole-second steps |
| `line_number`, `transport`, `state` | |

## Enumerate active calls

```c
void my_iter(echosdk_call_handle_t call, void *arg) {
    printf("active call: %p\n", call);
}
echosdk_call_foreach(my_iter, NULL);
```

## Per-dialog custom headers

Headers attached to the **specific call's** subsequent re-INVITEs, BYE, and REFER:

```c
echosdk_call_add_header(call, "X-Call-Context", "helpdesk-123");
```

## Media stats

Stats are delivered two ways:

- **Timed** — `ECHOSDK_EV_MEDIA_STATS` fires every `cfg.stats_interval_ms` ms (0 = disabled).
- **On demand** — poll synchronously at any time with `echosdk_call_get_stats`.

### C

```c
// Synchronous poll
echosdk_ev_media_stats_t stats;
echosdk_call_get_stats(call, &stats);
printf("MOS-LQ: %.2f  RTT: %.1f ms  loss: %.1f%%\n",
       stats.mos_lq, stats.rtt_ms, stats.loss_pct);

// Timed delivery via event callback
case ECHOSDK_EV_MEDIA_STATS: {
    const echosdk_ev_media_stats_t *s = &ev->u.stats;
    printf("tick=%u  MOS-LQ=%.2f\n", s->stats_tick, s->mos_lq);
    break;
}
```

### Python

The Python binding provides three ways to access call stats:

#### Push (event-driven, automatic)

Stats fire via `@sdk.on("media_stats")` whenever the SDK emits them (rate set by `stats_interval_ms` in `sdk.configure()`):

```python
@sdk.on("media_stats")
def _(ev):
    print(f"MOS={ev.mos_lq:.2f}  RTT={ev.rtt_ms:.0f}ms  loss={ev.loss_pct:.1f}%")
    # ev is a MediaStatsEvent with all stats fields
```

#### Custom polling rate — `call.poll_stats()`

```python
@sdk.on("established")
def _(ev):
    # Print stats every 2 s regardless of stats_interval_ms
    ev.call.poll_stats(interval=2.0, on_update=lambda s: s.print())
    # s is a CallStats object updated in-place each tick

# Stop polling manually from any thread:
call.stop_polling()
```

#### One-shot snapshot

```python
# New CallStats object with current values
snap = call.stats()
print(snap.mos_lq, snap.rtt_ms, snap.loss_pct)

# Update an existing CallStats object in-place
live = CallStats()
call.fetch_stats(live)
print(live)
```

#### `CallStats` fields

| Field | Type | Description |
|---|---|---|
| `available` | `bool` | `False` until the first update |
| `mos_lq` / `mos_cq` | `float` | MOS listening / conversational quality (1–5) |
| `mos_lq_rx` / `mos_cq_rx` | `float` | RX-side MOS scores |
| `mos_lq_min` / `mos_lq_avg` | `float` | Session min/avg MOS-LQ |
| `mos_method` | `int` | 0 = E-model, 1 = simplified |
| `rtt_ms` | `float` | Round-trip time ms |
| `jitter_ms` | `float` | RX jitter ms |
| `tx_jitter_ms` | `float` | TX jitter ms |
| `loss_pct` / `loss_pct_rx` | `float` | TX / RX packet loss % |
| `packets_sent` / `packets_received` | `int` | RTP packet counters |
| `packets_lost` / `packets_lost_rx` | `int` | Lost packet counters |
| `bytes_sent` / `bytes_received` | `int` | Byte counters |
| `bandwidth_kbps_tx` / `_rx` | `int` | Current bandwidth kbps |
| `avg_bandwidth_kbps_tx` / `_rx` | `int` | Session-average bandwidth kbps |
| `jitter_buffer_ms` | `int` | Current jitter buffer depth ms |
| `jitter_buffer_load` | `int` | Jitter buffer fill level |
| `jitter_buffer_target_ms` | `int` | Adaptive target depth ms |
| `jitter_buffer_adaptive` | `bool` | Adaptive mode active |
| `late_packets` | `int` | Packets arrived too late |
| `discarded_packets` | `int` | Packets discarded |
| `plc_frames` | `int` | PLC concealment frames |
| `plc_ratio` | `float` | PLC ratio (0–1) |
| `codec_name` | `str` | Negotiated codec e.g. `"opus"` |
| `codec_clock_rate` | `int` | Clock rate Hz e.g. `48000` |
| `codec_sample_rate` | `int` | Sample rate Hz |
| `codec_channels` | `int` | Channel count |
| `payload_type` | `int` | RTP payload type |
| `ssrc_tx` / `ssrc_rx` | `int` | SSRC identifiers |
| `remote_addr` | `str` | Remote RTP address:port |
| `audio_level_dbov` | `float` | Speaker level dBov; `NaN` = never measured, `-127` = measured silence |
| `mic_level_dbov` | `float` | Mic level dBov; `NaN` = never measured, `-127` = measured silence |
| `stats_tick` | `int` | Tick counter (1-based) |
| `call_duration_ms` | `int` | Call duration ms |
| `is_final` | `bool` | `True` on the last stats event after hangup |
