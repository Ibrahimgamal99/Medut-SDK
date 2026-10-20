#!/usr/bin/env bash
# Build Medut for Linux on the host's architecture (x86_64 or aarch64).
# Output: dist/linux/<uname -m>/medut.a + medut.so + include/, and with
#         MEDUT_PORTABLE=1 a stripped copy of medut.so in
#         bindings/flutter/linux/<x64|arm64>/ for the Flutter plugin.
#
# Prerequisites: cmake ninja gcc openssl-devel (or libssl-dev)
#                webrtc-audio-processing-devel (build-time only; optional at runtime)
#                libpulse-dev / pulseaudio-libs-devel — optional: if the -dev
#                symlink is absent the link falls back to libpulse.so.0 directly.
#
# MEDUT_PORTABLE=1 builds the library the plugin ships: OpenSSL is built from
# third_party/openssl and linked in, so medut.so needs no system OpenSSL, and
# run in the manylinux2014 image (scripts/build-linux-portable.sh, used by
# .github/workflows/desktop-libs.yml) it needs only glibc 2.17. Without it the
# build links the system OpenSSL 3 and suits local development and tests.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# PulseAudio link flag. Prefer the -dev-provided linker name; if that package
# is not installed but the runtime shared object is, link it by soname so the
# build works without pulseaudio-libs-devel / libpulse-dev.
PULSE_LDLIB="-lpulse"
if ! echo 'int main(void){return 0;}' | \
     gcc -x c - -lpulse -o /dev/null >/dev/null 2>&1; then
  if echo 'int main(void){return 0;}' | \
       gcc -x c - -l:libpulse.so.0 -o /dev/null >/dev/null 2>&1; then
    PULSE_LDLIB="-l:libpulse.so.0"
    echo "note: libpulse -dev not found; linking -l:libpulse.so.0" >&2
  else
    echo "ERROR: libpulse not found. Install pulseaudio-libs-devel /" \
         "libpulse-dev (or the pulseaudio-libs / libpulse0 runtime)." >&2
    exit 1
  fi
fi

BUILD_TYPE="${BUILD_TYPE:-Release}"
MACHINE="$(uname -m)"
case "${MACHINE}" in
  x86_64)  FLUTTER_ARCH=x64 ;;
  aarch64) FLUTTER_ARCH=arm64 ;;
  *) echo "ERROR: unsupported architecture ${MACHINE} (x86_64 or aarch64)" >&2; exit 1 ;;
esac
PORTABLE="${MEDUT_PORTABLE:-0}"
if [ "${PORTABLE}" = "1" ]; then
  # Its own build dir, so switching modes never reuses the other's cache.
  BUILD_DIR="${ROOT}/build/linux-${MACHINE}-portable"
  export MEDUT_OPENSSL_SRC=1   # fetch-third-party.sh then clones OpenSSL too
  OPENSSL_CMAKE=(-DMEDUT_BUILD_OPENSSL=ON)
else
  BUILD_DIR="${ROOT}/build/linux-${MACHINE}"
  OPENSSL_CMAKE=(-DMEDUT_BUILD_OPENSSL=OFF)
fi

bash "${SCRIPT_DIR}/fetch-third-party.sh"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -GNinja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DMEDUT_TLS=openssl \
  -DMEDUT_MODULES_PROFILE=desktop \
  -DMEDUT_WITH_WEBRTC_AEC="${MEDUT_WITH_WEBRTC_AEC:-ON}" \
  -DHAVE_THREADS=OFF \
  "${OPENSSL_CMAKE[@]}" \
  -DCMAKE_C_FLAGS="-std=gnu11 -D_GNU_SOURCE -D_DEFAULT_SOURCE -Wno-error=deprecated-declarations"

# gen_hpp re-embeds include/medut.h into bindings/cpp/medut.hpp; without
# it the C++ binding falls behind the header it ships with.
cmake --build "${BUILD_DIR}" --target medut gen_hpp -j"$(nproc)"
cmake --install "${BUILD_DIR}"

# ── Link shared library ───────────────────────────────────────────────────────
DIST_DIR="${ROOT}/dist/linux/${MACHINE}"
SO="${DIST_DIR}/medut.so"
echo ""
echo "=== Linking ${SO} ==="
# glibc_symver.c is already compiled into medut.a via the SRC_MODE build.
# No --wrap flags: the SIP fixes live in the patched libre sources
# (cmake/patch-re-sources.cmake), so the archive is self-contained.
#
# --whole-archive keeps the baresip modules, which register themselves from
# constructors. A portable build's medut.a also bundles OpenSSL, which has no
# such objects: link the components instead and let the linker take only the
# OpenSSL it needs, as the CMake MEDUT_SHARED link does.
if [ "${PORTABLE}" = "1" ]; then
  SYSROOT="${BUILD_DIR}/sysroot/lib"
  WHOLE=("${BUILD_DIR}/medut-core-build/medut_core.a" "${SYSROOT}/libre.a"
         "${SYSROOT}/libbaresip.a" "${SYSROOT}/libopus.a")
  # --exclude-libs: the built-in OpenSSL stays private. Exported, its
  # SSL_*/EVP_* names could bind to another OpenSSL already in the process
  # (GTK, libcurl, another plugin) and medut would call into that one.
  SSL_LIBS=("${SYSROOT}/libssl.a" "${SYSROOT}/libcrypto.a"
            -Wl,--exclude-libs,libssl.a:libcrypto.a)
else
  WHOLE=("${DIST_DIR}/medut.a")
  SSL_LIBS=(-lssl -lcrypto)
fi
gcc -shared \
  -Wl,--whole-archive "${WHOLE[@]}" -Wl,--no-whole-archive \
  "${SSL_LIBS[@]}" -lz -lpthread -lm -lresolv -ldl \
  "${PULSE_LDLIB}" \
  -Wl,--default-symver \
  -Wl,--version-script="${SCRIPT_DIR}/glibc-compat.ver" \
  -Wl,-z,defs \
  -o "${SO}"

# ── Check, and stage for the Flutter plugin ─────────────────────────────────
# Stripped: dlsym only needs the dynamic symbol table. Only a portable build
# replaces the plugin's committed copy (which linux/CMakeLists.txt bundles
# into apps); a local one is checked in dist/ and left there.
STRIPPED="${DIST_DIR}/medut.stripped.so"
strip --strip-unneeded -o "${STRIPPED}" "${SO}"
# Captured once: `nm | grep -q` under pipefail fails when grep exits early
# and nm dies of SIGPIPE.
SYMS=$(nm -D --defined-only "${STRIPPED}")
N=$(grep -c ' T medut_' <<<"${SYMS}" || true)
if [ "${N}" -lt 40 ] || \
   ! grep -Eq ' T medut_account_refresh_registration(@|$)' <<<"${SYMS}"; then
  echo "ERROR: ${SO}: ${N} medut_* exports, or no medut_account_refresh_registration" >&2
  exit 1
fi
if [ "${PORTABLE}" = "1" ]; then
  if readelf -d "${STRIPPED}" | grep -Eq 'NEEDED.*lib(ssl|crypto)\.so'; then
    echo "ERROR: portable ${SO} still links the system OpenSSL" >&2
    exit 1
  fi
  if grep -Eq ' T (SSL_|EVP_|OPENSSL_)' <<<"${SYMS}"; then
    echo "ERROR: portable ${SO} exports its built-in OpenSSL" >&2
    exit 1
  fi
fi
GLIBC_MAX=$(objdump -T "${STRIPPED}" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)

echo ""
echo "Done. Output:"
ls -lh "${DIST_DIR}/medut.a" "${SO}"
echo "${N} medut_* exports, needs ${GLIBC_MAX} or later."
if [ "${PORTABLE}" = "1" ]; then
  PLUGIN_SO="${ROOT}/bindings/flutter/linux/${FLUTTER_ARCH}/medut.so"
  mkdir -p "$(dirname "${PLUGIN_SO}")"
  mv "${STRIPPED}" "${PLUGIN_SO}"
  echo "Staged ${PLUGIN_SO}."
else
  rm -f "${STRIPPED}"
  echo "Local build (system OpenSSL): the plugin's linux/${FLUTTER_ARCH}/medut.so is"
  echo "left as committed. Test against this one with MEDUT_LIB=${SO}."
fi
