#!/usr/bin/env bash
# Verify a built medut.a archive.
# Usage:
#   ./tools/verify.sh <path/to/medut.a> [link]
#
# Without "link": checks symbol presence only.
# With "link":    also compiles and runs a linkability smoke-test
#                 (requires matching system headers + deps).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ARCHIVE="${1:?Usage: verify.sh <path/to/medut.a> [link]}"
MODE="${2:-check}"

if [ ! -f "${ARCHIVE}" ]; then
  echo "ERROR: archive not found: ${ARCHIVE}" >&2
  exit 1
fi

echo "============================================================"
echo "Verifying: ${ARCHIVE}"
echo "Size     : $(du -h "${ARCHIVE}" | cut -f1)"
echo "============================================================"

# ---------------------------------------------------------------------------
# 1. Symbol presence
# ---------------------------------------------------------------------------
echo ""
echo "--- Symbol presence ---"

# Cache nm output once — avoids nm getting SIGPIPE from grep -q exiting early
# (which pipefail would then misreport as a pipeline failure)
NM_CACHE="$(nm -A "${ARCHIVE}" 2>/dev/null)"

check_sym() {
  local SYM="$1"
  # Use here-string (<<<) to avoid echo|grep SIGPIPE → pipefail false-negative
  if grep -q " T ${SYM}\b" <<< "${NM_CACHE}"; then
    printf "  %-40s  [OK]\n" "${SYM}"
  else
    printf "  %-40s  [MISSING]\n" "${SYM}"
    MISSING_SYMS=$((MISSING_SYMS + 1))
  fi
}

MISSING_SYMS=0

# Medut public API
check_sym "medut_init"
check_sym "medut_call_ice_state"
check_sym "bsdk_ice_module_init"
check_sym "medut_shutdown"
check_sym "medut_config_init"
check_sym "medut_account_create"
check_sym "medut_account_destroy"
check_sym "medut_account_register"
check_sym "medut_account_unregister"
check_sym "medut_call_invite"
check_sym "medut_call_answer"
check_sym "medut_call_hangup"
check_sym "medut_call_hold"
check_sym "medut_call_resume"
check_sym "medut_call_send_dtmf"
check_sym "medut_call_transfer"
check_sym "medut_call_set_media_tap"
check_sym "medut_call_get_stats"
check_sym "medut_audio_mute"
check_sym "medut_audio_set_input_device"
check_sym "medut_audio_set_output_device"
check_sym "medut_audio_use_external"
check_sym "medut_audio_external_push"
check_sym "medut_audio_external_pull"
check_sym "medut_audio_external_format"
check_sym "medut_audio_external_is_active"
check_sym "medut_audio_external_set_voice_path"
check_sym "medut_pcap_start"
check_sym "medut_pcap_stop"
check_sym "medut_version"

# Phase 2
check_sym "medut_call_attended_transfer"
# Incoming REFER completion (RFC 3515) + call metadata
check_sym "medut_call_transfer_accept"
check_sym "medut_call_transfer_reject"
check_sym "medut_call_get_info"
check_sym "medut_message_send"
check_sym "medut_account_publish_presence"
check_sym "medut_account_set_100rel"

# Degraded-link handling
check_sym "medut_call_set_rtp_timeout"
check_sym "medut_call_set_bitrate"
check_sym "medut_set_adaptive_bitrate"
check_sym "medut_account_keepalive_now"

# Underlying baresip/libre internals (should still be in the merged archive)
check_sym "baresip_init"
check_sym "ua_init"
check_sym "re_main"
check_sym "websock_connect"
check_sym "sip_transp_add_websock"
check_sym "srtp_decrypt"

echo ""
if [ "${MISSING_SYMS}" -gt 0 ]; then
  echo "WARNING: ${MISSING_SYMS} expected symbol(s) not found."
  echo "         Check that STATIC=ON and the correct modules were built."
else
  echo "All expected symbols present."
fi

# ---------------------------------------------------------------------------
# 2. Object member count (sanity: merged archive should be non-trivial)
# ---------------------------------------------------------------------------
echo ""
echo "--- Object member count ---"
MEMBER_COUNT=$(grep -c " T " <<< "${NM_CACHE}" || true)
echo "  Exported text symbols : ${MEMBER_COUNT}"
if [ "${MEMBER_COUNT}" -lt 100 ]; then
  echo "WARNING: suspiciously few symbols — archive may be incomplete."
fi

# ---------------------------------------------------------------------------
# 3. Header parseability (preprocessor sanity)
# ---------------------------------------------------------------------------
INCLUDE_DIR="$(dirname "${ARCHIVE}")/include"
if [ -f "${INCLUDE_DIR}/baresip.h" ]; then
  echo ""
  echo "--- Header parseability ---"
  if clang -E -x c "${INCLUDE_DIR}/baresip.h" -I "${INCLUDE_DIR}" > /dev/null 2>&1; then
    echo "  baresip.h: [OK] (clang -E passed)"
  elif gcc  -E -x c "${INCLUDE_DIR}/baresip.h" -I "${INCLUDE_DIR}" > /dev/null 2>&1; then
    echo "  baresip.h: [OK] (gcc -E passed)"
  else
    echo "  baresip.h: [WARN] preprocessor failed — check include paths"
  fi
fi

# ---------------------------------------------------------------------------
# 4. Linkability smoke-test (optional, requires system deps)
# ---------------------------------------------------------------------------
if [ "${MODE}" = "link" ]; then
  echo ""
  echo "--- Linkability smoke-test ---"

  SMOKETEST_C="${ROOT}/tools/_smoketest.c"
  SMOKETEST_BIN="${ROOT}/tools/_smoketest"
  ACCOUNT_JSON="${ROOT}/tools/account.json"

  # ---------------------------------------------------------------------------
  # Read account.json (requires jq)
  # ---------------------------------------------------------------------------
  ACCT_ENABLED=0
  ACCT_DEFS=""
  if [ -f "${ACCOUNT_JSON}" ] && command -v jq > /dev/null 2>&1; then
    ACCT_ENABLED=$(jq -r '.enabled // false' "${ACCOUNT_JSON}" | grep -q true && echo 1 || echo 0)
    if [ "${ACCT_ENABLED}" = "1" ]; then
      _jq_str() { jq -r "$1 // empty" "${ACCOUNT_JSON}"; }
      URI=$(         _jq_str '.uri')
      PASS=$(        _jq_str '.password')
      TRANSPORT=$(    jq -r '.transport // "udp"' "${ACCOUNT_JSON}")
      SERVER_URL=$(  _jq_str '.server_url')
      AUTH=$(        _jq_str '.auth_user')
      DISPLAY=$(     _jq_str '.display_name')
      OUTBOUND=$(    _jq_str '.outbound')
      MEDIA_ENC=$(   _jq_str '.media_enc')
      ICE=$(          jq -r '.ice_enabled // false' "${ACCOUNT_JSON}" | grep -q true && echo 1 || echo 0)
      STUN=$(        _jq_str '.stun_server')
      TURN=$(        _jq_str '.turn_server')
      TURN_USER=$(   _jq_str '.turn_user')
      TURN_PASS=$(   _jq_str '.turn_pass')
      VERIFY_TLS=$(   jq -r '.verify_tls // false' "${ACCOUNT_JSON}" | grep -q true && echo 1 || echo 0)

      # Map transport string → medut_transport_t enum value
      case "${TRANSPORT}" in
        udp) TRANSPORT_ENUM=0 ;;
        tcp) TRANSPORT_ENUM=1 ;;
        tls) TRANSPORT_ENUM=2 ;;
        ws)  TRANSPORT_ENUM=3 ;;
        wss) TRANSPORT_ENUM=4 ;;
        *)   TRANSPORT_ENUM=0 ;;
      esac

      # Map media_enc string → medut_media_enc_t enum value
      case "${MEDIA_ENC}" in
        sdes|srtp)    MEDIA_ENC_ENUM=1 ;;
        dtls_srtp)    MEDIA_ENC_ENUM=2 ;;
        *)            MEDIA_ENC_ENUM=0 ;;
      esac

      # Build -D flags; nullable fields become NULL when empty
      _def_str() {
        local name="$1" val="$2"
        [ -n "${val}" ] && echo "-D${name}=\"${val}\"" || echo "-D${name}=NULL"
      }
      ACCT_DEFS="-DACCOUNT_ENABLED=1"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_URI        "${URI}")"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_PASS       "${PASS}")"
      ACCT_DEFS="${ACCT_DEFS} -DACCOUNT_TRANSPORT=${TRANSPORT_ENUM}"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_SERVER_URL "${SERVER_URL}")"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_AUTH       "${AUTH}")"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_DISPLAY    "${DISPLAY}")"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_OUTBOUND   "${OUTBOUND}")"
      ACCT_DEFS="${ACCT_DEFS} -DACCOUNT_MEDIA_ENC=${MEDIA_ENC_ENUM}"
      ACCT_DEFS="${ACCT_DEFS} -DACCOUNT_ICE=${ICE}"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_STUN       "${STUN}")"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_TURN       "${TURN}")"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_TURN_USER  "${TURN_USER}")"
      ACCT_DEFS="${ACCT_DEFS} $(_def_str ACCOUNT_TURN_PASS  "${TURN_PASS}")"
      ACCT_DEFS="${ACCT_DEFS} -DSIP_VERIFY_TLS=${VERIFY_TLS}"
      echo "  Account   : ${URI}"
      echo "  Transport : ${TRANSPORT}${SERVER_URL:+ → ${SERVER_URL}}"
      [ "${ICE}" = "1" ] && echo "  NAT       : ICE${STUN:+ + STUN}${TURN:+ + TURN}"
      [ -n "${MEDIA_ENC}" ] && echo "  Crypto    : ${MEDIA_ENC}"
    else
      echo "  Account : disabled (set \"enabled\": true in tools/account.json to test registration)"
    fi
  else
    echo "  Account : skipped (tools/account.json not found or jq missing)"
  fi

  cat > "${SMOKETEST_C}" <<'CSRC'
/* Medut linkability smoke-test — credentials loaded from tools/account.json */
#include "medut.h"
#include <stdio.h>
#include <unistd.h>

#ifndef ACCOUNT_ENABLED
#define ACCOUNT_ENABLED     0
#endif
#ifndef ACCOUNT_URI
#define ACCOUNT_URI         NULL
#endif
#ifndef ACCOUNT_PASS
#define ACCOUNT_PASS        NULL
#endif
#ifndef ACCOUNT_TRANSPORT
#define ACCOUNT_TRANSPORT   0       /* MEDUT_TRANSPORT_UDP */
#endif
#ifndef ACCOUNT_SERVER_URL
#define ACCOUNT_SERVER_URL  NULL
#endif
#ifndef ACCOUNT_AUTH
#define ACCOUNT_AUTH        NULL
#endif
#ifndef ACCOUNT_DISPLAY
#define ACCOUNT_DISPLAY     NULL
#endif
#ifndef ACCOUNT_OUTBOUND
#define ACCOUNT_OUTBOUND    NULL
#endif
#ifndef ACCOUNT_MEDIA_ENC
#define ACCOUNT_MEDIA_ENC   0       /* MEDUT_MEDIA_ENC_NONE */
#endif
#ifndef ACCOUNT_ICE
#define ACCOUNT_ICE         0
#endif
#ifndef ACCOUNT_STUN
#define ACCOUNT_STUN        NULL
#endif
#ifndef ACCOUNT_TURN
#define ACCOUNT_TURN        NULL
#endif
#ifndef ACCOUNT_TURN_USER
#define ACCOUNT_TURN_USER   NULL
#endif
#ifndef ACCOUNT_TURN_PASS
#define ACCOUNT_TURN_PASS   NULL
#endif
#ifndef SIP_VERIFY_TLS
#define SIP_VERIFY_TLS      0
#endif

static volatile int g_reg_done = 0;
static volatile int g_reg_ok   = 0;

static void evt(const medut_event_t *ev, void *ud)
{
    (void)ud;
    if (ev->type == MEDUT_EV_REG_STATE) {
        const medut_ev_reg_state_t *r = &ev->u.reg;
        if (r->state == MEDUT_REG_REGISTERED) {
            printf("  REG: registered OK\n");
            g_reg_ok   = 1;
            g_reg_done = 1;
        } else if (r->state == MEDUT_REG_FAILED ||
                   r->state == MEDUT_REG_RECONNECTING) {
            printf("  REG: %s — %s\n",
                   r->state == MEDUT_REG_RECONNECTING ? "RECONNECTING"
                                                        : "FAILED",
                   r->error_str ? r->error_str : "unknown");
            g_reg_done = 1;
        }
    }
}

int main(void)
{
    medut_config_t cfg;
    medut_config_init(&cfg);
    cfg.event_cb      = evt;
    cfg.verify_server = SIP_VERIFY_TLS;

    int err = medut_init(&cfg);
    if (err) { fprintf(stderr, "medut_init: %d\n", err); return 1; }

    printf("medut_init OK — version %s\n", medut_version());

#if ACCOUNT_ENABLED
    medut_account_config_t acct_cfg = {
        .uri          = ACCOUNT_URI,
        .password     = ACCOUNT_PASS,
        .transport    = (medut_transport_t)ACCOUNT_TRANSPORT,
        .server_url   = ACCOUNT_SERVER_URL,
        .auth_user    = ACCOUNT_AUTH,
        .display_name = ACCOUNT_DISPLAY,
        .outbound     = ACCOUNT_OUTBOUND,
        .media_enc    = (medut_media_enc_t)ACCOUNT_MEDIA_ENC,
        .ice_enabled  = ACCOUNT_ICE,
        .stun_server  = ACCOUNT_STUN,
        .turn_server  = ACCOUNT_TURN,
        .turn_user    = ACCOUNT_TURN_USER,
        .turn_pass    = ACCOUNT_TURN_PASS,
        .verify_tls   = SIP_VERIFY_TLS,
    };
    medut_account_handle_t acct = NULL;
    err = medut_account_create(&acct_cfg, &acct);
    if (err) { fprintf(stderr, "account_create: %d\n", err); goto done; }

    err = medut_account_register(acct);
    if (err) { fprintf(stderr, "account_register: %d\n", err); goto done; }

    printf("Waiting for registration (10s max)...\n");
    for (int i = 0; i < 100 && !g_reg_done; i++)
        usleep(100000);

    if (!g_reg_done)
        printf("  REG: timed out — no response in 10s\n");

    medut_account_unregister(acct);
    medut_account_destroy(acct);
done:
#endif

    medut_shutdown();
    printf("smoketest PASSED\n");
    return g_reg_done && !g_reg_ok ? 1 : 0;
}
CSRC

  SDK_INCLUDE_DIR="${ROOT}/include"
  INCS="-I ${SDK_INCLUDE_DIR} -I ${INCLUDE_DIR} -I ${INCLUDE_DIR}/re"

  # Try to link — platform link flags
  case "$(uname -s)" in
    Linux)
      LDFLAGS="-lpthread -lssl -lcrypto -lz -lm -ldl -lresolv -lpulse -lopus"
      ;;
    Darwin)
      LDFLAGS="-framework CoreFoundation -framework Security"
      ;;
    *)
      LDFLAGS=""
      ;;
  esac

  if ${CC:-gcc} -o "${SMOKETEST_BIN}" "${SMOKETEST_C}" \
      ${INCS} ${ACCT_DEFS} "${ARCHIVE}" ${LDFLAGS} 2>&1; then
    echo "  Compilation: [OK]"
    if HOME=/tmp/.Medut-smoketest "${SMOKETEST_BIN}"; then
      echo "  Execution  : [OK]"
    else
      echo "  Execution  : [FAILED]"
    fi
  else
    echo "  Compilation: [FAILED] — check system deps and link flags"
  fi

  rm -f "${SMOKETEST_C}" "${SMOKETEST_BIN}"
fi

echo ""
echo "============================================================"
echo "Verification complete."
echo "============================================================"
