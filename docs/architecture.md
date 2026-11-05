# How Medut is built

This page is for people working on the SDK itself: what lives where under
`src/`, which thread runs what, and the path a registration and a call take
through the code. For the app's view — the thread model the API promises, the
lifecycle, ABI rules — see [Overview](api/overview.md).

Medut runs its **own SIP and media stack** on
[libre](https://github.com/baresip/re), the portable SIP/RTP/TLS/STUN library.
The public API (`include/medut.h`) and the SDK's policy — registration retry,
network handover, degraded-link adaptation, stats, the event queue — sit on top
of that stack and talk to it through three internal headers. Parts of the
stack are adapted from [baresip](https://github.com/baresip/baresip)
(BSD-3-Clause); [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES) lists the files.

---

## Layers

```
 include/medut.h                public C API (frozen ABI; bindings/* wrap it)
 ───────────────────────────────────────────────────────────────────────────────
 src/api/      the medut_* API and the SDK's policy            bsdk.h
                 │  calls the stack through core/mdt.h
                 │  is told what happened through core/mdt_events.h (bsdk_ev_*)
 ───────────────────────────────────────────────────────────────────────────────
 src/core/     stack lifecycle, local addresses, DNS, logging,   mdt.h
               plug-in registries                                mdt_events.h
                                                                 mdt_mod.h
 src/sip/      SIP stack + transports, user agent, registration, mdt_sip.h
               out-of-dialog requests, calls, MESSAGE,           mdt_call.h
               presence, message waiting
 src/media/    RTP streams, jitter buffer, audio pipeline,       mdt_media.h
               codec / encryption registries, media config
 src/codec/    G.711, Opus
 src/menc/     DTLS-SRTP, SDES-SRTP
 src/ice/      Medut's ICE agent (a media-NAT plug-in)
 src/audio/    audio drivers (null, external), filters (AEC/NS/AGC, gains,
               WebRTC APM, PCM tap), driver selection
 platform/<os>/  OS glue: CA bundles, audio session, the audio driver
                 (sles_vc, pulse, coreaudio, wasapi, audiounit)
 ───────────────────────────────────────────────────────────────────────────────
 libre (third_party/re, patched by cmake/patch-re-sources.cmake) · libopus ·
 OpenSSL
```

| Directory | Prefix | What it holds |
|---|---|---|
| `src/api/` | `medut_*` (public), `bsdk_*` | Every public function; accounts and their retry/keepalive (`account.c`), calls (`call.c`, `transfer.c`), events (`event.c`), network handover (`netmon.c`), degraded-link adaptation (`adapt.c`), stats and MOS (`stats.c`), SRV/DNS (`dns.c`), WebSocket routing fixes (`ws_path.c`), the dispatch bridge to `re_main` (`dispatch.c`, `re_loop.c`), tracing and pcap. `bsdk.h` is the SDK's internal header. |
| `src/core/` | `mdt_*` | `mdt_stack_init()`/`mdt_media_init()`/… and their closes, the usable local addresses (route-aware), the DNS client, logging into the SDK's event queue, the plug-in registries. |
| `src/sip/` | `mdt_*` | `sip_stack.c` — one libre `struct sip` with UDP/TCP/TLS/WS/WSS on every usable address, the instance id, routing of incoming requests; `ua.c` — one user agent per account; `reg.c` — a registration binding; `sipreq.c` — authenticated requests outside a dialog; `call.c` — one call; `call_api.c` — calls as `mdt.h` offers them, and their events; `message.c` — SIP MESSAGE in and out; `presence.c` — presence: watching contacts (BLF), being watched, PUBLISH; `mwi.c` — message waiting (a subscription per registered account, and unsolicited NOTIFYs). |
| `src/media/` | `mdt_*` | `stream.c` (one RTP stream, its media-NAT and encryption), `rtprecv.c` + `jbuf.c` (receive, adaptive jitter buffer), `audio.c` (the TX pipeline and the stream's codecs), `aureceiver.c` (decode, burst concealment, playout buffer), `metric.c`, `rtpstat.c`, the codec and encryption registries, `config.c` (media settings from `medut_config_t`), `media_api.c` (a call's media as `mdt.h` offers it). |

### The three internal interfaces

- **`src/core/mdt.h`** — what the stack offers the SDK: lifecycle, user
  agents (`mdt_ua_*`), calls (`mdt_call_*`), a call's media, audio devices.
  Every function runs on `re_main`.
- **`src/core/mdt_events.h`** — what the stack reports, as typed hooks the SDK
  implements (`bsdk_ev_reg_ok()`, `bsdk_ev_call_closed()`, …), one per fact.
  The policy that turns a fact into a `MEDUT_EV_*` event lives once, in
  `src/api/`. The stack never posts SDK events itself.
- **`src/core/mdt_mod.h`** — the plug-in interface: audio sources and players
  (`mdt_ausrc`/`mdt_auplay`), audio filters (`mdt_aufilt`), media-NATs
  (`mdt_mnat`). The drivers in `src/audio/` and `platform/`, the filters, and
  the ICE agent are plug-ins. `test/compat_contract_test.c` pins the contract:
  allocation and destruction on `re_main`, frame callbacks on the driver's own
  thread, `errh` on `re_main`.

Plug-in sources include `src/core/medut_internal.h`. A name with the `mdt_`
prefix crosses a file boundary; private (static) names have none.

---

## Threads

| Thread | Runs |
|---|---|
| **App threads** | Public API calls. Each one is handed to `re_main` through `bsdk_dispatch()` / `bsdk_dispatch_sync()` (an mqueue, so it never waits behind libre's worker pool). |
| **`re_main`** | libre's event loop: all SIP, all signalling state, timers, RTP receive, the jitter buffer, decoding (a decode timer drains the jitter buffer), the receive-side filters, RTCP, ICE checks, DTLS. Every `mdt.h` function and every `bsdk_ev_*` hook runs here. |
| **Event thread** | Delivers queued `MEDUT_EV_*` events to the app's callback (`src/api/event.c`). |
| **Audio device threads** | Owned by the driver: capture pushes frames into the TX buffer; playback pulls decoded audio from the playout buffer (concealing a gap when it runs dry). |
| **"Audio TX"** | One per call: TX buffer → TX filters (gain, NS, AGC, AEC, tap) → encoder → SRTP → send. |
| **libre workers** | Blocking getaddrinfo and the like. |

---

## A registration

1. `medut_account_create()` → `src/api/account.c` resolves the account
   (config > global config > what the server URL implies) into an
   `mdt_ua_prm` and calls `mdt_ua_alloc()` (`src/sip/ua.c`). The UA keeps its
   own copies: AOR, credentials, outbound route, codec list (references into the
   registry), media encryption, ICE, NAT server.
2. `medut_account_register()` → `mdt_ua_register()`: the Contact carries
   `+sip.instance` (from `<tmp_dir>/.medut/uuid`), the ICE feature tag, and —
   asked for through `bsdk_ua_contact_params()` — the RFC 8599 `pn-*` params.
3. `src/sip/reg.c` creates the libre `sipreg` client. A re-REGISTER hands the
   push binding over to the new client instead of removing it
   (`sipreg_handoff_push()` / `sipreg_release()`, Medut patches to libre).
4. The registrar's answer comes back on `re_main` → `bsdk_ev_reg_ok()` /
   `bsdk_ev_reg_fail(scode, reason)` / `bsdk_ev_reg_unregistered()` →
   `src/api/event.c` decides REGISTERED / RECONNECTING / FAILED and arms the
   retry.

## An outgoing call

1. `medut_call_invite()` → `src/api/call.c` → `mdt_call_connect()`
   (`src/sip/call_api.c`): the dial string is completed into a request URI
   (`*43` → `sip:*43@domain`), the local address is chosen by route
   (`mdt_net_laddr_for()`), and `mdt_call_alloc()` (`src/sip/call.c`) builds
   the call with an SDP session, the ICE session and the encryption session.
2. With ICE, the INVITE waits for gathering; `mdt_call_invite()` sends it via
   libre's `sipsess_connect()`, the offer from `mdt_call_streams_alloc()` →
   `mdt_audio_alloc()` (`src/media/audio.c`) → `mdt_stream_alloc()`.
3. 18x → `MDT_CALL_EVENT_RINGING` / `PROGRESS`; 2xx → the answer is decoded,
   streams are updated (`mdt_stream_update()`), DTLS runs, and once the stream
   is secure the audio starts (`mdt_audio_update()`: encoder, decoder, the
   device). `ESTABLISHED` follows.
4. Each call event goes through `call_event_handler()` in `call_api.c` to the
   SDK's hooks (`bsdk_ev_call_established()`, …). A close becomes a
   `bsdk_close_cause` — SIP status, transport error, a caller's CANCEL — which
   `event.c` turns into ENDED / CANCELLED / FAILED.

## An incoming call

`sip_stack.c` receives the INVITE (`sipsess_listen`), finds the user agent it is
for (the registered Contact user, else the AOR user), checks what it requires
(420 for an unknown option-tag, 421 when 100rel is required and missing, 486
past four calls, a refused RFC 5373 auto-answer), and hands it to
`mdt_call_accept()`. The call answers 180, `MDT_CALL_EVENT_INCOMING` becomes
`bsdk_ev_call_incoming()`, and the app's `medut_call_answer()` sends the 200
through `mdt_call_respond()`.

## The audio path

```
 TX   device ─► ausrc read handler ─► TX aubuf ─► [Audio TX thread]
      filters (mic gain, NS, AGC, AEC, tap) ─► encoder ─► SRTP ─► RTP ─► socket

 RX   socket ─► SRTP ─► rtprecv ─► jitter buffer ─► [decode timer, re_main]
      decoder / FEC / PLC ─► filters (AEC, speaker gain, tap) ─► playout aubuf
      ─► [device thread] auplay write handler ─► device
```

One audio driver serves every call (`src/audio/device.c`): the platform's
(`sles_vc` on Android), `external` while the app owns the device, or `null`
when the build has none. Switching moves the established calls at once
(`mdt_media_device_changed()`).

---

## libre

libre is built from `third_party/re` at a pinned commit and patched in place by
[`cmake/patch-re-sources.cmake`](../cmake/patch-re-sources.cmake): the push
binding hand-off and refresh in `sipreg`, abandoning a stranded re-INVITE,
reporting how a re-INVITE ended (`sipsess_set_modify_handler()`), the
`.invalid` Contact host for WebSocket registrations, and the compile-time
switches that let `src/api/ws_path.c` own `websock_connect()` and
`sip_dialog_route()` (WebSocket path, Origin, dead-route rescue).

`scripts/fetch-third-party.sh` fetches libre, Opus and (for Android and iOS)
OpenSSL at their pinned commits.

Everything — the stack, libre, Opus — is merged into one archive
(`medut.a`) and linked into one shared library that exports only the
public API: `scripts/exports.ver` on Linux and Android (plus Android's JNI
entry points), `-exported_symbol _medut_*` on macOS and iOS, the `.def`
file generated from `include/medut.h` on Windows.  Tests that look inside
the stack link the archive.

---

## Testing

| | |
|---|---|
| `test/unit/` | Pure unit tests; several compile one `src/` file against libre only (the jitter buffer, burst concealment, ICE, WebSocket pinning, adaptation). `make -C test/unit test`. |
| `test/run-integration.sh` | The library end to end, offline: the plug-in contract, registration recovery, push params on the wire, re-init, threading. |
| `test/interop/` | Asterisk and FreeSWITCH in Docker, every transport and media encryption, with `tc netem` sidecars. |
| `tools/ab/` | The same echo call on two builds under the same network impairment, counterbalanced: setup time, time to audio, MOS, loss, jitter, concealment, tone purity. |

---

## Not ported yet

The stack replaces baresip; this part is still to come:

- **The mixer**: local 3-way calls, supervisor barge and whisper.

A REFER outside a dialog (a server asking the phone to place a call) is
answered 501 on purpose: `MEDUT_EV_TRANSFER_REQUEST` names the call that
received the REFER, and there is none. baresip never delivered it either.
