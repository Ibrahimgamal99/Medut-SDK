#!/usr/bin/env bash
# Build Medut for Linux x86_64.
# Output: dist/linux/x86_64/medut.a  +  dist/linux/x86_64/include/
#
# Prerequisites: cmake ninja gcc openssl-devel (or libssl-dev)
#                webrtc-audio-processing-devel (build-time only; optional at runtime)
#                libpulse-dev / pulseaudio-libs-devel — optional: if the -dev
#                symlink is absent the link falls back to libpulse.so.0 directly.
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
BUILD_DIR="${ROOT}/build/linux-x86_64"

bash "${SCRIPT_DIR}/fetch-third-party.sh"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -GNinja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DMEDUT_TLS=openssl \
  -DMEDUT_MODULES_PROFILE=desktop \
  -DMEDUT_WITH_WEBRTC_AEC="${MEDUT_WITH_WEBRTC_AEC:-ON}" \
  -DHAVE_THREADS=OFF \
  -DCMAKE_C_FLAGS="-std=gnu11 -D_GNU_SOURCE -D_DEFAULT_SOURCE -Wno-error=deprecated-declarations"

# gen_hpp re-embeds include/medut.h into bindings/cpp/medut.hpp; without
# it the C++ binding falls behind the header it ships with.
cmake --build "${BUILD_DIR}" --target medut gen_hpp -j"$(nproc)"
cmake --install "${BUILD_DIR}"

# ── Link shared library ───────────────────────────────────────────────────────
DIST_DIR="${ROOT}/dist/linux/x86_64"
SO="${DIST_DIR}/medut.so"
echo ""
echo "=== Linking ${SO} ==="
# glibc_symver.c is already compiled into medut.a via the SRC_MODE build.
# No --wrap flags: the SIP fixes live in the patched libre sources
# (cmake/patch-re-sources.cmake), so the archive is self-contained.
gcc -shared \
  -Wl,--whole-archive "${DIST_DIR}/medut.a" -Wl,--no-whole-archive \
  -lssl -lcrypto -lz -lpthread -lm -lresolv -ldl \
  "${PULSE_LDLIB}" \
  -Wl,--default-symver \
  -Wl,--version-script="${SCRIPT_DIR}/glibc-compat.ver" \
  -Wl,-z,defs \
  -o "${SO}"

echo ""
echo "Done. Output:"
ls -lh "${DIST_DIR}/medut.a" "${SO}"
