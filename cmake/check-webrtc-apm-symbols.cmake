# Fails the build if src/webrtc_apm.cpp references anything beyond the three
# AudioProcessingBuilder symbols src/webrtc_shim.c provides.
#
# Anything else would be resolved against libstdc++ or the WebRTC library at
# link time — the first puts a C++ runtime dependency on a C SDK, the second
# a hard DT_NEEDED on a library that is supposed to be optional (or, with the
# shared link's -z defs off, a .so that fails to load).  Default-constructing
# AudioProcessing::Config is the easy way to trip this: it references the
# out-of-line Pipeline::Pipeline().
#
# Invoked with -DNM=<nm> -DOBJS=<object files>.

cmake_minimum_required(VERSION 3.19)

set(_allowed
  "_ZN6webrtc22AudioProcessingBuilderC1Ev"
  "_ZN6webrtc22AudioProcessingBuilderD1Ev"
  "_ZN6webrtc22AudioProcessingBuilder6CreateEv"
  # compiler-emitted for large struct copies
  "memcpy" "memmove" "memset"
  "_GLOBAL_OFFSET_TABLE_"
  # hardened toolchains (-fstack-protector-strong, -D_FORTIFY_SOURCE=2, both
  # distro defaults): libc's, so no new dependency
  "__stack_chk_fail" "__stack_chk_guard" "__stack_chk_fail_local"
  "__memcpy_chk" "__memmove_chk" "__memset_chk")

foreach(_obj IN LISTS OBJS)
  execute_process(COMMAND "${NM}" -u "${_obj}"
                  OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "nm failed on ${_obj}")
  endif()
  string(REGEX MATCHALL "[^\n]+" _lines "${_out}")
  foreach(_l IN LISTS _lines)
    string(REGEX REPLACE "^[ \t]*U[ \t]+" "" _sym "${_l}")
    string(STRIP "${_sym}" _sym)
    string(REGEX REPLACE "@.*$" "" _sym "${_sym}")
    if(NOT _sym IN_LIST _allowed)
      list(APPEND _bad "${_sym}")
    endif()
  endforeach()
endforeach()

if(_bad)
  list(JOIN _bad "\n  " _bad_s)
  message(FATAL_ERROR
    "src/webrtc_apm.cpp references symbols webrtc_shim.c does not provide:\n"
    "  ${_bad_s}\n"
    "Keep it to virtual/inline APM calls (see the file's header comment).")
endif()
