# patch-baresip-sources.cmake
# Apply EchoSDK's baresip patches at build time.  Seven jobs:
#   1. Disable baresip's executables (baresip_exe, test/, webrtc/) everywhere.
#   2. Make dtls_srtp recover from a handshake that is perturbed or stalls.
#   3. Keep a re-REGISTER from un-REGISTERing the binding it refreshes.
#   4. Start RTCP on a DTLS call that multiplexes it (rtcp-mux).
#   5. Let Opus FEC follow the link: expected loss can fall back to 0, and
#      received FEC is used whatever our own encoder expects.
#   6. Conceal a burst of loss when the audio device needs it, not when the
#      next packet happens to arrive — no silence, no added delay.
#   7. Start the opus module from its defaults on every init, so one init's
#      settings do not carry into the next.
# Usage: cmake -DSOURCE_DIR=path/to/baresip -P patch-baresip-sources.cmake
#
# third_party/baresip is fetched by scripts/fetch-third-party.sh at a pinned
# revision and is gitignored, so an in-place patch cannot leak into upstream
# history.  Each job is idempotent behind its own marker and fails loudly when
# a baresip bump moves the text it splices at, rather than silently building
# without the fix.
#
# Jobs 5-7 are the ones nothing in the SDK calls into, so nothing would notice
# them missing.  Each defines a `bsdk_patch_*` string that src/core.c
# references: a tree without the patch fails to link, and
# `strings libechosdk.so | grep echosdk-patch` names the versions a binary
# carries.  The version is also the job's idempotence marker; a file patched by
# an older version is restored from the pinned revision and patched afresh
# (bsdk_restore_if_stale), since a changed hunk can be neither found in nor
# applied on top of its predecessor.  CMakeLists.txt re-runs this script when
# it changes.
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

# A file that an earlier version of a marked job patched goes back to the
# pinned revision, so the current version applies to the text it was written
# against.  Only for files whose every patch belongs to that one job.
function(bsdk_restore_if_stale _file _marker)
  file(READ "${_file}" _c)
  string(FIND "${_c}" "${_marker}" _cur)
  string(FIND "${_c}" "EchoSDK-patched" _any)
  if(NOT _cur EQUAL -1 OR _any EQUAL -1)
    return()
  endif()
  file(RELATIVE_PATH _rel "${SOURCE_DIR}" "${_file}")
  find_program(_BSDK_GIT git)
  if(NOT _BSDK_GIT)
    message(FATAL_ERROR
      "patch-baresip-sources: ${_rel} carries an older EchoSDK patch and git "
      "is not available to restore it. Re-fetch third_party/baresip "
      "(scripts/fetch-third-party.sh) and build again.")
  endif()
  execute_process(COMMAND "${_BSDK_GIT}" -C "${SOURCE_DIR}" checkout -- "${_rel}"
                  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
      "patch-baresip-sources: could not restore ${_rel} to repatch it: ${_err}")
  endif()
  message(STATUS "patch-baresip-sources: ${_rel} carried an older patch; "
                 "restored to the pinned revision")
endfunction()

# Apply hunks _OLD_<k>/_NEW_<k> (the caller's variables) for each <k> in
# _keys to _file, unless it already carries _marker.  Every anchor is checked
# before any is spliced, so a baresip bump never leaves a file half-patched.
macro(bsdk_patch_file _file _marker _what _why)
  bsdk_restore_if_stale("${_file}" "${_marker}")
  file(READ "${_file}" _pf_content)
  string(FIND "${_pf_content}" "${_marker}" _pf_done)
  if(NOT _pf_done EQUAL -1)
    message(STATUS "patch-baresip-sources: ${_file} already patched (${_what})")
  else()
    foreach(_pf_k ${_keys})
      string(FIND "${_pf_content}" "${_OLD_${_pf_k}}" _pf_pos)
      if(_pf_pos EQUAL -1)
        message(FATAL_ERROR
          "patch-baresip-sources: anchor ${_pf_k} not found in ${_file} "
          "(${_what}).\nThe pinned baresip revision has moved this code. "
          "Re-derive the patch — building without it ${_why}.")
      endif()
    endforeach()
    foreach(_pf_k ${_keys})
      string(REPLACE "${_OLD_${_pf_k}}" "${_NEW_${_pf_k}}"
             _pf_content "${_pf_content}")
    endforeach()
    file(WRITE "${_file}" "${_pf_content}")
    message(STATUS "patch-baresip-sources: patched ${_file} (${_what})")
  endif()
endmacro()

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

file(READ "${OPUS_ENC_C}" _content)
string(FIND "${_content}" "${_ENC_NEW}" _done)
if(NOT _done EQUAL -1)
  message(STATUS "patch-baresip-sources: ${OPUS_ENC_C} already patched (expected loss can return to 0)")
else()
  string(FIND "${_content}" "${_ENC_OLD}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR
      "patch-baresip-sources: anchor not found in ${OPUS_ENC_C} (expected "
      "loss can return to 0).\nThe pinned baresip revision has moved the opus "
      "module. Re-derive this patch — building without it leaves FEC "
      "redundancy stuck once raised.")
  endif()
  string(REPLACE "${_ENC_OLD}" "${_ENC_NEW}" _content "${_content}")
  file(WRITE "${OPUS_ENC_C}" "${_content}")
  message(STATUS "patch-baresip-sources: patched ${OPUS_ENC_C} (expected loss can return to 0)")
endif()

# decode.c: the FEC gate, and the marker src/core.c links against.
set(_OLD_DEC [=[
	fec = buf && len && opus_packet_loss > 0;
]=])
set(_NEW_DEC [=[
	/* EchoSDK-patched: whether the peer sent FEC is not a question our
	 * own expected loss (opus_packet_loss) can answer. */
	fec = buf && len;
]=])
set(_OLD_DECMARK [=[
int opus_decode_pkloss(struct audec_state *ads,
]=])
set(_NEW_DECMARK [=[
/* EchoSDK-patched: src/core.c references this, so a build without the FEC
 * patch below fails to link instead of shipping without it. */
extern const char bsdk_patch_opus_fec[];
const char bsdk_patch_opus_fec[] = "echosdk-patch opus-fec v1";


int opus_decode_pkloss(struct audec_state *ads,
]=])
set(_keys DEC DECMARK)
bsdk_patch_file("${OPUS_DEC_C}" "echosdk-patch opus-fec v1"
  "FEC decode not gated on our expected loss"
  "throws away the FEC the peer sends")

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
# Three parts, as WebRTC's NetEQ does it:
#
#   - At playout: when the device asks for more than the buffer holds and
#     the codec can conceal, decode concealment frames right then instead of
#     letting the buffer run dry into silence.  The device thread only
#     trylocks the receiver (a busy lock is the old silence, never a stall),
#     and stops after 1 s of it — measured in time, so it is 1 s at every
#     ptime — since past that the stream has stopped, it is not a burst.
#   - On arrival, of the lost frames: none that playout concealment has
#     already played; of the rest always the last (the arriving packet's FEC
#     rebuilds it, for at most a frame of buffer), and otherwise as many as
#     put the buffer back at its level after the last real frame, less the
#     one the new packet brings.  The *last* ones, so the FEC-rebuilt frame
#     and the decoder's run-in to the next packet are the ones kept.  Rounded
#     to the nearest frame: an early packet or a device that reads in bursts
#     leaves part of one, and rounding down dropped the frame FEC could have
#     rebuilt.
#   - On arrival, of the packet itself: dropped when playout concealment has
#     already played its slot.  Decoded, it went into the buffer in timestamp
#     order — spliced into the concealment — and its length stayed on the
#     delay.  Not when the buffer was shallower than one device read: there
#     the device outran the buffer rather than the packet being late, and
#     keeping the packet is how the buffer grows to fit a device that reads
#     40 ms at a time.  Dropped, every other slot was concealed for the rest
#     of the call.
#
# The concealment state starts over on a codec change: the old codec's frame
# size had playout concealment run the new decoder before its first packet
# (an opus_decode error on every device callback until one arrived).
# ---------------------------------------------------------------------------

set(AURECV_C "${SOURCE_DIR}/src/aureceiver.c")
if(NOT EXISTS "${AURECV_C}")
  message(FATAL_ERROR "patch-baresip-sources: ${AURECV_C} does not exist")
endif()

set(_OLD_AR1 [=[
	bool done_first;              /**< First auplay write done flag      */
};
]=])
set(_NEW_AR1 [=[
	bool done_first;              /**< First auplay write done flag      */
	/* EchoSDK-patched: burst concealment, see aurecv_plc_playout() */
	size_t plc_level;             /**< aubuf bytes after last real frame */
	size_t plc_frame_sz;          /**< Bytes of that frame               */
	uint64_t plc_next_ts;         /**< Timestamp just after it           */
	uint64_t plc_start_ts;        /**< plc_next_ts when it was pushed    */
	unsigned plc_run;             /**< Frames concealed at playout since */
	size_t plc_read_sz;           /**< Bytes the device read then        */
};
]=])
set(_OLD_AR2 [=[
static int aurecv_stream_decode(struct audio_recv *ar,
]=])
set(_NEW_AR2 [=[
/* EchoSDK-patched: src/core.c references this, so a build without burst
 * concealment fails to link instead of shipping without it. */
extern const char bsdk_patch_aurecv_plc[];
const char bsdk_patch_aurecv_plc[] = "echosdk-patch aurecv-plc v1";


/* EchoSDK-patched: how many of `lostc` lost frames to conceal.  None that
 * playout concealment has already played: again, they would only add delay.
 * Of the rest, always the last — the arriving packet's FEC rebuilds it, for
 * at most a frame of extra buffer — and otherwise as many as put the playout
 * buffer back at its level after the last real frame, less the one the
 * arriving packet brings.  Rounded to the nearest frame, not down: a packet
 * that arrives a little early, or a device that reads in bursts, leaves part
 * of a frame, and rounding that away lost the frame FEC could rebuild. */
static unsigned aurecv_plc_needed(struct audio_recv *ar, unsigned lostc)
{
	size_t cur, deficit, n;
	unsigned left;

	if (!ar->aubuf || !ar->plc_frame_sz)
		return lostc;             /* nothing to measure against yet */

	if (ar->plc_run >= lostc)
		return 0;
	left = lostc - ar->plc_run;

	cur = aubuf_cur_size(ar->aubuf);
	deficit = cur < ar->plc_level
		? (ar->plc_level - cur + ar->plc_frame_sz / 2) /
		  ar->plc_frame_sz
		: 0;
	n = deficit > 1 ? deficit - 1 : 1;

	return n < left ? (unsigned)n : left;
}


/* EchoSDK-patched: the device wants `need` bytes and the buffer holds less.
 * Decode concealment into it now, rather than let it run dry into silence
 * and conceal the gap only when — and after — the next packet arrives.  Up
 * to 1 s of it after the last real frame: past that the stream has stopped
 * (hold, a dead path), it is not a burst.  Called from the device thread
 * with aubuf_mtx held; never blocks on the receiver, which the network
 * thread holds while it decodes. */
static void aurecv_plc_playout(struct audio_recv *ar, size_t need)
{
	const struct aucodec *ac;

	if (mtx_trylock(ar->mtx) != thrd_success)
		return;

	ac = ar->ac;
	if (!ac || !ac->plch || !ar->dec || !ar->plc_frame_sz)
		goto out;                 /* no real frame yet, or cannot conceal */

	ar->plc_read_sz = need;

	for (int i = 0; i < 4 && aubuf_cur_size(ar->aubuf) < need; i++) {
		struct auframe af;
		size_t sampc = ar->sampvsz / aufmt_sample_size(ar->fmt);

		if (ar->plc_next_ts - ar->plc_start_ts >= AUDIO_TIMEBASE)
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
set(_OLD_AR3 [=[
	/* No decoder set */
	if (!ac)
		return 0;
]=])
set(_NEW_AR3 [=[
	/* No decoder set */
	if (!ac)
		return 0;

	/* EchoSDK-patched: playout concealment has already played this slot.
	 * Decoded, the packet would be spliced into the concealment in
	 * timestamp order and its length would stay on the delay.  Bounded:
	 * the RTP timestamp is not unwrapped here, and without the bound a
	 * wrap mid-call would discard every packet after it.
	 *
	 * Only while the buffer held a whole device read after the last real
	 * frame: then running dry means this packet was late.  A shallower
	 * buffer ran dry because the device reads more at once than it
	 * holds, and the packet is what lets it grow to fit — dropped, the
	 * next read conceals again, and the one after, for the whole call. */
	uint64_t ts = ((uint64_t) hdr->ts) * AUDIO_TIMEBASE / ac->crate;
	if (ar->plc_run && ts < ar->plc_next_ts &&
	    ar->plc_next_ts - ts < AUDIO_TIMEBASE &&
	    ar->plc_level >= ar->plc_read_sz) {
		++ar->n_discard;
		goto out;
	}
]=])
set(_OLD_AR4 [=[
	if (lostc && ac->plch) {
		for (unsigned i = 0; i < lostc; i++) {
]=])
set(_NEW_AR4 [=[
	if (lostc && ac->plch) {
		/* EchoSDK-patched: only the last aurecv_plc_needed() of them. */
		for (unsigned i = lostc - aurecv_plc_needed(ar, lostc);
		     i < lostc; i++) {
]=])
set(_OLD_AR5 [=[
	err = aurecv_push_aubuf(ar, &af);
 out:
	return err;
}
]=])
set(_NEW_AR5 [=[
	err = aurecv_push_aubuf(ar, &af);
	/* EchoSDK-patched: where burst concealment measures and continues. */
	if (!err && ar->aubuf && af.sampc && af.srate && af.ch) {
		ar->plc_level    = aubuf_cur_size(ar->aubuf);
		ar->plc_frame_sz = auframe_size(&af);
		ar->plc_next_ts  = af.timestamp +
			(uint64_t)(af.sampc / af.ch) * AUDIO_TIMEBASE / af.srate;
		ar->plc_start_ts = ar->plc_next_ts;
		ar->plc_run      = 0;
	}
 out:
	return err;
}
]=])
set(_OLD_AR6 [=[
	if (ar->aubuf)
		aubuf_read_auframe(ar->aubuf, af);
	else
		memset(af->sampv, 0, auframe_size(af));
]=])
set(_NEW_AR6 [=[
	if (ar->aubuf) {
		/* EchoSDK-patched: conceal now rather than run dry */
		if (aubuf_cur_size(ar->aubuf) < auframe_size(af))
			aurecv_plc_playout(ar, auframe_size(af));
		aubuf_read_auframe(ar->aubuf, af);
	}
	else
		memset(af->sampv, 0, auframe_size(af));
]=])
set(_OLD_AR7 [=[
	if (ac != ar->ac) {
		ar->ac = ac;
		ar->dec = mem_deref(ar->dec);
	}
]=])
set(_NEW_AR7 [=[
	if (ac != ar->ac) {
		ar->ac = ac;
		ar->dec = mem_deref(ar->dec);
		/* EchoSDK-patched: nothing to conceal from until the new
		 * codec's first frame. */
		ar->plc_level    = 0;
		ar->plc_frame_sz = 0;
		ar->plc_next_ts  = 0;
		ar->plc_start_ts = 0;
		ar->plc_run      = 0;
		ar->plc_read_sz  = 0;
	}
]=])
set(_keys AR1 AR2 AR3 AR4 AR5 AR6 AR7)
bsdk_patch_file("${AURECV_C}" "echosdk-patch aurecv-plc v1"
  "burst concealment at playout"
  "plays a burst of loss as silence, or leaves the call permanently delayed after one")

# ---------------------------------------------------------------------------
# Job 7: opus — every module_init starts from the defaults
#
# module_init() reads the opus_* config keys into file-scope globals and only
# ever writes the ones a key is present for, so a value set by one
# echosdk_init() survives into the next that leaves the key out: complexity
# 0 stays 0, a pinned expected loss stays pinned.  The channel count is the
# same one-way street — `if (!stereo || !sprop_stereo) opus.ch = 1;` can turn
# stereo off and nothing ever turns it back on, so after one mono init every
# later one is mono whatever it asks for.  Reset everything module_init() can
# change to its static initialiser first.
# ---------------------------------------------------------------------------

set(OPUS_C "${SOURCE_DIR}/modules/opus/opus.c")
if(NOT EXISTS "${OPUS_C}")
  message(FATAL_ERROR "patch-baresip-sources: ${OPUS_C} does not exist")
endif()

set(_OLD_OI1 [=[
static int module_init(void)
{
]=])
set(_NEW_OI1 [=[
/* EchoSDK-patched: src/core.c references this, so a build without the
 * defaults reset below fails to link instead of shipping without it. */
extern const char bsdk_patch_opus_init[];
const char bsdk_patch_opus_init[] = "echosdk-patch opus-init v1";


static int module_init(void)
{
]=])
set(_OLD_OI2 [=[
	fmtp[0] = '\0';
	fmtp_mirror[0] = '\0';
]=])
set(_NEW_OI2 [=[
	fmtp[0] = '\0';
	fmtp_mirror[0] = '\0';

	/* EchoSDK-patched: from the defaults every time; see job 7 in
	 * EchoSDK's cmake/patch-baresip-sources.cmake. */
	opus.ch          = 2;
	opus.srate       = 48000;
	opus.fmtp        = fmtp;
	opus.fmtp_ench   = NULL;
	opus_mirror      = false;
	opus_complexity  = 10;
	opus_application = OPUS_APPLICATION_AUDIO;
	opus_packet_loss = 0;
]=])
set(_OLD_OI3 [=[
	if (!stereo || !sprop_stereo)
		opus.ch = 1;
]=])
set(_NEW_OI3 [=[
	opus.ch = (stereo && sprop_stereo) ? 2 : 1;   /* EchoSDK-patched */
]=])
set(_keys OI1 OI2 OI3)
bsdk_patch_file("${OPUS_C}" "echosdk-patch opus-init v1"
  "defaults reset on every init"
  "carries one init's opus settings, and mono, into every later one")
