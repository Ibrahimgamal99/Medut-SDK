cmake_minimum_required(VERSION 3.19)
project(echosdk VERSION 1.0.0 LANGUAGES C)

include(ExternalProject)

# ---------------------------------------------------------------------------
# Source-only mode — used internally by echosdk_core_project ExternalProject.
# When ECHOSDK_SRC_MODE=ON this file compiles only src/*.c + platform/*.c
# into echosdk_core and returns; the outer build then merges it with re + baresip.
# ---------------------------------------------------------------------------
option(ECHOSDK_SRC_MODE "Compile EchoSDK sources only (internal use)" OFF)

# ---------------------------------------------------------------------------
# HAVE_UNIXSOCK is an ABI switch, not a feature flag — it MUST be identical in
# re, baresip and echosdk_core. Set here, above the ECHOSDK_SRC_MODE branch,
# because the core sources are configured by a *separate* CMake invocation
# from the one that configures re and baresip: anything set inside a branch
# reaches only one of them.
#
# re's `struct sa` embeds `struct sockaddr_un` in its union only when
# HAVE_UNIXSOCK != 0, which changes sizeof(struct sa) from ~32 to ~112 bytes.
# `struct sip_msg` opens with two `struct sa` (src, dst), so a disagreement
# shifts every field after them by ~160 bytes. This does not fail to link and
# does not crash — it builds, runs, and silently reads the other library's
# structs at the wrong offsets. Android used to pass USE_UNIXSOCK=OFF to re
# alone: re read scode=200 from a REGISTER response while baresip read 55857
# from the very same message, so registration completed on the wire and no
# event was ever emitted. The app sat in "registering" forever, Android only.
#
# The default is "on" in three separate places: re's own
# option(USE_UNIXSOCK ... ON), baresip's inherited copy, and the header guard
# `#if !defined(HAVE_UNIXSOCK) || HAVE_UNIXSOCK == 1` — so a target that never
# hears about the flag at all still gets the *large* struct. Set it explicitly
# on all three rather than trusting any of those defaults to agree.
set(ECHOSDK_USE_UNIXSOCK ON CACHE BOOL
    "Include AF_UNIX in re's struct sa (ABI-critical; must match everywhere)")
if(ECHOSDK_USE_UNIXSOCK)
  set(_BSDK_UNIXSOCK_CM "ON")
  set(_BSDK_UNIXSOCK_DEF "HAVE_UNIXSOCK=1")
else()
  set(_BSDK_UNIXSOCK_CM "OFF")
  set(_BSDK_UNIXSOCK_DEF "HAVE_UNIXSOCK=0")
endif()

if(ECHOSDK_SRC_MODE)
  cmake_minimum_required(VERSION 3.19)
  project(echosdk_core VERSION 1.0.0 LANGUAGES C)

  set(ECHOSDK_SYSROOT "" CACHE PATH "Path to installed re+baresip headers")

  # Platform-specific source files
  if(ANDROID_ABI)
    file(GLOB PLAT_SRCS CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/*.c")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJC)   # audio_ios.m (AVAudioSession setup)
    file(GLOB PLAT_SRCS CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/*.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/*.m")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJC)
    file(GLOB PLAT_SRCS CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/*.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/*.m")
  elseif(WIN32)
    file(GLOB PLAT_SRCS CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/*.c")
  else()
    file(GLOB PLAT_SRCS CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/*.c")
  endif()

  # CONFIGURE_DEPENDS so a newly added source is compiled by an incremental
  # build.  Without it the glob is evaluated once at configure time and cached:
  # a new .c file links as if it were never written, which is a silent and very
  # confusing failure for anyone adding one.
  file(GLOB CORE_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
  # echo-ice: EchoSDK's own ICE agent lives one directory down and the glob
  # above is deliberately flat, so it needs naming.  It compiles into the
  # core archive (it reads g_bsdk and calls netmon) and registers its
  # media-NAT from bsdk_ice_module_init(), not through baresip's mod_table.
  file(GLOB ICE_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ice/*.c")
  list(APPEND CORE_SRCS ${ICE_SRCS})
  # glibc_symver.c pins glibc symbol versions for portable desktop-Linux
  # binaries — it references strtoll@GLIBC_2.2.5 etc. and must not be
  # compiled anywhere else (Android uses bionic, Windows/macOS no glibc).
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(FILTER CORE_SRCS EXCLUDE REGEX ".*glibc_symver\\.c$")
  endif()

  add_library(echosdk_core STATIC ${CORE_SRCS} ${PLAT_SRCS})

  target_include_directories(echosdk_core PRIVATE
    "${ECHOSDK_SYSROOT}/include"
    "${ECHOSDK_SYSROOT}/include/re"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

  # Propagate the modules profile as a compile definition
  if(ECHOSDK_MODULES_PROFILE STREQUAL "mobile")
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_PROFILE_MOBILE=1)
  else()
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_PROFILE_DESKTOP=1)
  endif()

  # ABI-critical: must match re and baresip — see the HAVE_UNIXSOCK note above.
  target_compile_definitions(echosdk_core PRIVATE ${_BSDK_UNIXSOCK_DEF})

  # WebRTC AEC opt-in
  if(ECHOSDK_WITH_WEBRTC_AEC)
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_HAS_WEBRTC_AEC=1)
  endif()

  # Platform audio define so modules_init.c picks the right audio module.
  # Android uses OpenSLES: the aaudio module needs API >= 26 (libaaudio.so
  # does not exist in the android-24 sysroot) while minSdk is 24.
  if(ANDROID_ABI)
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_AUDIO_OPENSLES=1)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_AUDIO_AUDIOUNIT=1)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_AUDIO_COREAUDIO=1)
  elseif(WIN32)
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_AUDIO_WASAPI=1)
  else()
    target_compile_definitions(echosdk_core PRIVATE ECHOSDK_AUDIO_PULSE=1)
  endif()

  set_target_properties(echosdk_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    PREFIX "")

  install(TARGETS echosdk_core ARCHIVE DESTINATION .)
  return()
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Generate bindings/cpp/echosdk.hpp from include/echosdk.h
# ---------------------------------------------------------------------------
add_custom_command(
  OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/echosdk.hpp"
  COMMAND "${CMAKE_COMMAND}"
          -D "C_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/include/echosdk.h"
          -D "CPP_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/echosdk.hpp"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_hpp.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/echosdk.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_hpp.cmake"
  COMMENT "Regenerating echosdk.hpp from echosdk.h"
  VERBATIM
)
add_custom_target(gen_hpp ALL
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/echosdk.hpp")

set(ECHOSDK_TLS "openssl" CACHE STRING "TLS backend: openssl or mbedtls")
set(ECHOSDK_MODULES_PROFILE "desktop" CACHE STRING "Module profile: desktop or mobile")
# Desktop targets link the platform's own libssl/libcrypto. Android and iOS
# have no system OpenSSL, so cross-compiled targets build it from
# third_party/openssl.  (libre implements TLS/DTLS only over OpenSSL — an
# mbedTLS build silently stubs out SIP/TLS, SIP/WSS and DTLS-SRTP; see the
# Phase 0.5 note below.)
if((ANDROID_ABI OR CMAKE_SYSTEM_NAME STREQUAL "iOS") AND ECHOSDK_TLS STREQUAL "openssl")
  set(_build_openssl_default ON)
else()
  set(_build_openssl_default OFF)
endif()
option(ECHOSDK_BUILD_OPENSSL
       "Cross-build OpenSSL from third_party/openssl instead of using a system one"
       ${_build_openssl_default})
option(ECHOSDK_SHARED "Also produce a shared library (.so/.dylib/.dll) from the merged archive" OFF)
if(WIN32 OR ANDROID_ABI OR CMAKE_SYSTEM_NAME MATCHES "iOS")
  set(_aec_default OFF)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(WEBRTC_AP QUIET webrtc-audio-processing-1)
  endif()
  set(_aec_default ${WEBRTC_AP_FOUND})
endif()
option(ECHOSDK_WITH_WEBRTC_AEC
       "Include WebRTC AEC module (desktop only; requires libwebrtc-audio-processing-1)"
       ${_aec_default})

set(ECHOSDK_RE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/re"
    CACHE PATH "Path to libre source tree")
set(ECHOSDK_BARESIP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/baresip"
    CACHE PATH "Path to baresip source tree")

# ---------------------------------------------------------------------------
# Guard: baresip internals used by src/netmon.c
#
# call_reset_transp() and call_laddr() are exported from libbaresip but live in
# its private src/core.h, so EchoSDK hand-declares them in echosdk_internal.h.
# An upstream signature change would still link and then misbehave at runtime,
# so compare the prototypes at configure time and fail loudly instead.
#
# Whitespace is collapsed before matching so reformatting upstream does not
# trip a false alarm — only a real signature change does.
# ---------------------------------------------------------------------------
set(_BARESIP_CORE_H "${ECHOSDK_BARESIP_SOURCE_DIR}/src/core.h")
if(EXISTS "${_BARESIP_CORE_H}")
  file(READ "${_BARESIP_CORE_H}" _CORE_H_TEXT)
  string(REGEX REPLACE "[ \t\r\n]+" " " _CORE_H_FLAT "${_CORE_H_TEXT}")
  foreach(_proto
      "int call_reset_transp(struct call *call, const struct sa *laddr);"
      "const struct sa *call_laddr(const struct call *call);")
    string(FIND "${_CORE_H_FLAT}" "${_proto}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR
        "baresip's src/core.h no longer declares:\n"
        "    ${_proto}\n"
        "src/netmon.c hand-declares this in src/echosdk_internal.h for network "
        "handover. Update that declaration to match before building, or the "
        "mismatch will link silently and fail at runtime.")
    endif()
  endforeach()
endif()

set(ECHOSDK_MBEDTLS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls"
    CACHE PATH "Path to mbedTLS source tree (required when ECHOSDK_TLS=mbedtls)")
set(ECHOSDK_OPENSSL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl"
    CACHE PATH "Path to OpenSSL source tree (required when ECHOSDK_BUILD_OPENSSL=ON)")
set(ECHOSDK_OPUS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus"
    CACHE PATH "Path to libopus source tree (required when opus module is enabled)")
set(ECHOSDK_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    CACHE PATH "Top-level output directory for built archives")

set(SYSROOT "${CMAKE_BINARY_DIR}/sysroot")

# ---------------------------------------------------------------------------
# Module list
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules-mobile.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules-desktop.cmake")

if(ECHOSDK_MODULES_PROFILE STREQUAL "mobile")
  set(ECHOSDK_MODULES "${ECHOSDK_MOBILE_MODULES_BASE}")
  # Append platform audio
  if(ANDROID_ABI)
    set(ECHOSDK_MODULES "${ECHOSDK_MODULES};opensles")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # audiounit only: it provides both capture and playback on iOS via
    # kAudioUnitSubType_VoiceProcessingIO (hardware AEC).  avcapture is
    # VIDEO capture — linking it pulls AVCaptureDevice and makes App Store
    # review demand a camera-usage string for an audio-only SDK.
    set(ECHOSDK_MODULES "${ECHOSDK_MODULES};audiounit")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ECHOSDK_MODULES "${ECHOSDK_MODULES};audiounit;coreaudio")
  endif()
else()
  set(ECHOSDK_MODULES "${ECHOSDK_DESKTOP_MODULES}")
  if(WIN32)
    list(REMOVE_ITEM ECHOSDK_MODULES "pulse")
    list(APPEND ECHOSDK_MODULES "wasapi")
  endif()
endif()

# ---------------------------------------------------------------------------
# Platform ID for dist/ subdirectory naming
# ---------------------------------------------------------------------------
if(ANDROID_ABI)
  set(ECHOSDK_PLATFORM_ID "android/${ANDROID_ABI}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  if(CMAKE_OSX_SYSROOT MATCHES "simulator")
    set(ECHOSDK_PLATFORM_ID "ios/simulator")
  else()
    set(ECHOSDK_PLATFORM_ID "ios/device")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  string(REPLACE ";" "_" _ARCH_SLUG "${CMAKE_OSX_ARCHITECTURES}")
  set(ECHOSDK_PLATFORM_ID "macos/${_ARCH_SLUG}")
elseif(WIN32)
  set(ECHOSDK_PLATFORM_ID "windows/x64")
else()
  set(ECHOSDK_PLATFORM_ID "linux/x86_64")
endif()

# ---------------------------------------------------------------------------
# Cross-compilation passthrough args
# ---------------------------------------------------------------------------
set(CROSS_ARGS "")
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND CROSS_ARGS "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()
foreach(_var
    CMAKE_SYSTEM_NAME
    CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET
    ANDROID_ABI ANDROID_PLATFORM ANDROID_STL
    CMAKE_C_COMPILER CMAKE_CXX_COMPILER
    CMAKE_AR CMAKE_RANLIB CMAKE_NM CMAKE_STRIP
    CMAKE_FIND_ROOT_PATH CMAKE_SYSROOT)
  if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
    # Replace ; with | so multi-value vars (e.g. CMAKE_OSX_ARCHITECTURES)
    # survive cmake list serialization; ExternalProjects use LIST_SEPARATOR "|".
    string(REPLACE ";" "|" _cross_val "${${_var}}")
    list(APPEND CROSS_ARGS "-D${_var}=${_cross_val}")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Library file naming
# ---------------------------------------------------------------------------
if(MSVC)
  set(_LPFX "")
  set(_EXT  ".lib")
  set(_RE_LIB   "re-static.lib")
  set(_BARE_LIB "libbaresip.lib")
  set(_CORE_LIB "echosdk_core.lib")
  # Embed TrackFileAccess=false in every generated .vcxproj for sub-projects.
  # This prevents CL.exe's PostExecuteTool from writing .tlog files, which
  # fails with "user-mapped section open" when another process has them mapped.
  set(_NOTRACK_ARG "-DCMAKE_VS_GLOBALS=TrackFileAccess=false")
else()
  set(_LPFX "lib")
  set(_EXT  ".a")
  set(_RE_LIB   "libre.a")
  set(_BARE_LIB "libbaresip.a")
  set(_CORE_LIB "echosdk_core.a")
  set(_NOTRACK_ARG "")
endif()

set(_RE_A   "${SYSROOT}/lib/${_RE_LIB}")
set(_BARE_A "${SYSROOT}/lib/${_BARE_LIB}")
if(MSVC)
  set(_CORE_A "${CMAKE_BINARY_DIR}/echosdk-core-build/$<CONFIG>/${_CORE_LIB}")
else()
  set(_CORE_A "${CMAKE_BINARY_DIR}/echosdk-core-build/${_CORE_LIB}")
endif()

# ---------------------------------------------------------------------------
# Phase 0.5 (Android, iOS): cross-build OpenSSL
#
# libre implements TLS *only* over OpenSSL — src/tls/ has an openssl/ backend
# and nothing else; USE_MBEDTLS there only swaps the hmac/md5/sha primitives.
# Without OpenSSL, re compiles src/tls/stub.c and src/aes/stub.c, so tls_alloc,
# dtls_listen and aes_alloc are all no-ops. That silently costs the whole
# secure stack: baresip's SIP/TLS and SIP/WSS transports are behind
# #ifdef USE_TLS (which re-config.cmake defines only for OpenSSL), and its
# dtls_srtp module returns early on `NOT USE_OPENSSL`. Neither Android nor iOS
# has a system OpenSSL, so build one here.
# ---------------------------------------------------------------------------
set(OPENSSL_ARGS "")
set(OPENSSL_EXTRA_LIBS "")
set(OPENSSL_DEPENDS "")
set(ECHOSDK_OPENSSL_LINK "")

if(ECHOSDK_BUILD_OPENSSL)
  if(NOT EXISTS "${ECHOSDK_OPENSSL_SOURCE_DIR}/Configure")
    message(FATAL_ERROR
      "ECHOSDK_BUILD_OPENSSL=ON but OpenSSL sources not found at "
      "${ECHOSDK_OPENSSL_SOURCE_DIR}. "
      "Run: ECHOSDK_OPENSSL_SRC=1 scripts/fetch-third-party.sh")
  endif()

  find_package(Perl REQUIRED)   # OpenSSL's Configure is a perl script

  if(ANDROID_ABI)
    # OpenSSL names its Android targets by architecture, not by Android ABI.
    if(ANDROID_ABI STREQUAL "arm64-v8a")
      set(_OSSL_TARGET "android-arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
      set(_OSSL_TARGET "android-arm")
    elseif(ANDROID_ABI STREQUAL "x86_64")
      set(_OSSL_TARGET "android-x86_64")
    elseif(ANDROID_ABI STREQUAL "x86")
      set(_OSSL_TARGET "android-x86")
    else()
      message(FATAL_ERROR "No OpenSSL target mapping for ANDROID_ABI=${ANDROID_ABI}")
    endif()

    # Configure needs the NDK root plus its clang wrappers on PATH. Derive both
    # from the compiler CMake already resolved rather than trusting an env var:
    # <ndk>/toolchains/llvm/prebuilt/<host>/bin/clang.
    get_filename_component(_OSSL_LLVM_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
    if(DEFINED ANDROID_NDK)
      set(_OSSL_NDK "${ANDROID_NDK}")
    else()
      get_filename_component(_OSSL_NDK "${_OSSL_LLVM_BIN}/../../../.." ABSOLUTE)
    endif()

    if(DEFINED ANDROID_PLATFORM_LEVEL)
      set(_OSSL_API "${ANDROID_PLATFORM_LEVEL}")
    else()
      set(_OSSL_API "${CMAKE_SYSTEM_VERSION}")
    endif()

    set(_OSSL_ENV "ANDROID_NDK_ROOT=${_OSSL_NDK}"
                  "PATH=${_OSSL_LLVM_BIN}:$ENV{PATH}")
    set(_OSSL_TARGET_ARGS ${_OSSL_TARGET} "-D__ANDROID_API__=${_OSSL_API}")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # OpenSSL's perl build compiles exactly one architecture per Configure
    # run, so an iOS build needs a single-arch CMAKE_OSX_ARCHITECTURES —
    # scripts/build-ios.sh builds device-arm64 / sim-arm64 / sim-x86_64 as
    # separate slices and lipo-merges the results.
    list(LENGTH CMAKE_OSX_ARCHITECTURES _OSSL_NARCH)
    if(_OSSL_NARCH GREATER 1)
      message(FATAL_ERROR
        "ECHOSDK_BUILD_OPENSSL on iOS needs a single-arch build, got "
        "CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}'. Build one "
        "slice per architecture and lipo them (see scripts/build-ios.sh).")
    endif()
    # The *-xcrun targets locate the SDK via xcrun themselves; only the arch
    # and the minimum OS need pinning here so OpenSSL matches the rest of the
    # slice (iossimulator-{arm64,x86_64}-xcrun exist since OpenSSL 3.2).
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
      if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(_OSSL_TARGET "iossimulator-x86_64-xcrun")
      else()
        set(_OSSL_TARGET "iossimulator-arm64-xcrun")
      endif()
      set(_OSSL_MINVER "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
      set(_OSSL_TARGET "ios64-xcrun")
      set(_OSSL_MINVER "-mios-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    set(_OSSL_ENV)   # xcrun is on PATH already; no extra environment needed
    set(_OSSL_TARGET_ARGS ${_OSSL_TARGET} "${_OSSL_MINVER}")
  else()
    message(FATAL_ERROR
      "ECHOSDK_BUILD_OPENSSL=ON has no cross-target mapping for "
      "'${CMAKE_SYSTEM_NAME}'. Desktop builds use the system OpenSSL instead.")
  endif()

  set(_SSL_A    "${SYSROOT}/lib/libssl.a")
  set(_CRYPTO_A "${SYSROOT}/lib/libcrypto.a")

  include(ProcessorCount)
  ProcessorCount(_OSSL_JOBS)
  if(_OSSL_JOBS EQUAL 0)
    set(_OSSL_JOBS 1)
  endif()

  # --libdir=lib: without it OpenSSL picks lib64 on 64-bit targets and the
  # BUILD_BYPRODUCTS paths below would never appear.
  # no-apps/no-docs/no-tests: we only ever consume the static libraries.
  #
  # The trim list below is about size. OpenSSL 3's default provider registers
  # every algorithm in one table, so a static link drags in essentially all of
  # libcrypto whatever the app calls — several MB per ABI. None of these
  # ciphers/digests is negotiable by a SIP registrar or a DTLS-SRTP peer, so
  # dropping them costs no interoperability. Deliberately left enabled: md5
  # (SIP digest auth), all of DTLS (dtls_srtp), and psk/dsa, which a stray
  # deployment could still pick.
  set(_OSSL_TRIM
    no-legacy no-engine no-comp no-ssl3 no-ssl3-method no-weak-ssl-ciphers
    no-srp no-ec2m
    no-idea no-rc2 no-rc4 no-rc5 no-bf no-cast no-seed no-camellia no-aria
    no-md2 no-md4 no-mdc2 no-whirlpool
    no-sm2 no-sm3 no-sm4 no-gost)
  ExternalProject_Add(openssl_project
    SOURCE_DIR "${ECHOSDK_OPENSSL_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/openssl-build"
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env
        ${_OSSL_ENV}
        "${PERL_EXECUTABLE}" "${ECHOSDK_OPENSSL_SOURCE_DIR}/Configure"
        ${_OSSL_TARGET_ARGS}
        no-shared no-tests no-apps no-docs
        ${_OSSL_TRIM}
        "--prefix=${SYSROOT}"
        "--openssldir=${SYSROOT}/ssl"
        "--libdir=lib"
    BUILD_COMMAND
      ${CMAKE_COMMAND} -E env
        ${_OSSL_ENV}
        make -j${_OSSL_JOBS} build_libs
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E env
        ${_OSSL_ENV}
        make install_dev
    BUILD_BYPRODUCTS "${_SSL_A}" "${_CRYPTO_A}"
  )

  # Pre-seed FindOpenSSL's cache variables so re/baresip resolve the archives
  # we just built. The NDK toolchain restricts find_library() to the NDK
  # sysroot, so an unaided find_package(OpenSSL) would come up empty.
  set(OPENSSL_ARGS
    "-DOPENSSL_USE_STATIC_LIBS=ON"
    "-DOPENSSL_ROOT_DIR=${SYSROOT}"
    "-DOPENSSL_INCLUDE_DIR=${SYSROOT}/include"
    "-DOPENSSL_SSL_LIBRARY=${_SSL_A}"
    "-DOPENSSL_CRYPTO_LIBRARY=${_CRYPTO_A}"
  )
  set(OPENSSL_EXTRA_LIBS "${_SSL_A}" "${_CRYPTO_A}")
  set(OPENSSL_DEPENDS openssl_project)
  # Same rationale as ECHOSDK_MBEDTLS_LINK: hand baresip the archives directly
  # instead of relying on FindOpenSSL re-running in its cmake context.
  set(ECHOSDK_OPENSSL_LINK "|${_SSL_A}|${_CRYPTO_A}")
endif()

# ---------------------------------------------------------------------------
# Phase 1 (mobile only): build mbedTLS
# ---------------------------------------------------------------------------
set(MBEDTLS_ARGS "")
set(MBEDTLS_EXTRA_LIBS "")

if(ECHOSDK_TLS STREQUAL "mbedtls")
  if(NOT EXISTS "${ECHOSDK_MBEDTLS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "ECHOSDK_TLS=mbedtls but mbedTLS sources not found at "
      "${ECHOSDK_MBEDTLS_SOURCE_DIR}. "
      "Run: git submodule update --init third_party/mbedtls")
  endif()

  set(_MBEDTLS_A   "${SYSROOT}/lib/${_LPFX}mbedtls${_EXT}")
  set(_MBEDX509_A  "${SYSROOT}/lib/${_LPFX}mbedx509${_EXT}")
  set(_MBEDCRYPTO_A "${SYSROOT}/lib/${_LPFX}mbedcrypto${_EXT}")

  ExternalProject_Add(mbedtls_project
    SOURCE_DIR "${ECHOSDK_MBEDTLS_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/mbedtls-build"
    LIST_SEPARATOR "|"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DCMAKE_INSTALL_PREFIX=${SYSROOT}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DENABLE_TESTING=OFF
      -DENABLE_PROGRAMS=OFF
      -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
      -DUSE_STATIC_MBEDTLS_LIBRARY=ON
      ${_NOTRACK_ARG}
      ${CROSS_ARGS}
    BUILD_BYPRODUCTS "${_MBEDTLS_A}" "${_MBEDX509_A}" "${_MBEDCRYPTO_A}"
  )

  set(MBEDTLS_ARGS
    "-DUSE_OPENSSL=OFF"
    "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON"
    "-DUSE_MBEDTLS=ON"
    "-DMBEDTLS_HINTS=${SYSROOT}"
    "-DMBEDTLS_INCLUDE_DIR=${SYSROOT}/include"
    "-DMBEDTLS_LIBRARY=${_MBEDCRYPTO_A}|${_MBEDX509_A}|${_MBEDTLS_A}"
  )
  set(MBEDTLS_EXTRA_LIBS "${_MBEDTLS_A}" "${_MBEDX509_A}" "${_MBEDCRYPTO_A}")
  set(MBEDTLS_DEPENDS mbedtls_project)
  # Suffix appended to RE_LIBRARIES so baresip consumers (webrtc, selftest)
  # get all three mbedTLS archives without relying on FindMBEDTLS running in
  # baresip's transitive cmake context (where its module path is different).
  set(ECHOSDK_MBEDTLS_LINK "|${_MBEDCRYPTO_A}|${_MBEDX509_A}|${_MBEDTLS_A}")
else()
  set(MBEDTLS_DEPENDS "")
  set(MBEDTLS_ARGS
    "-DUSE_OPENSSL=ON"
    "-DUSE_MBEDTLS=OFF"
  )
  set(ECHOSDK_MBEDTLS_LINK "")
endif()

# ---------------------------------------------------------------------------
# Phase 1.5: build libopus (if opus module is in the profile)
# ---------------------------------------------------------------------------
set(OPUS_ARGS "")
set(OPUS_EXTRA_LIBS "")
set(OPUS_DEPENDS "")

if("opus" IN_LIST ECHOSDK_MODULES)
  if(NOT EXISTS "${ECHOSDK_OPUS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "opus module enabled but sources not found at ${ECHOSDK_OPUS_SOURCE_DIR}. "
      "Run: git submodule update --init third_party/opus")
  endif()

  set(_OPUS_A "${SYSROOT}/lib/${_LPFX}opus${_EXT}")

  set(_OPUS_PLATFORM_ARGS "")
  if(ANDROID_ABI STREQUAL "armeabi-v7a"
     AND (NOT DEFINED ANDROID_ARM_NEON OR ANDROID_ARM_NEON))
    # opus compiles dnn/arm/arm_dnn_map.c whenever OPUS_ARM_MAY_HAVE_NEON is
    # set, but the compute_activation_*/compute_conv2d_* implementations its
    # dispatch tables point at only exist when OPUS_DNN is on (DEEP_PLC / DRED
    # / OSCE — all off here). Result: four dangling symbols in libechosdk.so
    # and a dlopen() failure on 32-bit devices. Those tables are compiled out
    # under OPUS_ARM_PRESUME_NEON, which is what aarch64 already gets for free
    # (OpusConfig.cmake forces it there) — that is why only v7a is affected.
    # Safe to presume: the NDK builds armeabi-v7a with NEON on by default.
    list(APPEND _OPUS_PLATFORM_ARGS "-DOPUS_PRESUME_NEON=ON")
  endif()

  ExternalProject_Add(opus_project
    SOURCE_DIR "${ECHOSDK_OPUS_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/opus-build"
    LIST_SEPARATOR "|"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DCMAKE_INSTALL_PREFIX=${SYSROOT}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DOPUS_BUILD_SHARED_LIBRARY=OFF
      -DOPUS_BUILD_TESTING=OFF
      -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF
      ${_OPUS_PLATFORM_ARGS}
      ${_NOTRACK_ARG}
      ${CROSS_ARGS}
    BUILD_BYPRODUCTS "${_OPUS_A}"
  )

  set(OPUS_ARGS
    "-DOPUS_INCLUDE_DIR=${SYSROOT}/include"
    "-DOPUS_LIBRARY=${_OPUS_A}"
  )
  set(OPUS_EXTRA_LIBS "${_OPUS_A}")
  set(OPUS_DEPENDS opus_project)
endif()

# ---------------------------------------------------------------------------
# Phase 2: build libre (includes librem via USE_REM=ON)
# ---------------------------------------------------------------------------
set(RE_PLATFORM_ARGS "")
set(_RE_CFLAGS "")
list(APPEND RE_PLATFORM_ARGS "-DUSE_UNIXSOCK=${_BSDK_UNIXSOCK_CM}")
if(ECHOSDK_TLS STREQUAL "mbedtls" AND NOT MSVC)
  # Upstream re only adds OPENSSL_INCLUDE_DIR/ZLIB to re-objs includes —
  # never MBEDTLS_INCLUDE_DIR — so hand the header path to the compiler.
  string(APPEND _RE_CFLAGS " -isystem${SYSROOT}/include")
endif()
# The SDK owns websock_connect() and sip_dialog_route() on every platform:
# cmake/patch-re-sources.cmake renames libre's definitions to __real_* behind
# these compile-time switches, and src/ws_path.c provides the public names, so
# every call site resolves to the SDK at link time.  This replaced the GNU-ld
# --wrap flags the shared-library links used to pass — Apple's linker has no
# --wrap, so the WS routing and BYE fixes never reached iOS (which could not
# even link: the rename hook this flag enables did not exist in the pinned re).
string(APPEND _RE_CFLAGS
       " -DRE_WEBSOCK_CONNECT_OVERRIDE=1 -DRE_SIP_DIALOG_ROUTE_OVERRIDE=1")
if(NOT _RE_CFLAGS STREQUAL "")
  list(APPEND RE_PLATFORM_ARGS "-DCMAKE_C_FLAGS=${_RE_CFLAGS}")
endif()

ExternalProject_Add(re_project
  DEPENDS ${MBEDTLS_DEPENDS} ${OPENSSL_DEPENDS}
  SOURCE_DIR "${ECHOSDK_RE_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/re-build"
  LIST_SEPARATOR "|"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=${SYSROOT}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DLIBRE_BUILD_SHARED=OFF
    -DLIBRE_BUILD_STATIC=ON
    -DUSE_REM=ON
    -DHAVE_THREADS=OFF
    ${RE_PLATFORM_ARGS}
    ${MBEDTLS_ARGS}
    ${OPENSSL_ARGS}
    ${_NOTRACK_ARG}
    ${CROSS_ARGS}
  BUILD_BYPRODUCTS "${_RE_A}"
)

# Apply EchoSDK's libre patches (BYE reply-drain, the route/websock rename
# hooks) before anything is compiled.  third_party/re is fetched by
# scripts/fetch-third-party.sh at a pinned revision and gitignored — not a
# submodule — so the patch cannot leak into upstream history.  The script is
# idempotent and fails loudly when a re bump moves the code it splices at.
ExternalProject_Add_Step(re_project patch_sources
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${ECHOSDK_RE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-re-sources.cmake
  DEPENDEES update
  DEPENDERS configure
  COMMENT "Patching libre sources (EchoSDK SIP fixes)"
)

# Patch libre headers for MSVC compatibility on Windows
if(WIN32)
  ExternalProject_Add_Step(re_project patch_headers
    COMMAND ${CMAKE_COMMAND}
      -DSYSROOT=${SYSROOT}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fix-msvc-headers.cmake
    DEPENDEES install
    COMMENT "Patching libre headers for MSVC compatibility"
  )
endif()

# ---------------------------------------------------------------------------
# Phase 3: build libbaresip (STATIC=ON links all modules in)
# ---------------------------------------------------------------------------
string(REPLACE ";" "|" ECHOSDK_MODULES_SEP "${ECHOSDK_MODULES}")

ExternalProject_Add(baresip_project
  DEPENDS re_project ${OPUS_DEPENDS} ${OPENSSL_DEPENDS}
  SOURCE_DIR "${ECHOSDK_BARESIP_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/baresip-build"
  LIST_SEPARATOR "|"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=${SYSROOT}
    -DCMAKE_PREFIX_PATH=${SYSROOT}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DRE_INCLUDE_DIR=${SYSROOT}/include/re
    -DRE_INCLUDE_DIRS=${SYSROOT}/include/re
    "-DRE_LIBRARY=${_RE_A}${ECHOSDK_MBEDTLS_LINK}${ECHOSDK_OPENSSL_LINK}"
    "-DRE_LIBRARIES=${_RE_A}${ECHOSDK_MBEDTLS_LINK}${ECHOSDK_OPENSSL_LINK}"
    -Dre_DIR=${SYSROOT}/lib/cmake/re
    -DSTATIC=ON
    "-DMODULES=${ECHOSDK_MODULES_SEP}"
    -DBUILD_TESTING=OFF
    # Must match re and echosdk_core — see the HAVE_UNIXSOCK note above.
    -DUSE_UNIXSOCK=${_BSDK_UNIXSOCK_CM}
    "-DCMAKE_C_FLAGS=-DJBUF_STAT=1 -D${_BSDK_UNIXSOCK_DEF}"
    -DHAVE_THREADS=OFF
    ${MBEDTLS_ARGS}
    ${OPENSSL_ARGS}
    ${OPUS_ARGS}
    ${_NOTRACK_ARG}
    ${CROSS_ARGS}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target baresip
  # baresip adds test/ and webrtc/ to ALL unconditionally (BUILD_TESTING is not
  # honoured), so the default install step ("--target install" => builds ALL)
  # drags in selftest and baresip-webrtc. Neither ships in the SDK, and
  # test/sip/user.c does not even compile on Android (its "struct user" clashes
  # with the NDK's <sys/user.h>). Install the Development component directly:
  # that is the static archive, the public headers and libbaresip.pc, all of
  # which the BUILD_COMMAND above has already produced.
  INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --component Development
  BUILD_BYPRODUCTS "${_BARE_A}"
)

# baresip's executables (baresip_exe, test/, webrtc/) link libre without
# ws_path.c, which owns the public websock_connect/sip_dialog_route names on
# every platform now — disable them everywhere (the SDK only consumes the
# static library).
ExternalProject_Add_Step(baresip_project disable_executables
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${ECHOSDK_BARESIP_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-baresip-sources.cmake
  DEPENDEES update
  DEPENDERS configure
  COMMENT "Patching baresip sources (executables disabled)"
)

if(WIN32)
  ExternalProject_Add_Step(baresip_project patch_sources
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${ECHOSDK_BARESIP_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fix-msvc-baresip.cmake
    DEPENDEES update
    DEPENDERS configure
    COMMENT "Patching baresip sources for MSVC compatibility"
  )
endif()

# ---------------------------------------------------------------------------
# Phase 3.5: compile EchoSDK source layer against installed headers
# ---------------------------------------------------------------------------
ExternalProject_Add(echosdk_core_project
  DEPENDS baresip_project
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/echosdk-core-build"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DECHOSDK_SRC_MODE=ON
    -DECHOSDK_SYSROOT=${SYSROOT}
    -DECHOSDK_MODULES_PROFILE=${ECHOSDK_MODULES_PROFILE}
    -DECHOSDK_WITH_WEBRTC_AEC=${ECHOSDK_WITH_WEBRTC_AEC}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    ${_NOTRACK_ARG}
    ${CROSS_ARGS}
  # BUILD_ALWAYS: this project compiles EchoSDK's *own* src/ and platform/.
  # An ExternalProject build step is stamped and runs once, so without this an
  # incremental build silently relinks the .so from a stale echosdk_core.a and
  # local edits appear to have no effect. The inner ninja no-ops when nothing
  # changed, so the cost is a directory scan. Third-party projects keep their
  # stamps — their sources are pinned and only move when a revision changes.
  BUILD_ALWAYS 1
  BUILD_BYPRODUCTS "${_CORE_A}"
  INSTALL_COMMAND ""
)

# ---------------------------------------------------------------------------
# Phase 4: merge archives into single echosdk.a / bare.lib
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MergeStaticLibs.cmake")

if(MSVC)
  set(_OUT "${CMAKE_BINARY_DIR}/bare.lib")
else()
  set(_OUT "${CMAKE_BINARY_DIR}/echosdk.a")
endif()

echosdk_merge(
  OUTPUT   "${_OUT}"
  INPUTS   "${_CORE_A}" "${_RE_A}" "${_BARE_A}" ${MBEDTLS_EXTRA_LIBS}
           ${OPENSSL_EXTRA_LIBS} ${OPUS_EXTRA_LIBS}
  DEPENDS  echosdk_core_project
)

# ---------------------------------------------------------------------------
# Phase 4.5 (optional): link a shared library from the merged archive
# ---------------------------------------------------------------------------
if(ECHOSDK_SHARED)
  if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      set(_SHARED_EXT ".dylib")
    else()
      set(_SHARED_EXT ".dylib")
    endif()
    set(_SHARED_OUT "${CMAKE_BINARY_DIR}/echosdk${_SHARED_EXT}")
    add_custom_command(
      OUTPUT  "${_SHARED_OUT}"
      COMMAND "${CMAKE_C_COMPILER}" -dynamiclib
              -Wl,-all_load "${_OUT}" -Wl,-noall_load
              -lz
              -framework CoreFoundation -framework Security
              -o "${_SHARED_OUT}"
      DEPENDS echosdk "${_OUT}"
      COMMENT "Linking echosdk.dylib"
      VERBATIM
    )
  elseif(MSVC)
    set(_SHARED_OUT "${CMAKE_BINARY_DIR}/echosdk.dll")
    find_library(_SSL_LIB    NAMES libssl    ssl    PATH_SUFFIXES lib)
    find_library(_CRYPTO_LIB NAMES libcrypto crypto PATH_SUFFIXES lib)
    find_library(_ZLIB_LIB   NAMES zlib      z      zs     PATH_SUFFIXES lib)
    add_custom_command(
      OUTPUT  "${_SHARED_OUT}"
      COMMAND link.exe /DLL /NOLOGO "/OUT:${_SHARED_OUT}"
              "/WHOLEARCHIVE:${_OUT}"
              "${_SSL_LIB}" "${_CRYPTO_LIB}" "${_ZLIB_LIB}"
              ws2_32.lib iphlpapi.lib crypt32.lib secur32.lib bcrypt.lib
              ole32.lib oleaut32.lib avrt.lib advapi32.lib user32.lib
              shell32.lib qwave.lib
      DEPENDS echosdk "${_OUT}"
      COMMENT "Linking echosdk.dll"
      VERBATIM
    )
  else()
    set(_SHARED_LINK_EXTRA "")
    if(ANDROID_ABI)
      # lib prefix: Android's APK packager and dlopen conventions expect
      # lib<name>.so; the Dart loader opens "libechosdk.so".
      set(_SHARED_OUT "${CMAKE_BINARY_DIR}/libechosdk.so")
      # -lz: re's cmake defines USE_ZLIB whenever find_package(ZLIB) succeeds,
      # which it does against the NDK sysroot, and re_crc32() then calls zlib's
      # crc32() for the STUN FINGERPRINT attribute. Upstream re expects
      # ZLIB::ZLIB on the link line; this command links the merged archive by
      # hand, so it has to say so itself. Omitting it shipped a .so with a
      # dangling `crc32` that dlopen() rejected at load (BIND_NOW). libz.so is
      # one of Android's stable public system libraries.
      set(_SHARED_PLATFORM_LIBS -llog -lOpenSLES -landroid -lm -lz)
      # The custom command invokes the bare clang driver, so the Android
      # target triple + sysroot must be passed explicitly (the toolchain
      # file only wires them into CMake-managed targets).
      # -Wl,-z,max-page-size=16384: Google Play 16 KB page requirement.
      # -Wl,--no-undefined: a -shared link happily leaves strong undefined
      # symbols dangling and the failure only surfaces as a dlopen() error on
      # a real device. Fail at link time instead (weak undefs, e.g.
      # memfd_create below the target API level, are still allowed).
      set(_SHARED_LINK_EXTRA
          --target=${CMAKE_C_COMPILER_TARGET}
          --sysroot=${CMAKE_SYSROOT}
          -Wl,-z,max-page-size=16384
          -Wl,--no-undefined)
    else()
      set(_SHARED_OUT "${CMAKE_BINARY_DIR}/echosdk.so")
      set(_SHARED_PLATFORM_LIBS -lpthread -lssl -lcrypto -lz -lm -ldl -lresolv -lpulse)
    endif()
    # --whole-archive exists for the baresip modules: each registers itself
    # from a constructor, so nothing references it and on-demand extraction
    # would drop them all. OpenSSL has no such objects — it is only ever
    # called directly — so keep it outside the group and let the linker take
    # what it needs. Inside, --whole-archive would add ~6 MB of unreferenced
    # libcrypto to every ABI. The merged echosdk.a still bundles OpenSSL, so
    # static consumers keep getting one self-contained archive.
    if(OPENSSL_EXTRA_LIBS)
      set(_WHOLE_LIBS "${_CORE_A}" "${_RE_A}" "${_BARE_A}"
                      ${MBEDTLS_EXTRA_LIBS} ${OPUS_EXTRA_LIBS})
      set(_ONDEMAND_LIBS ${OPENSSL_EXTRA_LIBS})
    else()
      set(_WHOLE_LIBS "${_OUT}")
      set(_ONDEMAND_LIBS "")
    endif()
    # opus is already merged into echosdk.a via OPUS_EXTRA_LIBS — no -lopus needed
    # No --wrap flags: the SIP fixes formerly interposed here live in the
    # patched libre sources (cmake/patch-re-sources.cmake) and in ws_path.c's
    # ownership of the public websock_connect/sip_dialog_route names, so they
    # hold on every platform and dist/ archives are self-contained.
    add_custom_command(
      OUTPUT  "${_SHARED_OUT}"
      COMMAND "${CMAKE_C_COMPILER}" -shared
              ${_SHARED_LINK_EXTRA}
              -Wl,--whole-archive ${_WHOLE_LIBS} -Wl,--no-whole-archive
              ${_ONDEMAND_LIBS}
              ${_SHARED_PLATFORM_LIBS}
              -o "${_SHARED_OUT}"
      DEPENDS echosdk "${_OUT}"
      COMMENT "Linking echosdk.so"
      VERBATIM
    )
  endif()

  add_custom_target(echosdk_shared ALL DEPENDS "${_SHARED_OUT}")
endif()

# ---------------------------------------------------------------------------
# Install merged archive + headers to dist/
# ---------------------------------------------------------------------------
set(_DIST "${ECHOSDK_DIST_DIR}/${ECHOSDK_PLATFORM_ID}")

install(FILES "${_OUT}" DESTINATION "${_DIST}")
if(ECHOSDK_SHARED AND DEFINED _SHARED_OUT)
  install(FILES "${_SHARED_OUT}" DESTINATION "${_DIST}")
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/echosdk.h"
              "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/echosdk.hpp"
              DESTINATION "${_DIST}/include")
install(DIRECTORY "${SYSROOT}/include/re/"  DESTINATION "${_DIST}/include/re")
install(FILES     "${SYSROOT}/include/baresip.h" DESTINATION "${_DIST}/include")
