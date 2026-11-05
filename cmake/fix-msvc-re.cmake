# fix-msvc-re.cmake  (no-op — kept for reference only)
#
# Previously this script renamed websock_connect → __real_websock_connect in
# libre's websock.c at build time so ws_path.c could provide websock_connect
# as the MSVC wrapper without a duplicate symbol.  The in-place edit caused
# the patched version to be accidentally committed to the re submodule, which
# broke Linux builds (websock_connect became undefined for the executables
# linked against it).
#
# The rename is now controlled by the RE_WEBSOCK_CONNECT_OVERRIDE compile
# definition, which is passed via CMAKE_C_FLAGS in the re_project ExternalProject
# when building with MSVC.  websock.c uses:
#
#   #ifdef RE_WEBSOCK_CONNECT_OVERRIDE
#   int __real_websock_connect(...)
#   #else
#   int websock_connect(...)
#   #endif
#
# This script is no longer called by CMakeLists.txt and does nothing.

message(STATUS "fix-msvc-re.cmake: no-op (websock rename now handled via RE_WEBSOCK_CONNECT_OVERRIDE compile flag)")
