# Linux: the Dart side drives the C library over dart:ffi
# (lib/src/ffi_backend.dart), so there is nothing to compile here. The app's
# runner installs the prebuilt core into bundle/lib/, where
# DynamicLibrary.open('medut.so') finds it.
#
# medut.so is committed: scripts/build-linux.sh rebuilds it and refreshes this
# copy (stripped).
cmake_minimum_required(VERSION 3.10)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/medut.so")
  message(FATAL_ERROR "medut: ${CMAKE_CURRENT_SOURCE_DIR}/medut.so is missing; "
    "run scripts/build-linux.sh")
endif()

set(medut_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/medut.so"
  PARENT_SCOPE
)
