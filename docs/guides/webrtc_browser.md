# WebRTC Browser Interop

## Overview

Medut can act as the SIP side of a WebRTC call, bridging browser-based WebRTC clients to the SIP network. This requires:

1. **WSS transport** — WebSocket over TLS (browsers require secure origins).
2. **DTLS-SRTP** — media encryption mandated by WebRTC.
3. **ICE** — NAT traversal with STUN/TURN candidates.

---

## Architecture

```
Browser (WebRTC)                Medut (SIP)
     │                              │
     │  WSS + SIP over WebSocket    │
     │◄────────────────────────────►│
     │                              │
     │  DTLS-SRTP (media)           │
     │◄────────────────────────────►│
     │                              │────────► SIP network (UDP/TCP/TLS)
```

The SIP server (Asterisk, Kamailio, OpenSIPS) terminates the WebSocket on the WSS side and bridges to the SIP network.

---

## Medut configuration

```c
medut_account_config_t cfg = {
    .uri          = "alice@pbx.example.com",
    .password     = "secret",
    .server_url   = "wss://pbx.example.com:443/ws",
    .media_enc    = MEDUT_MEDIA_ENC_DTLS_SRTP,
    .ice_enabled  = true,
    .stun_server  = "stun:stun.l.google.com:19302",
    .turn_server  = "turn:turn.example.com:3478",
    .turn_user    = "turnuser",
    .turn_pass    = "turnpass",
};
```

### Required fields for WebRTC

| Field | Value | Why |
|---|---|---|
| `server_url` | `wss://...` | Browsers cannot use raw UDP/TCP SIP |
| `media_enc` | `MEDUT_MEDIA_ENC_DTLS_SRTP` | WebRTC mandates SRTP with DTLS key exchange |
| `ice_enabled` | `true` | WebRTC uses ICE for NAT traversal |
| `stun_server` | STUN URI | Needed for server-reflexive candidates |
| `turn_server` | TURN URI (recommended) | Fallback relay for restrictive NATs |

---

## Server setup

### Asterisk

`http.conf`:
```ini
[general]
enabled=yes
bindaddr=0.0.0.0
bindport=8089
tlsenable=yes
tlscertfile=/etc/asterisk/keys/asterisk.pem
tlsprivatekey=/etc/asterisk/keys/asterisk.key
```

`sip.conf`:
```ini
[alice]
type=friend
host=dynamic
transport=wss
dtlsenable=yes
dtlsautoarrange=yes
icesupport=yes
```

### Kamailio

```
loadmodule "websocket.so"
loadmodule "tls.so"

listen=tls:0.0.0.0:443

# WebSocket route
if (is_method("GET") && $hdr(Upgrade) =~ "websocket") {
    websocket_handle();
}
```

---

## Browser-side (JavaScript)

Use a SIP-over-WebSocket library such as **SIP.js** or **JsSIP**:

```javascript
const ua = new SIP.Web.SimpleUser({
    uri: 'alice@pbx.example.com',
    authorizationUsername: 'alice',
    authorizationPassword: 'secret',
    transportOptions: {
        server: 'wss://pbx.example.com:443/ws'
    }
});
```

The browser handles ICE, DTLS-SRTP, and media natively via `RTCPeerConnection`.

---

## DTLS fingerprint

Medut generates a DTLS fingerprint and includes it in the SDP (`a=fingerprint:sha-256 ...`). The remote side (browser) verifies this fingerprint during the DTLS handshake. No certificate configuration is needed on the Medut side — a self-signed certificate is generated automatically at startup.

---

## Codec considerations

WebRTC browsers support **Opus** and **PCMU/PCMA**. Set Opus as the preferred codec:

```c
medut_config_t gcfg;
medut_config_init(&gcfg);
gcfg.audio_codecs[0]  = MEDUT_CODEC_OPUS;
gcfg.audio_codecs[1]  = MEDUT_CODEC_PCMU;
gcfg.audio_codec_count = 2;
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| SDP negotiation fails | Codec mismatch | Ensure both sides offer Opus or PCMU |
| No media / ICE fails | TURN not configured | Add TURN server; check firewall allows UDP 3478 |
| DTLS handshake fails | Fingerprint mismatch | Check `trace_sdp_diff` for fingerprint in SDP |
| WSS connection refused | Server not configured for WSS | Verify server has TLS + WebSocket enabled |
| Browser security error | Mixed content (WS on HTTPS page) | Use `wss://`, not `ws://` |

### Debug checklist

1. Enable `trace_sdp_diff = true` and check SDP for:
   - `a=setup:actpass` (DTLS role)
   - `a=fingerprint:sha-256 ...` (DTLS fingerprint)
   - `a=candidate:...` (ICE candidates)
2. Enable `trace_sip = true` to verify SIP signaling over WSS.
3. Use `stats_interval_ms = 3000` to monitor if media packets are flowing.
