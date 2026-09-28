# patch-baresip-sources.cmake
# Apply EchoSDK's baresip patches at build time.  Two jobs:
#   1. Disable baresip's executables (baresip_exe, test/, webrtc/) everywhere.
#   2. Make dtls_srtp recover from a handshake that is perturbed or stalls.
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
