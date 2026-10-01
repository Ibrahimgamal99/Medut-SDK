#!/usr/bin/env bash
# Build EchoSDK for iOS as a DYNAMIC xcframework (macOS + Xcode required).
#
# Output:
#   dist/ios/EchoSDK.xcframework    device (arm64) + simulator (arm64, x86_64)
#   dist/ios/device/echosdk.a       static archive (for non-Flutter consumers)
#   dist/ios/simulator/echosdk.a    fat static archive (arm64 + x86_64)
#   dist/ios/include/               public headers
# Also refreshes the Flutter plugin's vendored copy in
# bindings/flutter/ios/Frameworks/ — no second command to run.
#
# A dynamic framework (not a static lib) is what the Flutter plugin vendors:
# with a static archive, symbols reached only via dart:ffi dlsym() are
# dead-stripped from Release app binaries; a dylib's exported symbols are
# always visible to DynamicLibrary.process(). CocoaPods embeds vendored
# dynamic frameworks automatically.
#
# TLS is OpenSSL, cross-built from source per slice (Phase 0.5 in
# CMakeLists.txt): libre implements TLS/DTLS only over OpenSSL, so an mbedTLS
# build silently ships without SIP/TLS, SIP/WSS and DTLS-SRTP — the secure
# stack this SDK exists to provide. OpenSSL's perl build does one architecture
# per Configure run, which is why the simulator is built as two thin slices
# and lipo-merged rather than one fat cmake build.
#
# Env knobs:
#   IOS_DEPLOYMENT_TARGET  minimum iOS (default 13.0)
#   BUILD_TYPE             CMake config (default Release)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_TYPE="${BUILD_TYPE:-Release}"
MIN_IOS="${IOS_DEPLOYMENT_TARGET:-13.0}"
DIST="${ROOT}/dist/ios"
# -lz: re's cmake defines USE_ZLIB whenever find_package(ZLIB) succeeds, which
# it does against the Apple SDK (libz.tbd), and re_crc32() then calls zlib's
# crc32() for the STUN FINGERPRINT attribute — same story as the Android link.
# -lresolv: re's check_symbol_exists(res_ninit) succeeds against the iOS SDK,
# so re/cmake/re-config.cmake defines HAVE_RESOLV and src/dns/res.c calls
# res_ninit/res_getservers/res_nclose to read the system resolver. Those live
# in libresolv.tbd, not libSystem — on Apple they are the res_9_* symbols, so
# without this the link fails with "Undefined symbols: _res_9_ninit". re only
# records `resolv` in its own RE_LIBS; this dylib is hand-linked, so it has to
# be repeated here (as on Linux, scripts/build-linux.sh).
FRAMEWORKS_FOR_LINK=(-framework AVFoundation -framework AudioToolbox
                     -framework CoreAudio -framework CoreFoundation
                     -lz -lresolv)

if ! command -v xcodebuild >/dev/null; then
  echo "ERROR: xcodebuild not found — this script requires macOS + Xcode." >&2
  exit 1
fi

# Fetch third-party sources (idempotent), OpenSSL included — see TLS note above.
ECHOSDK_TLS=openssl ECHOSDK_OPENSSL_SRC=1 bash "${SCRIPT_DIR}/fetch-third-party.sh"

# ---------------------------------------------------------------------------
# Single-config generator (Ninja, like every other platform script) — NOT
# -GXcode. Everything here is an ExternalProject, and ExternalProject_Add
# inherits the parent generator, so -GXcode builds libre under Xcode too.
# There, `add_library(re STATIC $<TARGET_OBJECTS:re-objs>)` has no real source
# file of its own, and Xcode emits no product for a target with an empty
# Compile Sources phase (CMake issue #17457). The build reports success without
# ever running libtool for libre.a, and re's install step then dies with
#   file INSTALL cannot find .../re-build/Release-iphoneos/libre.a
# CMakeLists.txt assumes single-config sub-builds elsewhere as well — _CORE_A
# is echosdk-core-build/echosdk_core.a with no $(CONFIGURATION) component — and
# nothing in this build needs Xcode: the dylibs are hand-linked with clang.
# ---------------------------------------------------------------------------
if command -v ninja >/dev/null 2>&1; then
  GENERATOR="Ninja"
else
  GENERATOR="Unix Makefiles"
fi
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# ---------------------------------------------------------------------------
# Helper: configure + build the static archive for one single-arch slice
# ---------------------------------------------------------------------------
build_slice() {
  local NAME="$1"; local SYSROOT="$2"; local ARCH="$3"
  local BUILD_DIR="${ROOT}/build/ios-${NAME}"

  rm -rf "${BUILD_DIR}"
  cmake -S "${ROOT}" -B "${BUILD_DIR}" -G "${GENERATOR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="${SYSROOT}" \
    -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MIN_IOS}" \
    -DECHOSDK_TLS=openssl \
    -DECHOSDK_MODULES_PROFILE=mobile

  cmake --build "${BUILD_DIR}" --target echosdk -j"${JOBS}"

  local LIB
  LIB="$(find "${BUILD_DIR}" -name "echosdk.a" | head -1)"
  mkdir -p "${DIST}/${NAME}"
  cp "${LIB}" "${DIST}/${NAME}/echosdk.a"

  # Headers (identical across slices — copy once)
  if [ ! -d "${DIST}/include" ]; then
    mkdir -p "${DIST}/include"
    cp "${ROOT}/include/echosdk.h" "${DIST}/include/"
    local RE_SYSROOT="${BUILD_DIR}/sysroot"
    [ -d "${RE_SYSROOT}/include" ] && cp -r "${RE_SYSROOT}/include/." "${DIST}/include/"
  fi
}

# ---------------------------------------------------------------------------
# Helper: link one dylib for one arch of one slice
# ---------------------------------------------------------------------------
link_dylib() {
  local SDK="$1"; local ARCH="$2"; local TARGET_SUFFIX="$3"
  local ARCHIVE="$4"; local OUT="$5"
  local SDK_PATH
  SDK_PATH="$(xcrun --sdk "${SDK}" --show-sdk-path)"

  xcrun --sdk "${SDK}" clang -dynamiclib \
    -arch "${ARCH}" \
    -isysroot "${SDK_PATH}" \
    -target "${ARCH}-apple-ios${MIN_IOS}${TARGET_SUFFIX}" \
    -Wl,-all_load "${ARCHIVE}" \
    "${FRAMEWORKS_FOR_LINK[@]}" \
    -lc++ \
    -install_name @rpath/EchoSDK.framework/EchoSDK \
    -o "${OUT}"
}

# ---------------------------------------------------------------------------
# Helper: assemble EchoSDK.framework for one slice from a (fat) dylib
# ---------------------------------------------------------------------------
make_framework() {
  local DYLIB="$1"; local FW_DIR="$2"
  rm -rf "${FW_DIR}"
  mkdir -p "${FW_DIR}"
  cp "${DYLIB}" "${FW_DIR}/EchoSDK"
  cat > "${FW_DIR}/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleExecutable</key><string>EchoSDK</string>
	<key>CFBundleIdentifier</key><string>dev.echosdk.core</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>EchoSDK</string>
	<key>CFBundlePackageType</key><string>FMWK</string>
	<key>CFBundleShortVersionString</key><string>1.0.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>MinimumOSVersion</key><string>${MIN_IOS}</string>
</dict>
</plist>
PLIST
  codesign --force --sign - "${FW_DIR}" >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# Device slice: arm64 / iphoneos
# ---------------------------------------------------------------------------
echo "=== iOS device (arm64, iphoneos) ==="
build_slice "device" "iphoneos" "arm64"
link_dylib iphoneos arm64 "" \
  "${DIST}/device/echosdk.a" "${DIST}/device/echosdk.dylib"
make_framework "${DIST}/device/echosdk.dylib" "${DIST}/device/EchoSDK.framework"

# ---------------------------------------------------------------------------
# Simulator slices: arm64 and x86_64 built thin (one OpenSSL arch per
# Configure run), dylibs lipo-merged. Device-arm64 and simulator-arm64 remain
# distinct slices by their Mach-O LC_BUILD_VERSION platform tag — the
# xcframework keeps them apart.
# ---------------------------------------------------------------------------
echo "=== iOS simulator (arm64, iphonesimulator) ==="
build_slice "sim-arm64" "iphonesimulator" "arm64"
link_dylib iphonesimulator arm64 "-simulator" \
  "${DIST}/sim-arm64/echosdk.a" "${DIST}/sim-arm64/echosdk.dylib"

echo "=== iOS simulator (x86_64, iphonesimulator) ==="
build_slice "sim-x86_64" "iphonesimulator" "x86_64"
link_dylib iphonesimulator x86_64 "-simulator" \
  "${DIST}/sim-x86_64/echosdk.a" "${DIST}/sim-x86_64/echosdk.dylib"

mkdir -p "${DIST}/simulator"
lipo -create \
  "${DIST}/sim-arm64/echosdk.dylib" \
  "${DIST}/sim-x86_64/echosdk.dylib" \
  -output "${DIST}/simulator/echosdk.dylib"
# Fat static archive for non-Flutter consumers, same layout as before.
lipo -create \
  "${DIST}/sim-arm64/echosdk.a" \
  "${DIST}/sim-x86_64/echosdk.a" \
  -output "${DIST}/simulator/echosdk.a"
make_framework "${DIST}/simulator/echosdk.dylib" \
  "${DIST}/simulator/EchoSDK.framework"

# ---------------------------------------------------------------------------
# Verify before packaging: exported API, the SIP-fix renames, and a real TLS
# stack (an OpenSSL-less build stubs tls_alloc and ships without WSS/DTLS-SRTP
# — exactly the regression this script's TLS choice exists to prevent).
# ---------------------------------------------------------------------------
for SLICE in device simulator; do
  BIN="${DIST}/${SLICE}/EchoSDK.framework/EchoSDK"

  # Capture each symbol table once. `nm ... | grep -q` is a footgun under
  # `set -o pipefail` (same one scripts/build-android.sh documents): grep exits
  # at the first match, nm dies of SIGPIPE — "error: write on a pipe with no
  # reader" — and pipefail then reports the whole pipeline as failed. A symbol
  # that IS present reads as missing, so the check fails precisely when it
  # passes. Grep the captured text instead.
  GLOBALS=$(xcrun nm -gU "${BIN}")
  DEFINED=$(xcrun nm -U "${BIN}")

  EXPORTS=$(grep -c ' _echosdk_' <<<"${GLOBALS}" || true)
  if [ "${EXPORTS}" -lt 40 ]; then
    echo "ERROR: ${SLICE}: only ${EXPORTS} echosdk_* symbols exported" >&2
    exit 1
  fi
  # The SIP fixes ride the patched libre sources (cmake/patch-re-sources.cmake):
  # libre's definitions are renamed to __real_* and ws_path.c owns the public
  # names. A missing rename means the patch step did not run.
  if ! grep -q '___real_websock_connect' <<<"${DEFINED}"; then
    echo "ERROR: ${SLICE}: websock rename missing (patch-re-sources.cmake not applied?)" >&2
    exit 1
  fi
  if ! grep -q '___real_sip_dialog_route' <<<"${DEFINED}"; then
    echo "ERROR: ${SLICE}: sip_dialog_route rename missing (patch-re-sources.cmake not applied?)" >&2
    exit 1
  fi
  if ! grep -q '_bsdk_ice_module_init' <<<"${DEFINED}"; then
    echo "ERROR: ${SLICE}: echo-ice missing (src/ice/*.c not compiled into echosdk_core?)" >&2
    exit 1
  fi
  if ! grep -q ' _SSL_CTX_new' <<<"${DEFINED}"; then
    echo "ERROR: ${SLICE}: OpenSSL missing from the binary — this build has no TLS/WSS/DTLS-SRTP" >&2
    exit 1
  fi
  # The external-audio entry points are the ONLY symbols the Flutter plugin
  # resolves at link time — EchoSDKExternalAudio.m calls them from the VPIO
  # render callback, while everything else reaches the SDK through dlsym
  # (lib/src/sdk.dart opens EchoSDK.framework/EchoSDK at runtime). A count of
  # exported echosdk_* symbols does not cover them, and their absence surfaces
  # only much later as "Undefined symbol: _echosdk_audio_external_push" when
  # the example app links. Name them explicitly.
  for SYM in _echosdk_audio_external_push \
             _echosdk_audio_external_pull \
             _echosdk_audio_external_format \
             _echosdk_audio_external_is_active; do
    if ! grep -q " ${SYM}$" <<<"${GLOBALS}"; then
      echo "ERROR: ${SLICE}: ${SYM} not exported — the Flutter plugin cannot link against this framework" >&2
      exit 1
    fi
  done
  # The plugin looks this one up with dlsym (so it still links against an
  # older framework), which means a missing export fails silently at run
  # time: the SDK's NS/AGC back on top of VoiceProcessingIO.
  if ! grep -q " _echosdk_audio_external_set_voice_path$" <<<"${GLOBALS}"; then
    echo "ERROR: ${SLICE}: _echosdk_audio_external_set_voice_path not exported — app-owned audio would stack the SDK's NS/AGC on VPIO" >&2
    exit 1
  fi
done

# ---------------------------------------------------------------------------
# Package as xcframework
# ---------------------------------------------------------------------------
echo "=== Creating xcframework ==="
rm -rf "${DIST}/EchoSDK.xcframework"
xcodebuild -create-xcframework \
  -framework "${DIST}/device/EchoSDK.framework" \
  -framework "${DIST}/simulator/EchoSDK.framework" \
  -output "${DIST}/EchoSDK.xcframework"

echo ""
echo "Done. Output: ${DIST}/EchoSDK.xcframework"
plutil -p "${DIST}/EchoSDK.xcframework/Info.plist" | grep -E "(Identifier|Library)" || true

# ---------------------------------------------------------------------------
# Stage into the Flutter plugin, where the podspec vendors it.
# ---------------------------------------------------------------------------
PLUGIN_FRAMEWORKS="${ROOT}/bindings/flutter/ios/Frameworks"
mkdir -p "${PLUGIN_FRAMEWORKS}"
rm -rf "${PLUGIN_FRAMEWORKS}/EchoSDK.xcframework"
cp -R "${DIST}/EchoSDK.xcframework" "${PLUGIN_FRAMEWORKS}/EchoSDK.xcframework"

echo ""
echo "Flutter plugin xcframework refreshed (commit it alongside the C change —"
echo "apps pin a git SHA, so an uncommitted rebuild never reaches consumers):"
du -sh "${PLUGIN_FRAMEWORKS}/EchoSDK.xcframework"
