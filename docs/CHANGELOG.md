# Changelog

All notable changes to Medut are documented here.

---

## [2.0.0] — 2026-11-05

Medut now runs on its own SIP and media stack on libre, and baresip is no
longer part of the build. The C ABI in `include/medut.h` is unchanged, so
existing C, C++, Python, Kotlin, Swift and Flutter callers keep compiling,
but some behaviour an app can observe has changed and is described in the
sections below: a CANCELled incoming call ends as CANCELLED, an explicit
unregister reports UNREGISTERED, a hold we place ourselves reports HELD, and
the shared library now exports only the public `medut_*` API. Apps that
linked against stack, libre or libopus symbols through `libmedut` must link
those libraries themselves.

### SDES-SRTP calls work with RFC-strict servers

- **`MEDUT_MEDIA_ENC_SDES` now offers `RTP/SAVP`.** It used to map to
  baresip's best-effort `srtp`, which puts the `a=crypto` lines on a plain
  `RTP/AVP` media line so that a peer without SRTP can fall back to RTP.
  FreeSWITCH refuses that outright — 488 "a=crypto in RTP/AVP, refer to
  rfc3711" — so SDES accounts could not call it at all (local FreeSWITCH 1.11
  and a production FreeSWITCH both). It is now `srtp-mand`: RTP/SAVP, as RFC
  3711 / RFC 4568 have it. Asterisk worked either way and still does.
- **Behaviour change:** a peer that does not support SRTP now answers 488
  instead of silently getting an unencrypted call — which is what asking for
  SDES should mean. Use `MEDUT_MEDIA_ENC_NONE` for peers without SRTP.

### Medut's own stack on libre replaces baresip

- **baresip is removed.** The SDK runs on its own SIP and media stack on
  libre, under `src/`, named `mdt_*`. Code adapted from baresip (BSD) keeps
  its copyright in each file header and in `THIRD_PARTY_NOTICES`.
  `include/medut.h` (the ABI) and the bindings are unchanged.
- **What it covers:**
  - the SIP layer — UDP/TCP/TLS/WS/WSS transports, TLS contexts, the user
    agent, registration (RFC 5626 outbound, RFC 8599 push params and binding
    hand-off), authenticated OPTIONS.  Its REGISTER is the same on the wire
    as before;
  - calls (`src/sip/call.c`) and media (`src/media/`): RTP streams, the
    adaptive jitter buffer, burst concealment, the audio pipeline, G.711 and
    Opus (`src/codec/`), DTLS-SRTP and SDES-SRTP (`src/menc/`);
  - the services outside a call (`src/sip/message.c`, `presence.c`,
    `mwi.c`): SIP MESSAGE, presence — watching (BLF), being watched,
    PUBLISH — and message waiting;
  - the audio drivers: PulseAudio (Linux, keeping the capture-hole counter),
    Core Audio (macOS), WASAPI (Windows) and audiounit (iOS), next to
    Android's sles_vc.
  A REFER outside a dialog is answered 501 (it names no call to transfer).
  The conference mixer (3-way, barge, whisper) is not part of this release.
- **The shared library exports only the public API.**  Linux and Android
  exported all ~2,250 symbols of the stack, libre and libopus — an app
  linking its own libre or libopus got whichever copy the loader found
  first.  Now only the `medut_*` functions (and Android's JNI entry
  points) are exported (`scripts/exports.ver`; `-exported_symbol` on macOS
  and iOS; Windows already used its `.def` file).  The Linux library is
  ~9 % smaller.
- `src/` is one directory per layer: `api/` (the public API and the SDK's
  policy), `core/`, `sip/`, `media/`, `codec/`, `menc/`, `audio/`, `ice/`.
  [docs/architecture.md](architecture.md) describes them, the threads and
  the path of a registration and a call.
- Third-party sources (re, opus, openssl, mbedtls) are fetched from the
  `github.com/Ibrahimgamal99` forks at the same pinned commits.
- libre patch V: `sipsess_set_modify_handler()` reports how a re-INVITE /
  UPDATE ended (2xx, refusal or timeout), for rolling back a refused hold.
- New test tooling: `test/compat_contract_test.c` (the plug-in contract),
  `test/run-integration.sh`, a local Asterisk + FreeSWITCH interop target
  (`test/interop/`) and an A/B harness comparing builds under network
  impairment (`tools/ab/`).
- **Behaviour changes:**
  - **Unregistering ends in `MEDUT_REG_UNREGISTERED`.** The registrar's 2xx
    to the un-REGISTER used to be reported as `MEDUT_REG_REGISTERED`, so an
    app showed "registered" for an account it had just unregistered.
  - **`Supported:` lists `ice` and `100rel` when the account has them on.**
    The list used to be fixed before the SDK applied the account settings, so
    neither ever appeared: `medut_account_set_100rel()` never got PRACK
    negotiated, and an incoming INVITE with `Require: 100rel` was refused
    with 420 even with 100rel enabled.
  - **`dscp_sip`, `dscp_rtp` and `medut_call_set_dscp_rtp()` take a DSCP
    value**, shifted into the TOS byte.  24 (AF31) used to be written as TOS
    24, which is DSCP 6.
  - **A caller's CANCEL ends the incoming call as `MEDUT_CALL_CANCELLED`**,
    with the CANCEL's Reason (RFC 3326, e.g. "Call completed elsewhere") when
    it has one.  It used to arrive as FAILED with "Connection reset by peer"
    — the error a dead socket gives — so CallKit / Telecom could not show it
    as missed.
  - The X-RTP-Stat header of the BYE names the reporter `EX=Medut`, and
    the self-signed DTLS certificate's CN is `dtls@medut`.
  - **BLF works.** `medut_account_subscribe_presence()` used to send no
    SUBSCRIBE at all (the SDK had taken over the handler baresip's presence
    module needed) and reported one `UNKNOWN` with no account.  It now
    subscribes from that account and reports the PBX's view of the contact
    — `OPEN`, `BUSY` on a call, `CLOSED` — with the watching account in the
    event.  Checked against Asterisk hints and FreeSWITCH.
  - **`medut_account_publish_presence()` publishes.** It used to only store
    the status (baresip's publish interval was 0).  It now sends a PUBLISH
    (RFC 3903, refreshed by entity tag) and NOTIFYs the account's own
    watchers; `UNKNOWN` withdraws it.  A server without a presence agent
    answers 489; that is logged, not retried.
  - **Watchers get valid presence.** A SUBSCRIBE to the account's presence
    is answered with `<basic>open|closed</basic>` (an rpid `busy` activity
    and a note), instead of `<basic>?</basic>`.  A SUBSCRIBE for another
    event package gets 489 Bad Event with `Allow-Events: presence` (it was
    400 "Bad Presence").
  - **Unsolicited MWI is heard.** A message-summary NOTIFY outside a
    subscription — Asterisk's only kind when the endpoint has `mailboxes=`,
    which refuses the SUBSCRIBE — raises `MEDUT_EV_MWI`.  It used to be
    answered 481 and dropped, so MWI never worked against such a PBX.
  - **MESSAGE and presence targets are written like a dial string:** `"bob"`,
    `"bob@host"`, a URI or `"Bob <sip:bob@host>"`.  Anything but a URI or a
    name-addr used to fail with EINVAL — including the `"bob@host"` form the
    documentation shows.
  - A REFER outside a dialog is answered 501.  It used to be challenged with
    401 (with `Refer-Sub: false`) or answered 501, and never reached the app
    either way.

### Hold: the call state follows both sides

All of these were the same on the baresip build.

- **Our own hold is reported.** `medut_call_hold()` used to change no
  state: `MEDUT_CALL_HELD` came only when the *peer* held us.  Android
  Telecom confirms a hold it asked for (`onHold()`) when HELD arrives, so a
  call the system held — for a second call, say — stayed "active" there.
  Now the state is `HELD` once the peer has accepted our hold, or while the
  peer holds us, and `ESTABLISHED` again when neither does; a hold on top of
  the peer's (or the other way round) changes nothing until both are gone.
- **A refused hold is undone.** A peer answering the hold re-INVITE with,
  say, 488 left the SDK believing the call held — `medut_call_is_held()`
  true, the receiver stopped — while the peer still sent media; the next
  hold was then a no-op.  The hold is now rolled back (libre patch V reports
  the outcome): media direction, receiver and `is_held()`, with the state
  never leaving `ESTABLISHED`.
- **A hold or resume asked for during another re-INVITE is sent.** It used
  to be applied locally and never offered when an offer was already in
  flight; it now goes out when that one ends.
- **`medut_call_info_t.is_remote_hold` is the peer's hold.**  It reported
  our own hold.
- **Dial strings with URI parameters reach the right user.**  `"600;transport=tcp"`
  and `"601;user=phone"` were completed as `sip:600;transport=tcp@domain`,
  making the parameters part of the user (Asterisk: 404).  The domain now
  goes before them; MESSAGE and presence targets are completed the same way.

### Audio at answer, negotiation events, MOS after a reset

All of these were the same on the baresip build.

- **WebRTC calls to Asterisk have audio ~80 ms after answer, not ~1.1 s.**
  Answering our offer with `setup:active`, Asterisk is the DTLS client: it
  sends its ClientHello the moment our nominated ICE check reaches it, a
  millisecond or two before our side sees that check succeed and starts
  DTLS.  The ClientHello was dropped as stray RTP and the handshake waited
  for its retransmission one second later.  The ICE layer now keeps the few
  DTLS records that come before media starts and hands them to DTLS when it
  does (`src/ice/ice_mod.c`).  WS and WSS alike; FreeSWITCH, plain RTP, SDES
  and ICE over TCP unchanged.
- **`MEDUT_EV_SDP_NEGOTIATION` names the codec.**  On an outgoing call the
  first event came before the answer was applied and had a NULL codec; an
  incoming call had no event for its first negotiation at all (the INVITE's
  offer came before the SDK had the call), only for later re-INVITEs.
- **A jitter-buffer reset is not loss.**  A flush (call start, hold,
  resume, a new SSRC) zeroes the buffer's counters, and the MOS window then
  saw "frames played: 0" against a discard — `mos_lq_rx` 1.81 on the first
  tick after a resume, ~3.9 on a call's first tick.  Flushes no longer count
  as discards, and a window with one counts from the flush on.  Late and
  overflow discards still lower the MOS as before.
- `MEDUT_EV_MEDIA_STATS` pauses while a call is `HELD` — now also for our
  own hold, as it always did for the peer's.

### Call recovery when the signalling connection dies mid-call

- **A stall repair no longer waits on a dead WebSocket.** When its re-INVITE
  has no answer for 4 s on TCP/TLS/WS, netmon checks the connection (one
  OPTIONS); if that goes unanswered too it hands over: transport flush,
  re-REGISTER, and the re-INVITE re-sent on the fresh connection. Before, the
  request sat in the dead connection, later repair rounds deferred behind it,
  and libre's Timer B ended the call 32 s later (src/netmon.c `offer_wait()`).
- **Calls follow a WebSocket that reconnects on its own.** When a WS/WSS
  registration fails because its connection closed and the retry registers
  on a new one (no handover), each call of the account is re-INVITEd so its
  dialog moves too. Before, the PBX's in-dialog requests went to the closed
  connection: a remote BYE 9 s after a reset never arrived, and the call was
  only found dead by a later 481 (`bsdk_netmon_account_reconnected()`).
- **libre hunk U:** an offer that ends without an answer (transport error, a
  2xx without SDP) is withdrawn instead of leaving the session unable to
  re-INVITE again; `sipsess_reinvite_abandon()` drops a re-INVITE stranded on
  a connection a transport reset closed (baresip job 12 exposes it as
  `bsdk_call_reinvite_abandon()`).
- **No false "media-stall recovered".** The alert stays raised through a
  repair and a DTLS re-key and clears only when packets arrive; a call still
  silent after a repair asks netmon for its next round directly (src/adapt.c).
- **A stall repair ends when media comes back while it waits.** A repair
  held behind the late answer to its own re-INVITE used to send a second one
  once that answer landed, although audio was already flowing again — a
  pointless re-key and a second "Reconnecting…" in the app.
- **ICE: the answer to our own restart is no longer misread as the peer
  restarting.** baresip's `call_modify()` applies the SDP right after sending
  an offer, while the remote SDP is still the previous answer; that cleared
  the agent's "awaiting the answer to our restart" flag, so the real answer
  (with Asterisk's new credentials) read as a peer restart and turned our side
  CONTROLLED against a CONTROLLED answerer. Nobody nominated and the PBX
  stopped taking our audio a few seconds later — 4 of 11 network down/up runs
  before, 0 of 5 after. Regression test `test_restart_answer_after_local_apply`
  (with `bsdk_ice_call_role()`).
- A stall repair also ends when media returns while it waits for an address
  after the network dropped (not only while it waits for an answer); "the far
  end has gone quiet" is logged once per silence instead of every period.
- **Local hangups read "Local hangup"** in the call's end reason; only a ring
  turned away keeps "Rejected by user" (baresip job 12). A migration failure
  that hangs up says "Network migration failed".
- Log fixes: "ICE failed under a repair" no longer promises a restart that an
  unanswered offer is blocking; running out of repair rounds is logged once at
  info level; Kotlin Telecom disconnect/abort/reject requests are logged.

### Incoming calls ring at once; STUN/TURN addresses are cached

- **An incoming call rings as soon as its INVITE arrives.**  With ICE on,
  the stack reported the call only once ICE had gathered its candidates —
  which waits for every STUN/TURN server to be resolved and to answer.  On a
  network whose first nameserver was slow, the phone rang 1.7 s after the
  caller already heard ringback.  `MEDUT_EV_INCOMING_CALL` now fires at
  once; an answer given before gathering is done is sent the moment it is
  (the 200 OK needs the candidates).
- **STUN/TURN server addresses are resolved ahead of calls.**  Every call
  looked its servers up afresh (an SRV query, then A/AAAA) before it could
  gather.  The addresses are now resolved when an account with ICE
  registers — the first REGISTER and every refresh — and kept for the
  process; a call uses them at once and never waits on DNS.  An address
  older than 5 minutes is looked up again in the background, and one that
  cannot be refreshed is used for up to 24 hours (`src/ice/ice_mod.c`).
- Measured against the local Asterisk with ICE: ring 1–2 ms after the INVITE
  (2,001 ms with an unanswering STUN server, 55 ms with a reachable one);
  the first call already uses a cached STUN address.

### DNS: every nameserver asked at once

- **A slow first nameserver no longer delays lookups.**  libre's resolver
  asked the first server and moved to the next only after 500 ms, so a slow
  or dead first server delayed every SRV / A query — registration, a SIP
  server, a STUN server.  Each round now goes to every server and the first
  positive answer wins (libre patch W).  A negative answer — NXDOMAIN, no
  records, SERVFAIL — counts in the first round only from the first-listed
  server, so a name only the local resolver knows (split-horizon DNS, an
  internal PBX) is not lost to a public resolver's quicker "no such name";
  from the second round any answer counts.  A dead first server with a
  nonexistent name answers in ~0.5 s; both dead still time out (5.5 s, was
  up to 11 s with two servers).
- Platforms using libre's resolver: Linux, macOS, Windows, iOS.  On Android
  the stack resolves through the system's getaddrinfo() (Android exposes no
  nameservers to native code), which this does not change; the STUN/TURN
  address cache above takes those lookups out of the call path there.
- `test/unit/test_dns_parallel.c` pins it with two fake nameservers: slow
  first server (22 ms, was 500+), split horizon, first server's NXDOMAIN,
  SERVFAIL, dead server, one server, both dead.

### Registration after a network change; shutdown frees the SIP stack

- **A network change no longer leaves a dead Contact at the registrar.**
  Every handover re-creates an account's registration client and frees the
  old one, which un-REGISTERed its binding — but libre builds a REGISTER's
  Contact from the socket it goes out on, and the handover had just replaced
  the sockets, so the un-REGISTER named the *new* address (`expires=0` for
  the binding about to be made, sent just before it) and the old binding
  stayed until it expired.  Over UDP/TCP/TLS each handover added one: a soak
  run against Asterisk reached five per account, and `Dial()` picked one of
  them — about half the calls after the first few handovers rang a closed
  port.  The un-REGISTER also raced the new REGISTER for the same Contact;
  in the other order it deleted the live binding.  Now the new client takes
  over the old one's binding (libre patch Y, `sipreg_takeover()`): the old
  client sends nothing, and the new REGISTER carries the old Contact with
  `expires=0` next to its own, until a 2xx — one request adds the new
  binding and removes the old (RFC 3261 s10.2.2, as pjsip does).  A retry
  without a handover (same Contact) sends one Contact, refreshing it in
  place.  A new push token removes the old token's binding the same way.
  When the first REGISTER after a handover fails (the new network not
  passing traffic yet, a timeout, a 5xx), the client that retries carries
  the removal on, so the old network's binding still goes once one succeeds
  — reproduced with the route to the PBX blackholed for 4 s after an address
  change: two bindings left before, one after, on UDP, TCP and TLS.
  WebSocket accounts (one instance Contact on every network) and a push
  binding carried on unchanged behave as before.  Behind a NAT the old
  binding is keyed on a public address the client does not know, so it still
  expires on its own.  Checked on the wire against Asterisk 22 with
  `rewrite_contact=yes`; `test/unit/test_sipreg_takeover.c` pins it with a
  fake registrar.
- **`medut_shutdown()` waits for the un-REGISTERs, then frees the SIP
  stack.**  Freeing each account sends its un-REGISTER, and the loop was
  stopped right after: an un-REGISTER challenged with 401 never sent its
  authenticated retry (the binding stayed at the registrar, which went on
  routing calls to a device that had shut down), and every request still out
  held the SIP stack, so the stack, its sockets, TLS contexts and DNS client
  stayed allocated for the life of the process — about 260 libre blocks and
  every SIP socket per init/shutdown cycle.  Shutdown now lets those requests
  finish (up to 2 s; 12–60 ms against a LAN PBX), aborts what is left, and
  the stack is freed: 0 libre blocks after shutdown in a debug build.

### Calls keep their Contact through a handover

- **A remote hangup during or after a handover reaches the call.**  A
  handover re-creates every SIP transport, each on a new port (unless
  `local_port` is fixed), so a call's Contact names a socket that is gone.
  A call whose media address did not move — a second address appeared (a
  VPN, a hotspot) or went — was not re-INVITEd ("same path"), on the
  reasoning that address-routed transports need none; only WebSocket calls
  were.  The PBX's in-dialog requests then went nowhere: against Asterisk a
  remote BYE took 5.8 s to end the call (found by the media-stall
  watchdog), and a PBX hold or transfer NOTIFY was simply lost.  Every call
  is now re-INVITEd after a transport reset that moved its Contact (no ICE
  restart, the same media): the BYE ends the call in 80–110 ms on UDP, TCP
  and TLS.  This was also behind the soak run's stray dead calls and its
  "486 Max Calls" when handovers landed mid-call; 150 iterations with
  mid-call handovers now run clean.
- Measured with a real address change (the old address removed, as on a
  Wi-Fi ↔ mobile switch): a PBX hangup in the handover's settle window ends
  the call when its re-INVITE goes out (≈1.2 s, bounded by `net_settle_ms`);
  one after it, in 60–80 ms.

### One reconnect per network switch over WebSocket

- **A late network report no longer reconnects an account that already has.**
  When the network goes, the OS destroys the WebSocket, the registration
  fails and its own retry reconnects over the new network — on Android
  usually before the OS reports the change (A54, Wi-Fi off mid-call:
  re-registered over mobile data at +2.7 s, report at +4.4 s).  The handover
  that report started then marked the account RECONNECTING and flushed the
  brand-new connection to open another: a second WebSocket and REGISTER
  1.6 s after the first, for one switch.  Now, when every account is a WS/WSS
  one that reconnected on its own within the last 30 s, over a connection
  whose local address is still the device's, the handover keeps those
  connections: no RECONNECTING, no transport reset, no re-REGISTER.  Calls
  are still migrated (re-INVITE, ICE restart when the path moved).  An
  earlier report, a TCP/TLS/UDP account, or an old connection that merely
  survived (a VPN appearing) keeps the full handover.  The registration now
  records the real local address of the connection it was answered on; for
  WS/WSS the REGISTER only names the transport's listen address.
- Reproduced locally (WSS, the container's address replaced and its sockets
  destroyed as Android does, the report 0.2 or 1.5 s after the account's own
  reconnect): one RECONNECTING and one transport reset before, none after;
  with a call up, audio continued through it and a PBX hangup ended the call
  in 89 ms.

### Logging: the host application's stderr is its own again

- **`medut_init()` no longer sends the process's stderr to /dev/null.**
  It did, to hide a few libre messages printed straight to stderr — and with
  them everything the host application itself wrote to stderr for the rest
  of the process (a desktop app's own logging, a Python or Flutter host's
  tracebacks).  Those libre paths (SIP transport: unhandled or undecodable
  messages, WebSocket connect / close / send failures; mqueue errors) now
  go through libre's dbg instead, which the SDK already delivers as
  `MEDUT_EV_LOG` — warnings for what was stderr, info for what was stdout
  (libre patch Z).  They used to reach stdout too: a WebSocket account
  printed "websock: connecting to ..." on the host's stdout on every
  connect.  Checked: a malformed datagram to the SIP port now arrives as a
  log event ("sip: msg decode err"), and nothing reaches stdout or stderr.

### User-Agent

- **The default `User-Agent` / `Server` header is `Medut/<version>`**
  (now `Medut/2.0.0`), from the header's version macros; it said
  `Medut/1.0` regardless of the release.  An empty `user_agent` also gets
  the default.  `medut_config_t.user_agent` set by the app is sent as is.

### Build: libre changes reach the shipped archive; debug builds can place calls

- **A change to libre alone is merged into `medut.a` in the same build.**
  The merge step depended on libre's archive as a build byproduct, but the
  copy it merges is written by the *install* step, after ninja had already
  judged the merge up to date — so after a patch in
  `cmake/patch-re-sources.cmake` (and nothing else) an incremental build
  shipped the old libre.  Linux hid it (the build script reconfigures, which
  rewrites the merge script); the Apple and Windows merge rules did not.
  The merge now also depends on each external project's install stamp.
- **libre patch X: the thread that runs `re_main()` owns the context.**
  libre records its owner thread in `libre_init()`, which the SDK calls on
  the app's thread before running the loop on its own thread; a debug build
  then refused every socket the loop thread opened (each call's RTP sockets)
  as coming from a foreign thread, so it could not place a call.  Release
  builds compile that check out and are unchanged.  With it, a debug build's
  thread check also proves the point: over a soak run it reported no socket
  opened off the loop thread.

### CI

- **"Build mobile libraries" builds only the libraries** — the Android
  `libmedut.so` (NDK 28.2.13676358, as built locally) and the iOS
  `Medut.xcframework` — and commits both to the branch it ran on (input
  `commit`, default on).  The example-app and Dart checks it also ran moved,
  unchanged, to the new "Flutter checks" workflow.
- **One workflow builds the libraries, one checks the apps.**  "iOS
  xcframework" is gone: it rebuilt the xcframework that "Build mobile
  libraries" already builds and commits (~8 min of macOS).  Its iOS checks —
  compiling MedutKit and building the example app through the pod — are a
  job of "Flutter checks" now, against the committed framework as the
  Android job is against the committed jniLibs.  `scripts/fetch-prebuilt.sh`
  takes a "Build mobile libraries" run's artifacts (the xcframework zip, and
  now the Android jniLibs too) for a run that did not commit.

### Fixed

- **Kotlin push wait could deadlock with shutdown.** `MedutPush.wake` matched
  the incoming call on the SDK event thread through the stack gate, which
  `Medut.shutdown()` holds while it joins that thread. The listener now only
  queues the call and the push thread does the matching.
- **Kotlin: a throwing listener no longer crashes the app** or stops the
  other listeners; `Medut.publish` catches and logs it.
- **Kotlin: ended calls are no longer resurrected.** A stats, ICE, quality or
  network event after a call's terminal state re-created its wrapper in
  `Medut.calls` forever; only an incoming call or a live call state creates
  one now.
- **Kotlin account store** survives Keystore and crypto failures (logged, key
  recreated), and lives in `noBackupFilesDir` (moved there on first use).
- **Kotlin incoming-call notification** checks the notification permission
  and, on API 34+, `canUseFullScreenIntent()`, and builds a `CallStyle` only
  with a full-screen intent (it threw without one); otherwise a plain
  high-priority notification with the same actions. New
  `MedutAndroid.notificationsEnabled()` / `fullScreenIntentAllowed()`.
- **Kotlin: a call Telecom refuses** (`SecurityException`) gets the automatic
  audio mode, focus and routing instead of none; a Decline with no ring or
  call clears the stale notification.
- **Kotlin audio route listeners survive** `MedutAndroid.shutdown()` /
  `start()` (`MedutAndroid.addAudioRouteListener`).
- **iOS: every VoIP push is reported to CallKit**, also when the stack or the
  CallKit integration is not running (iOS ends apps that skip it).
- **iOS push placeholders bind by SIP Call-ID** (`call_id`), oldest first only
  as a fallback; **cancel pushes** (`type: cancel`) end the matching ring.
- **iOS push wake-up refreshes the registration in place**
  (`Account.refreshRegistration()`) instead of re-registering, which
  un-REGISTERed the binding the server was holding the INVITE for.
- **iOS:** `MedutPush.clearPushToken()` (an empty token clears too); a CallKit
  answer no longer waits forever for audio activation (3 s fallback); the
  audio category is re-asserted before CallKit reports; a call CallKit
  refuses is not reported as ended locally; callback properties are
  lock-protected; SIP log lines are private and at debug level; interruptions
  honour `shouldResume`; account store keys keep the port.
- **Flutter:** the audio routes stream survives `shutdown()` then `start()`;
  an event missing its call or stats arrives as `UnknownEvent` instead of
  stopping the stream; `AudioSessionMode` crosses as explicit raw values;
  `logLevel` is clamped to what the native SDKs accept; a second `start()`
  with a different config warns; destroying an account twice is a no-op; the
  call finalizer is quiet after `detach()`.
- **The DTLS patch marker is versioned** (`medut-patch dtls-recovery v2`), so
  a third-party tree patched by an older version is restored and re-patched
  instead of silently keeping the old hunk.

- **RFC 8599 push params now reach the wire.** `pn-provider` / `pn-prid` /
  `pn-param` were built from the account config but never put in the
  REGISTER Contact, so no server had a token to push to (baresip job 11).
- **`medut_account_set_push_token()`** applies the new token: a REGISTER with
  it goes out at once, or once an in-flight one is answered, and over UDP,
  TCP and TLS the old token's binding is removed. A token too long for the
  Contact is refused and the old one stays. An account the app has not
  registered is no longer registered by it.
- **A push binding survives a re-created registration client** (handover,
  retry): the old client no longer un-REGISTERs a binding its successor
  carries on, which an RFC 8599 server saw as the device leaving (libre
  hunk R).
- **Push wake-up refreshes the registration** instead of sending an OPTIONS
  probe (Kotlin, Swift). RFC 8599 servers release a held INVITE only on the
  binding refresh.
- **A refreshed push token is saved** with the remembered accounts (Kotlin,
  Swift, Flutter), so a process a later push starts registers with it.
- **Kotlin in minified builds:** `MedutAndroid.start` failed with
  `NoSuchFieldError` once R8 renamed the enum and config types the JNI reads
  fields by. The consumer rules keep them.
- **Kotlin mute:** a Telecom route change no longer un-mutes a call the app
  muted itself.
- **Kotlin notification Answer** opens an activity directly, as Android 12+
  blocks one started from a notification's broadcast receiver; the ringtone
  loops (`FLAG_INSISTENT`).
- **Flutter plugin** forwards events and sets `onAppOwnedAudioError` only
  after Dart's `start()`, and chains to the host app's handler.

### Changed

- **Mobile architecture.** The Kotlin (`bindings/kotlin`) and Swift
  (`bindings/swift`) SDKs own the stack for the life of the process; the
  Flutter plugin is a Pigeon layer over them on Android and iOS; on Linux,
  macOS and Windows the same Dart API drives the C library over `dart:ffi`.
  Every Dart action is async and throws `MedutException`.
- **DTLS-SRTP recovery.** As the DTLS server, a handshake that is under way
  is no longer reset at the deadline; only a peer that stays silent reaches
  the client-role fallback. Fixes silent inbound calls against FreePBX over
  WSS, where the ClientHello landed just before the first deadline.
- **pcap** is documented as SIP-only; RTP, STUN and DTLS were never written
  to the file.

### Added

- **`LogLevel.OFF` (-1) in the Kotlin and Swift SDKs**, so a mobile app can
  turn log events off entirely, as `medut_config_t.log_level` below 0 already
  did in C. The Flutter plugin passes a negative `logLevel` through as -1 on
  Android and iOS instead of raising it to 0.
- **`refresh_registration()`** in the C++ (`Account::refresh_registration`)
  and Python (`Account.refresh_registration`) bindings.
- **Flutter:** `refreshRegistration`, `setReliableProvisional`, static
  `pushWake` / `pushNewToken`, `strerror`, `MedutConfig.copyWith`, rejected
  codecs and warnings on SDP negotiation events, desktop over `dart:ffi`. See
  `bindings/flutter/CHANGELOG.md`.
- **`medut_account_refresh_registration()`** (Kotlin
  `Account.refreshRegistration()`): re-sends the REGISTER from the client
  holding the binding (same Call-ID and Contact), as a push wake-up needs.
- **Push payload contract** (`docs/guides/push.md`): `type` (call / cancel),
  `call_id`, `caller_uri`, `caller_name`, `aor`, with configurable names.
  Kotlin: `MedutPush.wake(context, data)` rings through Telecom as soon as a
  call push arrives and binds the INVITE to the ring by Call-ID (an Answer
  tapped before it is carried out, a Decline rejects it); a cancel push or
  no INVITE in time ends the ring as missed. `MedutPush.onNewToken`,
  `PushPayload.parse`, `MedutTelecom.onShowPushIncomingCallUi`,
  `AccountStore.updatePushToken`.
- **Push guide** with the server contract and an OpenSIPS configuration.
- **Incoming-call filter** (`MedutTelecom.incomingCallFilter`,
  `MedutCallKit.incomingCallFilter`): reject before the system rings.
- **System-side state callbacks:** `MedutTelecom.onCallAudioStateChanged`;
  `MedutCallKit.onCallMutedChanged`, `onCallHeldChanged`.
- `MedutCallKit.update(options:)`; `MedutTelecom.answerPushRing` /
  `declinePushRing` and the ring key on `onShowPushIncomingCallUi`;
  `TelecomOptions.callActivity`; public `EXTRA_CALL_ID` / `ACTION_ANSWER` /
  `ACTION_DECLINE`.
- `AccountConfig.headers` / `registerHeaders`, saved by the account stores.
- **`endedLocally` on the call-state event** (Kotlin
  `MedutEvent.CallStateChanged`, Swift `CallStateEvent`, Flutter
  `CallStateEvent`): true when this side ended the call, including a Decline
  from the system call UI, a Bluetooth headset or a watch. An incoming call
  that ends unanswered with it set was declined, not missed.
- `MedutAndroid.setStartOnBoot()`: opt-in registration after a reboot.

- **Kotlin SDK:** Telecom integration through a self-managed
  ConnectionService (system call UI, Telecom-owned audio and routes, a
  ringing call notification with Answer / Decline), `MedutPush.wake()` for
  FCM, a Keystore-encrypted account store restored at start, the media tap,
  the cellular network request for ICE multipath, logcat output.
- **Swift SDK:** CallKit integration through the SDK's own provider,
  `MedutPush.wake(payload:)` for PushKit (reports the call to CallKit before
  the INVITE arrives), a Keychain account store restored at start, unified
  log output.
- **Flutter:** `Medut.start(telecom:)`, `createAccount(remember:)`,
  `Account.destroy(forget:)`, `forgetAccount()`, `rememberedAccounts()`; the
  example app starts the stack natively on both platforms.
- **CI:** `ios-xcframework` workflow builds the xcframework, compiles
  MedutKit, builds the example app and can commit the framework back.
  `build-mobile` runs the Dart analyzer and tests on plugin changes.
- **Android plugin** migrated to Flutter's built-in Kotlin (minimum Flutter
  3.44).

---

## [1.0.0] — 2026-10-12

Initial release of Medut, a general-purpose SIP client SDK with a flat C ABI
(`include/medut.h`) and C++, Python and Flutter/Dart bindings.

### Added

- **Signalling:** registration, calls (invite, answer, hangup, hold/resume,
  DTMF), blind and attended transfer (REFER), SIP MESSAGE, presence
  (PUBLISH, SUBSCRIBE/NOTIFY, BLF, MWI), PRACK/100rel, session timers,
  per-account and per-call custom headers, any number of accounts per stack.
- **Transports:** UDP, TCP, TLS, WS and WSS, with DNS SRV resolution and
  failover and server certificate verification.
- **Media security:** SRTP-SDES and DTLS-SRTP.
- **NAT traversal:** ICE with STUN and TURN over UDP, TCP and TLS; ICE server
  lists in the browser URL grammar, relay-only transport policy, automatic ICE
  restart, and experimental multipath gathering.
- **Mobility:** network handover (Wi-Fi ↔ 4G/5G) with transport rebind,
  re-REGISTER, call re-INVITE with media verification, and a `RECONNECTING`
  registration state.
- **Degraded links:** media-stall alerts, RTP timeout, SIP OPTIONS keepalive,
  adaptive Opus bitrate, Opus FEC.
- **Push notifications:** RFC 8599 Contact URI parameters (APNs, FCM) and
  REGISTER-only custom headers.
- **Audio:** Opus, G.711, G.722, G.726; PCM tap, WAV recording, mute, device
  enumeration and hot-switch, gain, AEC (suppressor or WebRTC full-duplex),
  NS, AGC, and app-owned audio devices.
- **Observability:** SIP trace, SDP diff, pcap capture, RTCP/MOS statistics,
  quality alerts, jitter buffer and audio level reporting.
- **Platforms:** Linux, macOS, Windows, Android and iOS.
