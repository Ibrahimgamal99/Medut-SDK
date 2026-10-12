#!/usr/bin/env bash
# Clone the third_party sources the build needs, at the revisions below.
# third_party/ is gitignored, so this is how a fresh checkout gets them.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# name|url|revision|marker
# The marker is the file whose presence means "already cloned and usable".
# It is CMakeLists.txt for the CMake projects; OpenSSL has no CMake build, so
# its marker is the Configure script the ExternalProject actually invokes.
DEPS=(
  "re|https://github.com/baresip/re.git|fd6c71be2a981f2e961c59c7f1b39b5cbd30494e|CMakeLists.txt"
  "baresip|https://github.com/baresip/baresip.git|d5ed26ccff4c884f27061cdf022f18332cda4c9c|CMakeLists.txt"
  "opus|https://github.com/xiph/opus.git|503d81b138d76621aae4b12786e90de48aa8db3a|CMakeLists.txt"
)
if [[ "${MEDUT_TLS:-openssl}" == "mbedtls" ]]; then
  DEPS+=("mbedtls|https://github.com/Mbed-TLS/mbedtls.git|2f2b202f8e72ef01aa0b743ef9df2abb0a3527d9|CMakeLists.txt")
fi
# OpenSSL sources are only needed where there is no system OpenSSL to link
# against — i.e. cross-compiled targets like Android. Desktop builds keep
# using the platform's own libssl/libcrypto, so they skip this clone.
# (openssl-3.5.7, an LTS release.)
if [[ "${MEDUT_OPENSSL_SRC:-0}" == "1" ]]; then
  DEPS+=("openssl|https://github.com/openssl/openssl.git|8cf17aaeb4599f8af87fefd810b5b5fee90fe69e|Configure")
fi

for dep in "${DEPS[@]}"; do
  IFS='|' read -r name url rev marker <<<"${dep}"
  dir="${ROOT}/third_party/${name}"

  # A tree that is already here must also be at the pinned revision: a pin
  # bump used to leave every existing checkout on the old one, building
  # sources nobody had reviewed.  Moving it uses checkout -f, which drops the
  # Medut patches the build applied (it re-applies them: the patch steps
  # depend on .git/HEAD) — and also any hand edits under third_party/.
  if [[ -f "${dir}/${marker}" ]]; then
    have=$(git -C "${dir}" rev-parse HEAD 2>/dev/null || true)
    if [[ "${have}" == "${rev}" ]]; then
      echo "==> ${name}: present"
      continue
    fi
    echo "==> ${name}: ${have:0:12} -> ${rev:0:12}"
    git -C "${dir}" fetch -q --depth 1 origin "${rev}"
    git -C "${dir}" checkout -q -f FETCH_HEAD
    continue
  fi

  echo "==> ${name}: cloning ${rev:0:12} from ${url}"
  git init -q "${dir}"
  git -C "${dir}" remote add origin "${url}" 2>/dev/null || true
  git -C "${dir}" fetch -q --depth 1 origin "${rev}"
  git -C "${dir}" checkout -q FETCH_HEAD
done

# mbedTLS needs its `framework` submodule (scripts/config.py imports
# mbedtls_framework from it). Fetch it shallowly at the pinned gitlink rev.
MBEDTLS_DIR="${ROOT}/third_party/mbedtls"
if [[ -f "${MBEDTLS_DIR}/.gitmodules" && ! -f "${MBEDTLS_DIR}/framework/CMakeLists.txt" ]]; then
  fw_rev="$(git -C "${MBEDTLS_DIR}" ls-tree HEAD framework | awk '{print $3}')"
  if [[ -n "${fw_rev}" ]]; then
    echo "==> mbedtls/framework: cloning ${fw_rev:0:12}"
    git init -q "${MBEDTLS_DIR}/framework"
    git -C "${MBEDTLS_DIR}/framework" remote add origin \
      "https://github.com/Mbed-TLS/mbedtls-framework.git" 2>/dev/null || true
    git -C "${MBEDTLS_DIR}/framework" fetch -q --depth 1 origin "${fw_rev}"
    git -C "${MBEDTLS_DIR}/framework" checkout -q FETCH_HEAD
  fi
fi
