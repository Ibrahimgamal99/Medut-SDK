#!/usr/bin/env bash
# Build Medut for Android (arm64-v8a, armeabi-v7a, x86_64).
# Output per ABI:
#   dist/android/<abi>/medut.a       merged static archive
#   dist/android/<abi>/libmedut.so   shared library (16 KB page aligned)
#   dist/android/<abi>/include/
# Also refreshes the Flutter plugin's bundled (stripped) copies in
# bindings/flutter/android/src/main/jniLibs/<abi>/ — no second command to run.
#
# Prerequisites:
#   - Android NDK (set ANDROID_NDK, or detected from ANDROID_NDK_ROOT /
#     ANDROID_NDK_LATEST_HOME, or the newest ndk/<ver> under the SDK)
#   - cmake ninja
#
# Env knobs:
#   ANDROID_ABIS         ABIs to build (default: arm64-v8a armeabi-v7a x86_64)
#   ANDROID_PLATFORM     API level (default: android-24; re needs >= 24)
#   MEDUT_BUILD_ROOT   Build directory root (default: <repo>/build).
#                        Point at a native filesystem (e.g. ~/.cache) when the
#                        repo lives on NTFS — incremental builds there silently
#                        reuse stale objects.
#   MEDUT_INCREMENTAL  Set to 1 to keep existing build dirs (default: clean).
#                        Only medut_core is BUILD_ALWAYS, so an incremental
#                        build picks up src/ and platform/ changes but NOT a
#                        change under third_party/ (a libre or baresip patch):
#                        those ExternalProjects are stamped and skipped, and the
#                        result silently ships the previous objects.  After
#                        touching cmake/patches/, build clean.  CI always does.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_TYPE="${BUILD_TYPE:-Release}"
ABIS="${ANDROID_ABIS:-arm64-v8a armeabi-v7a x86_64}"
API="${ANDROID_PLATFORM:-android-24}"  # re requires >= 24 for getifaddrs
BUILD_ROOT="${MEDUT_BUILD_ROOT:-${ROOT}/build}"

# Fetch third-party sources (idempotent — skips dirs that already exist).
# OpenSSL, not mbedTLS: libre only implements TLS/DTLS over OpenSSL, so an
# mbedTLS build silently loses SIP/TLS, SIP/WSS, DTLS-SRTP and AES (see the
# Phase 0.5 comment in CMakeLists.txt).
MEDUT_TLS=openssl MEDUT_OPENSSL_SRC=1 bash "${SCRIPT_DIR}/fetch-third-party.sh"

# Locate NDK. Explicit env wins; otherwise fall back to the SDK's ndk/<ver>
# directory (the layout `sdkmanager "ndk;<ver>"` and Android Studio produce),
# picking the highest version installed.
NDK="${ANDROID_NDK:-${ANDROID_NDK_ROOT:-${ANDROID_NDK_LATEST_HOME:-}}}"
if [ -z "${NDK}" ]; then
  for SDK in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" \
             "${HOME}/Android/Sdk" "${HOME}/Library/Android/sdk"; do
    [ -n "${SDK}" ] && [ -d "${SDK}/ndk" ] || continue
    NDK="$(find "${SDK}/ndk" -maxdepth 1 -mindepth 1 -type d \
             -name '[0-9]*' | sort -V | tail -n1)"
    [ -n "${NDK}" ] && break
  done
fi
if [ -z "${NDK}" ]; then
  echo "ERROR: Set ANDROID_NDK to the NDK root directory (or install one via" \
       "'sdkmanager \"ndk;<version>\"')." >&2
  exit 1
fi
TOOLCHAIN="${NDK}/build/cmake/android.toolchain.cmake"
if [ ! -f "${TOOLCHAIN}" ]; then
  echo "ERROR: NDK toolchain not found at ${TOOLCHAIN}" >&2
  exit 1
fi
NDK_HOST_TAG="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
LLVM_BIN="${NDK}/toolchains/llvm/prebuilt/${NDK_HOST_TAG}/bin"

# The Flutter plugin ships prebuilt .so files from src/main/jniLibs/<abi>/ —
# Gradle never compiles the native side. Staged below, per ABI.
JNILIBS="${ROOT}/bindings/flutter/android/src/main/jniLibs"

for ABI in ${ABIS}; do
  echo "=== Building Android ${ABI} ==="
  BUILD_DIR="${BUILD_ROOT}/android-${ABI}"
  if [ "${MEDUT_INCREMENTAL:-0}" != "1" ]; then
    rm -rf "${BUILD_DIR}"
  fi

  cmake -S "${ROOT}" -B "${BUILD_DIR}" -GNinja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DANDROID_ABI="${ABI}" \
    -DANDROID_PLATFORM="${API}" \
    -DANDROID_STL=c++_static \
    -DMEDUT_TLS=openssl \
    -DMEDUT_MODULES_PROFILE=mobile \
    -DMEDUT_SHARED=ON

  cmake --build "${BUILD_DIR}" --target medut medut_shared -j"$(nproc)"
  cmake --install "${BUILD_DIR}"

  DIST_DIR="${ROOT}/dist/android/${ABI}"
  SO="${DIST_DIR}/libmedut.so"

  # ── Verify the artifact before trusting it ────────────────────────────────
  # (grep -q + pipefail is a footgun: grep exits at first match and llvm-nm
  #  dies with SIGPIPE — capture the symbol table once instead.)
  echo "  === Verifying ${SO} ==="
  DEFINED=$("${LLVM_BIN}/llvm-nm" -D --defined-only "${SO}")
  EXPORTS=$(grep -c ' T medut_' <<<"${DEFINED}" || true)
  if [ "${EXPORTS}" -lt 40 ]; then
    echo "ERROR: only ${EXPORTS} medut_* symbols exported" >&2; exit 1
  fi
  # The SIP fixes ride the patched libre sources (cmake/patch-re-sources.cmake):
  # libre's definitions are renamed to __real_* and ws_path.c owns the public
  # names. A missing __real_* means the patch step did not run and the shipped
  # library would carry the unrouted-BYE bug again.
  if ! grep -q '__real_websock_connect' <<<"${DEFINED}"; then
    echo "ERROR: websock rename missing (patch-re-sources.cmake not applied?)" >&2; exit 1
  fi
  if ! grep -q '__real_sip_dialog_route' <<<"${DEFINED}"; then
    echo "ERROR: sip_dialog_route rename missing (patch-re-sources.cmake not applied?)" >&2; exit 1
  fi
  # Medut's own ICE agent (src/ice/) must be in the archive: without it no
  # account can enable ICE and every WebRTC endpoint fails to register.
  if ! grep -q 'bsdk_ice_module_init' <<<"${DEFINED}"; then
    echo "ERROR: medut-ice missing (src/ice/*.c not compiled into medut_core?)" >&2; exit 1
  fi
  UNDEF=$("${LLVM_BIN}/llvm-nm" -D --undefined-only "${SO}" \
          | grep -E ' (mbedtls_|opus_|SSL_|EVP_|X509_|__real_websock_connect|__real_sip_dialog_route|AAudio)' || true)
  if [ -n "${UNDEF}" ]; then
    echo "ERROR: unresolved symbols in ${SO}:" >&2
    echo "${UNDEF}" >&2; exit 1
  fi
  # Every *strong* undefined symbol must be exported by something in DT_NEEDED.
  # The whitelist above only catches names we thought to look for: a dangling
  # `crc32` (re's USE_ZLIB path, no -lz on the link line) sailed past it and
  # only failed at dlopen() on a device. -Wl,--no-undefined now catches this at
  # link time; this re-checks the shipped artifact. Weak undefs (nm prints 'w',
  # e.g. memfd_create above our API level) are legal and skipped.
  case "${ABI}" in
    arm64-v8a)   TRIPLE=aarch64-linux-android ;;
    armeabi-v7a) TRIPLE=arm-linux-androideabi ;;
    x86_64)      TRIPLE=x86_64-linux-android  ;;
    x86)         TRIPLE=i686-linux-android    ;;
    *) echo "ERROR: no triple mapping for ABI ${ABI}" >&2; exit 1 ;;
  esac
  STUBS="${NDK}/toolchains/llvm/prebuilt/${NDK_HOST_TAG}/sysroot/usr/lib/${TRIPLE}/${API#android-}"
  NEEDED=$("${LLVM_BIN}/llvm-readelf" -d "${SO}" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
  PROVIDED=$(for lib in ${NEEDED}; do
               [ -f "${STUBS}/${lib}" ] &&
                 "${LLVM_BIN}/llvm-nm" -D --defined-only "${STUBS}/${lib}" |
                 awk '{print $NF}'
             done | sed 's/@.*//' | sort -u)
  WANTED=$("${LLVM_BIN}/llvm-nm" -D --undefined-only "${SO}" |
           awk '$1 == "U" {print $2}' | sed 's/@.*//' | sort -u)
  MISSING=$(comm -23 <(printf '%s\n' "${WANTED}") <(printf '%s\n' "${PROVIDED}"))
  if [ -n "${MISSING}" ]; then
    echo "ERROR: ${SO} references symbols no DT_NEEDED library provides" >&2
    echo "       (dlopen would fail on device). NEEDED: ${NEEDED//$'\n'/ }" >&2
    printf '         %s\n' ${MISSING} >&2; exit 1
  fi
  BAD_ALIGN=$("${LLVM_BIN}/llvm-readelf" -l "${SO}" | awk '/LOAD/ {print $NF}' \
              | grep -v '0x4000' || true)
  if [ -n "${BAD_ALIGN}" ]; then
    echo "ERROR: LOAD segment not 16 KB aligned: ${BAD_ALIGN}" >&2; exit 1
  fi

  # ── Stage into the Flutter plugin ─────────────────────────────────────────
  # Stripped of debug info: these copies are tracked in git and shipped inside
  # the pub package, while the dist/ ones keep full symbols for debugging.
  mkdir -p "${JNILIBS}/${ABI}"
  "${LLVM_BIN}/llvm-strip" --strip-unneeded -o "${JNILIBS}/${ABI}/libmedut.so" "${SO}"

  echo "  -> dist/android/${ABI}/medut.a"
  echo "  -> dist/android/${ABI}/libmedut.so"
  echo "  -> bindings/flutter/android/src/main/jniLibs/${ABI}/libmedut.so (stripped)"
done

echo ""
echo "Done. Outputs:"
find "${ROOT}/dist/android" \( -name "medut.a" -o -name "libmedut.so" \) -exec ls -lh {} \;

echo ""
echo "Flutter plugin jniLibs refreshed (commit these alongside the C change —"
echo "apps pin a git SHA, so an uncommitted rebuild never reaches consumers):"
find "${JNILIBS}" -name '*.so' -exec ls -lh {} \;
