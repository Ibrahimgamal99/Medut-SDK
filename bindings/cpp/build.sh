#!/usr/bin/env bash
# Build the C++ examples for Medut.
# Builds the SDK first if it hasn't been built yet.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DIST_DIR="${ROOT}/dist/linux/x86_64"
BUILD_DIR="${SCRIPT_DIR}/build"

# ── 1. Build the SDK if needed ────────────────────────────────────────────────
if [[ ! -f "${DIST_DIR}/medut.so" ]]; then
  echo "==> SDK not found — building it first..."
  bash "${ROOT}/scripts/build-linux.sh"
fi

# ── 2. Configure + build examples ─────────────────────────────────────────────
echo "==> Configuring C++ examples..."
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMEDUT_DIST_DIR="${DIST_DIR}"

echo "==> Building..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo ""
echo "Done. Binaries in: ${BUILD_DIR}"
echo "Run an example:"
echo "  ${BUILD_DIR}/quickstart alice@pbx.example.com secret"
echo "  ${BUILD_DIR}/quickstart alice@pbx.example.com secret bob@pbx.example.com"
