#!/usr/bin/env bash
# Build the portable Linux core the Flutter plugin ships, inside the
# manylinux2014 image (CentOS 7, glibc 2.17) for the host's architecture:
#
#   docker run --rm -v "$PWD:/src" -w /src \
#     quay.io/pypa/manylinux2014_$(uname -m):2026.10.16-1 \
#     bash scripts/build-linux-portable.sh
#
# .github/workflows/desktop-libs.yml runs exactly that on x64 and arm64
# runners. The result, bindings/flutter/linux/<x64|arm64>/medut.so, needs
# glibc 2.17+, libpulse.so.0 and libz.so.1 and nothing else: OpenSSL is
# linked in (MEDUT_PORTABLE=1 scripts/build-linux.sh).
#
# The image has no webrtc-audio-processing 1.x, which the WebRTC AEC needs at
# build time (it is dlopen'ed at run time, so the distro still supplies it),
# so build 1.3 into .cache/ first; CI caches that directory.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MACHINE="$(uname -m)"

if [ ! -f /etc/centos-release ] || [ ! -d /opt/python ]; then
  echo "ERROR: run this inside quay.io/pypa/manylinux2014_${MACHINE} (see the header)" >&2
  exit 1
fi

# The checkout belongs to the host user; git refuses to read it otherwise.
git config --global --add safe.directory '*'

# ── Tools and libraries ──────────────────────────────────────────────────────
# perl-core: OpenSSL's Configure needs IPC::Cmd and friends.
yum install -y -q pulseaudio-libs-devel zlib-devel perl-core
PY=/opt/python/cp312-cp312/bin
"${PY}/pip" install -q --root-user-action=ignore cmake ninja meson
export PATH="${PY}:${PATH}"

# ── webrtc-audio-processing 1.3 (build-time only) ────────────────────────────
WAP_VERSION=1.3
WAP_SHA256=2365e93e778d7b61b5d6e02d21c47d97222e9c7deff9e1d0838ad6ec2e86f1b9
WAP_PREFIX="${ROOT}/.cache/webrtc-audio-processing-${WAP_VERSION}-${MACHINE}"
if [ ! -f "${WAP_PREFIX}/lib/pkgconfig/webrtc-audio-processing-1.pc" ]; then
  WORK="$(mktemp -d)"
  curl -sfL -o "${WORK}/wap.tar.xz" \
    "https://freedesktop.org/software/pulseaudio/webrtc-audio-processing/webrtc-audio-processing-${WAP_VERSION}.tar.xz"
  echo "${WAP_SHA256}  ${WORK}/wap.tar.xz" | sha256sum -c -
  tar -xf "${WORK}/wap.tar.xz" -C "${WORK}"
  # abseil comes from the bundled wrap (the image has none), which installs
  # its headers next to the library's.
  meson setup "${WORK}/build" "${WORK}/webrtc-audio-processing-${WAP_VERSION}" \
    --prefix="${WAP_PREFIX}" --libdir=lib --buildtype=release \
    --force-fallback-for=abseil-cpp
  meson install -C "${WORK}/build"
  rm -rf "${WORK}"
fi
export PKG_CONFIG_PATH="${WAP_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
# The .pc names only include/webrtc-audio-processing-1; the absl/ headers its
# API uses sit one level up.
export CPLUS_INCLUDE_PATH="${WAP_PREFIX}/include"

# ── Build ────────────────────────────────────────────────────────────────────
MEDUT_PORTABLE=1 bash "${SCRIPT_DIR}/build-linux.sh"

case "${MACHINE}" in
  x86_64)  FLUTTER_ARCH=x64 ;;
  aarch64) FLUTTER_ARCH=arm64 ;;
esac
SO="${ROOT}/bindings/flutter/linux/${FLUTTER_ARCH}/medut.so"

# ── Check it on glibc 2.17 ───────────────────────────────────────────────────
# This image is the oldest system the library claims to support: load it
# here, the way an app does, and call into it.
GLIBC_MAX=$(objdump -T "${SO}" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
if [ "$(printf '%s\nGLIBC_2.17\n' "${GLIBC_MAX}" | sort -V | tail -1)" != "GLIBC_2.17" ]; then
  echo "ERROR: ${SO} needs ${GLIBC_MAX}, newer than glibc 2.17" >&2
  exit 1
fi
PROBE="$(mktemp -d)"
cat > "${PROBE}/probe.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
	void *h = dlopen(argv[1], RTLD_NOW);
	if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	const char *(*ver)(void) = (const char *(*)(void))dlsym(h, "medut_version");
	if (!ver) { fprintf(stderr, "no medut_version\n"); return 1; }
	printf("medut_version() = %s\n", ver());
	return 0;
}
EOF
gcc -o "${PROBE}/probe" "${PROBE}/probe.c" -ldl
"${PROBE}/probe" "${SO}"
rm -rf "${PROBE}"
echo "${SO}: loads on glibc $(ldd --version | head -1 | grep -o '[0-9.]*$'), needs ${GLIBC_MAX}"

# Hand the outputs back to the checkout's owner (the container runs as root).
OWNER="$(stat -c '%u:%g' "${ROOT}")"
chown -R "${OWNER}" "${ROOT}/bindings/flutter/linux" "${ROOT}/dist" "${ROOT}/build" \
  "${ROOT}/.cache" "${ROOT}/third_party" 2>/dev/null || true
