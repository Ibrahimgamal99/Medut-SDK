# patch-baresip-sources.cmake
# Apply EchoSDK's baresip patches at build time.  Six jobs:
#   1. Disable baresip's executables (baresip_exe, test/, webrtc/) everywhere.
#   2. Make dtls_srtp recover from a handshake that is perturbed or stalls.
#   3. Keep a re-REGISTER from un-REGISTERing the binding it refreshes.
#   4. Start RTCP on a DTLS call that multiplexes it (rtcp-mux).
#   5. Let Opus FEC follow the link: expected loss can fall back to 0, and
#      received FEC is used whatever our own encoder expects.
#   6. Conceal a burst of loss when the audio device needs it, not when the
#      next packet happens to arrive — no silence, no added delay.
# Usage: cmake -DSOURCE_DIR=path/to/baresip -P patch-baresip-sources.cmake
#
# third_party/baresip is fetched by scripts/fetch-third-party.sh at a pinned
# revision and is gitignored, so an in-place patch cannot leak into upstream
# history.  Each job is idempotent behind its own marker and fails loudly when
# a baresip bump moves the text it splices at, rather than silently building
# without the fix.
#
# The SDK owns the public websock_connect() and sip_dialog_route() names:
# libre's definitions are renamed to __real_* (cmake/patch-re-sources.cmake)
# and the only provider of the public symbols is EchoSDK's src/ws_path.c.
# baresip's executables link libre directly without ws_path.c, so they stop
# linking — and the SDK only ever consumes libbaresip.a anyway.  This is the
# executable-disabling half of what fix-msvc-baresip.cmake did for MSVC,
# promoted to every platform now that the rename is unconditional.
#
# Idempotent behind the "# EchoSDK-disabled" marker (the legacy
# "# EchoSDK-msvc-disabled" marker from fix-msvc-baresip.cmake also counts, so
# a Windows tree patched by the older script is left alone).

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR must be defined")
endif()

set(BARESIP_CMAKE "${SOURCE_DIR}/CMakeLists.txt")
if(NOT EXISTS "${BARESIP_CMAKE}")
  message(FATAL_ERROR "patch-baresip-sources: ${BARESIP_CMAKE} does not exist")
endif()

file(READ "${BARESIP_CMAKE}" CONTENT)

string(FIND "${CONTENT}" "# EchoSDK-disabled" _ALREADY)
string(FIND "${CONTENT}" "# EchoSDK-msvc-disabled" _ALREADY_MSVC)
if(NOT _ALREADY EQUAL -1 OR NOT _ALREADY_MSVC EQUAL -1)
  message(STATUS "patch-baresip-sources: ${BARESIP_CMAKE} already patched (executables disabled)")
  set(_SKIP_EXES TRUE)
endif()

# A plain return() here would take the dtls_srtp job below down with it on
# every rebuild — the tree stays patched, so this branch is the common case.
if(NOT _SKIP_EXES)
foreach(_anchor
    "add_subdirectory(webrtc)"
    "add_subdirectory(test)"
    "add_executable(baresip_exe src/main.c)"
    "set_target_properties(baresip_exe PROPERTIES OUTPUT_NAME baresip)"
    "install(TARGETS baresip_exe baresip")
  string(FIND "${CONTENT}" "${_anchor}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR
      "patch-baresip-sources: anchor '${_anchor}' not found in ${BARESIP_CMAKE}. "
      "The pinned baresip revision has moved; re-derive this patch — building "
      "without it fails at link (baresip's executables lack ws_path.c).")
  endif()
endforeach()

string(REPLACE
  "add_subdirectory(webrtc)"
  "# add_subdirectory(webrtc)  # EchoSDK-disabled"
  CONTENT "${CONTENT}")
string(REPLACE
  "add_subdirectory(test)"
  "# add_subdirectory(test)  # EchoSDK-disabled"
  CONTENT "${CONTENT}")
string(REPLACE
  "add_executable(baresip_exe src/main.c)"
  "add_executable(baresip_exe EXCLUDE_FROM_ALL src/main.c)  # EchoSDK-disabled"
  CONTENT "${CONTENT}")
# The exe's OUTPUT_NAME collides with the library target's name: Ninja resolves
# `--build --target baresip` to the FILE named baresip (the excluded exe) and
# builds it anyway. Rename the never-shipped exe so the target name means the
# library on every generator.
string(REPLACE
  "set_target_properties(baresip_exe PROPERTIES OUTPUT_NAME baresip)"
  "set_target_properties(baresip_exe PROPERTIES OUTPUT_NAME baresip_exe)  # EchoSDK-disabled"
  CONTENT "${CONTENT}")
string(REPLACE
  "install(TARGETS baresip_exe baresip"
  "install(TARGETS baresip  # EchoSDK-disabled baresip_exe"
  CONTENT "${CONTENT}")

file(WRITE "${BARESIP_CMAKE}" "${CONTENT}")
message(STATUS "patch-baresip-sources: executables disabled in ${BARESIP_CMAKE}")
endif()

# ---------------------------------------------------------------------------
# Job 2: dtls_srtp — a handshake that is perturbed or stalls must recover
#
# media_start() latches `started` and nothing ever clears it, so the module
# starts a DTLS association exactly once per stream for the life of a call.
# Two consequences, both reached on-device (2026-08-26, inbound call over WSS
# behind a NAT presenting more than one egress IP):
#
#   - A re-negotiation that reverses the DTLS role is decoded into `st->active`
#     and then discarded, because media_start() returns early.  Each side ends
#     up waiting for the other to be the client.
#   - A handshake that simply never completes — a ClientHello lost while the
#     remote address moved under it, a peer that restarted its own association
#     across a re-INVITE — is never retried.
#
# Either way `menc_secure` is never set, stream_is_ready() stays false,
# audio_update() is never called, and the call sits fully established with
# tx 0 / rx 0 and no audio in either direction while signalling looks perfect.
#
# src/ice/ice_mod.c stops the SDK from causing the first case itself (it holds its
# candidate re-offer until the handshake finishes).  These patches make the
# module survive the case when something else causes it.
#
# The `setup` hunk goes further and stops the SDK from needing to survive it on
# an inbound call at all: the answerer advertises `a=setup:passive`, so the peer
# is the side that sends the ClientHello and our media never has to be accepted
# from an address the peer was not told about.  Measured on-device (2026-08-31):
# 9 of 9 inbound calls whose ICE-selected candidate differed from the signalled
# one came up silent under the old `active` answer, against a clean outbound
# call in the same capture, where the peer connects to us.  The `role` hunk
# keeps the advertised role and the role taken in step.
#
# The hunks live as verbatim .old/.new file pairs in cmake/patches/ rather than
# as inline strings: the replacement is ~90 lines of C, and CMake escaping of
# tabs, semicolons and quotes at that size is its own source of bugs.
# ---------------------------------------------------------------------------

set(DTLS_SRTP_C "${SOURCE_DIR}/modules/dtls_srtp/dtls_srtp.c")
if(NOT EXISTS "${DTLS_SRTP_C}")
  message(FATAL_ERROR "patch-baresip-sources: ${DTLS_SRTP_C} does not exist")
endif()

file(READ "${DTLS_SRTP_C}" DCONTENT)
string(FIND "${DCONTENT}" "EchoSDK-patched" _DTLS_DONE)

if(NOT _DTLS_DONE EQUAL -1)
  message(STATUS "patch-baresip-sources: ${DTLS_SRTP_C} already patched (dtls recovery)")
else()
  set(_HUNKS state setup recovery role estab destr)

  # Verify every anchor before splicing any of them, so a baresip bump can
  # never leave the file half-patched.
  foreach(_h ${_HUNKS})
    set(_of "${CMAKE_CURRENT_LIST_DIR}/patches/dtls_srtp-${_h}.old")
    set(_nf "${CMAKE_CURRENT_LIST_DIR}/patches/dtls_srtp-${_h}.new")
    if(NOT EXISTS "${_of}" OR NOT EXISTS "${_nf}")
      message(FATAL_ERROR "patch-baresip-sources: missing hunk files for '${_h}'")
    endif()
    file(READ "${_of}" _old)
    string(FIND "${DCONTENT}" "${_old}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR
        "patch-baresip-sources: anchor not found in ${DTLS_SRTP_C} for hunk "
        "'${_h}'.\nThe pinned baresip revision has moved this code. Re-derive "
        "cmake/patches/dtls_srtp-${_h}.old against the new revision — building "
        "without it reintroduces permanently silent calls.")
    endif()
  endforeach()

  foreach(_h ${_HUNKS})
    file(READ "${CMAKE_CURRENT_LIST_DIR}/patches/dtls_srtp-${_h}.old" _old)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/patches/dtls_srtp-${_h}.new" _new)
    string(REPLACE "${_old}" "${_new}" DCONTENT "${DCONTENT}")
  endforeach()

  file(WRITE "${DTLS_SRTP_C}" "${DCONTENT}")
  message(STATUS "patch-baresip-sources: patched ${DTLS_SRTP_C} (dtls handshake recovery)")
endif()

# ---------------------------------------------------------------------------
# Job 3: reg.c — re-registering must not un-REGISTER the binding it refreshes
#
# reg_register() frees the account's registration client and allocates a new
# one on every ua_register(): each handover, retry and keepalive recovery.
# Freed with mem_deref(), the old client un-REGISTERs its binding on the way
# out, and with the per-instance WebSocket Contact (patch-re-sources.cmake,
# hunk N) that binding is the one the new client is about to REGISTER — so
# the AOR goes empty for a round trip, or for good when the un-REGISTER is
# deferred behind an in-flight request (hunk O).  sipreg_release() is libre's
# EchoSDK-patched free for exactly this hand-off.
# ---------------------------------------------------------------------------

set(REG_C "${SOURCE_DIR}/src/reg.c")
if(NOT EXISTS "${REG_C}")
  message(FATAL_ERROR "patch-baresip-sources: ${REG_C} does not exist")
endif()

file(READ "${REG_C}" RCONTENT)
set(_REG_OLD [=[
	failed = sipreg_failed(reg->sipreg);
	reg->sipreg = mem_deref(reg->sipreg);
	err = sipreg_alloc(&reg->sipreg, uag_sip(), reg_uri,
]=])
set(_REG_NEW [=[
	failed = sipreg_failed(reg->sipreg);
	/* EchoSDK-patched: the new client refreshes this one's binding; see
	 * sipreg_release() in libre. */
	reg->sipreg = sipreg_release(reg->sipreg);
	err = sipreg_alloc(&reg->sipreg, uag_sip(), reg_uri,
]=])

string(FIND "${RCONTENT}" "${_REG_NEW}" _REG_DONE)
if(NOT _REG_DONE EQUAL -1)
  message(STATUS "patch-baresip-sources: ${REG_C} already patched (sipreg_release)")
else()
  string(FIND "${RCONTENT}" "${_REG_OLD}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR
      "patch-baresip-sources: anchor not found in ${REG_C} for sipreg_release.\n"
      "The pinned baresip revision has moved reg_register(). Re-derive this "
      "patch — building without it lets every re-REGISTER remove its own "
      "WebSocket binding.")
  endif()
  string(REPLACE "${_REG_OLD}" "${_REG_NEW}" RCONTENT "${RCONTENT}")
  file(WRITE "${REG_C}" "${RCONTENT}")
  message(STATUS "patch-baresip-sources: patched ${REG_C} (sipreg_release)")
endif()

# ---------------------------------------------------------------------------
# Job 4: dtls_srtp — under rtcp-mux the RTP handshake secures RTCP as well
#
# call.c starts RTCP on MENC_EVENT_SECURE only when the event text names
# "RTCP", which dtls_srtp sends when the separate RTCP component's handshake
# completes.  Under rtcp-mux media_start() never starts that component — RTCP
# rides the RTP socket and is keyed by the RTP handshake (srtp.c protects both
# through the one helper) — so the only event is "audio,RTP" and RTCP is never
# started.  A DTLS call then sends no SR or RR for its whole life, unless a
# re-INVITE happens to run update_media() once the stream is secure.
#
# With no SR from us, every report block the peer sends carries LSR = 0, libre
# never computes an RTT (re/src/rtp/sess.c handle_rr_block), and rtt_ms reads
# 0 on every DTLS call; the PBX gets no RTCP from us either, so its own QoS
# has no RTT for the leg.  Upstream baresip main carries the same text.
#
# Own anchor check rather than a hunk in Job 2: that job is skipped wholesale
# on any tree already carrying "EchoSDK-patched", so a hunk added there would
# never reach an existing checkout.
# ---------------------------------------------------------------------------

file(READ "${DTLS_SRTP_C}" MCONTENT)
set(_MUX_OLD [=[
		if (re_snprintf(buf, sizeof(buf), "%s,%s",
				sdp_media_name(ds->sdpm),
				comp->is_rtp ? "RTP" : "RTCP"))
]=])
set(_MUX_NEW [=[
		/* EchoSDK-patched: under rtcp-mux this handshake keys RTCP too
		 * and there is no RTCP component to report it, while call.c
		 * starts RTCP only on an event that names it. */
		if (re_snprintf(buf, sizeof(buf), "%s,%s",
				sdp_media_name(ds->sdpm),
				!comp->is_rtp ? "RTCP" :
				ds->mux ? "RTP,RTCP" : "RTP"))
]=])

string(FIND "${MCONTENT}" "${_MUX_NEW}" _MUX_DONE)
if(NOT _MUX_DONE EQUAL -1)
  message(STATUS "patch-baresip-sources: ${DTLS_SRTP_C} already patched (rtcp-mux secure event)")
else()
  string(FIND "${MCONTENT}" "${_MUX_OLD}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR
      "patch-baresip-sources: anchor not found in ${DTLS_SRTP_C} for the "
      "rtcp-mux secure event.\nThe pinned baresip revision has moved "
      "dtls_estab_handler(). Re-derive this patch — building without it "
      "leaves every rtcp-mux DTLS call sending no RTCP.")
  endif()
  string(REPLACE "${_MUX_OLD}" "${_MUX_NEW}" MCONTENT "${MCONTENT}")
  file(WRITE "${DTLS_SRTP_C}" "${MCONTENT}")
  message(STATUS "patch-baresip-sources: patched ${DTLS_SRTP_C} (rtcp-mux secure event)")
endif()

# ---------------------------------------------------------------------------
# Job 5: opus — FEC redundancy that follows the link (src/adapt.c)
#
# src/adapt.c sizes the encoder's expected packet loss from the loss the peer
# reports, and re-runs the encoder update to apply it.  Two things in the
# opus module stand in the way:
#
#   - encode.c only calls OPUS_SET_PACKET_LOSS_PERC when the value is
#     non-zero, so a level can go up but never come back to 0: a link that
#     recovers keeps paying for redundancy for the rest of the call.  0 is
#     libopus's own default, so setting it unconditionally changes nothing
#     for an encoder that never had a level.
#   - decode.c gates FEC decoding on that same global — what *we* expect to
#     lose on our uplink, which says nothing about whether the packet the
#     *peer* sent carries a redundant copy.  With the level at 0 on a clean
#     uplink, every FEC frame the peer spent bits on was thrown away.  libopus
#     decodes a packet without FEC data as ordinary concealment, so asking
#     for FEC whenever the next packet is at hand costs nothing.
# ---------------------------------------------------------------------------

set(OPUS_ENC_C "${SOURCE_DIR}/modules/opus/encode.c")
set(OPUS_DEC_C "${SOURCE_DIR}/modules/opus/decode.c")
foreach(_f "${OPUS_ENC_C}" "${OPUS_DEC_C}")
  if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "patch-baresip-sources: ${_f} does not exist")
  endif()
endforeach()

set(_ENC_OLD [=[
	if (opus_packet_loss) {
		opus_encoder_ctl(aes->enc,
				 OPUS_SET_PACKET_LOSS_PERC(opus_packet_loss));
	}
]=])
set(_ENC_NEW [=[
	/* EchoSDK-patched: unconditional, so the level can return to 0. */
	opus_encoder_ctl(aes->enc,
			 OPUS_SET_PACKET_LOSS_PERC(opus_packet_loss));
]=])
set(_DEC_OLD [=[
	fec = buf && len && opus_packet_loss > 0;
]=])
set(_DEC_NEW [=[
	/* EchoSDK-patched: whether the peer sent FEC is not a question our
	 * own expected loss (opus_packet_loss) can answer. */
	fec = buf && len;
]=])

foreach(_j "ENC;${OPUS_ENC_C};expected loss can return to 0"
           "DEC;${OPUS_DEC_C};FEC decode not gated on our expected loss")
  list(GET _j 0 _k)
  list(GET _j 1 _file)
  list(GET _j 2 _what)
  file(READ "${_file}" _content)
  string(FIND "${_content}" "${_${_k}_NEW}" _done)
  if(NOT _done EQUAL -1)
    message(STATUS "patch-baresip-sources: ${_file} already patched (${_what})")
    continue()
  endif()
  string(FIND "${_content}" "${_${_k}_OLD}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR
      "patch-baresip-sources: anchor not found in ${_file} (${_what}).\n"
      "The pinned baresip revision has moved the opus module. Re-derive this "
      "patch — building without it leaves FEC redundancy stuck once raised "
      "and throws away the FEC the peer sends.")
  endif()
  string(REPLACE "${_${_k}_OLD}" "${_${_k}_NEW}" _content "${_content}")
  file(WRITE "${_file}" "${_content}")
  message(STATUS "patch-baresip-sources: patched ${_file} (${_what})")
endforeach()

# ---------------------------------------------------------------------------
# Job 6: aureceiver — conceal a burst when the audio device needs it
#
# aurecv_stream_decode() only learns of a loss when the packet after it comes
# out of the jitter buffer, and then synthesises one concealment frame per
# lost packet (baresip fd532492).  For a burst shorter than the playout buffer
# that is in time.  For a longer one it is too late both ways: the device has
# already drained the buffer and played silence, and the late frames then
# queue up behind it — the buffer overfills to its 160 ms cap and, in fixed
# mode, never drains back.  Measured live (2026-09-30, Asterisk echo):
#
#   burst           upstream                      capping alone (no playout)
#   100 ms / 1 s    concealed, +140 ms for good   no delay, 32 silent gaps
#   400 ms / 4 s    ~270 ms silence, +120 ms      no delay, still silence
#   3 s outage      +80 ms for good               no delay
#
# Two parts, as WebRTC's NetEQ does it:
#
#   - At playout: when the device asks for more than the buffer holds and
#     the codec can conceal, decode concealment frames right then instead of
#     letting the buffer run dry into silence.  The device thread only
#     trylocks the receiver (a busy lock is the old silence, never a stall),
#     and stops after 1 s — past that the stream has stopped, it is not a
#     burst.
#   - On arrival: conceal only as many of the lost frames as put the buffer
#     back at its level after the last real frame, less the one the new
#     packet brings — none when playout concealment already covered the gap,
#     all of them for a short burst.  The *last* ones, so the FEC-rebuilt
#     frame and the decoder's run-in to the next packet are the ones kept.
#     Rounded down: an underfull buffer refills by itself, an overfull one
#     does not.
# ---------------------------------------------------------------------------

set(AURECV_C "${SOURCE_DIR}/src/aureceiver.c")
if(NOT EXISTS "${AURECV_C}")
  message(FATAL_ERROR "patch-baresip-sources: ${AURECV_C} does not exist")
endif()

set(_AR1_OLD [=[
	bool done_first;              /**< First auplay write done flag      */
};
]=])
set(_AR1_NEW [=[
	bool done_first;              /**< First auplay write done flag      */
	/* EchoSDK-patched: burst concealment, see aurecv_plc_playout() */
	size_t plc_level;             /**< aubuf bytes after last real frame */
	size_t plc_frame_sz;          /**< Bytes of that frame               */
	uint64_t plc_next_ts;         /**< Timestamp just after it           */
	unsigned plc_run;             /**< Frames concealed at playout since */
};
]=])
set(_AR2_OLD [=[
static int aurecv_stream_decode(struct audio_recv *ar,
]=])
set(_AR2_NEW [=[
/* EchoSDK-patched: frames of playout concealment before a silent stream is
 * taken for a stopped one (hold, a dead path) rather than a burst: 1 s. */
#define AURECV_PLC_PLAYOUT_MAX 50u


/* EchoSDK-patched: how many of `lostc` lost frames to conceal — enough to
 * put the playout buffer back at its level after the last real frame, less
 * the one the next packet brings.  Concealment frames for time the device
 * has already played would only add delay. */
static unsigned aurecv_plc_needed(struct audio_recv *ar, unsigned lostc)
{
	size_t cur, deficit;

	if (!ar->aubuf || !ar->plc_frame_sz)
		return lostc;             /* nothing to measure against yet */

	cur = aubuf_cur_size(ar->aubuf);
	if (cur >= ar->plc_level)
		return 0;

	deficit = (ar->plc_level - cur) / ar->plc_frame_sz;
	if (deficit <= 1)
		return 0;

	return deficit - 1 < lostc ? (unsigned)(deficit - 1) : lostc;
}


/* EchoSDK-patched: the device wants `need` bytes and the buffer holds less.
 * Decode concealment into it now, rather than let it run dry into silence
 * and conceal the gap only when — and after — the next packet arrives.
 * Called from the device thread with aubuf_mtx held; never blocks on the
 * receiver, which the network thread holds while it decodes. */
static void aurecv_plc_playout(struct audio_recv *ar, size_t need)
{
	const struct aucodec *ac;

	if (mtx_trylock(ar->mtx) != thrd_success)
		return;

	ac = ar->ac;
	if (!ac || !ac->plch || !ar->dec || !ar->plc_frame_sz)
		goto out;                 /* no real frame yet, or cannot conceal */

	for (int i = 0; i < 4 && aubuf_cur_size(ar->aubuf) < need; i++) {
		struct auframe af;
		size_t sampc = ar->sampvsz / aufmt_sample_size(ar->fmt);

		if (ar->plc_run >= AURECV_PLC_PLAYOUT_MAX)
			break;
		if (ac->plch(ar->dec, ar->fmt, ar->sampv, &sampc, NULL, 0) ||
		    !sampc)
			break;

		auframe_init(&af, ar->fmt, ar->sampv, sampc, ac->srate, ac->ch);
		af.timestamp = ar->plc_next_ts;
		ar->plc_next_ts += (uint64_t)(sampc / ac->ch) * AUDIO_TIMEBASE /
		                   ac->srate;

		if (aurecv_process_decfilt(ar, &af) ||
		    aurecv_push_aubuf(ar, &af))
			break;
		++ar->plc_run;
	}

 out:
	mtx_unlock(ar->mtx);
}


static int aurecv_stream_decode(struct audio_recv *ar,
]=])
set(_AR3_OLD [=[
	if (lostc && ac->plch) {
		for (unsigned i = 0; i < lostc; i++) {
]=])
set(_AR3_NEW [=[
	if (lostc && ac->plch) {
		/* EchoSDK-patched: only the last aurecv_plc_needed() of them. */
		for (unsigned i = lostc - aurecv_plc_needed(ar, lostc);
		     i < lostc; i++) {
]=])
set(_AR4_OLD [=[
	err = aurecv_push_aubuf(ar, &af);
 out:
	return err;
}
]=])
set(_AR4_NEW [=[
	err = aurecv_push_aubuf(ar, &af);
	/* EchoSDK-patched: where burst concealment measures and continues. */
	if (!err && ar->aubuf && af.sampc && af.srate && af.ch) {
		ar->plc_level    = aubuf_cur_size(ar->aubuf);
		ar->plc_frame_sz = auframe_size(&af);
		ar->plc_next_ts  = af.timestamp +
			(uint64_t)(af.sampc / af.ch) * AUDIO_TIMEBASE / af.srate;
		ar->plc_run      = 0;
	}
 out:
	return err;
}
]=])
set(_AR5_OLD [=[
	if (ar->aubuf)
		aubuf_read_auframe(ar->aubuf, af);
	else
		memset(af->sampv, 0, auframe_size(af));
]=])
set(_AR5_NEW [=[
	if (ar->aubuf) {
		/* EchoSDK-patched: conceal now rather than run dry */
		if (aubuf_cur_size(ar->aubuf) < auframe_size(af))
			aurecv_plc_playout(ar, auframe_size(af));
		aubuf_read_auframe(ar->aubuf, af);
	}
	else
		memset(af->sampv, 0, auframe_size(af));
]=])

file(READ "${AURECV_C}" _content)
set(_changed FALSE)
foreach(_k AR1 AR2 AR3 AR4 AR5)
  string(FIND "${_content}" "${_${_k}_NEW}" _done)
  if(NOT _done EQUAL -1)
    continue()
  endif()
  string(FIND "${_content}" "${_${_k}_OLD}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR
      "patch-baresip-sources: anchor ${_k} not found in ${AURECV_C}.\n"
      "The pinned baresip revision has moved aureceiver.c. Re-derive this "
      "patch — building without it plays a burst of loss as silence, or "
      "leaves the call permanently delayed after one.")
  endif()
  string(REPLACE "${_${_k}_OLD}" "${_${_k}_NEW}" _content "${_content}")
  set(_changed TRUE)
endforeach()
if(_changed)
  file(WRITE "${AURECV_C}" "${_content}")
  message(STATUS "patch-baresip-sources: patched ${AURECV_C} (burst concealment at playout)")
else()
  message(STATUS "patch-baresip-sources: ${AURECV_C} already patched (burst concealment)")
endif()
