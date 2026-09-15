# Degraded Links — bad signal, slow uplink, black-holed paths

## The problem

[Network handover](network_handover.md) answers one question: *my address
changed, where did my media go?* This guide answers the other one, which is far
more common on mobile: **the address never changes and the link goes bad.**

A phone at one bar. A congested uplink. A cell that stops forwarding packets
without ever tearing down the PDP context. A carrier NAT that quietly drops a
UDP binding. In every one of those cases:

- the local IP is unchanged, so handover sees nothing to do;
- the SIP dialog is healthy, so the stack reports the call as up;
- the transport says "connected", because TCP has nothing to send and UDP never
  had a connection to lose;
- and the user hears silence.

Nothing in SIP notices any of this on its own. The settings below are what make
it visible, and in some cases survivable.

---

## Start here

Everything on this page is driven by the RTCP stats tick, so that has to be on.
It is on by default — `stats_interval_ms = 2000` — but if you set it to 0 you
also switch off RTCP accounting inside baresip, which silently disables quality
alerts, media-stall detection and adaptive bitrate along with it.

```c
echosdk_config_t cfg;
echosdk_config_init(&cfg);        /* sensible defaults for all of the below */

cfg.stats_interval_ms = 2000;     /* required by everything here */
```

---

## 1. Notice that audio stopped

Two settings, one non-fatal and one fatal. They are independent; most apps want
the first, and only some want the second.

```c
cfg.media_stall_ms = 4000;   /* warn  — default */
cfg.rtp_timeout_s  = 0;      /* end   — default off */
```

`media_stall_ms` fires `ECHOSDK_EV_QUALITY_ALERT` with issue
`ECHOSDK_QUALITY_MEDIA_STALL` when inbound RTP stops advancing, and fires again
with `recovering = true` when it resumes. The call is left alone. This is what
turns "the user says they can't hear anything" into an event you can act on.

`rtp_timeout_s` is baresip's `avt.rtp_timeout`: it **ends** the call. It is off
by default because ending a call is destructive and some deployments run
legitimate one-way media. 30–60 s is the usual choice when it is wanted.

```c
case ECHOSDK_EV_QUALITY_ALERT: {
    const echosdk_ev_quality_alert_t *a = &ev->u.quality_alert;
    if (a->issue != ECHOSDK_QUALITY_MEDIA_STALL)
        break;
    if (a->recovering)
        ui_clear_warning(a->call);
    else
        ui_warn(a->call, "no audio for %.0f s", a->value / 1000.f);
    break;
}
```

Both are suppressed while a call is held (no RTP is expected) and while a
handover migration is in flight (`netmon` is already narrating that outage in
richer terms).

The stall clock measures time since media *could* have been flowing, not since
the call was answered. On an ICE + DTLS-SRTP call the connectivity checks and
the handshake have to finish before the first packet can arrive, and that
window is not silence — it is setup. The clock therefore starts when the media
path becomes able to carry RTP, and restarts if a renegotiation puts the
encryption back into handshake. Without that, any call whose handshake took
longer than `media_stall_ms` reported a stall on the very tick its audio came
up, and the repair below then fired on a call that had never had a chance to
receive anything.

Per-call override:

```c
echosdk_call_set_rtp_timeout(call, 45);   /* 0 = never, for this call */
```

---

## 2. Notice that the *path* died

A stall tells you media stopped. It does not tell you whether you are still
reachable — and an unreachable registration means no inbound calls at all, with
nothing on the wire to reveal it until the next refresh.

```c
cfg.keepalive_interval   = 30000;   /* default */
cfg.keepalive_reregister = true;    /* default */
```

Every 30 s of registered idle time the SDK sends a SIP OPTIONS request to the
proxy. It does two jobs at once:

- **refreshes the UDP NAT binding.** Carrier NAT drops idle UDP mappings after
  30–180 s. The default `reg_expires` of 3600 s is far longer than that, so
  without a probe there is a long window where the registrar thinks you are
  reachable and the NAT no longer agrees.
- **tests reachability.** Any response counts as reachable, including a 405
  Method Not Allowed — a proxy that refuses OPTIONS still had to receive it. No
  response means the path is black-holed, and with `keepalive_reregister` the
  SDK re-REGISTERs immediately instead of waiting up to an hour.

A probe that goes unanswered is also reported: the account moves to
`ECHOSDK_REG_RECONNECTING`, so an app whose status indicator is bound to the
registration state says "Reconnecting…" for a binding that is registered on
paper and unreachable in fact. It goes back to `ECHOSDK_REG_REGISTERED` when the
re-REGISTER lands — or, with `keepalive_reregister` off, when a later probe is
answered again.

The probe is skipped while a call is up on that account: RTP already holds the
binding open, and adding a request that competes with media for a congested
uplink is exactly wrong.

On foreground or push wake, ask directly rather than waiting for the tick:

```c
echosdk_account_keepalive_now(account);
```

---

## 3. Fail fast instead of stalling for 32 seconds

A request onto a black-holed link gets no response at all, and RFC 3261 bounds
that with Timer B / Timer F at 64·T1 = 32 s. In libre those are compile-time
constants, so the SDK enforces its own bound:

```c
cfg.sip_timer_b_ms = 10000;   /* outgoing INVITE with no response  */
cfg.sip_timer_f_ms = 10000;   /* REGISTER with no response         */
```

`sip_timer_b_ms` watches only the `CALLING` state — where nothing at all has
come back. Once a provisional response arrives the call moves to `RINGING`, the
far end is demonstrably reachable, and how long to let it ring is a product
decision, not a transport timeout. On expiry the call is cancelled with 408 and
surfaces as `ECHOSDK_CALL_FAILED` / `ECHOSDK_ERR_TIMEOUT`.

`sip_t1_ms` and `sip_t2_ms` exist for completeness and have no effect —
retransmission intervals live inside libre's transaction layer.

---

## 4. Come back gracefully, as a fleet

```c
cfg.reg_retry_jitter = 0.2;    /* default */
cfg.dns_srv_failover = true;   /* default */
```

**Jitter.** Every device that lost the same Wi-Fi runs the same backoff from the
same instant. Without randomisation they all re-REGISTER in step and the
registrar takes the whole fleet as one burst — and because the schedules never
diverge, the herd re-forms on every subsequent attempt. Each delay is drawn from
`[d·(1−jitter), d·(1+jitter)]`.

**SRV failover.** A retry loop that re-sends to the host it just timed out on
never consults the priority order the SRV records were published to express. The
SDK resolves `_sip._<transport>.<domain>` once per account and advances one
target per failed attempt, in (priority, weight) order, wrapping at the end.

It is skipped — deliberately — when there is no ordered list to walk or when an
operator has already made the choice: an explicit `outbound_proxy`, a server
given as an IP literal, an explicit port (RFC 3263 §4 step 1 skips NAPTR/SRV),
or a WS/WSS URL.

---

## 5. Use less bandwidth when there is less bandwidth

```c
cfg.adaptive_bitrate       = true;
cfg.adapt_min_bitrate      = 12000;   /* default */
cfg.adapt_max_bitrate      = 32000;   /* default */
cfg.opus_expected_loss_pct = 15;      /* Opus FEC — see below */
cfg.opus.fec               = true;
```

Adaptation reads the loss the **peer** reports in its RTCP receiver report —
what the other end is actually losing, not what you are. Above
`adapt_loss_down_pct` the bitrate halves; after `adapt_recover_ticks`
consecutive ticks below `adapt_loss_up_pct` it rises by 25%. Between the two
thresholds is a dead band, so a link hovering near one of them does not
oscillate.

It is applied through the codec's encoder-update path: **no re-INVITE, no
renegotiation, no gap in the audio.** That also means it only does anything for
a codec with a variable bitrate — Opus in practice. A G.711 call is left alone;
there is nothing to vary.

At runtime:

```c
echosdk_set_adaptive_bitrate(true, 8000, 24000);
echosdk_call_set_bitrate(call, 16000);   /* manual; 0 = negotiated rate */
```

### Opus FEC needs both settings

`opus.fec` only *permits* in-band FEC. `opus_expected_loss_pct` is what makes
the encoder spend part of its budget on the redundant LBRR copy of the previous
frame, and what makes the decoder look for it. With `opus.fec` alone, nothing is
concealed. FEC costs bitrate and quality even on a clean link, which is why it
is off by default; 10–20 is reasonable for mobile.

### A note on G.711

Fixed-rate codecs have no bitrate to adapt and, in this build, no packet-loss
concealment either — baresip's `plc` module is a wrapper around spandsp, which
is not among the vendored dependencies. On a lossy link a lost G.711 frame is
played as silence. Offering Opus is what makes such a link survivable.

---

## What to set, by deployment

| Deployment | Settings |
|---|---|
| Mobile app, cellular-first | `media_stall_ms=4000`, `keepalive_interval=30000`, `sip_timer_b_ms=10000`, `adaptive_bitrate=true`, `opus_expected_loss_pct=15` + `opus.fec=true`, `net_ice_handover=FAIL_FAST` |
| Desktop softphone on LAN | Defaults. `keepalive_interval` can go to 0 behind a NAT you control |
| Call centre, wired, G.711 PBX | `rtp_timeout_s=60` (an agent on a dead call should be freed), `media_stall_ms=3000`. Adaptation does nothing here — negotiate Opus if the PBX will |
| Kiosk / unattended | `rtp_timeout_s=45` so nothing gets stuck on a silent call with no one to notice |

---

## What this does not do

- **It does not fix a link that cannot carry a call.** Below roughly 12 kbit/s
  of usable throughput there is no Opus configuration that sounds acceptable.
  Adaptation buys headroom; it does not manufacture bandwidth.
- **It does not adapt the jitter buffer bounds.** The buffer inside those bounds
  is adaptive (baresip's `jbtype = ADAPTIVE`), but `jitter_buffer_min_ms` /
  `jitter_buffer_max_ms` are static per call. Widen them up front for mobile —
  see [Media](../api/media.md#jitter-buffer).
- **It does not switch codecs mid-call.** That needs a re-INVITE and full
  renegotiation; the codec list is fixed when the call is answered.
- **It does not restart ICE.** See
  [Network handover](network_handover.md#ice-calls).

## A stall is not always yours to repair

`ECHOSDK_QUALITY_MEDIA_STALL` says inbound RTP stopped.  It does not say why,
and the two causes want opposite responses:

* **Your path broke** — a NAT rebinding, a multi-WAN router moving your flow,
  an interface that went away.  The repair is a new offer, and the SDK makes
  one (see [Network handover](network_handover.md)).
* **The far end went quiet** — a peer that muted, stopped sending without
  signalling hold, or is waiting on something of its own.  There is nothing to
  repair, and offering anyway costs audio: Asterisk answers a re-offer with
  `a=connection:new`, rebuilds its RTP instance, and a call that was merely
  quiet comes back silent.

The ICE agent answers the question.  While its connectivity checks are still
being answered on the selected pair — `ECHOSDK_ICE_CONNECTED` or
`ECHOSDK_ICE_COMPLETED` — the path is alive in both directions and the SDK
declines the repair, logging *"the far end has gone quiet, not our path"*.
The quality alert still reaches the app, because a user staring at a silent
call wants to be told.  `ECHOSDK_ICE_DISCONNECTED` or `ECHOSDK_ICE_FAILED` is
the opposite verdict and the repair runs as before, as it does for a call
without ICE.

So: treat `MEDIA_STALL` as "no audio is arriving", and read
`echosdk_call_ice_state()` (or the `ice_state` field on the stats tick) to
learn whose problem it is.
