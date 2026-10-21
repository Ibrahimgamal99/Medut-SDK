#!/usr/bin/env bash
# Regenerate bindings/python/medut/_medut_clean.h from include/medut.h.
#
# This lives in its own script so that every caller shares one pipeline and
# they cannot drift. The callers are bindings/python/build.sh and
# .github/workflows/pypi.yml; an earlier CI job inlined a near-copy that was
# missing MEDUT_NO_PACKED_ENUM — see below for what that costs. Keep new
# callers invoking this script rather than inlining the preprocessor command.
#
# The output must not depend on the host's gcc either: pypi.yml checks that
# what it regenerates matches the committed header byte for byte.
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
# that preserves the layout.
#
# -std=gnu2x pins the language rather than taking gcc's default, which is
# gnu17 before gcc 15 and gnu23 from it.  In C23 bool is a keyword; before it
# <stdbool.h> makes bool a macro, and the expansion both respells it _Bool
# and breaks the line around it, so the two defaults give different headers.
# gcc 13 is the oldest that treats bool as a keyword under this flag.
gcc -std=gnu2x -E \
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
    | sed 's/\b_Bool\b/bool/g' \
    | sed '/^[[:space:]]*$/N;/^\n$/d' \
    > "${OUTPUT}"

# _Bool becomes bool, never int: a gcc that ignored -std=gnu2x's keyword
# would still hand over _Bool, and both are 1 byte to cffi.  Mapping it to
# int, as this line once did, widened every bool to 4 bytes under gcc 13's
# default gnu17: medut 1.0.0's wheels were generated that way and put
# medut_config_t at 816 bytes against the library's 776, with event_cb 0x20
# past where medut_init() reads it, so init always failed with -1.

# No size_t/wchar_t prologue: cffi resolves both itself in cdef(), the awk
# filter above already drops the system-header typedefs, and the committed
# header has never carried one.  The old CI copy used to prepend them — a
# second, quieter way the two pipelines had diverged.

echo "==> Wrote ${OUTPUT}"
