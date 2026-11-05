#!/usr/bin/env bash
# A/B comparison of two SDK builds over identical calls and network conditions.
#
#   tools/ab/run-ab.sh --a <dist-dir> --b <dist-dir>
#                      [--runs N] [--secs S]
#                      [--profiles clean,loss3,jitter30,burst,rtt200]
#                      [--scenarios rtc-wss,plain-udp,fs-rtc-wss,fs-plain-udp,...]
#
# A and B are installed builds (what scripts/build-linux.sh puts in
# dist/linux/x86_64): main against a branch, or any two builds.  For every profile the PBX's traffic is shaped with tc
# netem in its own network namespace (test/interop/docker-compose.yml, so no
# root on the host), and the builds take turns call by call in counterbalanced
# order — A B, B A, A B, … — so drift in the network or the PBX, and any
# advantage of going first or second, lands on both alike.  (Plain A B A B
# once made a ~1 s DTLS retransmission look like a difference between builds:
# it followed the first slot, not the build.)
#
# Each call is one tools/ab/ab_call run: an echo call carrying a 440 Hz tone,
# reporting setup time, time to audio, the SDK's own receive-side MOS / loss /
# jitter / jitter-buffer delay / concealment, and whether the tone came back.
# Raw results go to build/ab/<stamp>.jsonl; a summary table to stdout.
#
# Needs: docker compose -f test/interop/docker-compose.yml up -d (and
# test/interop/gen-certs.sh before the first start).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

A="" B="" RUNS=3 SECS=15
PROFILES="clean,loss3,jitter30,burst,rtt200"
SCENARIOS="rtc-wss,plain-udp"
while [[ $# -gt 0 ]]; do
	case "$1" in
	--a)         A="$2"; shift 2 ;;
	--b)         B="$2"; shift 2 ;;
	--runs)      RUNS="$2"; shift 2 ;;
	--secs)      SECS="$2"; shift 2 ;;
	--profiles)  PROFILES="$2"; shift 2 ;;
	--scenarios) SCENARIOS="$2"; shift 2 ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
done
[[ -z "${A}" || -z "${B}" ]] && { sed -n '2,8p' "$0"; exit 2; }

COMPOSE=(docker compose -f "${ROOT}/test/interop/docker-compose.yml")
PBX=172.31.250.10     # Asterisk
FS=172.31.250.20      # FreeSWITCH
OUT="${ROOT}/build/ab"
mkdir -p "${OUT}"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULTS="${OUT}/${STAMP}.jsonl"

# ── builds ───────────────────────────────────────────────────────────────────
build_harness() {   # label dist-dir
	local d
	d="$(cd "$2" && pwd)"
	[[ -f "${d}/medut.so" ]] || { echo "no medut.so in $2" >&2; exit 2; }
	gcc -O1 -std=gnu11 -Iinclude tools/ab/ab_call.c "${d}/medut.so" \
		-Wl,-rpath,"${d}" -lm -lpthread -o "${OUT}/ab_call_$1" || exit 2
}
build_harness A "${A}"
build_harness B "${B}"

# ── network profiles (tc netem on the PBX's egress) ──────────────────────────
netem_args() {
	case "$1" in
	clean)    echo "" ;;
	loss3)    echo "loss 3%" ;;
	loss10)   echo "loss 10%" ;;
	jitter30) echo "delay 40ms 30ms distribution normal" ;;
	burst)    echo "loss 4% 60%" ;;               # correlated: bursts
	rtt200)   echo "delay 200ms" ;;
	*) echo "unknown profile $1" >&2; exit 2 ;;
	esac
}
apply_profile() {   # on every PBX, so a scenario always sees the profile
	local args side
	args="$(netem_args "$1")"
	for side in asterisk-netem freeswitch-netem; do
		"${COMPOSE[@]}" exec -T "${side}" tc qdisc del dev eth0 root \
			>/dev/null 2>&1 || true
		if [[ -n "${args}" ]]; then
			# shellcheck disable=SC2086
			"${COMPOSE[@]}" exec -T "${side}" \
				tc qdisc add dev eth0 root netem ${args} || exit 1
		fi
	done
}
trap 'apply_profile clean' EXIT

# ── scenarios ────────────────────────────────────────────────────────────────
# Asterisk accounts: test/interop/asterisk/pjsip.conf, echo on 600.
# FreeSWITCH users 1000-1019, echo on 9196.
scenario_args() {
	case "$1" in
	rtc-wss)      echo "--server wss://${PBX}:8089/ws --user sip:rtc1@${PBX} --menc dtls --ice --dial 600" ;;
	rtc-ws)       echo "--server ws://${PBX}:8088/ws --user sip:rtc1@${PBX} --menc dtls --ice --dial 600" ;;
	plain-udp)    echo "--server sip:${PBX}:5060;transport=udp --user sip:plain1@${PBX} --menc none --dial 600" ;;
	plain-tcp)    echo "--server sip:${PBX}:5060;transport=tcp --user sip:plain1@${PBX} --menc none --dial 600" ;;
	sdes-tls)     echo "--server sips:${PBX}:5061 --user sip:sdes1@${PBX} --menc sdes --dial 600" ;;
	ice-tcp)      echo "--server sip:${PBX}:5060;transport=tcp --user sip:ice1@${PBX} --menc none --ice --dial 600" ;;
	fs-rtc-wss)   echo "--server wss://${FS}:7443 --user sip:1001@${FS} --menc dtls --ice --dial 9196" ;;
	fs-plain-udp) echo "--server sip:${FS}:5060;transport=udp --user sip:1000@${FS} --menc none --dial 9196" ;;
	fs-ice-tcp)   echo "--server sip:${FS}:5060;transport=tcp --user sip:1003@${FS} --menc none --ice --dial 9196" ;;
	fs-sdes-tls)  echo "--server sips:${FS}:5061 --user sip:1002@${FS} --menc sdes --dial 9196" ;;
	# G.711 only: compares builds independent of the Opus encoder.
	g711-udp)     echo "--server sip:${PBX}:5060;transport=udp --user sip:plain1@${PBX} --menc none --codec PCMU --dial 600" ;;
	g711-tcp)     echo "--server sip:${PBX}:5060;transport=tcp --user sip:plain1@${PBX} --menc none --codec PCMU --dial 600" ;;
	fs-g711-udp)  echo "--server sip:${FS}:5060;transport=udp --user sip:1000@${FS} --menc none --codec PCMU --dial 9196" ;;
	fs-g711-tcp)  echo "--server sip:${FS}:5060;transport=tcp --user sip:1000@${FS} --menc none --codec PCMU --dial 9196" ;;
	*) echo "unknown scenario $1" >&2; exit 2 ;;
	esac
}

# ── run ──────────────────────────────────────────────────────────────────────
echo "A = ${A}"
echo "B = ${B}"
echo "results: ${RESULTS}"
IFS=, read -r -a profs <<<"${PROFILES}"
IFS=, read -r -a scens <<<"${SCENARIOS}"
for prof in "${profs[@]}"; do
	apply_profile "${prof}"
	for scen in "${scens[@]}"; do
		sargs="$(scenario_args "${scen}")"
		for ((i = 1; i <= RUNS; i++)); do
			order="A B"
			(( i % 2 == 0 )) && order="B A"
			for side in ${order}; do
				# shellcheck disable=SC2086
				line="$("${OUT}/ab_call_${side}" ${sargs} --pass medut \
					--secs "${SECS}" \
					--tag "${side}|${prof}|${scen}|${i}" 2>/dev/null |
					grep '^{' | tail -1)"
				[[ -z "${line}" ]] &&
					line="{\"tag\":\"${side}|${prof}|${scen}|${i}\",\"ok\":false,\"error\":\"no output\"}"
				echo "${line}" >>"${RESULTS}"
				printf '  %-28s %s\n' "${side} ${prof} ${scen} #${i}" \
					"$(grep -o '"ok":[a-z]*' <<<"${line}")"
			done
		done
	done
done
apply_profile clean
trap - EXIT

# ── summary ──────────────────────────────────────────────────────────────────
python3 -I - "${RESULTS}" <<'PY'
import json, sys, statistics as st
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
groups = {}
for r in rows:
    side, prof, scen, _ = r["tag"].split("|")
    groups.setdefault((prof, scen), {}).setdefault(side, []).append(r)

def med(rs, k):
    v = [r[k] for r in rs if r.get("ok") and r.get(k, -1) >= 0]
    return st.median(v) if v else float("nan")

cols = [("tone_purity", "purity", "{:.3f}"),
        ("mos_rx_avg", "MOS rx", "{:.2f}"), ("loss_rx_pct", "loss %", "{:.1f}"),
        ("jitter_ms", "jitter", "{:.1f}"), ("jb_ms", "jbuf ms", "{:.0f}"),
        ("plc", "PLC", "{:.0f}"), ("setup_ms", "setup", "{:.0f}"),
        ("audio_ms", "audio", "{:.0f}")]
print()
print("| profile | scenario | build | ok | " + " | ".join(c[1] for c in cols) + " |")
print("|---|---|---|---|" + "---|" * len(cols))
for (prof, scen), sides in sorted(groups.items()):
    for side in ("A", "B"):
        rs = sides.get(side, [])
        ok = sum(1 for r in rs if r.get("ok"))
        vals = [fmt.format(med(rs, k)) for k, _, fmt in cols]
        print(f"| {prof} | {scen} | {side} | {ok}/{len(rs)} | " + " | ".join(vals) + " |")
print("\nmedians over successful calls; purity = share of the echoed audio's "
      "power at the 440 Hz tone sent (1.0 clean); MOS/loss/jitter are the "
      "SDK's own receive-side MEDIA_STATS averages per call")
PY
