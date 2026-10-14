#!/usr/bin/env bash
# Download the prebuilt iOS xcframework from a build-mobile CI run into the
# Swift package, where MedutKit and (through a symlink) the Flutter plugin
# vendor it.
#
#   scripts/fetch-prebuilt.sh                 latest successful run on main
#   scripts/fetch-prebuilt.sh --wait          wait for the newest run, then sync
#   scripts/fetch-prebuilt.sh --run-id 123    a specific run
#   scripts/fetch-prebuilt.sh --branch dev    latest successful run on a branch
#
# iOS only. Android has no CI job any more (see .github/workflows/
# build-mobile.yml) — build its jniLibs locally with scripts/build-android.sh.
#
# Why this exists: the bindings ship *prebuilt* binaries, so a consumer adding
# them as a git dependency gets whatever is committed — not whatever CI last
# built. CI uploads the xcframework as an artifact, but nothing commits it
# back, so the two drift silently. That drift has already shipped once: the
# committed libmedut.so predated the baresdk_* -> medut_* rename and exported
# none of the medut_* symbols, so apps built, installed, and then failed at the
# first SDK call.
#
# Nothing here is committed for you — review, then `git add` the path printed
# at the end.
#
# Requires: gh (authenticated). Symbol verification additionally wants nm; it
# falls back to probing the string table where unavailable.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

WORKFLOW="build-mobile.yml"
BRANCH="main"
RUN_ID=""
WAIT=0
EXPLICIT=0

IOS_DEST="${ROOT}/bindings/swift/Frameworks/Medut.xcframework"

while [ $# -gt 0 ]; do
  case "$1" in
    --run-id) RUN_ID="$2"; EXPLICIT=1; shift 2 ;;
    --branch) BRANCH="$2"; shift 2 ;;
    --wait)   WAIT=1; shift ;;
    -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "ERROR: unknown argument '$1' (try --help)" >&2; exit 1 ;;
  esac
done

if ! command -v gh >/dev/null; then
  echo "ERROR: gh not found. Install the GitHub CLI: https://cli.github.com" >&2
  exit 1
fi
if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: gh is not authenticated. Run: gh auth login" >&2
  exit 1
fi

cd "${ROOT}"

# ---------------------------------------------------------------------------
# Pick the run
# ---------------------------------------------------------------------------
if [ -n "${RUN_ID}" ]; then
  : # explicit
elif [ "${WAIT}" = "1" ]; then
  # Newest run on the branch whatever its state, then block until it settles.
  RUN_ID="$(gh run list --workflow="${WORKFLOW}" --branch="${BRANCH}" \
              --limit 1 --json databaseId --jq '.[0].databaseId')"
  if [ -z "${RUN_ID}" ]; then
    echo "ERROR: no ${WORKFLOW} runs found on '${BRANCH}'" >&2
    exit 1
  fi
  echo "=== Waiting for run ${RUN_ID} ==="
  # `gh run watch` exits non-zero when the run fails; the status check below
  # reports which job died, so don't let -e kill us before we get there.
  gh run watch "${RUN_ID}" --exit-status || true
else
  RUN_ID="$(gh run list --workflow="${WORKFLOW}" --branch="${BRANCH}" \
              --status=success --limit 1 --json databaseId --jq '.[0].databaseId')"
  if [ -z "${RUN_ID}" ]; then
    echo "ERROR: no successful ${WORKFLOW} run on '${BRANCH}'." >&2
    echo "       Pass --run-id to sync from a specific run anyway." >&2
    exit 1
  fi
fi

# gh has jq built in (--jq), so this needs no jq on PATH.
read -r SHA STATUS CONCL <<<"$(gh run view "${RUN_ID}" \
  --json headSha,status,conclusion \
  --jq '[.headSha, .status, .conclusion] | @tsv')"
TITLE="$(gh run view "${RUN_ID}" --json displayTitle --jq .displayTitle)"

echo "=== Run ${RUN_ID} — ${TITLE} (${SHA:0:12}) — ${STATUS} ${CONCL} ==="

# A failed run's artifact can be from a job that died partway, so the payload
# may be incomplete even when it uploaded. Refuse unless the run is explicitly
# named; the checks below are the backstop if you override.
if [ "${CONCL}" != "success" ] && [ "${EXPLICIT}" != "1" ]; then
  echo "ERROR: run ${RUN_ID} concluded '${CONCL}', not 'success'." >&2
  echo "       Re-run with --run-id ${RUN_ID} to sync from it anyway." >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

# ---------------------------------------------------------------------------
# iOS — the artifact holds the framework's *contents* (the workflow's upload
# path ends in "/"), so the destination directory has to supply the
# Medut.xcframework name itself.
# ---------------------------------------------------------------------------
echo "=== iOS: ios-xcframework ==="
gh run download "${RUN_ID}" -n ios-xcframework -D "${TMP}/ios"
if [ ! -f "${TMP}/ios/Info.plist" ]; then
  echo "ERROR: artifact is not an xcframework payload (no top-level Info.plist)" >&2
  exit 1
fi
rm -rf "${IOS_DEST}"
mkdir -p "$(dirname "${IOS_DEST}")"
mv "${TMP}/ios" "${IOS_DEST}"

# The framework binary must stay executable. actions/upload-artifact has
# historically dropped the exec bit, and git tracks it — a 644 binary is a
# broken framework that looks fine in a diff.
find "${IOS_DEST}" -name Medut -type f -exec chmod 755 {} +

SLICES=$(find "${IOS_DEST}" -name Medut -type f | wc -l | tr -d ' ')
if [ "${SLICES}" -lt 2 ]; then
  echo "ERROR: expected device + simulator slices, found ${SLICES}" >&2
  exit 1
fi
echo "  ${SLICES} slices:"
find "${IOS_DEST}" -name Medut -type f | while read -r BIN; do
  echo "    $(basename "$(dirname "$(dirname "${BIN}")")")"
done

# ---------------------------------------------------------------------------
# Verify the core exports the medut_* API. This is the check that would have
# caught the baresdk_* blobs before they shipped.
#
# nm reads Mach-O only on macOS; elsewhere fall back to probing the string
# table, which is enough to catch an empty or wrong-project binary.
# Process substitution, not a pipe: a piped `while` runs in a subshell, where
# `exit 1` below would end the subshell and let the script report success.
while read -r BIN; do
  SLICE="$(basename "$(dirname "$(dirname "${BIN}")")")"
  if command -v nm >/dev/null && nm -gU "${BIN}" >/dev/null 2>&1; then
    N=$(nm -gU "${BIN}" | grep -c ' _medut_' || true)
  else
    N=$(strings -a "${BIN}" 2>/dev/null | grep -c '^_\?medut_' || true)
  fi
  if [ "${N}" -lt 40 ]; then
    echo "ERROR: ios/${SLICE}: only ${N} medut_* symbols" >&2
    exit 1
  fi
  echo "  ios/${SLICE}: ${N} medut_* symbols"
done < <(find "${IOS_DEST}" -name Medut -type f)

echo ""
echo "Synced from run ${RUN_ID} (${SHA:0:12}). Review and commit:"
echo "  git add bindings/swift/Frameworks"
echo "(Android jniLibs are not fetched — build them with scripts/build-android.sh)"
