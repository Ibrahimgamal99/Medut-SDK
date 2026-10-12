#!/usr/bin/env bash
# Regenerate bindings/python/medut/_medut_clean.h from include/medut.h.
#
# This lives in its own script so that every caller shares one pipeline and
# they cannot drift. Today the only caller is bindings/python/build.sh; a
# wheel-publishing CI job used to be the second, and its near-copy was missing
# MEDUT_NO_PACKED_ENUM — see below for what that costs. Keep new callers
# invoking this script rather than inlining the preprocessor command.
#
# Usage: gen_header.sh [<input medut.h> [<output _medut_clean.h>]]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

INPUT="${1:-${ROOT}/include/medut.h}"
OUTPUT="${2:-${SCRIPT_DIR}/medut/_medut_clean.h}"

# MEDUT_NO_PACKED_ENUM is required, not cosmetic.  medut_aec_mode_t is a
# packed (1-byte) enum in the real ABI, and -D'__attribute__(x)=' below strips
# the packed attribute — so cffi widens the field to 4 bytes and every struct
# member after cfg.aec_mode lands at the wrong offset.  The total size can
# still match by padding coincidence, which is why the struct_size check in
# medut_init() does not catch it.  The define selects the uint8_t typedef
# that preserves the layout, the same way bindings/flutter/ffigen.yaml does.
gcc -E \
    -DMEDUT_NO_PACKED_ENUM=1 \
    -D'__extension__=' \
    -D'__attribute__(x)=' \
    -D'__restrict=' \
    -D'__inline__=inline' \
    -D'inline=' \
    "${INPUT}" \
    | awk -v src="${INPUT}" '
        /^# [0-9]+ / {
            match($0, /"([^"]+)"/, a)
            in_sdk = (a[1] == src)
            next
        }
        in_sdk { print }
    ' \
    | grep -v '__typeof__' \
    | grep -v 'nullptr_t' \
    | grep -v '__max_align' \
    | grep -v 'max_align_t' \
    | sed 's/_Bool/int/g' \
    | sed '/^[[:space:]]*$/N;/^\n$/d' \
    > "${OUTPUT}"

# No size_t/wchar_t prologue: cffi resolves both itself in cdef(), the awk
# filter above already drops the system-header typedefs, and the committed
# header has never carried one.  The old CI copy used to prepend them — a
# second, quieter way the two pipelines had diverged.

echo "==> Wrote ${OUTPUT}"
