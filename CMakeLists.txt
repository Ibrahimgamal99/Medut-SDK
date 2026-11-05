cmake_minimum_required(VERSION 3.19)
project(medut VERSION 2.0.0 LANGUAGES C)

include(ExternalProject)

# ---------------------------------------------------------------------------
# Source-only mode — used internally by medut_core_project ExternalProject.
# When MEDUT_SRC_MODE=ON this file compiles only src/<layer>/*.c + platform/<os>/*.c
# into medut_core and returns; the outer build then merges it with re + opus.
# ---------------------------------------------------------------------------
option(MEDUT_SRC_MODE "Compile Medut sources only (internal use)" OFF)

# ---------------------------------------------------------------------------
# HAVE_UNIXSOCK is an ABI switch, not a feature flag — it MUST be identical in
# re and medut_core. Set here, above the MEDUT_SRC_MODE branch, because the
# core sources are configured by a *separate* CMake invocation from the one
# that configures re: anything set inside a branch reaches only one of them.
#
# re's `struct sa` embeds `struct sockaddr_un` in its union only when
# HAVE_UNIXSOCK != 0, which changes sizeof(struct sa) from ~32 to ~112 bytes.
# `struct sip_msg` opens with two `struct sa` (src, dst), so a disagreement
# shifts every field after them by ~160 bytes. This does not fail to link and
# does not crash — it builds, runs, and silently reads the other library's
# structs at the wrong offsets. Android once passed USE_UNIXSOCK=OFF to re
# alone: re read scode=200 from a REGISTER response while the code above it
# read 55857 from the very same message, so registration completed on the
# wire and no event was ever emitted. The app sat in "registering" forever.
#
# The default is "on" in two places: re's own option(USE_UNIXSOCK ... ON) and
# the header guard `#if !defined(HAVE_UNIXSOCK) || HAVE_UNIXSOCK == 1` — so a
# target that never hears about the flag at all still gets the *large*
# struct. Set it explicitly everywhere rather than trusting the defaults.
set(MEDUT_USE_UNIXSOCK ON CACHE BOOL
    "Include AF_UNIX in re's struct sa (ABI-critical; must match everywhere)")
if(MEDUT_USE_UNIXSOCK)
  set(_BSDK_UNIXSOCK_CM "ON")
  set(_BSDK_UNIXSOCK_DEF "HAVE_UNIXSOCK=1")
else()
  set(_BSDK_UNIXSOCK_CM "OFF")
  set(_BSDK_UNIXSOCK_DEF "HAVE_UNIXSOCK=0")
endif()

if(MEDUT_SRC_MODE)
  cmake_minimum_required(VERSION 3.19)
  project(medut_core VERSION 2.0.0 LANGUAGES C)

  set(MEDUT_SYSROOT "" CACHE PATH "Path to installed re + opus headers")

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
  # One directory per layer (docs/architecture.md): the public API and the
  # SDK's policy (api), the stack's core, SIP, media, codecs, media
  # encryption, audio drivers and filters, and Medut's ICE agent.
  set(CORE_SRCS)
  foreach(_layer api core sip media codec menc audio ice)
    file(GLOB _LAYER_SRCS CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/src/${_layer}/*.c")
    list(APPEND CORE_SRCS ${_LAYER_SRCS})
  endforeach()

  add_library(medut_core STATIC ${CORE_SRCS} ${PLAT_SRCS})

  target_include_directories(medut_core PRIVATE
    "${MEDUT_SYSROOT}/include"
    "${MEDUT_SYSROOT}/include/re"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

  # Propagate the modules profile as a compile definition
  if(MEDUT_MODULES_PROFILE STREQUAL "mobile")
    target_compile_definitions(medut_core PRIVATE MEDUT_PROFILE_MOBILE=1)
  else()
    target_compile_definitions(medut_core PRIVATE MEDUT_PROFILE_DESKTOP=1)
  endif()

  # ABI-critical: must match re — see the HAVE_UNIXSOCK note above.
  target_compile_definitions(medut_core PRIVATE ${_BSDK_UNIXSOCK_DEF})

  # WebRTC AEC opt-in: Medut's own filter over libwebrtc-audio-processing-1
  # (src/audio/webrtc_apm.cpp, driven from src/audio/processing.c).  The
  # library is dlopen'ed through src/audio/webrtc_shim.c, so it is a build-time dependency
  # only.  The wrapper is the only C++ in the SDK and is built so that it
  # needs no libstdc++; the PRE_LINK check below keeps it that way.
  if(MEDUT_WITH_WEBRTC_AEC)
    enable_language(CXX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBRTC_AP REQUIRED webrtc-audio-processing-1)

    # Pin the SONAME of the library these headers came from.  An older
    # soversion has a different vtable order and Config layout.
    find_library(_BSDK_WEBRTC_LIB NAMES webrtc-audio-processing-1
                 HINTS ${WEBRTC_AP_LIBRARY_DIRS} REQUIRED)
    execute_process(COMMAND ${CMAKE_OBJDUMP} -p ${_BSDK_WEBRTC_LIB}
                    OUTPUT_VARIABLE _bsdk_od RESULT_VARIABLE _bsdk_rc)
    string(REGEX MATCH "SONAME[ \t]+([^ \t\r\n]+)" _bsdk_m "${_bsdk_od}")
    if(NOT _bsdk_rc EQUAL 0 OR NOT CMAKE_MATCH_1)
      message(FATAL_ERROR "cannot read the SONAME of ${_BSDK_WEBRTC_LIB}")
    endif()
    set(_BSDK_WEBRTC_SONAME "${CMAKE_MATCH_1}")
    message(STATUS "WebRTC AEC: ${_BSDK_WEBRTC_SONAME} "
                   "(webrtc-audio-processing ${WEBRTC_AP_VERSION})")

    add_library(bsdk_webrtc_apm OBJECT
      "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/webrtc_apm.cpp")
    target_include_directories(bsdk_webrtc_apm SYSTEM PRIVATE
      ${WEBRTC_AP_INCLUDE_DIRS})
    target_compile_options(bsdk_webrtc_apm PRIVATE
      ${WEBRTC_AP_CFLAGS_OTHER}
      -fno-exceptions -fno-rtti -fno-threadsafe-statics)
    set_target_properties(bsdk_webrtc_apm PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON)
    target_sources(medut_core PRIVATE $<TARGET_OBJECTS:bsdk_webrtc_apm>)

    add_custom_command(TARGET medut_core PRE_LINK
      COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
              "-DOBJS=$<TARGET_OBJECTS:bsdk_webrtc_apm>"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-webrtc-apm-symbols.cmake"
      COMMAND_EXPAND_LISTS
      VERBATIM)

    target_compile_definitions(medut_core PRIVATE
      MEDUT_HAS_WEBRTC_AEC=1
      MEDUT_WEBRTC_SONAME="${_BSDK_WEBRTC_SONAME}")
  endif()

  # Platform audio define: src/audio/device.c registers this build's driver
  # (platform/<os>/). Android uses OpenSL ES (sles_vc.c): AAudio needs
  # API >= 26 (libaaudio.so does not exist in the android-24 sysroot) while
  # minSdk is 24.
  if(ANDROID_ABI)
    target_compile_definitions(medut_core PRIVATE MEDUT_AUDIO_OPENSLES=1)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(medut_core PRIVATE MEDUT_AUDIO_AUDIOUNIT=1)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(medut_core PRIVATE MEDUT_AUDIO_COREAUDIO=1)
  elseif(WIN32)
    target_compile_definitions(medut_core PRIVATE MEDUT_AUDIO_WASAPI=1)
  else()
    target_compile_definitions(medut_core PRIVATE MEDUT_AUDIO_PULSE=1)
  endif()

  set_target_properties(medut_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    PREFIX "")

  install(TARGETS medut_core ARCHIVE DESTINATION .)
  return()
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Generate bindings/cpp/medut.hpp from include/medut.h
# ---------------------------------------------------------------------------
add_custom_command(
  OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/medut.hpp"
  COMMAND "${CMAKE_COMMAND}"
          -D "C_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/include/medut.h"
          -D "CPP_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/medut.hpp"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_hpp.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/medut.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_hpp.cmake"
  COMMENT "Regenerating medut.hpp from medut.h"
  VERBATIM
)
add_custom_target(gen_hpp ALL
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/medut.hpp")

set(MEDUT_TLS "openssl" CACHE STRING "TLS backend: openssl or mbedtls")
set(MEDUT_MODULES_PROFILE "desktop" CACHE STRING "Module profile: desktop or mobile")
# Desktop targets link the platform's own libssl/libcrypto. Android and iOS
# have no system OpenSSL, so cross-compiled targets build it from
# third_party/openssl.  (libre implements TLS/DTLS only over OpenSSL — an
# mbedTLS build silently stubs out SIP/TLS, SIP/WSS and DTLS-SRTP; see the
# Phase 0.5 note below.)
if((ANDROID_ABI OR CMAKE_SYSTEM_NAME STREQUAL "iOS") AND MEDUT_TLS STREQUAL "openssl")
  set(_build_openssl_default ON)
else()
  set(_build_openssl_default OFF)
endif()
option(MEDUT_BUILD_OPENSSL
       "Cross-build OpenSSL from third_party/openssl instead of using a system one"
       ${_build_openssl_default})
option(MEDUT_SHARED "Also produce a shared library (.so/.dylib/.dll) from the merged archive" OFF)
# WebRTC AEC: desktop Linux only.  src/audio/webrtc_shim.c resolves the library's
# symbols by their ELF names and loads it with dlopen by SONAME, neither of
# which carries over to Mach-O or Windows.  Only the 1.x API is supported:
# 2.x (pkg-config webrtc-audio-processing-2) returns scoped_refptr from
# Create() and is a different ABI.
set(_aec_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID_ABI)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(WEBRTC_AP QUIET webrtc-audio-processing-1)
    if(WEBRTC_AP_FOUND)
      set(_aec_default ON)
    else()
      pkg_check_modules(WEBRTC_AP2 QUIET webrtc-audio-processing-2)
      if(WEBRTC_AP2_FOUND)
        message(STATUS "webrtc-audio-processing-2 found, but only 1.x is "
                       "supported: building without WebRTC AEC")
      endif()
    endif()
  endif()
endif()
option(MEDUT_WITH_WEBRTC_AEC
       "Include the WebRTC AEC/NS filter (desktop Linux only; requires webrtc-audio-processing-1)"
       ${_aec_default})
if(MEDUT_WITH_WEBRTC_AEC)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID_ABI)
    message(FATAL_ERROR "MEDUT_WITH_WEBRTC_AEC is supported on desktop Linux only")
  endif()
  if(NOT WEBRTC_AP_FOUND)
    message(FATAL_ERROR "MEDUT_WITH_WEBRTC_AEC needs webrtc-audio-processing-1 "
                        "(1.x; 2.x is not supported)")
  endif()
endif()

set(MEDUT_RE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/re"
    CACHE PATH "Path to libre source tree")
set(MEDUT_MBEDTLS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls"
    CACHE PATH "Path to mbedTLS source tree (required when MEDUT_TLS=mbedtls)")
set(MEDUT_OPENSSL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openssl"
    CACHE PATH "Path to OpenSSL source tree (required when MEDUT_BUILD_OPENSSL=ON)")
set(MEDUT_OPUS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus"
    CACHE PATH "Path to libopus source tree")
set(MEDUT_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    CACHE PATH "Top-level output directory for built archives")

set(SYSROOT "${CMAKE_BINARY_DIR}/sysroot")

# ---------------------------------------------------------------------------
# Platform ID for dist/ subdirectory naming
# ---------------------------------------------------------------------------
if(ANDROID_ABI)
  set(MEDUT_PLATFORM_ID "android/${ANDROID_ABI}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  if(CMAKE_OSX_SYSROOT MATCHES "simulator")
    set(MEDUT_PLATFORM_ID "ios/simulator")
  else()
    set(MEDUT_PLATFORM_ID "ios/device")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  string(REPLACE ";" "_" _ARCH_SLUG "${CMAKE_OSX_ARCHITECTURES}")
  set(MEDUT_PLATFORM_ID "macos/${_ARCH_SLUG}")
elseif(WIN32)
  set(MEDUT_PLATFORM_ID "windows/x64")
else()
  set(MEDUT_PLATFORM_ID "linux/x86_64")
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
  set(_CORE_LIB "medut_core.lib")
  # Embed TrackFileAccess=false in every generated .vcxproj for sub-projects.
  # This prevents CL.exe's PostExecuteTool from writing .tlog files, which
  # fails with "user-mapped section open" when another process has them mapped.
  set(_NOTRACK_ARG "-DCMAKE_VS_GLOBALS=TrackFileAccess=false")
else()
  set(_LPFX "lib")
  set(_EXT  ".a")
  set(_RE_LIB   "libre.a")
  set(_CORE_LIB "medut_core.a")
  set(_NOTRACK_ARG "")
endif()

set(_RE_A   "${SYSROOT}/lib/${_RE_LIB}")
if(MSVC)
  set(_CORE_A "${CMAKE_BINARY_DIR}/medut-core-build/$<CONFIG>/${_CORE_LIB}")
else()
  set(_CORE_A "${CMAKE_BINARY_DIR}/medut-core-build/${_CORE_LIB}")
endif()

# ---------------------------------------------------------------------------
# Phase 0.5 (Android, iOS): cross-build OpenSSL
#
# libre implements TLS *only* over OpenSSL — src/tls/ has an openssl/ backend
# and nothing else; USE_MBEDTLS there only swaps the hmac/md5/sha primitives.
# Without OpenSSL, re compiles src/tls/stub.c and src/aes/stub.c, so tls_alloc,
# dtls_listen and aes_alloc are all no-ops. That silently costs the whole
# secure stack: SIP/TLS, SIP/WSS and DTLS-SRTP are all behind #ifdef USE_TLS
# (which re-config.cmake defines only for OpenSSL). Neither Android nor iOS
# has a system OpenSSL, so build one here.
# ---------------------------------------------------------------------------
set(OPENSSL_ARGS "")
set(OPENSSL_EXTRA_LIBS "")
set(OPENSSL_DEPENDS "")
set(MEDUT_OPENSSL_LINK "")

if(MEDUT_BUILD_OPENSSL)
  if(NOT EXISTS "${MEDUT_OPENSSL_SOURCE_DIR}/Configure")
    message(FATAL_ERROR
      "MEDUT_BUILD_OPENSSL=ON but OpenSSL sources not found at "
      "${MEDUT_OPENSSL_SOURCE_DIR}. "
      "Run: MEDUT_OPENSSL_SRC=1 scripts/fetch-third-party.sh")
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
        "MEDUT_BUILD_OPENSSL on iOS needs a single-arch build, got "
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
      "MEDUT_BUILD_OPENSSL=ON has no cross-target mapping for "
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
    SOURCE_DIR "${MEDUT_OPENSSL_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/openssl-build"
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env
        ${_OSSL_ENV}
        "${PERL_EXECUTABLE}" "${MEDUT_OPENSSL_SOURCE_DIR}/Configure"
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

  # Pre-seed FindOpenSSL's cache variables so re resolves the archives
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
  set(MEDUT_OPENSSL_LINK "|${_SSL_A}|${_CRYPTO_A}")
endif()

# ---------------------------------------------------------------------------
# Phase 1 (mobile only): build mbedTLS
# ---------------------------------------------------------------------------
set(MBEDTLS_ARGS "")
set(MBEDTLS_EXTRA_LIBS "")

if(MEDUT_TLS STREQUAL "mbedtls")
  if(NOT EXISTS "${MEDUT_MBEDTLS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "MEDUT_TLS=mbedtls but mbedTLS sources not found at "
      "${MEDUT_MBEDTLS_SOURCE_DIR}. "
      "Run: git submodule update --init third_party/mbedtls")
  endif()

  set(_MBEDTLS_A   "${SYSROOT}/lib/${_LPFX}mbedtls${_EXT}")
  set(_MBEDX509_A  "${SYSROOT}/lib/${_LPFX}mbedx509${_EXT}")
  set(_MBEDCRYPTO_A "${SYSROOT}/lib/${_LPFX}mbedcrypto${_EXT}")

  ExternalProject_Add(mbedtls_project
    SOURCE_DIR "${MEDUT_MBEDTLS_SOURCE_DIR}"
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
  # All three mbedTLS archives, for whatever links libre.
  set(MEDUT_MBEDTLS_LINK "|${_MBEDCRYPTO_A}|${_MBEDX509_A}|${_MBEDTLS_A}")
else()
  set(MBEDTLS_DEPENDS "")
  set(MBEDTLS_ARGS
    "-DUSE_OPENSSL=ON"
    "-DUSE_MBEDTLS=OFF"
  )
  set(MEDUT_MBEDTLS_LINK "")
endif()

# ---------------------------------------------------------------------------
# Phase 1.5: build libopus (if opus module is in the profile)
# ---------------------------------------------------------------------------
set(OPUS_ARGS "")
set(OPUS_EXTRA_LIBS "")
set(OPUS_DEPENDS "")

# libopus: the SDK's Opus codec (src/codec/) links it directly.
if(TRUE)
  if(NOT EXISTS "${MEDUT_OPUS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "opus module enabled but sources not found at ${MEDUT_OPUS_SOURCE_DIR}. "
      "Run: git submodule update --init third_party/opus")
  endif()

  set(_OPUS_A "${SYSROOT}/lib/${_LPFX}opus${_EXT}")

  set(_OPUS_PLATFORM_ARGS "")
  if(ANDROID_ABI STREQUAL "armeabi-v7a"
     AND (NOT DEFINED ANDROID_ARM_NEON OR ANDROID_ARM_NEON))
    # opus compiles dnn/arm/arm_dnn_map.c whenever OPUS_ARM_MAY_HAVE_NEON is
    # set, but the compute_activation_*/compute_conv2d_* implementations its
    # dispatch tables point at only exist when OPUS_DNN is on (DEEP_PLC / DRED
    # / OSCE — all off here). Result: four dangling symbols in libmedut.so
    # and a dlopen() failure on 32-bit devices. Those tables are compiled out
    # under OPUS_ARM_PRESUME_NEON, which is what aarch64 already gets for free
    # (OpusConfig.cmake forces it there) — that is why only v7a is affected.
    # Safe to presume: the NDK builds armeabi-v7a with NEON on by default.
    list(APPEND _OPUS_PLATFORM_ARGS "-DOPUS_PRESUME_NEON=ON")
  endif()

  # opus only applies -fvisibility=hidden when BUILD_SHARED_LIBS is on, but its
  # aarch64 NEON MDCT (celt/arm/celt_tx_neon.S) assumes it: it reaches the C
  # twiddle tables (celt_tx_tab_*) with a direct adrp/add, which lld rejects
  # against a preemptible symbol once the static archive is linked into
  # libmedut.so. The public opus_* API keeps default visibility via
  # OPUS_EXPORT, so the exported surface is unchanged apart from opus
  # internals no longer leaking out of the .so.
  ExternalProject_Add(opus_project
    SOURCE_DIR "${MEDUT_OPUS_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/opus-build"
    LIST_SEPARATOR "|"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DCMAKE_INSTALL_PREFIX=${SYSROOT}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DCMAKE_C_VISIBILITY_PRESET=hidden
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
if(MEDUT_TLS STREQUAL "mbedtls" AND NOT MSVC)
  # Upstream re only adds OPENSSL_INCLUDE_DIR/ZLIB to re-objs includes —
  # never MBEDTLS_INCLUDE_DIR — so hand the header path to the compiler.
  string(APPEND _RE_CFLAGS " -isystem${SYSROOT}/include")
endif()
# The SDK owns websock_connect() and sip_dialog_route() on every platform:
# cmake/patch-re-sources.cmake renames libre's definitions to __real_* behind
# these compile-time switches, and src/api/ws_path.c provides the public names, so
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
  SOURCE_DIR "${MEDUT_RE_SOURCE_DIR}"
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

# Apply Medut's libre patches (BYE reply-drain, the route/websock rename
# hooks) before anything is compiled.  third_party/re is fetched by
# scripts/fetch-third-party.sh at a pinned revision and gitignored — not a
# submodule — so the patch cannot leak into upstream history.  The script is
# idempotent and fails loudly when a re bump moves the code it splices at.
#
# The patch steps depend on their scripts and on the checkout's .git/HEAD, so
# a changed patch or a new pinned revision re-patches and rebuilds (configure,
# build and install follow the step).  Without them an existing build dir kept
# the sysroot of whatever patch it was first built with.
function(bsdk_patch_depends _out _script _srcdir)
  set(_deps "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_script}")
  if(EXISTS "${_srcdir}/.git/HEAD")
    list(APPEND _deps "${_srcdir}/.git/HEAD")
  endif()
  set(${_out} ${_deps} ${ARGN} PARENT_SCOPE)
endfunction()

bsdk_patch_depends(_RE_PATCH_DEPS patch-re-sources.cmake "${MEDUT_RE_SOURCE_DIR}")
ExternalProject_Add_Step(re_project patch_sources
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${MEDUT_RE_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-re-sources.cmake
  DEPENDS ${_RE_PATCH_DEPS}
  DEPENDEES update
  DEPENDERS configure
  COMMENT "Patching libre sources (Medut SIP fixes)"
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
# Phase 3: the Medut sources — what they build against
# ---------------------------------------------------------------------------
set(_CORE_DEPENDS re_project ${OPUS_DEPENDS} ${OPENSSL_DEPENDS})

ExternalProject_Add(medut_core_project
  DEPENDS ${_CORE_DEPENDS}
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/medut-core-build"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DMEDUT_SRC_MODE=ON
    -DMEDUT_SYSROOT=${SYSROOT}
    -DMEDUT_MODULES_PROFILE=${MEDUT_MODULES_PROFILE}
    -DMEDUT_WITH_WEBRTC_AEC=${MEDUT_WITH_WEBRTC_AEC}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    ${_NOTRACK_ARG}
    ${CROSS_ARGS}
  # BUILD_ALWAYS: this project compiles Medut's *own* src/ and platform/.
  # An ExternalProject build step is stamped and runs once, so without this an
  # incremental build silently relinks the .so from a stale medut_core.a and
  # local edits appear to have no effect. The inner ninja no-ops when nothing
  # changed, so the cost is a directory scan. Third-party projects keep their
  # stamps — their sources are pinned and only move when a revision changes.
  BUILD_ALWAYS 1
  BUILD_BYPRODUCTS "${_CORE_A}"
  INSTALL_COMMAND ""
)

# ---------------------------------------------------------------------------
# Phase 4: merge archives into single medut.a / bare.lib
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MergeStaticLibs.cmake")

if(MSVC)
  set(_OUT "${CMAKE_BINARY_DIR}/bare.lib")
else()
  set(_OUT "${CMAKE_BINARY_DIR}/medut.a")
endif()

# Merge again whenever an input library was reinstalled.  Their archives are
# declared BUILD_BYPRODUCTS, but each project's *install* step writes the copy
# merged here, after ninja has already judged the merge up to date: a change
# to libre alone (a patch in cmake/patch-re-sources.cmake) left the shipped
# archive without it until some SDK source changed too.  The install stamps
# are outputs ninja knows about.
set(_MERGE_STAMPS "")
foreach(_ep re_project opus_project openssl_project mbedtls_project)
  if(TARGET ${_ep})
    ExternalProject_Get_Property(${_ep} STAMP_DIR)
    list(APPEND _MERGE_STAMPS "${STAMP_DIR}/${_ep}-install")
  endif()
endforeach()

medut_merge(
  OUTPUT   "${_OUT}"
  INPUTS   "${_CORE_A}" "${_RE_A}" ${MBEDTLS_EXTRA_LIBS}
           ${OPENSSL_EXTRA_LIBS} ${OPUS_EXTRA_LIBS}
  DEPENDS  medut_core_project ${_MERGE_STAMPS}
)

# ---------------------------------------------------------------------------
# Phase 4.5 (optional): link a shared library from the merged archive
# ---------------------------------------------------------------------------
if(MEDUT_SHARED)
  if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      set(_SHARED_EXT ".dylib")
    else()
      set(_SHARED_EXT ".dylib")
    endif()
    set(_SHARED_OUT "${CMAKE_BINARY_DIR}/medut${_SHARED_EXT}")
    add_custom_command(
      OUTPUT  "${_SHARED_OUT}"
      COMMAND "${CMAKE_C_COMPILER}" -dynamiclib
              -Wl,-all_load "${_OUT}" -Wl,-noall_load
              "-Wl,-exported_symbol,_medut_*"
              -lz
              -framework CoreFoundation -framework Security
              -o "${_SHARED_OUT}"
      DEPENDS medut "${_OUT}"
      COMMENT "Linking medut.dylib"
      VERBATIM
    )
  elseif(MSVC)
    set(_SHARED_OUT "${CMAKE_BINARY_DIR}/medut.dll")
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
      DEPENDS medut "${_OUT}"
      COMMENT "Linking medut.dll"
      VERBATIM
    )
  else()
    set(_SHARED_LINK_EXTRA "")
    if(ANDROID_ABI)
      # lib prefix: Android's APK packager and dlopen conventions expect
      # lib<name>.so; the Dart loader opens "libmedut.so".
      set(_SHARED_OUT "${CMAKE_BINARY_DIR}/libmedut.so")
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
      set(_SHARED_OUT "${CMAKE_BINARY_DIR}/medut.so")
      set(_SHARED_PLATFORM_LIBS -lpthread -lssl -lcrypto -lz -lm -ldl -lresolv -lpulse)
    endif()
    # --whole-archive: a -shared link of archives nothing references yet
    # would otherwise produce an empty library. OpenSSL is only ever
    # called directly — so keep it outside the group and let the linker take
    # what it needs. Inside, --whole-archive would add ~6 MB of unreferenced
    # libcrypto to every ABI. The merged medut.a still bundles OpenSSL, so
    # static consumers keep getting one self-contained archive.
    if(OPENSSL_EXTRA_LIBS)
      set(_WHOLE_LIBS "${_CORE_A}" "${_RE_A}"
                      ${MBEDTLS_EXTRA_LIBS} ${OPUS_EXTRA_LIBS})
      set(_ONDEMAND_LIBS ${OPENSSL_EXTRA_LIBS})
    else()
      set(_WHOLE_LIBS "${_OUT}")
      set(_ONDEMAND_LIBS "")
    endif()
    # opus is already merged into medut.a via OPUS_EXTRA_LIBS — no -lopus needed
    # No --wrap flags: the SIP fixes formerly interposed here live in the
    # patched libre sources (cmake/patch-re-sources.cmake) and in ws_path.c's
    # ownership of the public websock_connect/sip_dialog_route names, so they
    # hold on every platform and dist/ archives are self-contained.
    add_custom_command(
      OUTPUT  "${_SHARED_OUT}"
      COMMAND "${CMAKE_C_COMPILER}" -shared
              ${_SHARED_LINK_EXTRA}
              # Only the public API (and Android's JNI entry points) is
              # exported: scripts/exports.ver.
              "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/scripts/exports.ver"
              -Wl,--whole-archive ${_WHOLE_LIBS} -Wl,--no-whole-archive
              ${_ONDEMAND_LIBS}
              ${_SHARED_PLATFORM_LIBS}
              -o "${_SHARED_OUT}"
      DEPENDS medut "${_OUT}"
      COMMENT "Linking medut.so"
      VERBATIM
    )
  endif()

  add_custom_target(medut_shared ALL DEPENDS "${_SHARED_OUT}")
endif()

# ---------------------------------------------------------------------------
# Install merged archive + headers to dist/
# ---------------------------------------------------------------------------
set(_DIST "${MEDUT_DIST_DIR}/${MEDUT_PLATFORM_ID}")

install(FILES "${_OUT}" DESTINATION "${_DIST}")
if(MEDUT_SHARED AND DEFINED _SHARED_OUT)
  install(FILES "${_SHARED_OUT}" DESTINATION "${_DIST}")
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/medut.h"
              "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/medut.hpp"
              DESTINATION "${_DIST}/include")
install(DIRECTORY "${SYSROOT}/include/re/"  DESTINATION "${_DIST}/include/re")

