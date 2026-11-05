#!/usr/bin/env bash
# Build and run the offline integration tests against one Linux build.
#
#   test/run-integration.sh [dist-dir]
#
# dist-dir defaults to dist/linux/x86_64 (what scripts/build-linux.sh
# installs).  The tests are written against the public API and, for
# compat_contract_test, the plug-in API (src/core/mdt_mod.h).
#
# Environment:
#   SAN=0       build without sanitizers (needs no libasan/libtsan runtime)
#   ONLY=name   run one test
#   V=1         let the tests print the SDK log
#
# Exit status: 0 when every test built and passed.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

DIST="${1:-dist/linux/x86_64}"
OUT="${ROOT}/build/itest"
mkdir -p "${OUT}"

if [[ ! -f "${DIST}/medut.a" || ! -f "${DIST}/medut.so" ]]; then
	echo "no build in ${DIST} — run scripts/build-linux.sh first" >&2
	exit 2
fi

PULSE="-lpulse"
gcc -x c - -lpulse -o /dev/null >/dev/null 2>&1 <<<'int main(void){return 0;}' \
	|| PULSE="-l:libpulse.so.0"
LIBS="-lssl -lcrypto -lz -lpthread -lm -lresolv -ldl -lstdc++ ${PULSE}"
INC="-Iinclude -Isrc -I${DIST}/include -I${DIST}/include/re"

pass=0; fail=0; failed=()

# name  sanitizer  link(a = static archive, white-box | so = shared, public API)
run() {
	local name="$1" san="$2" mode="$3" bin="${OUT}/$1" log="${OUT}/$1.log"
	[[ -n "${ONLY:-}" && "${ONLY}" != "${name}" ]] && return
	[[ "${SAN:-1}" == 0 ]] && san=""

	if [[ "${mode}" == a ]]; then
		gcc -g -O1 -std=gnu11 ${san} ${INC} "test/${name}.c" \
			-Wl,--whole-archive "${DIST}/medut.a" -Wl,--no-whole-archive \
			${LIBS} -o "${bin}" 2>"${log}"
	else
		gcc -g -O1 -std=gnu11 ${san} -Iinclude "test/${name}.c" \
			"${ROOT}/${DIST}/medut.so" -Wl,-rpath,"${ROOT}/${DIST}" \
			-o "${bin}" 2>"${log}"
	fi
	if [[ $? -ne 0 ]]; then
		echo "BUILD FAIL  ${name}  (${log})"
		fail=$((fail + 1)); failed+=("${name}")
		return
	fi

	if timeout 180 "${bin}" >"${log}" 2>&1; then
		echo "PASS        ${name}"
		pass=$((pass + 1))
	else
		echo "FAIL        ${name}  (exit $?, ${log})"
		fail=$((fail + 1)); failed+=("${name}")
	fi
}

run compat_contract_test        ""                    a
run audio_external_test         "-fsanitize=address"  a
run owned_events_test           "-fsanitize=address"  a
run reattach_test               "-fsanitize=address"  a
run reinit_test                 "-fsanitize=address"  a
run thread_safety_test          "-fsanitize=thread"   a
run event_queue_accounting_test ""                    so
run push_contact_test           ""                    so
run reconnect_state_test        ""                    so
run reattach_shutdown_test      ""                    so

echo "== ${pass} passed, ${fail} failed${failed:+: ${failed[*]}}"
[[ ${fail} -eq 0 ]]
