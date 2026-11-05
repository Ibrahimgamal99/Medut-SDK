# Linux: the Dart side drives the C library over dart:ffi
# (lib/src/ffi_backend.dart), so there is nothing to compile here. The app's
# runner installs the prebuilt core into bundle/lib/, where
# DynamicLibrary.open('medut.so') finds it.
#
# One committed core per Flutter target, x64/medut.so and arm64/medut.so,
# both built by .github/workflows/desktop-libs.yml (commit=true) in the
# manylinux2014 image: glibc 2.17+, OpenSSL linked in, stripped.
cmake_minimum_required(VERSION 3.10)

# flutter build passes linux-x64 or linux-arm64; fall back to the host.
if(FLUTTER_TARGET_PLATFORM MATCHES "^linux-(x64|arm64)$")
  set(_medut_arch "${CMAKE_MATCH_1}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(_medut_arch "arm64")
else()
  set(_medut_arch "x64")
endif()

set(_medut_lib "${CMAKE_CURRENT_SOURCE_DIR}/${_medut_arch}/medut.so")
if(NOT EXISTS "${_medut_lib}")
  message(FATAL_ERROR "medut: ${_medut_lib} is missing; run the "
    "desktop-libs.yml workflow with commit=true")
endif()

set(medut_bundled_libraries "${_medut_lib}" PARENT_SCOPE)
