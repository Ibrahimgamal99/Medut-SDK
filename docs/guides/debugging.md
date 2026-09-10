# Debugging SIP

## Init/shutdown trace

`echosdk_init()` and `echosdk_shutdown()` walk through ~14 stages. To trace which step a hang or crash occurs at, set the `ECHOSDK_DEBUG_INIT` env var before launching:

```bash
# Linux / macOS
ECHOSDK_DEBUG_INIT=1 ./your_app
```

```powershell
# Windows
$env:ECHOSDK_DEBUG_INIT=1
.\your_app.exe
```

Output looks like:

```
[bsdk] step 1: deep_copy
[bsdk] step 2: log_init
[bsdk] step 3: libre_init
...
[bsdk] step 14: done
```

Leave the variable unset for normal (silent) operation.

---

## Log levels

Start with `log_level = 2` (info) for call flow visibility:

```c
cfg.log_level = 2;  // 0=err, 1=warn, 2=info, 3=debug
```

Use `log_level = 3` (debug) for maximum detail — internal state machines, timer events, packet parsing. Expect high output volume at this level.

---

## SIP trace

Enable per-message SIP tracing:

```c
cfg.trace_sip = true;
```

Each `ECHOSDK_EV_SIP_TRACE` event contains the full SIP message. Log it:

```c
case ECHOSDK_EV_SIP_TRACE: {
    const char *dir = (ev->u.sip_trace.dir == ECHOSDK_MEDIA_DIR_TX)
                      ? "SEND" : "RECV";
    printf("[%s %s %s]\n%s\n",
           dir, ev->u.sip_trace.transport,
           ev->u.sip_trace.remote_addr,
           ev->u.sip_trace.raw_message);
    break;
}
```

### What to look for

| Check | Where |
|---|---|
| Registration sent? | Look for `REGISTER` in TX direction |
| Auth challenge? | Look for `401 Unauthorized` in RX; then `REGISTER` with `Authorization` header in TX |
| INVITE sent? | TX `INVITE` with correct `Contact` and SDP |
| SDP codecs match? | Compare `m=audio` line in INVITE and 200 OK |
| BYE received? | RX `BYE` — who terminated and why? |
| Error responses? | `4xx`, `5xx`, `6xx` — check `Reason` header |

---

## Pcap capture

Write packets to a file for Wireshark analysis:

```c
echosdk_pcap_start("/tmp/debug.pcap");
// ... reproduce issue ...
echosdk_pcap_stop();
```

Open in Wireshark:

```bash
wireshark /tmp/debug.pcap
```

### Useful Wireshark filters

| Filter | Purpose |
|---|---|
| `sip` | All SIP messages |
| `sip.Method == "REGISTER"` | Registration flow |
| `sip.Method == "INVITE"` | Call setup |
| `sip.Status-Code == 401` | Auth challenges |
| `rtp` | Media packets |
| `rtcp` | Quality reports |
| `sip || rtp || rtcp` | Full call flow |

### Wireshark tips

1. Right-click a SIP message → **Follow SIP Call** to see the complete dialog.
2. **Statistics → RTP → Stream Analysis** for jitter/loss graphs.
3. **Telephony → VoIP Calls** for a call ladder diagram.

---

## SDP negotiation

Enable SDP diff to see codec and encryption negotiation:

```c
cfg.trace_sdp_diff = true;
```

The `ECHOSDK_EV_SDP_NEGOTIATION` event shows:

```c
case ECHOSDK_EV_SDP_NEGOTIATION: {
    printf("Codec: %s  Crypto: %s\n",
           ev->u.sdp.negotiated_codec,
           ev->u.sdp.negotiated_crypto);
    // Check rejected_codecs and warnings arrays
    break;
}
```

Common issues:
- **No matching codec** → `negotiated_codec` is NULL; check `audio_codecs` config.
- **Crypto mismatch** → `negotiated_crypto` is "NONE" when you expected "DTLS-SRTP"; check `media_enc` setting.

---

## Media stats

Periodic stats reveal audio quality problems:

```c
cfg.stats_interval_ms = 3000;
```

```c
case ECHOSDK_EV_MEDIA_STATS: {
    printf("MOS-LQ=%.2f loss=%.1f%% jitter=%.1fms RTT=%.1fms\n",
           ev->u.stats.mos_lq, ev->u.stats.loss_pct,
           ev->u.stats.jitter_ms, ev->u.stats.rtt_ms);
    break;
}
```

### Interpreting stats

| Metric | Good | Warning | Bad |
|---|---|---|---|
| MOS-LQ | ≥ 4.0 | 3.6–4.0 | < 3.6 |
| Loss | < 1% | 1–3% | > 3% |
| Jitter | < 20 ms | 20–50 ms | > 50 ms |
| RTT | < 100 ms | 100–200 ms | > 200 ms |

---

## Registration debugging

| Problem | Check |
|---|---|
| No REGISTER sent | `echosdk_account_register()` called? Network reachable? |
| 401 Unauthorized | Wrong `uri` or `password`; check `auth_user` override |
| 403 Forbidden | Account not provisioned on server; check `uri` domain |
| 408 Timeout | Wrong `server_host`/`server_port`; firewall blocks SIP port |
| Registration drops | `reg_expires` too short; keepalive not working; NAT timeout |

### Retry policy

The SDK retries automatically with exponential backoff:

```
attempt 1: wait 2s    (reg_retry_initial_ms)
attempt 2: wait 4s    (× backoff = 2.0)
attempt 3: wait 8s
attempt 4: wait 16s
...
max:      wait 5 min  (reg_retry_max_ms)
```

Monitor retries via `retry_attempt` and `retry_delay_ms` in `ECHOSDK_EV_REG_STATE`.

---

## Common error codes

| Code | Meaning | Action |
|---|---|---|
| `ECHOSDK_ERR_DNS` | Cannot resolve server hostname | Check DNS / network |
| `ECHOSDK_ERR_TRANSPORT` | TCP/TLS/WebSocket connection failed | Check firewall, TLS cert |
| `ECHOSDK_ERR_AUTH` | SIP authentication failed | Verify credentials |
| `ECHOSDK_ERR_TIMEOUT` | No response (timer B/F expired) | Check server reachability |
| `ECHOSDK_ERR_SERVER_5XX` | Server error | Check server logs |
| `ECHOSDK_ERR_WS_PROTOCOL_REJECTED` | WebSocket upgrade rejected | Check `ws_origin`, server config |
| `ECHOSDK_ERR_STATE` | API called in wrong lifecycle state | Check call/account state before calling |

---

## Silent failures worth knowing about

A SIP stack has several places where a request is accepted and then never
arrives, and where the value that would tell you so is indistinguishable from a
legitimate one. These are the log lines the SDK emits so that does not happen.

### The BYE that never left

```
EchoSDK/sipsess: BYE queued
EchoSDK/sipsess: BYE could not be sent (…) — the peer will stay on the call
                 until it gives up on its own
```

libre discards the return value of `sipsess_bye()`, and there are three
separate ways a BYE can be skipped, so a hangup can report success while the
far end stays connected. Read the line at teardown:

| Line at hangup | Meaning |
|---|---|
| `BYE queued` | The request was accepted. If nothing reaches the wire, the failure is in routing or transport. |
| `BYE could not be sent (…)` | The session layer refused; the error names why. |
| neither | The session destructor never got that far — a transaction is still pending. |

### In-dialog requests routed away from the WebSocket flow

```
EchoSDK: ws in-dialog route echo:5060 is not the registration flow;
         routing over pbx.example.com:443 instead (RFC 7118 B.2)
```

A UAS dialog's route set is only the peer's Record-Route; unlike a UAC's, it
does not begin with your outbound proxy. A PBX behind a reverse proxy commonly
Record-Routes its own internal address — or omits Record-Route entirely, leaving
libre to route to a Contact like `sip:asterisk@echo:5060`, a hostname that
resolves nowhere. The lookup then fails *asynchronously*, long after the request
was accepted. This line means the SDK put it back on the registration flow.

### ICE candidates the peer was never told about

```
EchoSDK/ice: selected local candidate <addr> was never signalled (offered <addr>)
             — re-offering so the peer accepts our media
```

See [NAT traversal](nat_traversal.md#when-the-peer-drops-your-media). Note that
this re-offer keeps the session credentials, so it only helps with peers that
re-read candidates without an ICE restart. The restart the SDK does perform is
the one on network handover, logged as `restarting ICE on <addr>`.

### ICE gathering that never finishes

```
EchoSDK/ice: candidate gathering did not complete in time; offering the
             candidates gathered so far (cfg.ice_gathering_timeout_ms=2000)
```

With a media-NAT configured the INVITE is deferred until gathering reports
complete. `cfg.ice_gathering_timeout_ms` bounds that wait; without it an
outgoing call can sit in `CALLING` with no SIP message on the wire at all.

The word before "gathering" says which wait it was: `candidate` for the one on
dial, `restart` for the re-gather of an ICE restart on network handover.

### ICE restart on handover

```
EchoSDK/ice: restarting ICE on 100.82.7.19 — new credentials, re-gathering,
             re-INVITE follows within 2000 ms
```

How an ICE call is migrated: the whole ICE session is replaced so the re-INVITE
carries candidates for the network the device is on now. See
[network handover](network_handover.md#ice-calls). The re-INVITE itself appears
after this line, when the re-gather reports or the deadline expires — so a
`CALL_MIGRATING` event without a re-INVITE on the wire for up to
`ice_gathering_timeout_ms` is expected here, not a stall.

If a call still fails to migrate, look for the reasons a restart could not be
performed:

```
EchoSDK/ice: restart: replacement session failed (...) — keeping the current ICE state
EchoSDK/ice: restart gathering failed (...) — offering the candidates gathered so far
```

The first falls back to the plain re-INVITE and emits `CALL_ICE_STALE`; the
second still offers, deliberately, because reporting a media-NAT failure to
baresip would close a call that is up and merely needs a new offer.

### `STUN Response: 401 Unauthorized` on a restarted call

```
connchk: {audio.1} STUN Response: 401 Unauthorized
ice: {audio.1} checklist update: no valid candidate pair (validlist=0)
ice: connectivity check failed: No such file or directory [2]
EchoSDK/ice: the restarted ICE generation did not nominate a pair within 6000 ms
```

The peer rejected the connectivity checks for the generation an ICE restart
just created. Asterisk/FreePBX does this predictably: it answers a restart
offer with `a=connection:new`, destroys and rebuilds its RTP instance, and
401s checks that arrive before the replacement ICE session is ready. libre
starts a check list once per generation, so pairs that fail this way stay
Failed and nothing re-checks them.

The call is left up and `ECHOSDK_NET_CALL_ICE_FAILED` is emitted. Audio usually
keeps working, because the stream is held on the pair a previous generation
validated — check the `peer` field in the periodic level line to confirm which
address it is actually on:

```
level mic -13.7 dBov / spk -13.1 dBov  tx 173 rx 53  peer 82.129.158.253:18308
```

A `peer` showing the PBX's *private* address (`172.16.11.52`) alongside
`spk -72.2 dBov` and climbing `rx` is the signature of a stream pinned to an
unreachable address: packets arrive at the socket, none of them decrypt.

Reading the check list itself: a two-local × three-remote candidate set
normally forms only **two** pairs, and that is correct, not a bug. Pairs across
address families are never formed (an IPv6 link-local remote against IPv4
locals contributes nothing), and srflx candidates are pruned against their base
per RFC 8445 §6.1.2.4 — an srflx whose `rel-addr` equals the host candidate
duplicates it. What is left is one pair to the peer's private address, which
always times out, and one to its public address, which is the only pair that
can ever succeed. The valid list is then built as a side effect of that check
succeeding (the response's XOR-MAPPED-ADDRESS yields the peer-reflexive pair
that gets nominated), so a rejected check means an empty valid list and nothing
to nominate.

### Audio levels: `NaN` is not silence

`mic_level_dbov` and `audio_level_dbov` start as `NaN` and become a real dBov
figure only once a frame has been measured. `-127.0` is a *measurement* — it
means the samples were all zero.

| Reading | Meaning |
|---|---|
| `NaN` | No frame was measured. Media never started, or the tap is not in the chain. |
| `-127.0` | Frames were measured and are digital silence — a dead capture path, not a quiet room. |
| anything else | Real audio. |

Reading them together is what separates "no media at all" from "media flowing
one way": `mic NaN / spk NaN` with zero RTP counters is a dead call, while
`mic -127.0 / spk -35.0` with healthy counters is a capture problem.

### "No common audio or video codecs" that is not about codecs

baresip disables a stream whose media profile it cannot match and then reports
the call as having no common codecs. An account offering `RTP/AVP` against a
peer offering `UDP/TLS/RTP/SAVPF` fails with a full, perfectly compatible codec
list on both sides. When the account offers no encryption the SDK says so
instead:

> No common media: this account offers unencrypted RTP and the peer requires
> encrypted media (set media_enc, e.g. DTLS-SRTP)

A WSS/WebRTC-facing PBX always offers `SAVPF`; set `media_enc` to DTLS-SRTP.

---

## Debug workflow

1. Set `log_level = 2`, `trace_sip = true`, `trace_sdp_diff = true`.
2. Start pcap capture: `echosdk_pcap_start("debug.pcap")`.
3. Reproduce the issue.
4. Stop pcap: `echosdk_pcap_stop()`.
5. Review SIP trace output for message flow.
6. Open pcap in Wireshark for detailed analysis.
7. Set `log_level = 3` if more detail is needed.
