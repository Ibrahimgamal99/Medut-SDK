# Network Handover — Wi-Fi ↔ 4G/5G

## The problem

A phone that walks out of Wi-Fi range onto cellular gets a new IP address while
the SIP stack is still running. Three things break at once, and none of them
recover on their own:

| What breaks | Symptom without handover |
|---|---|
| SIP transports are bound to an address that no longer exists | Outgoing requests fail; a dead TCP/TLS/WSS socket stalls until Timer B (32 s) rather than failing fast |
| The registrar's binding still points at the old Contact | Inbound calls are routed into the void until the next REGISTER refresh — up to `reg_expires` seconds |
| Active calls advertise the old address in the SDP `c=` line | The peer keeps sending RTP where you can no longer receive it — one-way or dead audio |

EchoSDK repairs all three. The sequence is:

```
detect → settle → rebind transports → re-REGISTER → re-INVITE → verify media
```

---

## Wiring it up

The SDK cannot see the OS connectivity signal, so tell it:

```c
echosdk_network_changed();
```

Safe to call from any thread, as often as the OS fires. Calls are coalesced —
the handover runs once, after the address set has been stable for
`net_settle_ms`.

### Android

```kotlin
val cm = getSystemService(ConnectivityManager::class.java)
cm.registerDefaultNetworkCallback(object : ConnectivityManager.NetworkCallback() {
    override fun onAvailable(network: Network)  = EchoSDK.networkChanged()
    override fun onLost(network: Network)       = EchoSDK.networkChanged()
    override fun onCapabilitiesChanged(n: Network, c: NetworkCapabilities) =
        EchoSDK.networkChanged()
})
```

Set `net_monitor_interval_s = 0` on mobile — polling `getifaddrs()` on a timer
wastes battery when the OS already has the signal.

### iOS

```swift
let monitor = NWPathMonitor()
monitor.pathUpdateHandler = { _ in echosdk_network_changed() }
monitor.start(queue: DispatchQueue.global(qos: .utility))
```

### Desktop

Leave the built-in poller on (`net_monitor_interval_s`, default 10 s) and you
need no platform code at all. It reconciles the kernel's address list against
the stack's on every tick. If you already subscribe to NetworkManager,
`SCNetworkReachability`, or `NotifyAddrChange`, call `echosdk_network_changed()`
from there and set the interval to 0.

---

## Watching it happen

Every stage arrives as `ECHOSDK_EV_NETWORK`:

```c
case ECHOSDK_EV_NETWORK: {
    const echosdk_ev_network_t *n = &ev->u.network;
    switch (n->event) {
    case ECHOSDK_NET_CHANGE_DETECTED:
        log("network changed — settling");
        break;
    case ECHOSDK_NET_DOWN:
        log("no usable network — holding");
        break;
    case ECHOSDK_NET_UP:
        log("network back: %s", n->local_addr);
        break;
    case ECHOSDK_NET_TRANSPORT_RESET:
        log("SIP transports rebound on %s", n->local_addr);
        break;
    case ECHOSDK_NET_CALL_MIGRATING:
        log("link settled — rebuilding the media path %u/%u",
            n->attempt, n->max_attempts);
        break;
    case ECHOSDK_NET_CALL_MIGRATE_ACCEPTED:
        log("peer accepted the new path — waiting for audio to resume");
        break;
    case ECHOSDK_NET_CALL_MIGRATED:
        log("media recovered after %.1f s", n->elapsed_ms / 1000.0);
        break;
    case ECHOSDK_NET_CALL_MIGRATION_FAILED:
        log("media did not recover (%s)", echosdk_strerror(n->error));
        break;
    default:
        break;
    }
    break;
}
```

Python:

```python
@sdk.on("network")
def _(ev):
    if ev.stage == "call_migrating":
        print(f"link settled — rebuilding media path {ev.attempt}/{ev.max_attempts}")
    elif ev.stage == "call_migrate_accepted":
        print("peer accepted the new path — waiting for audio to resume")
    elif ev.stage == "call_migrated":
        print(f"media recovered after {ev.elapsed_ms / 1000:.1f}s")
```

### Stages

| Stage | Meaning |
|---|---|
| `CHANGE_DETECTED` | The local address set changed; the debounce timer is running |
| `DOWN` | No routable address. The handover is held, not abandoned — nothing is flushed while there is nowhere to rebind |
| `UP` | A routable address is back |
| `TRANSPORT_RESET` | SIP transports flushed and re-bound; dead TCP/TLS/WSS connections dropped |
| `REREGISTERING` | REGISTER re-sent for one account (`account` field is set) |
| `CALL_MIGRATING` | re-INVITE sent with the new address in the SDP |
| `CALL_MIGRATE_ACCEPTED` | The peer answered the offer. Audio is not confirmed yet |
| `CALL_MIGRATED` | RTP observed on the new path. `elapsed_ms` is the audio gap |
| `CALL_DEFERRED` | Migration is parked and will be retried — the dialog cannot take a re-INVITE yet, the new source address is not yet resolvable (route still being installed), or the media encryption is still handshaking |
| `CALL_MIGRATION_FAILED` | Gave up after `max_attempts` |
| `HANDOVER_FAILED` | The transport rebind itself failed; retrying with backoff |

### The registration state follows too

An app does not have to subscribe to `ECHOSDK_EV_NETWORK` just to keep its
status indicator honest. From `CHANGE_DETECTED` (or `DOWN`, if the link went
away entirely) every account the app asked to register moves to
`ECHOSDK_REG_RECONNECTING`, and stays there through the re-REGISTER until it is
answered — the binding at the registrar points at an address the device has
left, so reporting `REGISTERED` across a handover would show a green dot over a
path that cannot take an inbound call.

A handover that exhausts its transport-rebind attempts (`HANDOVER_FAILED` with
`attempt == max_attempts`) hands those accounts to their own registration retry
policy, so the reconnect keeps being driven by something rather than waiting for
the next network change.

The `NET_*` stages above are still the ones that describe *what* is being
repaired, and the per-call migration events are the only place media recovery
is reported. `RECONNECTING` is just the one-line summary a registration
indicator can bind to.

---

## Media is verified, not assumed

A re-INVITE that gets a `200 OK` can still leave audio dead — the peer's NAT
binding has not moved, the answer came back with the wrong direction, or the new
path is filtered. So after each re-INVITE the SDK samples the stream's RX packet
counter and checks it has advanced `net_verify_ms` later. If it has not, it
re-offers, and after `net_max_attempts` it reports
`CALL_MIGRATION_FAILED`.

Set `net_verify_ms = 0` to skip the check and treat the re-INVITE as sufficient.

Held calls are exempt: they carry no RTP, so the answered re-INVITE is the only
confirmation available.

---

## Configuration

| Field | Default | Description |
|---|---|---|
| `net_monitor_interval_s` | 10 | Interface poll period in seconds; 0 = off |
| `net_settle_ms` | 1500 | How long the address set must be stable before acting |
| `net_reinvite_calls` | true | Re-INVITE active calls onto the new address |
| `net_verify_ms` | 4000 | Wait for RTP before retrying; 0 disables the media check |
| `net_max_attempts` | 6 | Retry ceiling for both the rebind and each call migration |
| `net_hangup_on_migration_failure` | false | End calls whose media could not be migrated |

Runtime overrides:

```c
echosdk_network_set_monitor_interval(0);              /* mobile */
echosdk_network_set_handover_policy(true, false);     /* reinvite, don't hang up */
```

Queries:

```c
char ip[64];
echosdk_network_local_addr(ip, sizeof(ip));   /* "" when there is no address */
bool up = echosdk_network_is_up();            /* false = no routable address */
```

---

## What is handled

| Case | Behaviour |
|---|---|
| Wi-Fi → cellular, cellular → Wi-Fi | Full sequence; SDP `c=` follows the new source address |
| Both networks briefly down | `DOWN` emitted, handover held with exponential backoff (1→32 s), applied when an address returns. Transports are never flushed into a void |
| Interface still coming up (address burst) | Debounced — the handover runs once, on the final address set |
| Route changed, addresses unchanged | `echosdk_network_changed()` still re-checks each call's source address and re-INVITEs only what actually moved |
| Address changed but a call's path did not | That call is skipped — no pointless re-INVITE. **Except over WS/WSS**, see the row below |
| WS/WSS call, path unchanged | Still re-INVITEd. A WebSocket client has no listening port, so its Contact is the RFC 7118 placeholder `sip:user@<ip>:9;transport=wss` and the server reaches it by remembering which WebSocket the dialog's requests arrived on. A transport reset always builds a new WebSocket, so without the re-INVITE that association is stale: media keeps flowing and your own BYE still gets out, but an **inbound BYE can never be delivered** and the call hangs in `ESTABLISHED`. The re-INVITE re-binds the dialog to the live connection |
| IPv4 ↔ IPv6 | Handled; the address family follows the new source address |
| DNS servers changed | Resolvers refreshed, unless you pinned your own nameservers |
| Dead TCP/TLS/WSS sockets | Dropped by the transport flush, so the next request opens a fresh connection instead of stalling to Timer B |
| Account in retry backoff | Backoff cancelled and the attempt counter reset — a new network deserves an immediate try, not the tail of a 5-minute backoff |
| Account created but never registered | Left alone |
| Call in an early dialog (`CALLING`/`RINGING`) | Deferred and migrated once the dialog is established, rather than hung up |
| New default route not installed yet | Common during Wi-Fi→cellular: the address is up before the route is. The call is parked as `CALL_DEFERRED` and address discovery re-runs on each verify tick, up to `net_max_attempts`, then reports `CALL_MIGRATION_FAILED` |
| DTLS-SRTP still handshaking | A re-INVITE landing mid-handshake is the strongest predictor of a permanently silent call — the handshake is still retransmitting when the address moves under it, and `menc_secure` is never set. The call is parked as `CALL_DEFERRED` and the offer goes out as soon as the encryption comes up. Bounded at 12 s (the length of the DTLS retry ladder), after which the migration proceeds anyway, because a migration that never goes out guarantees the silence it was avoiding |
| Peer media address not yet known | Same treatment — parked and retried rather than skipped |
| re-INVITE glare (`491`) | Retried by the SIP layer (3 s / 1 s per RFC 3261) |
| `401`/`407` on the re-INVITE | Re-authenticated and retried automatically |
| `408`/`481` on the re-INVITE | Dialog terminated — the call is genuinely gone |
| Call on hold | Migrated with its direction attribute preserved; media check skipped |
| Link-local-only addresses | Enough to rebind, reported as `DOWN` since no off-link registrar is reachable |

RTP sockets are never re-created. baresip binds them to the wildcard address, so
they follow the new default route automatically — only the address advertised in
the SDP has to change.

---

## ICE calls

**An ICE call is migrated with a real RFC 8445 §9 ICE restart.**

Rewriting the SDP address is enough for a direct-RTP call and does nothing for an
ICE call. Two reasons, both inside baresip's ice module:

- it owns the *media* address — it writes the selected local candidate there, and
  a media-level `c=` line overrides the session-level one (RFC 4566 §5.7), which
  is the only one a plain re-INVITE changes;
- nothing re-gathers. Its update handler re-encodes the candidate list it already
  has, under the same `ice-ufrag` / `ice-pwd`.

So the offer would re-advertise the network the call just left, while the peer
drops what arrives from the new source — with ICE, a source that is not in the
candidate list is not accepted (Asterisk: `Source not in ICE active candidate
list`).

The SDK therefore replaces the whole ICE session for that call: new credentials,
a fresh gather on the interface that now carries the default route, and the
re-INVITE built from the result, carrying the new candidates *and* the new
address at both levels. The RTP sockets are kept — they are wildcard-bound, so
they already follow the new route, and the media encryption is keyed to them.

Nothing in the app has to change for this. What you observe is the ordinary
sequence, with the re-INVITE arriving up to `cfg.ice_gathering_timeout_ms` later
than it would for a direct-RTP call:

    CHANGE_DETECTED → TRANSPORT_RESET → REREGISTERING
                    → CALL_MIGRATING → CALL_MIGRATE_ACCEPTED → CALL_MIGRATED

`cfg.ice_gathering_timeout_ms` (default 2 s) bounds the re-gather as well as the
one on dial: when it expires the offer goes out with whatever was gathered by
then, and a gather that completes afterwards re-offers the fuller set. Here a
configured 0 does **not** mean "wait indefinitely" — the call is live and silent
while this runs, so a 3 s bound applies instead.

An ICE restart is only attempted when the local address actually moved. A
WebSocket call whose path did not change is still re-INVITEd, but for an
unrelated reason — to re-bind the dialog to the new WebSocket — and putting
working media through a restart for that would cost audio for nothing.

### When the restart cannot be done

Two cases remain: the call has no ICE session left to restart (every stream had
its media-NAT disabled), or the replacement session could not be allocated. Those
fall back to the plain re-INVITE with the pre-handover candidate set, which
recovers a call the peer can reach directly or through a still-valid TURN relay,
and cannot recover one it cannot.

Such a call emits `ECHOSDK_NET_CALL_ICE_STALE` once per handover, *before* that
re-INVITE, so you can tell the user something useful while the attempt is in
flight rather than after it has failed. `cfg.net_ice_handover` then decides how
long to keep trying:

| Value | Behaviour |
|---|---|
| `ECHOSDK_ICE_HANDOVER_BEST_EFFORT` (default) | The full `net_verify_ms` × `net_max_attempts` budget — 24 s at the defaults. Right when calls often are direct or TURN-relayed, because those do recover |
| `ECHOSDK_ICE_HANDOVER_FAIL_FAST` | One attempt, then `CALL_MIGRATION_FAILED`. Right when calls are ICE+TURN over cellular: re-offering the same wrong candidates cannot succeed, and 24 s of silence is worse for the user than a prompt "reconnecting…" |

Neither applies to a call whose ICE *was* restarted: it is not offering the wrong
candidates any more, so it keeps the full retry budget — what it is waiting on
(the peer's NAT rebinding) is exactly what a direct-RTP call waits on.

`max_attempts` in the event reflects the budget the call is actually held to, so
a fail-fast call renders as `1/1` rather than promising retries it will not make.

```c
case ECHOSDK_NET_CALL_ICE_STALE:
    /* This call could not be re-gathered; recovery may not work. */
    ui_show_reconnecting(n->call);
    break;

case ECHOSDK_NET_CALL_MIGRATION_FAILED:
    redial(n->call);   /* a fresh call gathers fresh candidates */
    break;
```

Re-placing the call remains the complete remedy for one that failed: a new call
allocates a new media session and gathers on the network you are actually on.

### When the restart is done but nothing validates

A restart that is offered and answered can still end with no usable candidate
pair: every pair in the check list fails, nothing is nominated, and the media
path is never proven. The common cause is a peer that rejected the connectivity
checks. Asterisk/FreePBX answers an ICE restart offer with `a=connection:new`,
tears down and rebuilds its own RTP instance, and replies `401 Unauthorized` to
checks that arrive while that rebuild is in flight; libre's check list is
one-shot per generation, so those pairs stay Failed.

Such a call emits `ECHOSDK_NET_CALL_ICE_FAILED`. It is *not* the same as
`CALL_ICE_STALE` — there the candidates could not be re-gathered at all; here
they were gathered, offered and answered, and the checks failed.

The call is not ended, and it is usually not silent either: the SDK holds the
stream on the pair a previous ICE generation validated rather than letting the
SDP exchange leave it on the address the peer signals, which for a PBX behind
NAT is an RFC 1918 address the phone cannot reach. Treat the event as "the new
path was never proven", and let the media-stall alert tell you whether audio is
actually flowing:

```c
case ECHOSDK_NET_CALL_ICE_FAILED:
    /* Media is probably still up on the previous pair — do not hang up.
     * Watch for MEDIA_STALL to decide whether it really died. */
    log_warn("ICE could not validate a path for this call");
    break;
```

### Stalls do not restart ICE on an unchanged address

The media-stall repair and the handover repair share machinery but not this
decision. A handover restarts ICE because the local address moved and the old
candidates are genuinely wrong. A stall on an address that has *not* moved gets
a plain re-offer instead: restarting would ask the peer to discard a working
ICE session and re-run the checks, and against a peer that rebuilds its RTP
instance to do so that trade loses a call more often than it saves one. A stall
that rotates to another interface still restarts — there the new candidates are
the whole point.
