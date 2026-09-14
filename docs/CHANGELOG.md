# Changelog

All notable changes to EchoSDK are documented here.

---

## [Unreleased]

### Fixed

- **Every API call could stall for seconds behind unrelated DNS work — seen as
  a slow `register()`, and on Flutter as a frozen UI.** `bsdk_dispatch()` and
  `bsdk_dispatch_sync()` posted their work to re_main with
  `re_thread_async_main(NULL, …)`. That reads like a "run on the event loop"
  primitive, but with `work = NULL` the item still transits libre's async
  worker pool: `re_async()` appends it to the work list, one of the
  `RE_THREAD_WORKERS = 4` worker threads dequeues it, and only then is it
  pushed to the loop. The same four threads run libre's *blocking*
  `getaddrinfo()` (which `core.c` forces on for Android, where re's resolver
  has no nameservers) and its blocking mDNS `.local` ICE-candidate resolution
  (libre's own comment: "~5s"). A SIP hostname costs two slots — A and AAAA
  go out together — so two accounts registering at once filled the pool, and
  every call behind them waited for a resolve to return: `register()`,
  `answer()`, `hangup()`, `invite()`, `hold()`, `dtmf()`, all of it.

  Measured against a local registrar: registration completes in 2–4 ms with
  up to three workers busy, **3.0 s with four, 6.0 s with eight** — the
  REGISTER is not even on the wire until a slot frees. The 28 synchronous
  APIs block the *caller* for that long, and the Flutter binding makes its FFI
  calls on the Dart isolate with no offload, so the app's UI froze with them.

  Dispatch now has its own `mqueue`: `mqueue_push()` is one atomic pipe write
  from any thread and the handler runs on re_main with no worker in between —
  the primitive `re_loop.c` already used to wake the loop for shutdown. A
  dispatch that arrives after the loop has stopped now returns
  `ECHOSDK_ERR_STATE` instead of a `cnd_wait()` that never returns.
  Regression test: `test/unit/test_dispatch_starve.c` holds all four workers
  and times a dispatch.

- **ICE: a candidate pair that failed its first check and then answered was
  selected while still marked `Failed`, and could not be nominated.** libre
  treats `Failed` as terminal — `icem_candpair_set_state()` refuses to move a
  completed pair — which is right for "do not re-check it" and wrong for the
  ordinary carrier-NAT sequence where the first request times out before the
  binding exists and the retransmit is what gets answered. Two things then
  went wrong in `handle_success()`: the peer-reflexive address the response
  revealed got a *cloned* pair, and `icem_candpair_clone()` copied the
  parent's `state`, so the new pair was born `Failed`; and
  `icem_candpair_make_valid()` set `valid`, moved the pair to the valid list
  and cleared `err`/`scode`, but its `set_state(SUCCEEDED)` was a no-op.

  On-device the log line is
  `icecomp: {audio.1} set_selected: invalid state 'Failed' [{comp=1} Failed { V } seth_lte0:prflx:… <---> host:…]`
  — valid, not nominated, and no `(err)`/`[scode]` because they were just
  erased. Media did flow (`cp_sel` is set and nothing reading it checks
  state) and the checklist completed (that test is valid-list membership).
  What broke is nomination: `concluding_ice()` picks the pair to nominate
  with `icem_candpair_find_st(…, SUCCEEDED)`, a *state* filter, so when this
  was the only valid pair for the component it found nothing, never set
  `comp->concluded` and never sent USE-CANDIDATE.

  Two libre hunks in `cmake/patch-re-sources.cmake`: the clone starts the way
  `icem_candpair_alloc()` starts every pair, and `make_valid` steps a
  `Failed` pair back to `InProgress` so the transition is allowed. The
  patcher's idempotency check is now per hunk (it is skipped when its
  replacement text is already present) rather than per file — the old
  "file contains `EchoSDK-patched`" test would have reported the second
  `candpair.c` hunk as done after applying the first. Regression test:
  `test/unit/test_ice_candpair.c`.

- **A network handover could re-INVITE a call in the middle of its DTLS-SRTP
  handshake and leave it permanently silent.** `ice_shim.c` has held its
  candidate re-offer for the handshake since the 22-call measurement that
  identified a mid-DTLS re-INVITE as the single strongest predictor of a dead
  call — the handshake is still retransmitting when the address changes under
  it, the retries go into the void, `menc_secure` is never set, and
  `stream_is_ready()` stays false for the life of the call. netmon's migration
  offer is the same re-INVITE arriving at the same moment by a different route,
  and was never covered by that hold.

  It now defers, reusing the `BSDK_MIG_DEFERRED` state that already parks a
  migration whose dialog cannot carry an offer yet: the verify tick re-runs it,
  so the offer goes out on the first tick after the encryption comes up, and
  `BEVENT_CALL_MENC` bounces that tick early rather than waiting a full
  `net_verify_ms`. The wait is taken before the attempt counter, so it never
  spends a retry.

  Bounded at 12 s — the length of the handshake ladder itself (`dtls_srtp`
  waits 1200 ms on the first attempt and 2500 ms on each of four restarts), so
  past it the module has given up and there is nothing left to protect. Unlike
  `ice_shim`, which drops its re-offer at the deadline, the migration is sent:
  a candidate re-offer is an optimisation, but a migration offer is the
  recovery, and holding it forever guarantees the silence it was avoiding. The
  bound also covers a call configured for DTLS whose peer answered plain RTP,
  which never reads as secure and would otherwise never migrate.

- **A call whose media came up slowly was diagnosed as stalled, "repaired"
  with an ICE restart, and left permanently silent.** Three calls on-device
  (2026-09-10) went the same way: established, media working or about to work,
  a false media-stall alert around 4–5 s, an ICE restart, and dead audio for
  the rest of the call. A browser (sip.js) against the same FreePBX on the same
  network was unaffected, because nothing in a browser stack restarts ICE four
  seconds into a healthy call. Four separate defects, each of which had to be
  fixed for the call to survive:

  *The stall clock counted setup time.* `stall_since` was stamped at
  `CALL_ESTABLISHED` and only advanced when packets arrived, so the ICE
  connectivity checks and the DTLS-SRTP handshake — everything that has to
  finish before the first packet *can* arrive — accumulated on it. The grace
  window meant to cover that was conditioned on the handshake still being
  unfinished, so it fell away the instant DTLS completed while the time it had
  been covering stayed on the clock. A handshake slower than
  `cfg.media_stall_ms` therefore produced an alert on the very tick media
  became able to flow. Measured: established 15:21:53.203, DTLS complete
  15:21:54.288, alert at 15:21:58.055 reporting 4854 ms — of which only 3765 ms
  were after media was ready, i.e. below the threshold. The clock now restarts
  on the setup→ready edge, and a renegotiation that re-enters handshake raises
  the flag again so later handshakes on the same call are covered too.

  *A stall forced an ICE restart even when nothing had moved.* The stall repair
  set `net_mig_path_moved` unconditionally, purely to route itself through the
  ICE-restart branch — a handover on an unchanged address bails out with "same
  path, no re-INVITE", and the stall path deliberately overrode exactly that
  guard. Against a peer that answers a restart offer with `a=connection:new`,
  as Asterisk/FreePBX does every time, that is a bad trade: it tears down and
  rebuilds its RTP instance, our connectivity checks race the rebuild and are
  answered `401 Unauthorized`, and the check list ends Failed with an empty
  valid list. Five restarts across three calls, 401 every time. The flag now
  reflects reality — set only when the address rotation actually picked a
  different local address — so a stall on an unchanged address gets the plain
  re-offer instead, and never asks the peer to throw its ICE session away.

  *A failed ICE generation stranded the stream on the peer's private address.*
  Two places dropped the pair ICE had validated: `bsdk_ice_restart()` cleared
  `sel_set` on every media, and the restart-answer path cleared it again and
  returned without calling `reassert_selected()`. Since `stream_update()` has
  by then reset the stream to the address the peer *signals* — for a PBX behind
  NAT, its RFC 1918 one (`c=IN IP4 172.16.11.52`) — and the only thing that
  would ever repoint it is `ice_connected()` from the new generation, a
  generation whose checks were rejected left the stream aimed there for the
  rest of the call: rx counter climbing, `spk -72.2 dBov`, SRTP replay
  failures, silence. The validated pair now survives a restart and is
  re-applied until the new generation nominates its own; `ice_connected()`
  overwrites it the moment one is nominated, and `reassert_selected()`'s
  existing guard still follows a peer that genuinely moved its media.

  *Nothing reported ICE failure.* baresip's ice module answers a check list
  that ends with no valid pair by logging a warning and returning — no estab
  handler, no connected handler, no event — so a call whose media path was
  never validated was indistinguishable from a working one through the API.
  New `ECHOSDK_NET_CALL_ICE_FAILED` network event, raised by a watchdog armed
  when the answer to a restart offer arrives and cancelled as soon as a pair is
  nominated. The call is **not** ended: with the fix above audio usually
  survives a failed restart, and redialling is the app's decision. Exposed in
  the C, C++, Python (`call_ice_failed`) and Dart (`NetworkStage.callIceFailed`)
  bindings; documented in [Events](api/events.md).

- **Call quality was scored from lifetime numbers, so one early burst of loss
  depressed MOS for the rest of the call.** `loss_pct` / `loss_pct_rx` are now
  rates over the last poll window (`stats_interval_ms`) rather than
  call-lifetime averages — the same convention as WebRTC `getStats()` and
  PJSIP, and what the E-model expects as input. The `packets_lost` /
  `packets_lost_rx` counters beside them stay cumulative per RFC 3550.
  Alongside that, the E-model implementation was reworked: LQ and CQ are now
  properly separated (LQ carries no delay term; CQ subtracts the G.107 delay
  impairment `Id` from a real one-way delay estimate — `RTT/2` plus the jitter
  buffer depth actually in use — so `CQ <= LQ` always, instead of a penalty
  that only appeared past 300 ms); the RX-direction scores use *effective*
  loss, network loss plus packets the jitter buffer discarded (late, overflow,
  flush), previously the largest source of MOS over-estimation on a jittery
  call; wideband codecs are scored on the G.107.1 R scale (Ro = 129) rather
  than the narrowband one, checked against ITU's own reference calculator; and
  `mos_lq_avg` is averaged in the R-factor domain and converted once, since MOS
  is non-linear in R. `echosdk_call_get_stats()` now zeroes `out` before
  anything else, so every field is defined even on an error return, and
  `rtt_ms` reading `0.0` means "no RR received yet", not "zero delay". Docs
  updated: [Observability](api/observability.md) and [Events](api/events.md).

- **Documentation: the event-callback contract was documented backwards.**
  `echosdk_event_cb_t` said "do not call EchoSDK APIs from inside this
  callback". The opposite is true and always has been — the event dispatch
  thread exists precisely so a consumer can re-enter the API without
  deadlocking `re_main`, and every example in the docs answers a call from
  inside the handler. The comment in `include/echosdk.h`, the C++ wrapper and
  the Dart FFI bindings now states the real contract: re-entry is safe, keep
  the handler fast (< 10 ms) because it runs inline with the event queue.

- **The BYE and in-dialog WS routing fixes now hold on every platform — and
  the iOS build actually builds.** Both fixes used to ride GNU ld's `--wrap`,
  which only the Linux/Android links passed, so iOS (and Windows/macOS static
  consumers) silently shipped without them; worse, the Apple escape hatch the
  code referenced (`RE_WEBSOCK_CONNECT_OVERRIDE` in re's websock.c) never
  existed in the pinned libre, so `scripts/build-ios.sh` could not even link.
  The interpositions moved into the sources: `cmake/patch-re-sources.cmake`
  patches the fetched libre at build time — the ACK reply-drain is fixed in
  `sipsess/reply.c` itself, and `websock_connect()` / `sip_dialog_route()` are
  renamed to `__real_*` behind compile-time switches so EchoSDK's `ws_path.c`
  owns the public names everywhere. No link step passes `--wrap` any more, and
  `dist/` archives are self-contained (consumers no longer need the flags).
  `src/sipsess_fix.c` is gone; the `sip_transp_send` interposition is gone too
  — routing over the registration flow already makes libre's connection lookup
  match the one WebSocket, so the address rewrite had nothing left to do.
  baresip's demo executables are disabled on every platform
  (`cmake/patch-baresip-sources.cmake`) since they link libre without
  `ws_path.c` — the SDK only ever consumed the library.

- **iOS builds had no TLS, WSS or DTLS-SRTP.** `scripts/build-ios.sh` selected
  mbedTLS, but libre implements TLS/DTLS only over OpenSSL — an mbedTLS build
  stubs `tls_alloc`/`dtls_listen` and ships without the secure stack (the
  Android script has warned about exactly this all along). The OpenSSL
  cross-build (Phase 0.5) now covers iOS: one OpenSSL per slice
  (`ios64-xcrun`, `iossimulator-{arm64,x86_64}-xcrun`), the simulator built as
  two thin slices and lipo-merged, and the script fails the artifact if
  `SSL_CTX_new` is missing from the framework.

- **One incoming SIP MESSAGE, presence NOTIFY, MWI NOTIFY or REFER used to
  silence the SDK permanently.** This is the most serious bug the SDK has
  shipped, and any app with a BLF or presence subscription hit it within seconds
  of start-up.

  The event queue is a bounded list plus a length counter. Producers increment
  it, the single consumer decrements it once per event drained, and every
  producer refuses to enqueue once the length reaches `ECHOSDK_EV_QUEUE_MAX`.
  Four producers — incoming SIP MESSAGE, MWI NOTIFY, presence/BLF NOTIFY and the
  incoming REFER — appended to the list by hand and never incremented the
  counter. Each such event therefore left the counter one below the queue's real
  depth, and the first one drained from an otherwise-empty queue took it from 0
  to `SIZE_MAX` (it is a `size_t`). From that instant `len >= max` was
  permanently true and **every subsequent event was dropped for the life of the
  process** — call state, registration, DTMF, network handover, logs. The app
  saw a stack that had simply stopped talking, with no error anywhere.

  All four now go through `bsdk_event_post_qev()`, which has always done the
  accounting correctly. `log.c` and `trace.c` keep their own copies on purpose —
  a dropped log must not `warning()` its way back into the log handler — and now
  carry a comment saying so, plus the reason the increment is not optional.

  Covered by a new gate test, `test/event_queue_accounting_test.c`, which drives
  a real SIP MESSAGE into the stack over UDP and asserts that ordinary events
  still flow afterwards. It needs no SIP server.

- **A queued event that was dropped leaked the call it described.** The
  queue-full path in `bsdk_event_post_qev()` freed the event with a bare
  `mem_deref()`, but a `CALL_CLOSED` event owns a reference to the call wrapper
  (~16 KB of SDP and mixing buffers), and the wrapper's destructor is what closes
  an open WAV recording. Both other release paths released it by hand; the drop
  path — the one nobody exercises — did not. The reference is now released by a
  destructor attached at allocation, so all three paths get it right and a new
  call site cannot forget. All 17 allocation sites go through `bsdk_qev_alloc()`.

- **`cfg.local_ip` and `cfg.local_port` were documented, accepted, and silently
  ignored.** Neither was ever passed to the stack, so the SDK always bound an
  OS-assigned port on every interface. An app that pinned a port for a firewall
  rule or a port-forward got no error and no effect. Both now populate baresip's
  local SIP address; a port with no address binds that port on each local
  address, and an IPv6 literal is bracketed.

- **Any failure inside `echosdk_init()` crashed the host application.** The
  unwind path called `bsdk_event_close()` unconditionally, which joined an event
  thread that had not been created — `thrd_t` has no "not a thread" value, and
  joining an unassigned one segfaults. Init failures now return their error code
  as documented.

- **A network handover that ran out of attempts stranded every live call.**
  After `net_max_attempts` failed transport resets, the accounts were handed to
  the registration retry policy but the calls were not: the handover generation
  was never bumped, so nothing re-INVITEd them and no timer was armed to try
  again. With `net_monitor_interval_s = 0` — the setting mobile apps are told to
  use — nothing woke the SDK up again either. The calls stayed up with dead audio
  and the app was never told. They now get `ECHOSDK_NET_CALL_MIGRATION_FAILED`
  (and are hung up when `net_hangup_on_migration_failure` is set). The round
  state is also cleared, so the next handover announces itself instead of
  reporting an `elapsed_ms` accumulated from the failed one.

- **`net_max_attempts` above 255 made a migration retry for ever.** The per-call
  counter was a `uint8_t` while the public config field is a `uint32_t`, so the
  "give up" test could never fire. The counter is now `uint32_t`, and the field
  documents that the useful range is small because each attempt costs a
  `net_verify_ms` wait.

- **A stale ICE-restart flag disabled ICE restart for the rest of a call.** When
  the gathering deadline fired, `restarting` was left set. If the ICE module then
  never reported — which is the exact failure the deadline exists for — every
  later `bsdk_ice_restart()` returned `EALREADY`, so a second network handover
  silently skipped the restart and re-offered the candidates of a network the
  device had already left.

- **A network change re-REGISTERed accounts whose credentials the registrar had
  rejected.** `mark_accounts_reconnecting()` deliberately refuses to move a
  terminal `FAILED` account back into recovery; the handover's re-REGISTER
  ignored that and retried anyway, leaving the account reporting `FAILED` to the
  app while carrying `reconnecting = true` internally. Accounts that failed on
  `ECHOSDK_ERR_AUTH` are now left alone. Every other failure — transport,
  timeout, 5xx — is still retried, because a new network is exactly what might
  fix those.

- **Python wheels published from CI had a corrupt `echosdk_config_t` layout.**
  The release workflow generated the cffi header with its own copy of the
  `gcc -E` pipeline, which was missing `-DECHOSDK_NO_PACKED_ENUM=1` while still
  stripping `__attribute__`. cffi therefore saw `echosdk_aec_mode_t` as 4 bytes
  where the compiled library has 1, so every field after `cfg.aec_mode` was at
  the wrong offset — `sizeof` 760 against the library's 752 — and the
  `version`/`struct_size` guard could not catch it, because `echosdk_config_init()`
  fills both from the C side. Locally built wheels were correct; released ones
  were not. Both callers now share `bindings/python/gen_header.sh`, so they
  cannot drift again.

### Added

- **`echosdk_call_transfer_accept()` / `echosdk_call_transfer_reject()` — an
  incoming REFER can finally be answered.** The SDK raised
  `ECHOSDK_EV_TRANSFER_REQUEST` and then had nothing to offer: no way to follow
  the transfer, no way to refuse it, and — because RFC 3515 makes a REFER an
  implicit subscription — no way to send the final `message/sipfrag` NOTIFY the
  transferor is waiting on. The subscription stayed parked at `100 Trying` until
  it expired. The docs suggested hanging up and dialling the Refer-To URI, which
  looks reasonable and is exactly the thing that loses the NOTIFY: the new call
  is then unrelated to the REFER.

  `echosdk_call_transfer_accept()` places the call *linked* to the original, so
  the stack reports the outcome by itself — `200 OK` once the new call is
  established, or the failure status if it never is.
  `echosdk_call_transfer_reject(call, scode, reason)` sends the terminating
  NOTIFY and leaves the call up, so the user stays on the line. Answer every
  `ECHOSDK_EV_TRANSFER_REQUEST` with exactly one of them.

  `echosdk_ev_transfer_req_t` gains a trailing `auto_followed` flag, always
  false today, so an app written now cannot be made to place a duplicate call if
  auto-follow policy is ever added. Appending it keeps the struct ABI intact.

  Exposed as `Call.transfer_accept()` / `transfer_reject()` in Python,
  `transferAccept()` / `transferReject()` in Dart, and
  `Call::transfer_accept()` / `transfer_reject()` in C++.

- **`echosdk_call_get_info()` — the call metadata every comparable SDK exposes.**
  Peer URI and display name, local and contact URIs, Call-ID, diverter,
  direction, remote-hold state, last SIP status, duration, setup duration, line
  number, transport and state. Until now the peer URI was only visible on the
  incoming-call event, so anything that needed it later had to cache it.

  It fills a caller-owned `echosdk_call_info_t` of fixed char arrays, matching
  `echosdk_audio_device_t`: nothing in it can dangle when the call ends, and
  every binding can copy it without a lifetime rule. Safe to call at any point,
  including after the call has ended.

  Two fields are deliberately absent rather than faked: there is no
  `remote_user_agent`, because the stack exposes no getter for it, and the
  forwarding field is named `diverter_uri` because it carries Diversion /
  History-Info, not Referred-By. `is_remote_hold` is likewise named for what it
  is — the peer holding *us*; local hold remains `echosdk_call_is_held()`.

- **`ECHOSDK_REG_RECONNECTING` — a registration the SDK is recovering is no
  longer reported as a failure.** Every transient loss used to arrive as
  `ECHOSDK_REG_FAILED`: a REGISTER that timed out on a train, a 5xx, a keepalive
  probe the proxy stopped answering. The retry loop then fixed it a second later,
  but the app had already been told the registration failed and had no way to
  tell that from wrong credentials — so the honest UI was an error the user could
  do nothing about. A network handover was the mirror image: the transports were
  flushed and the registrar's binding pointed at an address the device had left,
  yet the state stayed `REGISTERED`, so a status indicator showed a green dot
  over a path that could not take an inbound call.

  The state now splits by who owns the recovery. `RECONNECTING` is everything the
  SDK is getting back by itself:

  - a REGISTER that failed on transport, timeout or 5xx with a retry armed —
    `retry_attempt` / `retry_delay_ms` carry the countdown, as they always did,
  - a keepalive probe (`cfg.keepalive_interval`) that went unanswered: registered
    on paper, unreachable in fact,
  - a network handover or a link that dropped entirely, from
    `ECHOSDK_NET_CHANGE_DETECTED` (or `NET_DOWN`) until the REGISTER lands on the
    new path.

  `FAILED` is kept for what the SDK has stopped acting on: `ECHOSDK_ERR_AUTH`, an
  exhausted `reg_retry_max_attempts`, or a retry the app cancelled with
  `echosdk_account_cancel_retry()`.

  The recovery does not flicker: an account stays `RECONNECTING` across every
  attempt it makes — a retry's own REGISTER is reported as `RECONNECTING`, not
  `REGISTERING` — and leaves it only for `REGISTERED` or `FAILED`. Two gaps that
  left an app stuck are closed with it: an exhausted retry budget used to go
  silent, and so did a keepalive path that came back without a re-REGISTER
  (`keepalive_reregister = false`); both now report the transition they owe.
  A handover that runs out of transport-rebind attempts hands its accounts to
  their own retry policy rather than leaving a recovery nothing is driving.

  The enum value is appended (5, after `UNREGISTERING`) so the existing ones keep
  their ABI. An app that renders `FAILED` and nothing else now shows no status
  for a transient outage instead of a wrong one — the one line worth adding is a
  `RECONNECTING` case. Dialling is unaffected during a handover: the guard that
  refuses to dial a dead transport asks baresip whether a registration is behind
  it rather than keying on the state alone.

### Fixed

- **A network handover left an ICE call with dead audio.** Wi-Fi ↔ cellular
  migrated a direct-RTP call correctly and could not migrate an ICE one at all:
  the re-INVITE re-advertised the network the call had just left, so the peer
  kept sending RTP to an address that was gone while dropping what arrived from
  the new one (Asterisk: `Source not in ICE active candidate list`). The call
  stayed up, silent, until `net_max_attempts` ran out — 24 s at the defaults.

  Two things caused it, both in baresip's ice module. `call_reset_transp()`, the
  whole of the migration, sets the *session* address; ice.c writes the selected
  local candidate into the *media* address, and a media-level `c=` line overrides
  the session-level one (RFC 4566 §5.7). And nothing re-gathers: the module's
  update handler runs `ice_start()` with `started` already true, which re-encodes
  the candidate list it already has under the same `ice-ufrag` / `ice-pwd`.

  An ICE call is now migrated with a genuine RFC 8445 §9 ICE restart. The inner
  media-NAT session is thrown away and allocated again, which mints new
  credentials, writes them into the SDP and re-gathers on the interface that now
  carries the default route; baresip turns the completion into the re-INVITE, so
  the offer carries the new candidates *and* the new address at both levels. The
  RTP sockets are kept — they are wildcard-bound, so they already follow the new
  route, and the media encryption is keyed to them.

  This is what the media-NAT vtable interposition in `src/ice_shim.c` makes
  possible: the ice module's session object is ours to replace. Its media object
  now goes to baresip through a wrapper of ours, so the pointer baresip holds as
  `stream->mns` survives the swap, and a restart whose gather fails releases the
  offer instead of reporting an error — baresip answers an mnat failure with
  `CALL_EVENT_CLOSED`, which would hang up the call the restart was rescuing.

  Behaviour notes: a restart is only attempted when the local address actually
  moved, so a WebSocket call re-INVITEd purely to re-bind its dialog does not put
  working media through one. `cfg.ice_gathering_timeout_ms` bounds the re-gather
  as it bounds the one on dial (0 there means 3 s, not "for ever" — the call is
  live and silent while it runs). `ECHOSDK_NET_CALL_ICE_STALE` and
  `net_ice_handover` now apply only to a call the restart could not be performed
  for, and `FAIL_FAST` no longer caps a restarted call at one attempt: what it is
  waiting on is the peer's NAT rebinding, exactly like a direct-RTP call. The
  `ice` field on the event is now the negotiated truth (does this call have a
  live ICE media-NAT?) rather than `cfg.ice_enabled`, so an ICE-enabled account
  whose call ended up on plain RTP is no longer held to the ICE policy.

- **In-dialog requests on an incoming call never reached the server.** A UAS
  dialog's route set is only the peer's Record-Route — unlike a UAC's, it does
  not begin with our outbound proxy. A PBX behind a reverse proxy commonly sends
  no Record-Route at all, leaving libre to route to the Contact:

      Contact: <sip:asterisk@echo:5060;transport=ws>

  `echo` is the PBX's internal hostname and resolves nowhere, 5060 is not our
  port, and `ws` is not our transport. libre cannot parse that as an address
  literal, falls into `addr_lookup()`, and the DNS query fails *asynchronously* —
  long after `sipsess_bye()` has already returned 0.

  So hanging up an answered incoming call reported success and put nothing on
  the wire: the caller stayed connected until they gave up, and their BYE then
  arrived at a dialog we had dismantled, answered `481 Call Does Not Exist`. The
  re-INVITE carrying updated ICE candidates died the same way, leaving media
  dead.

  `sip_dialog_route()` is now interposed so in-dialog requests follow the
  WebSocket flow the registration established, which RFC 7118 §B.2 requires
  anyway. Only WS/WSS dialogs are affected; UDP, TLS and transport-less routes
  are passed through untouched.

- **Hanging up an answered incoming call sent no BYE.** The local side tore
  down and reported ENDED while the caller stayed on a live call; their own BYE
  then arrived at a dialog we had already dismantled and got `481 Call Does Not
  Exist`. Outgoing calls were unaffected.

  A UAS that rings before answering puts *two* entries on libre's
  `sess->replyl` — `sipsess_reply_1xx` (the 180) and `sipsess_reply_2xx` (the
  200) both append, stamped with the same INVITE CSeq. `sipsess_reply_ack()`
  matches with `list_apply()`, which stops at the first hit, so one entry
  survives the call. At hangup `termwait()` sees a non-empty list, takes a
  reference and returns, and the session destructor never reaches the branch
  that sends the BYE. It is not lost outright: the leftover reply's 64·T1 timer
  fires 32 s later and the BYE finally goes, which for the caller is
  indistinguishable from never. A UAC sends no replies at all, which is why
  outgoing hangups always worked.

  `src/sipsess_fix.c` drains every reply the ACK matches. Interposed with
  `--wrap` (GNU ld) rather than patched into libre, so it was **active on Linux
  and Android only** at the time; the fix has since moved into the patched
  libre sources and applies on every platform (see Unreleased).

- **Incoming calls had no audio: media was sent from an address the peer was
  never told about.** ICE learns server-reflexive candidates from STUN before
  the offer/answer, so those get signalled. A *peer-reflexive* candidate is
  discovered from the connectivity checks themselves and is never signalled
  (RFC 8445 §5.1.3) — and when ICE nominates one, media leaves from an address
  the peer will not accept. Asterisk logs

      res_rtp_asterisk.c: DTLS packet from <ip:port> dropped.
      Source not in ICE active candidate list.

  and since DTLS-SRTP gates RTP behind the handshake, the call connects,
  signalling looks perfect, and neither side hears anything. It hits the
  answerer hardest: per RFC 5763 the answerer picks `a=setup:active` and is
  therefore the side that transmits first.

  baresip already sets `send_reinvite` when the selected candidate changes but
  only acts on it when its conncheck handler is called with `update` set, which
  does not happen on this path. `src/ice_shim.c` now compares the address ICE
  settled on against the one actually advertised and asks baresip to re-offer
  when they differ.

- **A re-INVITE pointed media at the peer's signalled address, undoing ICE.**
  Symptom: a call that established cleanly, completed its connectivity check,
  started its DTLS handshake — and then sat there with `tx 0 rx 0` and both
  audio levels `NaN` until someone hung up, in both directions.

  baresip keeps the address it sends media to in `stream->tx.raddr_rtp`, and two
  things write it. `stream_mnat_connected()` writes the remote candidate ICE
  nominated. `stream_remote_set()`, reached from `stream_update()`, writes
  `sdp_media_raddr()` — the address in the peer's SDP, verbatim. The second runs
  on every `call_update_media()`: the re-offer above, ice.c's own re-INVITE, a
  re-INVITE the peer sends, a session refresh, hold/unhold. It overwrites the
  first, and nothing puts it back — the ice module's update handler refreshes
  *local* addresses only, and no new connectivity check runs, so no further
  connected handler fires.

  When the peer is behind NAT the two addresses are not the same. A PBX
  signalling `c=IN IP4 172.16.11.52` but reachable, per ICE, at
  `82.129.158.253:19914` left the stream aimed at an RFC 1918 address on the far
  side of the internet from the first re-INVITE onwards. With DTLS-SRTP the
  handshake dies first: it is still retransmitting when the address moves under
  it, the retries go nowhere, `menc_secure` is never set, and
  `stream_is_ready()` stays false forever — so `audio_update()` is never called,
  the microphone is never opened, and the call runs with no media at all while
  signalling looks perfect.

  `src/ice_shim.c` re-applies the nominated pair from the media-NAT update
  handler, which `call_apply_sdp()` calls after every `stream_update()` and which
  therefore covers all of those paths at once. It only does so while the peer is
  still signalling the same address as when the pair was nominated: a peer that
  signals a *different* one has genuinely moved its media, and following it is
  correct.

  Two related corrections in the same file: the "was this address signalled?"
  baseline is now taken from the SDP as it stands when gathering reports, rather
  than from the session address that is all that exists before gathering — so a
  server-reflexive candidate that the offer did carry is no longer re-offered on
  every NAT'd call.

- `mic_level_dbov` / `audio_level_dbov` were seeded with `-127.0f`, which
  `tap_compute_dbov()` also returns for all-zero PCM — so "never measured" and
  "measured digital silence" were the same value, despite the header
  documenting `NaN when unavailable`. Now seeded `NAN`, as documented, which is
  what makes a dead capture path distinguishable from a quiet one.

- **Outgoing calls could never send their INVITE.** With ICE enabled, baresip
  does not send the INVITE when the call is placed — it defers it until the
  media-NAT reports that candidate gathering finished. Nothing bounded that
  wait, and one path never reported at all: the ice module's no-STUN/TURN case
  arms a 1 ms timer whose handler walks the session's media list and fires the
  gather callback once per entry, so an empty list meant the callback was never
  invoked. The call then sat in `ECHOSDK_CALL_CALLING` indefinitely — zero SIP
  messages on the wire under `trace_sip`, no event of any kind, and
  `echosdk_call_invite()` had already returned success, so nothing in the SDK
  or the app could tell. A refused hangup looked dead because there was no
  transaction to cancel.

  `cfg.ice_gathering_timeout_ms` (default 2000, 0 = wait indefinitely) is the
  bound. On expiry the offer is released with the candidates gathered so far,
  which is what every other SIP client does: JsSIP, SIP.js and dart-sip-ua all
  resolve their offer on `icegatheringstatechange == complete` *or* a timer
  (dart-sip-ua's `ice_gathering_timeout` defaults to 500 ms), and pjsua has
  `PJSUA_ICE_TRANSPORT_INIT_TIMEOUT`, whose own comment calls it "a safety net
  so the calling thread cannot block indefinitely if the callback never
  arrives". Gathering is not cancelled — a late completion re-offers the fuller
  set in a re-INVITE, and a late failure is dropped rather than tearing down a
  call whose offer is already on the wire.

- One dial on an IPv4-only network permanently disabled ICE for the whole
  account. `echosdk_call_invite()` retries with the media-NAT cleared when
  `ua_connect()` reports `EAFNOSUPPORT` (a STUN/TURN lookup that needs IPv6),
  but never restored it — so every later call on that account was silently
  downgraded to no-ICE, including calls placed after the device moved to a
  network where ICE was both available and needed.

- `echosdk_call_invite()` returned success without writing `*out` when
  `ua_connect()` reported success but produced no call, leaving the caller with
  an untouched handle. It now returns `ECHOSDK_ERR_INVAL`.

- `echosdk_call_get_stats()` reported 0 for `mos_lq_min`, `mos_lq_avg`,
  `stats_tick` and `call_duration_ms` at every point in a call, however many
  stats ticks had already run — the session-history block was only filled on
  the event path. The getter now reads them from the call without advancing
  them, so polling cannot skew the averages it is reporting.

- baresip reports a media-encryption mismatch as "no common audio or video
  codecs", which misattributes the failure: an account offering `RTP/AVP`
  against a peer offering `UDP/TLS/RTP/SAVPF` fails with a full and perfectly
  compatible codec list on both sides. When the account offers no encryption the
  SDK now names the real mismatch. The opposite direction keeps baresip's
  wording — the peer's capabilities are not ours to describe, and a genuine
  codec mismatch is still possible there.

- The example app offered an ICE toggle with nothing to point it at, so
  enabling ICE could only ever gather host candidates — the configuration that
  produces the peer-reflexive failure above. It now has STUN and TURN server
  fields, and defaults `media_enc` to DTLS-SRTP — a WSS-facing PBX offers
  `SAVPF` and rejects an unencrypted account outright.

### Known limitations

- **The re-offer of an unsignalled ICE candidate is best effort.** A peer is
  only obliged to re-read candidates on an ICE *restart* (new ufrag/pwd, RFC
  8445 §9), and baresip fixes those at media-session creation, so the SDK cannot
  trigger one. Asterisk answers the re-INVITE `200 OK` and leaves its candidate
  list unchanged. On a carrier-grade NAT that maps one local port to different
  public IPs per destination, **TURN is the only reliable answer** — see
  [NAT traversal](guides/nat_traversal.md#carrier-grade-nat-when-stun-is-not-enough).

- The BYE and in-dialog routing fixes relied on GNU ld's `--wrap` and were
  therefore **active on Linux and Android only** at the time. Since resolved:
  both fixes moved into the patched libre sources and apply on every platform
  (see Unreleased).

- `file(GLOB)` in CMakeLists.txt lacked `CONFIGURE_DEPENDS`, so a newly added
  source file was silently left out of every incremental build until the next
  fresh configure.

### Changed

- The examples now show **In call · M:SS**, ticking once a second, for as long
  as the call is up — Flutter (under the peer name, `On hold · M:SS` while
  held), Python and C++ (a status line rewritten in place). Previously they
  showed only the raw state name, and the only duration anywhere was buried in
  a stats block.

  The clock is wall-time in the example, not `call_duration_ms` from the stats
  event: that field only advances once per `stats_interval_ms` and is not
  emitted at all when stats are off, so a timer built on it either moves in
  five-second jumps or never moves. It starts on `ESTABLISHED` rather than on
  dial, because talk time is what a user expects to see, and it keeps running
  across hold.

  In the two CLI examples every other line of output now goes through a helper
  that clears the status line first, so event callbacks firing on the SDK's
  thread no longer land on top of the timer.

- The Python stats block header now carries the call duration
  (`Media Stats (tick=7, in call 1:35)`), which it captured but never printed.

### Added

- **Degraded-link handling** — the SDK already recovered from a *changed*
  address (network handover); it had nothing for a link that keeps its address
  and stops working. That is the ordinary mobile failure: one bar of signal, a
  saturated uplink, a cell that stops forwarding packets without dropping the
  PDP context, a carrier NAT that quietly expires a UDP binding. In every case
  the local IP is unchanged so handover sees nothing, the dialog is healthy so
  the stack reports the call as up, and the user hears silence. New guide:
  [Degraded links](guides/degraded_links.md).

  - `cfg.media_stall_ms` (default 4000) fires `ECHOSDK_QUALITY_MEDIA_STALL`
    when inbound RTP stops advancing and again with `recovering` when it
    resumes. Non-fatal, and the only way this condition becomes observable at
    all — when RTP stops, every other metric simply stops changing.
    Suppressed on held calls and during a handover migration, where
    `ECHOSDK_EV_NETWORK` already narrates the outage in more detail.
  - `cfg.rtp_timeout_s` (default 0 = off) wires baresip's `avt.rtp_timeout`,
    which was never set: the fatal counterpart that ends such a call. Left off
    because ending a call is destructive and some deployments run legitimate
    one-way media. Per-call override: `echosdk_call_set_rtp_timeout()`.
  - `cfg.adaptive_bitrate` steps the Opus encoder down under the loss the
    *peer* reports over RTCP and back up on recovery — halve down, +25% up,
    with a dead band between `adapt_loss_down_pct` and `adapt_loss_up_pct` so a
    link hovering near a threshold does not oscillate, and
    `adapt_recover_ticks` clean ticks required before any increase. Applied
    through the codec's encoder-update path, so there is no re-INVITE, no
    renegotiation and no gap in the audio. Runtime control:
    `echosdk_set_adaptive_bitrate()`, `echosdk_call_set_bitrate()`.
  - `cfg.opus_expected_loss_pct` supplies the `opus_packet_loss` the opus
    module needs. `cfg.opus.fec` alone only *permitted* in-band FEC: the
    encoder sizes its redundant LBRR frame from this percentage and baresip's
    decoder gates FEC reconstruction on it being non-zero, so `opus.fec` on its
    own concealed nothing.

- **Keepalive is a real mechanism instead of a dead config field** —
  `cfg.keepalive_interval` had a documented default of 30000 and was read by no
  code at all. It now drives a SIP OPTIONS probe on that period, which does two
  jobs: it refreshes the UDP NAT binding — carrier NAT drops idle mappings
  after 30–180 s, while the default `reg_expires` is 3600 — and its answer, or
  absence, tests reachability. Any response counts, including 405: a proxy that
  refuses OPTIONS still received it. On failure the account reports
  `ECHOSDK_ERR_TIMEOUT` and, with the new `cfg.keepalive_reregister` (default
  true), re-REGISTERs immediately rather than leaving an unreachable
  registration nominally healthy for up to an hour. Suppressed while a call is
  up on the account: RTP already holds the binding open, and an extra request
  competing with media for a congested uplink is exactly wrong.
  `echosdk_account_keepalive_now()` runs the same probe on demand, for a
  foreground or push-wake handler.

- **`cfg.sip_timer_b_ms` / `cfg.sip_timer_f_ms` now do something** — both were
  dead fields. A request onto a black-holed link gets no response at all, and
  RFC 3261 bounds that at 64·T1 = 32 s, which in libre is a compile-time
  constant with no runtime knob — so an app that would rather fail in eight
  seconds and offer to retry had nowhere to say so. `sip_timer_b_ms` now arms
  an SDK-side watchdog that cancels an outgoing call still in `CALLING` with
  408, surfacing as `ECHOSDK_CALL_FAILED` / `ECHOSDK_ERR_TIMEOUT`; only
  `CALLING` is watched, because a call that reached `RINGING` has proven the
  path works and how long to let it ring is a product decision.
  `sip_timer_f_ms` now bounds the registration watchdog, which was hardcoded to
  35 s. `sip_t1_ms` / `sip_t2_ms` remain compile-time in libre and are
  documented as informational rather than left to look configurable.

- **RFC 3263 SRV failover** — `src/dns.c` implemented the whole NAPTR→SRV chain
  and `bsdk_dns_resolve()` had no callers, so every retry re-sent to the host
  that had just timed out and a down primary proxy was never failed over to the
  secondary the records exist to name. Registration now resolves the target
  list once per account and advances one target per failed attempt, in
  (priority, weight) order, wrapping at the end. `cfg.dns_srv_failover`
  (default true) gates it; it is skipped when there is no ordered list to walk
  or when an operator has already chosen — a pinned `outbound_proxy`, an IP
  literal, an explicit port, or a WS/WSS URL. Results are now sorted, which
  they were not: the handlers appended them in DNS-answer order, so the "first"
  target was an accident of packet timing.

- **`cfg.reg_retry_jitter`** (default 0.2) — the retry backoff was documented as
  "exponential backoff with jitter" and had none. Every device that lost the
  same network ran the same schedule from the same instant, so the fleet arrived
  at the registrar as one burst, and because the schedules never diverged the
  herd re-formed on every subsequent attempt.

- **`cfg.net_ice_handover`** and `ECHOSDK_NET_CALL_ICE_STALE` — an ICE call
  cannot re-gather candidates mid-call (baresip fixes the local ufrag/pwd when
  the media session is allocated and its mnat update handler re-runs
  `icem_update()` rather than re-gathering), so the handover re-INVITE
  necessarily carries stale candidates. That cannot be fixed from outside the
  library, so instead it is made visible and bounded: the event is emitted once
  per call per handover *before* the offer goes out, and
  `ECHOSDK_ICE_HANDOVER_FAIL_FAST` reports failure after one attempt rather
  than spending `net_verify_ms` × `net_max_attempts` — 24 s of silence at the
  defaults — on an offer that cannot succeed. `max_attempts` in the event now
  reflects the budget the call is actually held to.

- `test/unit/test_fmtp_bitrate.c` — 45 assertions over the fmtp rewriting that
  makes adaptive bitrate safe, including that feeding its own output back in
  reaches a fixed point rather than growing the string on every adaptation step.

### Changed

- **Media statistics are on by default** (`cfg.stats_interval_ms` 0 → 2000) and
  the three quality-alert thresholds are set to the values their own field docs
  recommended (`mos_alert_threshold` 3.5, `loss_alert_threshold` 5.0,
  `jitter_alert_threshold` 40.0). `stats_interval_ms` is the master switch for
  RTCP accounting in baresip, so at 0 the loss/jitter/RTT/MOS fields read back
  as zero from `echosdk_call_get_stats()` too — an app that never set it got no
  quality signal anywhere and nothing said so. Everything added above reads
  from this tick. Set it to 0 to opt out.

- A `408` on a call now maps to `ECHOSDK_ERR_TIMEOUT` rather than
  `ECHOSDK_ERR_INVAL`. It is what the new setup watchdog cancels with, and what
  a proxy sends when its own transaction timed out; neither is the
  malformed-request sense of `INVAL`.

### Fixed

- **Python: every config field after `cfg.aec_mode` was written to the wrong
  offset.** `echosdk_aec_mode_t` is a packed 1-byte enum, and the cffi header
  generator strips `__attribute__((packed))` along with every other attribute,
  so cffi widened the field to 4 bytes and shifted the 29 members that follow —
  `stats_interval_ms`, the whole `net_*` group, the retry policy, the session
  timers. `sizeof` still matched by padding coincidence, which is why the
  `struct_size` check in `echosdk_init()` never caught it.
  `bindings/python/build.sh` now passes `-DECHOSDK_NO_PACKED_ENUM=1`, selecting
  the `uint8_t` typedef that preserves the layout, the same way
  `bindings/flutter/ffigen.yaml` already did. Verified by comparing every field
  offset and every struct size against the C compiler: 0 mismatches.

- **Python: `configure(transport="udp")` raised `TypeError`.** The string form is
  what `configure()` documents and what `create_account()` already accepted, but
  the global path assigned it straight to an integer field, so the documented
  call failed from inside `_ensure_init()`. `transport` and `media_enc` are now
  translated there too.

- `bindings/flutter`: `EchoSDKConfig.statsIntervalMs` and the three alert
  thresholds defaulted to 0 and were written unconditionally, which would have
  overwritten the new native defaults with "disabled". They now mirror the
  native defaults.

- `test/unit/Makefile`: `test_ws_pin` did not link. Its own comment says to keep
  `--wrap` and the flag was absent, so `make test` failed out of the box on the
  first target that needs the sysroot.

- **App-owned audio device** — `echosdk_audio_use_external(true)` stops the SDK opening any capture or playback device of its own (no OpenSL ES, no AudioUnit) and hands the microphone and speaker to the host app, which moves PCM across `echosdk_audio_external_push()` / `echosdk_audio_external_pull()` from whatever the platform gives it — `AudioRecord`/`AudioTrack`, `AVAudioEngine`, a WebRTC `AudioDeviceModule`, or a file for testing. `echosdk_audio_external_format()` reports the rate/channels/ptime the call negotiated. Switching is live, including mid-call, and `false` restores the platform device. Note it replaces the *device*: the platform echo cancellers the SDK relies on belong to the drivers being displaced, so an app that takes this over owns AEC too.

- **App-owned audio reaches every binding** — Flutter: `EchoSDK.useAppOwnedAudio()`, `appOwnedAudioFormat` (an `ExternalAudioFormat` with `samplesPerFrame`), `appOwnedAudioActive`. C++: `SDK::use_external_audio()`, `audio_push()`, `audio_pull()`, `audio_format()`, `audio_external_active()`. Python: `use_external_audio()`, `external_audio_push()` (any S16LE buffer — `bytes`, `array('h')`, numpy), `external_audio_pull()`, `external_audio_format()`, `external_audio_active()`. The Dart facade deliberately omits push/pull: the realtime loop belongs in the plugin's native layer, because a GC pause on the capture path is a dropped frame.

- **The software echo suppressor is available again while the app owns the device** — it was vetoed wherever the platform driver cancels echo, but that driver is exactly what the app-owned device displaces, so the veto left mobile with no canceller anywhere in the chain and no way to ask for one. It is now offered as a fallback, still **off** by default (an app that takes the device is expected to capture through the platform voice path itself, and ducking its TX silently would be the SDK fighting the platform), and forced off again when the platform device returns so the two never stack.

- `bindings/flutter/example/integration_test/app_owned_audio_test.dart` — on-device gate test for the app-owned device: registers, calls an echo service, and asserts on frame counters and capture level from the native engine rather than on call state, because "connected but silent" is the failure this feature exists to prevent and call state cannot see it. The engine reports `pushFrames`, `pullFrames`, `pushErrors`, `capturePeak` and `nonSilentFrames` through `appOwnedAudioStatus()` for exactly this purpose.

- `bindings/python/examples/external_audio.py` — pushes a synthesised tone as the microphone and writes the far end to a WAV, so the app-owned device can be verified end to end on a desktop with no audio hardware.
- `test/unit/test_audio_external.c` and `test/audio_external_test.c` — unit and gate tests for the device, covering each of the regressions below.

### Fixed

- **After a reconnect, a WebSocket call could never be ended by the far end** — handover skips the re-INVITE when the local address has not changed ("same path"), which is correct for address-routed transports. A WebSocket client has no listening port, though: its Contact is the RFC 7118 placeholder `sip:user@<ip>:9;transport=wss` and the server reaches it by remembering which WebSocket the dialog's requests arrived on. A transport reset always builds a *new* WebSocket, so that association went stale on every reconnect even when the IP never moved — media kept flowing and the app's own BYE still got out (it is routed, not received), but an inbound BYE had nowhere to be delivered and the call hung in `ESTABLISHED` for the rest of the session. WS/WSS calls now re-INVITE on a transport reset regardless of the address, which re-binds the dialog to the live connection. Address-routed transports keep the shortcut and gain no extra signalling. Verified on device: reconnect mid-call, then the far end hangs up → `CALL_MIGRATING` → `CALL_MIGRATED` → inbound BYE delivered → `ENDED`.

- **Every remote hangup was reported as a call failure** — libre signals a peer-initiated termination by passing `ECONNRESET` to the session close handler: its BYE handler answers 200 OK and then calls `sipsess_terminate(sess, ECONNRESET, NULL)` (`re/src/sipsess/listen.c`), and the peer-CANCEL path does the same. baresip's close handler tests `err` before `msg`, so a perfectly normal hangup arrived as `"Connection reset by peer [104]"` and was classified `ECHOSDK_CALL_FAILED` with `ECHOSDK_ERR_TRANSPORT`. Apps that branch on `ENDED` left the call on screen, and every hangup looked like a network fault. A call that was ESTABLISHED and closes with a transport error is now reported as `ECHOSDK_CALL_ENDED` with the reason `"Remote hangup"`; SIP failures (486, 603, 4xx/5xx/6xx) and pre-answer errors are untouched. Verified on device against a live PBX: remote BYE → `ended`, 486 → `failed`, 603 → `failed`, local hangup → `ended`.

- **WebSocket calls opened a second connection for every dialog** — RFC 7118 gives a WS client one connection and routes everything over it, but libre routes by address: for an in-dialog request it resolves the dialog's Route/Contact and looks the connection up by peer address, so a target that is not the registration peer gets a whole new WebSocket. Behind a reverse proxy that target is the server's own loopback address (Asterisk advertises `127.0.0.1:8088` in Record-Route), so every call opened a second socket that existed only for the life of the dialog. A loopback WS destination was rewritten to the address the registration is already connected to, so libre finds and reuses the existing connection. (Since superseded: in-dialog requests are routed over the registration flow itself — RFC 7118 §B.2 — which makes the connection lookup match without any address rewrite, on every platform.)

- **Android app-owned audio captured near-silence on every stereo call** — the reference engine asked `AudioRecord` for the channel count the codec negotiated, and Opus routinely negotiates 48 kHz **stereo**. Phones have no stereo voice-communication capture path: `CHANNEL_IN_STEREO` yields an `AudioRecord` that initialises, reads without error and returns audio roughly 36x too quiet — a call that looks perfectly healthy by frame count and that the far end cannot hear. Capture is now always mono and up-mixed to the negotiated layout before `push()`. Found on a Galaxy A54 (Android 16) against a live Asterisk echo test; peak capture level went from 0.0017 to 0.062 of full scale on the same call.

- **App-owned audio: `push()` never returned when the call negotiated a ptime under 20 ms** — the capture buffer's pre-fill threshold was a fixed 20 ms while the drain loop released one *ptime* at a time. Below the threshold `aubuf_read_auframe()` returns silence without draining, so the loop's condition stayed true forever: the app's realtime capture thread spun holding the device lock, and the SIP thread then deadlocked behind it on teardown. A peer offering `a=ptime:10` was enough. The threshold is now one frame, and the loop also breaks if a read fails to drain — a spin there is too expensive to leave to one invariant.

- **App-owned audio: microphone audio was discarded at the start of every call** — the capture FIFO was left in `aubuf`'s live mode, whose first read drops the whole backlog down to one frame to cut latency. That is right for a playback jitter buffer and wrong for a capture path, where the backlog is microphone audio the app has already handed over: the first ~4 frames of every call went missing. Live mode is now off for capture.

- **App-owned audio: ending the second of two calls took the microphone for the rest of the session** — the selected device was a bare pointer, cleared only when the closing device was the selected one. Closing the newer of two open devices therefore left the older one alive but unreachable and `push()` returned `ENODEV` forever, so call-waiting and hold/resume both silenced the surviving call. Devices are tracked in a list now: the newest still wins, and closing it falls back to whichever is still open.

- **App-owned audio did not survive a stack restart** — nothing called `bsdk_audio_external_close()` at shutdown, while `baresip_init()` re-initialises the device lists on the way back up. The driver believed it was still registered, skipped re-registering, and `echosdk_audio_use_external(true)` went on **returning 0** while the module name resolved to nothing — every call after a restart came up with no audio at all, with no error anywhere. Shutdown now closes the driver, and the lock is created once per process and never destroyed, so an app's realtime thread racing shutdown cannot land on a destroyed mutex.

- **The microphone was processed by filters the app had switched off** — `aufilt_register()` enables what it registers, and the SDK's TX filters were then enabled with `if (flag) aufilt_enable(name, true)`. A one-way enable never disables, so `bsdk_ns`, `bsdk_agc` and `bsdk_aec` all ran regardless of config: noise suppression and AGC ran with `ns`/`agc` at their default `false`, and the echo suppressor ran under `ECHOSDK_AEC_OFF`. Stacked on the TX path that is a −20 dB noise gate, a normaliser with a 0.1 gain floor, and a 16.5 dB duck whenever the far end has audio — worst case a caller the other end cannot hear. All five filters now take the flag directly.

- **Mobile: the software echo suppressor fought the hardware one** — Android captures through the `VOICE_COMMUNICATION` recording preset and iOS through `VoiceProcessingIO`, so the device cancels the echo before the SDK sees a sample. `aec_mode` still defaulted to `SUPPRESSOR` there (in C and in Flutter), which ducked TX by 16.5 dB every time the far end had any audio above roughly −44 dBFS and took ~0.85 s to release — removing no echo that was still present, and half-duplexing a call the hardware had already made full-duplex. The suppressor is now skipped wherever the platform driver cancels echo, including the runtime `echosdk_set_aec_mode()` path; the stock `opensles` fallback (generic preset, no platform AEC) still gets it.

- **Android: calls came up with audio in one direction only** — the platform was put into `MODE_IN_COMMUNICATION` from the call-established event, over an async method-channel hop, while the native core opened its OpenSL streams the moment media started. Android fixes a stream's routing when it is *created*, so whenever the mode landed second the playback stream stayed on the voice-call domain with nothing routing it: RTP flowed both ways, the far end heard everything, and the local user heard silence. `Account.call()` and `Call.answer()` now claim the audio session before starting media (`answer()` returns a `Future`, awaiting it is optional), and the event path is only a backstop.

- **WebSocket calls dropped after ~32 seconds once a second account had existed** — connection pinning (which keeps in-dialog requests on the WebSocket flow instead of dialling the dialog's Record-Route, an unroutable `127.0.0.1:8088` behind a reverse proxy) switched off when two accounts named different servers, and stayed off for the life of the process. One stale account — the mistyped login attempt an app forgot to destroy — was enough to lose the ACK for every later call and have the server tear each dialog down on its ~32 s timer with media still flowing. Servers are refcounted now: pinning returns as soon as one is left, and while they are ambiguous a target that cannot work (a loopback address belonging to no account) is redirected rather than dialled. Covered by `test/unit/test_ws_pin.c`.

- **Android: gaps in the audio the far end heard** — the OpenSL capture queue was primed with one buffer instead of both, so the recorder had nowhere to write between a completion callback and the next `Enqueue` and AudioFlinger dropped that slice of input, ~10 ms at a time, for the whole call.

- **Flutter example re-registered by leaking accounts** — tapping Register built a new account and dropped the old one on the floor, still retrying its own registration forever and still counting against WebSocket pinning. It destroys the previous account first.

- **iOS: init seized the audio session from CallKit** — `echosdk_init()` called `[AVAudioSession setActive:YES]` unconditionally, so merely starting the SDK (at app launch, or on a PushKit wake while CallKit was still reporting the call) took the exclusive PlayAndRecord route. Apple requires `CXProvider` to be the only activator, in `provider(_:didActivateAudioSession:)`. Activation is now controlled by `platform_audio_activate`; CallKit apps set it to `false` and get category + mode without activation.

- **Global codec list was never marshalled from Flutter** — `EchoSDKConfig.audioCodecs` existed in Dart and stopped there: nothing wrote it into the native config, so a global codec preference was silently ignored (per-account `AccountConfig.audioCodecs` did work). The global list is now a first-class native field and reaches baresip in order.
- **Re-entry on a live stack was unusable** — a host that loses its own runtime while the process survives (Android headless Flutter engine destroying the Dart isolate between push wakeups) came back to a stack that was still up, and `echosdk_init()` could only answer `ECHOSDK_ERR_ALREADY`: the event sink still pointed at the dead runtime and the handles it held were gone. There is now a reattach path — see "Reattaching to a live stack" in `docs/api/overview.md`.

### Added

- `platform_audio_activate` in `echosdk_config_t` (default `true`) — whether `echosdk_init()` activates the platform audio session it configures. iOS only; other platforms ignore it. Flutter: `EchoSDKConfig.platformAudioActivate`, normally paired with `EchoSDK.start(manageAudioSession: false)`.
- `audio_codec_names[8][32]` + `audio_codec_name_count` in `echosdk_config_t` — the global counterpart to the per-account name list, so codecs with no `echosdk_codec_t` constant (`"g729"`) and any codec a loaded module registers can be selected globally. Precedence: account names → account enums → global names → global enums.
- `echosdk_is_initialized()` — is the stack up in this process.
- `echosdk_set_event_handler(cb, userdata, deliver_owned_events)` — re-point event delivery at a new consumer, or park it with `NULL`, without tearing the stack down.
- `echosdk_account_foreach()`, `echosdk_account_get_aor()`, `echosdk_account_get_reg_state()` — re-derive account handles and their state instead of creating duplicates.
- `echosdk_call_get_account()`, `echosdk_call_get_state()` — pair with the existing `echosdk_call_foreach()` to recover a call that arrived while nobody was listening (events are dropped across the gap; the call itself is not).
- Flutter: `EchoSDK.start()` reattaches to a running stack by default (`reattachIfRunning: false` to opt out), `EchoSDK.reattached`, `EchoSDK.accounts` / `EchoSDK.calls`, `EchoSDK.detach()`, `Account.aor`, `Account.regState`.
- `test/reattach_test.c` — gate test for both fixes.

---

## [1.4.1] — 2026-05-14

### Fixed

- **Windows build — MSBuild tlog locking** — sub-project builds (`libre`, `baresip`, `opus`) could fail with "The requested operation cannot be performed on a file with a user-mapped section open" when Windows Defender (or any AV) held `.tlog` dependency-tracking files mapped. Fixed by passing `CMAKE_VS_GLOBALS=TrackFileAccess=false` to every `ExternalProject_Add` so MSBuild skips the tlog write step entirely.
- **Windows DLL — missing exports** — `echosdk_strerror` and `echosdk_version` were absent from `echosdk.def` because the DEF-generation regex required whitespace before the function name, which doesn't match pointer-returning signatures (`const char *fn(`). Fixed regex to accept either whitespace or `*` as the separator.

### Changed

- **Windows build consolidation** — `generate-def.ps1` and `relink-dll.ps1` are removed; their logic is now inlined into `scripts/build-windows.ps1`. One script does everything: configure → build → install → DEF → link DLL.
- **Windows build script** — auto-detects vcpkg at common locations (`C:\vcpkg`, `D:\vcpkg`, `%USERPROFILE%\vcpkg`) when `VCPKG_ROOT` is not set.

---

## [1.4.0] — 2026-05-11

### Fixed

- **RTCP ICE crash on incoming calls** — when ICE is enabled, baresip opens a second ICE session for RTCP. A failure in that session caused a crash on incoming calls. Fixed by enabling RTCP multiplexing (RFC 5761) by default: RTCP shares the RTP port, so no separate RTCP ICE session is created.

### Added

- `rtcp_mux` field in `echosdk_config_t` (default `true`). Set to `false` to opt out of RTCP-mux and revert to separate RTCP ports (not recommended when ICE is enabled).

---

## [1.3.0] — 2026-05-10

### Added

#### Push notifications (RFC 8599 + REGISTER-only headers)

- `echosdk_push_provider_t` enum — `ECHOSDK_PUSH_PROVIDER_NONE` / `APNS` / `APNS_SANDBOX` / `FCM`.
- Three new fields in `echosdk_account_config_t`: `push_provider`, `push_token`, `push_param`. When set, the SDK encodes RFC 8599 `pn-provider` / `pn-prid` / `pn-param` URI parameters **inside** the Contact angle brackets on every REGISTER. Self-hosted servers (Kamailio, drachtio, FreeSWITCH) read these from the registrar and use them to wake the device via APNs or FCM.
- `echosdk_account_set_push_token(acct, token)` — update the push token at runtime without re-creating the account. Re-registers immediately when safe (defers if a REGISTER/UNREGISTER transaction is in flight or a retry backoff is pending). Pass `NULL` to clear all push params.
- `echosdk_account_add_register_header(acct, name, value)` — attach a custom SIP header to REGISTER requests **only**. Does not appear on INVITE, BYE, REFER, or any other request. Use this for hosted / vendor servers (Twilio, Plivo, Asterisk PJSIP) that dispatch push via non-standard headers rather than RFC 8599.
- Flutter binding: `createAccount()` `pushProvider` / `pushToken` / `pushParam` named params; `Account.setPushToken()`; `Account.addRegisterHeader()`.
- Python binding: `PUSH_PROVIDER_NONE/APNS/APNS_SANDBOX/FCM` constants; `SDK.create_account()` `push_provider` / `push_token` / `push_param` params; `Account.set_push_token()`; `Account.add_register_header()`.
- C++ binding: same enum, new config fields, `Account::set_push_token()`, `Account::add_register_header()`, `SDK::create_account()` overload.

---

## [1.2.0] — 2026-05-10

### Added

#### Audio recording
- `echosdk_call_record_start(call, path)` — record call audio to a single mixed WAV file (PCM S16LE). Both the received (RX) and sent (TX) audio are clip-summed into one stream.
- `echosdk_call_record_stop(call)` — stop recording and finalize the WAV header with correct sizes. The file is also closed automatically if the call is destroyed.
- Recording runs independently of the PCM media tap — both can be active simultaneously on the same call.

#### Registration retry control
- `echosdk_account_set_retry_policy(acct, initial_ms, max_ms, backoff, max_attempts)` — override the retry policy for a specific account at runtime without recreating it. Overrides the global `reg_retry_*` fields in `echosdk_config_t` for that account only.
- `echosdk_account_cancel_retry(acct)` — cancel a pending retry timer and reset the attempt counter. The account stays in `FAILED` state; call `echosdk_account_register()` to restart manually.
- `echosdk_account_retry_now(acct)` — skip the current backoff delay and re-register immediately. Resets the attempt counter.

---

## [1.1.0] — 2026-05-08

### Fixed — Critical call flow bugs

#### Incoming calls
- **AOR construction** now includes port and IPv6 brackets. Previously, registrations against a non-default SIP port received `404 Not Found` on every inbound call because Asterisk/FreeSWITCH matched AOR by exact string.
- **Silent INCOMING_CALL drop under load** — the call wrapper is now registered only after a queue slot is confirmed. Previously under memory pressure the wrapper was registered but the event dropped, leaving baresip in EARLY state with the app unaware.
- **Race on fast cancel** — if the caller cancels before the event thread delivers INCOMING_CALL, the CLOSED event no longer nulls the call handle first. `echosdk_call_answer()` no longer spuriously returns `ENOENT`.
- **Incoming call destructor** — tap lock and custom header list now have a proper destructor; previously leaked on every received call.

#### Outgoing calls
- **Orphaned baresip call on alloc failure** — if wrapper `mem_alloc` fails after `ua_connect` succeeds, the SIP INVITE is now cancelled with 500. Previously the call rang on the wire while the app received `ENOMEM` with no handle.

#### Registration
- **SIP error code parsing** — `strstr(reason, "5")` replaced with proper 3-digit extraction; 415, 451, 486 are no longer misclassified as 5xx. 407 Proxy Auth now correctly maps to `ECHOSDK_ERR_AUTH`.

#### NAT / ICE
- **STUN + TURN co-existence** — when both `stun_server` and `turn_server` are configured, TURN takes precedence as the active ICE server. Previously configuring TURN always silently overwrote the STUN setting.

#### Memory
- **Call wrapper leak** — wrappers are freed after `ECHOSDK_EV_CALL_CLOSED` is delivered. Previously every completed call leaked one wrapper.
- **Stats queue bypass** — stats events now respect `ev_queue_max` and update `ev_queue_len`; previously they bypassed the limit and could grow the queue without bound on long calls.

#### Mutex safety
- **Init-time mutex lock** (UB on Windows/RTOS) — `mtx_lock` before `mtx_init` replaced with `bsdk_call_global_init()` called from `echosdk_init`.
- **Shutdown mutex order** — `mtx_destroy` now called after `mtx_unlock` in `bsdk_call_global_reset`.

#### IPv6
- **AOR formatting** — AOR now correctly produces `sip:user@[2001:db8::1]:5060;transport=tls`.
- **URI parsing** — `parse_account_uri` now skips `[…]` brackets before scanning for `:port`, preventing the host from splitting at the first colon inside an IPv6 literal.

---

### Changed — Build system
- Platform build scripts (`build-linux.sh`, `build-android.sh`, `build-macos.sh`, `build-windows.ps1`) now produce **both** the static archive and the shared library in one run. No separate shared-lib scripts required.
- Shared libraries are now **fully self-contained** — no extra packages needed at runtime:
  - **Linux** — OpenSSL, zlib, pthreads, libm, libresolv baked in as `.a`; only `libdl` / `libc` (glibc) remain dynamic.
  - **macOS** — Homebrew OpenSSL `.a` embedded; only Apple system frameworks remain.
  - **Windows** — vcpkg `libssl.lib`, `libcrypto.lib`, `zlib.lib` linked directly into the DLL.
  - **Android / iOS** — mbedTLS was already merged in; no change.
- Removed: `scripts/build_shared_linux.sh`, `scripts/build_shared_macos.sh`, `scripts/build_shared_android.sh`, `scripts/build_shared_windows.sh`.

---

### Changed — Bindings
- C / C++ compile commands simplified — no extra `-l` flags needed when linking against the self-contained shared lib.
- **One-command setup scripts** added for every language binding — each `build.sh` builds the SDK if needed and then installs/compiles the binding in one step:
  - `bash bindings/cpp/build.sh`
  - `bash bindings/python/build.sh`
  - `bash bindings/nodejs/build.sh`
  - `bash bindings/rust/build.sh`
- **Python** — `_loader.py` now auto-discovers `echosdk.so` in `dist/<platform>/<arch>/` when running from a source checkout. Manual `LD_LIBRARY_PATH` or file copy no longer required.
- **Rust** — `build.rs` auto-selects the correct `dist/` sub-directory (`linux/x86_64`, `linux/arm64`, `macos/universal`, `windows/x64`) based on the Cargo target. `ECHOSDK_LIB_DIR` export no longer required for native builds.
- **Node.js** — `binding.gyp` replaced fragile relative `-L` paths with absolute paths resolved at build time via `node -p`. Sets `-Wl,-rpath` so the addon finds `echosdk.so` at runtime without `LD_LIBRARY_PATH`. Supports `ECHOSDK_DIST_DIR` env var to override.
- **C++** — `CMakeLists.txt` auto-detects platform and architecture (`linux/x86_64`, `linux/arm64`, `macos/universal`, `windows/x64`) instead of hardcoding `linux/x86_64`.
- Quickstart docs updated for all languages to reflect one-command setup.

---

## [1.0.0] — 2025-05-08

### Added

#### Core SDK
- SIP UA with full INVITE/BYE/REGISTER flow
- Transports: UDP, TCP, TLS, WebSocket (WS), secure WebSocket (WSS)
- Media encryption: none, SDES (RFC 4568), DTLS-SRTP (RFC 5764)
- ICE / STUN / TURN NAT traversal
- Audio codecs: Opus, G.711 (PCMU/PCMA), G.722
- Audio processing: AEC, noise suppression, AGC
- DTMF via RFC 4733 RTP events
- Blind and attended call transfer (REFER)
- SIP MESSAGE (in/out of dialog)
- Presence: PUBLISH and SUBSCRIBE/NOTIFY
- MWI (message-waiting indication)
- 100rel / PRACK support (RFC 3262)
- Session timers (RFC 4028)
- Multi-account support with per-account config overrides
- Custom SIP headers (per-account and per-call)
- Audio device selection (input/output by name)
- PCM media tap (per-call RX/TX audio frame callback)

#### Observability
- RTCP media stats: loss, jitter, RTT, bandwidth
- MOS scoring: E-Model (ITU-T G.107) and simplified
- SIP trace (per-message TX/RX capture)
- SDP negotiation trace (codec + crypto result)
- Pcap capture (Wireshark-compatible output)
- Configurable log levels (error/warn/info/debug)
- Registrar warning events

#### Platforms
- Linux x86_64 (OpenSSL)
- macOS universal (x86_64 + arm64, OpenSSL)
- Windows x64 (OpenSSL via vcpkg)
- Android (arm64-v8a, armeabi-v7a, x86_64, mbedTLS)
- iOS device + simulator (mbedTLS)

#### Build system
- CMake with ExternalProject (re + baresip + optional mbedTLS)
- Static archive merge (`echosdk.a`) via libtool / lib.exe / ar MRI
- `ECHOSDK_SHARED` option for shared library output (`.so` / `.dylib` / `.dll`)
- Platform build scripts: `build-linux.sh`, `build-macos.sh`, `build-ios.sh`, `build-android.sh`, `build-windows.ps1`

#### C++ binding
- Header-only RAII wrapper (`bindings/cpp/echosdk.hpp`)
- `SDK`, `Account`, `Call` classes with automatic resource cleanup

#### Python binding
- cffi-based wrapper (`bindings/python/`)
- `SDK`, `Account`, `Call` Pythonic classes
- Event delivery via `queue.SimpleQueue` generator pattern
- Clean header preprocessing via `generate_clean_header.sh`

#### Rust binding
- `EchoSDK-sys` crate (bindgen auto-generated raw FFI)
- `EchoSDK` crate (safe wrapper with `Result` error handling)
- Event delivery via `std::sync::mpsc` channel

#### Node.js binding
- N-API C++ addon (`bindings/nodejs/`)
- `SDK`, `Account`, `Call` JavaScript classes
- TypeScript declarations (`.d.ts`)
- `node-gyp` build configuration

#### Flutter / Dart binding
- dart:ffi + ffigen wrapper (`bindings/flutter/`)
- `EchoSDK`, `Account`, `Call` Dart classes
- Event delivery via `Stream<EchoSDKEvent>` with `StreamController`
- Multi-platform library loading (Android, iOS, macOS, Windows, Linux)

#### Documentation
- API reference: overview, config, accounts, calls, media, events, observability
- Quickstart guides: C/C++, Python, Rust, Node.js, Flutter
- How-to guides: NAT traversal, TLS/WSS, multi-account, WebRTC browser interop, debugging
- Configuration examples (`accounts_example.json`)
