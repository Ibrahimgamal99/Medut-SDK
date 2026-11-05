#!/usr/bin/env bash
# Self-signed certificate for the interop PBXs' TLS and WSS listeners.
# Written to test/interop/certs/ (gitignored); the test accounts run with
# verify_server off, so nothing needs to trust it.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/certs"
mkdir -p "${DIR}"
[[ -f "${DIR}/interop.crt" ]] && exit 0
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
	-subj "/CN=medut-interop" \
	-addext "subjectAltName=IP:172.31.250.10,IP:172.31.250.20,DNS:localhost" \
	-keyout "${DIR}/interop.key" -out "${DIR}/interop.crt" 2>/dev/null
chmod 644 "${DIR}/interop.key"
echo "certificate written to ${DIR}"
