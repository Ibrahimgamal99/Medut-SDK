# Windows: the Dart side drives the C library over dart:ffi
# (lib/src/ffi_backend.dart), so there is nothing to compile here. The app's
# runner installs the prebuilt core next to the .exe, where
# DynamicLibrary.open('medut.dll') finds it.
#
# One committed core per Flutter target, x64/medut.dll and arm64/medut.dll,
# both from .github/workflows/desktop-libs.yml (commit=true);
# scripts/build-windows.ps1 refreshes them on a local build.
cmake_minimum_required(VERSION 3.14)

# flutter build passes windows-x64 or windows-arm64; fall back to -A.
if(FLUTTER_TARGET_PLATFORM MATCHES "^windows-(x64|arm64)$")
  set(_medut_arch "${CMAKE_MATCH_1}")
elseif(CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|arm64)$")
  set(_medut_arch "arm64")
else()
  set(_medut_arch "x64")
endif()

set(_medut_lib "${CMAKE_CURRENT_SOURCE_DIR}/${_medut_arch}/medut.dll")
if(NOT EXISTS "${_medut_lib}")
  message(FATAL_ERROR "medut: ${_medut_lib} is missing; run the "
    "desktop-libs.yml workflow with commit=true, or scripts/build-windows.ps1")
endif()

set(medut_bundled_libraries "${_medut_lib}" PARENT_SCOPE)
