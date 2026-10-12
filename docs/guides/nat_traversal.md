# NAT Traversal — ICE / STUN / TURN

## The problem

When your SIP device is behind a NAT router, the IP address and port in the SDP offer are private (e.g. `192.168.1.50`). The remote side cannot reach that address. You need NAT traversal to discover your public address and ensure media flows in both directions.

---

## Quick diagnosis

| Symptom | Likely cause |
|---|---|
| One-way audio (you hear them, they don't hear you) | SDP contains private IP; RTP flows outbound but replies can't return |
| No audio either direction | Firewall blocks RTP ports entirely |
| Registration works but calls fail | SIP signaling passes (UDP hole kept alive) but RTP ports are blocked |
| Works from some networks, not others | Symmetric NAT (STUN alone won't work — need TURN) |

---

## STUN (simple NAT)

STUN discovers your public IP:port mapping. Works when the NAT is **not symmetric** (i.e., the same mapping is used for all destinations).

```c
echosdk_account_config_t cfg = {
    .uri          = "alice@pbx.example.com",
    .password     = "secret",
    .ice_enabled  = true,
    .stun_server  = "stun:stun.l.google.com:19302",
};
```

STUN alone is sufficient for most home/office routers with full-cone or address-restricted NAT.

---

## ICE (Interactive Connectivity Establishment)

ICE tries multiple candidate addresses (host, server-reflexive via STUN, relay via TURN) and picks the best working pair.

```c
echosdk_account_config_t cfg = {
    .uri          = "alice@pbx.example.com",
    .password     = "secret",
    .ice_enabled  = true,
    .stun_server  = "stun:stun.l.google.com:19302",
};
```

ICE is **required** when using DTLS-SRTP (WebRTC interop). Enable it for any scenario where NAT type is unknown.

---

## TURN (guaranteed relay)

TURN relays media through a server when direct connectivity fails (symmetric NAT, corporate firewalls). It always works but adds latency and bandwidth cost.

When both `stun_server` and `turn_server` are set, TURN takes priority as the active ICE server (TURN includes STUN-equivalent discovery). Set only `stun_server` when you don't have a TURN server.

```c
echosdk_account_config_t cfg = {
    .uri          = "alice@pbx.example.com",
    .password     = "secret",
    .ice_enabled  = true,
    .stun_server  = "stun:stun.l.google.com:19302",
    .turn_server  = "turn:turn.example.com:3478",
    .turn_user    = "alice",
    .turn_pass    = "turn_secret",
};
```

### TURN server options

| Server | Transport | Port | Notes |
|---|---|---|---|
| coturn | UDP/TCP | 3478 | Most widely used; open source |
| Twilio TURN | UDP/TLS | 3478/443 | Managed service |
| Metered TURN | UDP/TCP/TLS | various | Managed service |

### coturn quick setup

```
# /etc/turnserver.conf
listening-port=3478
realm=example.com
user=alice:turn_secret
lt-cred-mech
fingerprint
```

---

## Configuration patterns

### Pattern 1: Basic (STUN only, home/office)

```c
.ice_enabled  = true,
.stun_server  = "stun:stun.l.google.com:19302",
```

### Pattern 2: Robust (STUN + TURN fallback)

```c
.ice_enabled  = true,
.stun_server  = "stun:stun.l.google.com:19302",
.turn_server  = "turn:turn.example.com:3478",
.turn_user    = "alice",
.turn_pass    = "turn_secret",
```

### Pattern 3: Enterprise firewall (TURN only, TLS)

```c
.ice_enabled  = true,
.turn_server  = "turn:turn.example.com:443?transport=tcp",
.turn_user    = "alice",
.turn_pass    = "turn_secret",
```

### Pattern 4: WebRTC browser interop (ICE + DTLS-SRTP + TURN)

```c
.media_enc    = ECHOSDK_MEDIA_ENC_DTLS_SRTP,
.ice_enabled  = true,
.stun_server  = "stun:stun.l.google.com:19302",
.turn_server  = "turn:turn.example.com:3478",
.turn_user    = "alice",
.turn_pass    = "turn_secret",
```

## Several servers, relay-only, and credentials that expire

`stun_server` / `turn_server` hold one of each. `ice_servers` is the list form
and what the bindings' `IceServer` / `ice_servers` map to:

```c
echosdk_ice_server_t servers[] = {
    { "stun:stun.l.google.com:19302", NULL, NULL },
    { "stun:stun1.l.google.com:19302", NULL, NULL },
    { "turn:turn.example.com:3478?transport=udp", "user", "pass" },
};
cfg.ice_servers          = servers;
cfg.ice_server_count     = 3;
cfg.ice_transport_policy = ECHOSDK_ICE_TRANSPORT_ALL;
```

Every entry is resolved and asked on every gather — the first offer and the
re-gather of each ICE restart — and the list is read again at each restart,
so `echosdk_account_set_ice_servers()` with fresh TURN credentials reaches the
next handover without touching the call. When both forms are set the list
wins; when only the legacy fields are set, both of them are used (TURN first)
rather than TURN shadowing STUN.

`ECHOSDK_ICE_TRANSPORT_RELAY` is the browser's `iceTransportPolicy: "relay"`:
only relayed candidates are offered or accepted, and a call whose TURN
allocation fails ends at once with `ECHOSDK_ERR_TIMEOUT` instead of carrying
on over a direct path the deployment forbids.

TURN over UDP, TCP and TLS are all gathered from. Put the TCP/TLS entry on
port 443 (`turn:turn.example.com:443?transport=tcp`, `turns:turn.example.com:443`):
it is the only path that still carries audio on networks that block or shape
UDP, and it costs nothing on networks that do not, because a UDP relay or a
direct pair outranks it. STUN over TCP/TLS is not used for gathering.

A `turns:` entry is verified like any other TLS connection: the chain against
`ca_cert_path` (the platform's CA bundle when that is unset — the same one SIP
over TLS uses) and the name against the host in the URL, which is also sent as
SNI, so a TURN server behind a TLS load balancer gets the name it needs to pick
a certificate. Name the host in the URL rather than an address: a `turns:` URL
with an address literal can only have its chain checked, not its identity. A
server whose certificate does not check out is skipped, and gathering carries
on with the rest of the list. Setting `verify_server = false` disables the
check for TURN as well as for SIP.

---

## ICE candidate types

| Type | Source | Signalled in the SDP? | Reliability |
|---|---|---|---|
| Host | Local interface IP | yes | Works on same LAN |
| Server-reflexive (srflx) | STUN response | yes | Works on most NATs |
| Peer-reflexive (prflx) | Discovered during connectivity checks | **no** (RFC 8445 §5.1.3) | Works, but the peer was never told about it |
| Relay | TURN allocation | yes | Always works |

ICE prioritises by type and selects the first working pair. Note the third row:
peer-reflexive candidates are learned *after* the offer/answer, so they are
never signalled. That matters more than it looks — see below.

---

## When the peer drops your media

A peer that filters incoming media against the candidates you signalled will
discard packets from a peer-reflexive address. Asterisk does exactly this:

```
res_rtp_asterisk.c: DTLS packet from <ip:port> dropped.
Source not in ICE active candidate list.
```

The call connects, signalling looks perfect, and neither side hears anything.
With DTLS-SRTP it is total silence rather than degraded audio, because RTP is
gated behind a handshake that never completes.

It hits the **answerer** hardest. Per RFC 5763 the answerer picks
`a=setup:active` and is therefore the side that transmits first — into an
address the peer will not accept.

The SDK re-offers when ICE settles on an address that was never signalled:

```
EchoSDK/ice: selected local candidate 41.33.94.42:62417 was never signalled
             (offered 213.212.207.242:62417) — re-offering so the peer accepts our media
```

**Do not rely on that alone.** A re-INVITE carries the new candidates, but a
peer is only obliged to re-read them on an ICE *restart* — a new ufrag/pwd per
RFC 8445 §9. This re-offer is not one: it keeps the session's credentials, and
against Asterisk it is answered `200 OK` with the candidate list left unchanged.
Treat it as a best effort that helps with cooperative peers, not as NAT
traversal. (The SDK does perform a genuine restart when the *network* changes —
see [network handover](network_handover.md#ice-calls) — because there the
candidates are not merely incomplete, they are unreachable.)

### Carrier-grade NAT: when STUN is not enough

A mobile network may map the *same local port* to a **different public IP per
destination**, and change the mapping over time:

```
srflx:213.212.207.242:62417   ← what the STUN server sees
prflx:41.33.94.42:62417       ← what the PBX sees
```

The reflexive address STUN reports is then simply not the address your PBX
sees, so the candidate you signalled is wrong no matter how promptly you signal
it. Calls appear to work intermittently — whenever the two views happen to
coincide.

**Use TURN.** A relay candidate is allocated up front, signalled in the initial
offer or answer, and does not move, so there is nothing for the peer to reject.
This is the case TURN exists for, and no amount of candidate re-signalling
substitutes for it.

Symptoms that point here rather than at a config mistake:

- media works on Wi-Fi and fails on cellular, or vice versa
- the same account works from one location and not another
- `srflx` and `prflx` in the SDK's ICE dump carry **different** IP addresses
- the peer logs dropped packets from an address that is in neither party's SDP

---

## Troubleshooting

1. **Enable SIP trace** to check the SDP for candidate lines:
   ```
   a=candidate:... srflx ...
   a=candidate:... relay ...
   ```

2. **Check stats** — if `rtt_ms` is very high and `loss_pct` is zero, you're probably on TURN relay.

3. **Firewall rules** — ensure outbound UDP to STUN (port 3478) and TURN (port 3478/443) is allowed.

4. **NAT type test** — use `stun-client` or the STUN binding request/response to determine if your NAT is symmetric.

5. **Compare the two reflexive views.** In the SDK's ICE dump, read the `srflx`
   and `prflx` lines together. Identical addresses mean STUN is telling you the
   truth and STUN alone is enough. Different addresses mean the NAT is
   destination-dependent and only TURN will make media deterministic.

6. **Check whether media is gated on a handshake.** With `media_enc` set to
   DTLS-SRTP, `dtls connect to …` with no following `DTLS-SRTP complete` means
   the handshake is being dropped, not that a codec or a route is wrong. Zero
   `tx`/`rx` counters alongside a healthy dialog is the same signal.
