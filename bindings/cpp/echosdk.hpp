/**
 * echosdk.hpp — header-only C++17 RAII wrapper around the EchoSDK C API.
 *
 * Self-contained: embeds the C declarations from echosdk.h so you only need
 * this one header + the compiled shared library (echosdk.so / .dll / .dylib).
 *
 * If you have already included echosdk.h before this header, the embedded
 * declarations are skipped automatically.
 *
 * Usage:
 *   #include "echosdk.hpp"
 *
 *   EchoSDK::SDK sdk;
 *   sdk.config().transport = ECHOSDK_TRANSPORT_TLS;
 *   sdk.on_event([](const echosdk_event_t& ev){ ... });
 *   auto acct = sdk.create_account("alice@pbx.example.com", "secret");
 *   acct.register_account();
 */

#pragma once

/* ── Forward-declare or include the C API ────────────────────────────────── */
#ifndef ECHOSDK_H
/**
 * @file echosdk.h  EchoSDK public API
 *
 * Single header for all platforms. Include this and link echosdk.a.
 * Never include re.h or baresip.h directly from consumer code.
 *
 * ABI stability contract:
 *   - Fields are only appended to structs; never reordered or removed.
 *   - echosdk_config_t carries version + struct_size for forward compat.
 *   - Opaque handle types are stable across minor versions.
 */


#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── DLL export macros (Windows shared library build) ──────────────────── */
#ifdef _WIN32
#  ifdef ECHOSDK_SHARED_BUILD
#    define ECHOSDK_EXPORT __declspec(dllexport)
#  elif defined(ECHOSDK_SHARED)
#    define ECHOSDK_EXPORT __declspec(dllimport)
#  else
#    define ECHOSDK_EXPORT
#  endif
#else
#  define ECHOSDK_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────────────────── */

#define ECHOSDK_VERSION_MAJOR 1
#define ECHOSDK_VERSION_MINOR 0
#define ECHOSDK_VERSION_PATCH 0

ECHOSDK_EXPORT const char *echosdk_version(void);
ECHOSDK_EXPORT const char *echosdk_strerror(int err);

/* ── Opaque handles ───────────────────────────────────────────────────────── */

typedef struct echosdk_account  *echosdk_account_handle_t;
typedef struct echosdk_call     *echosdk_call_handle_t;

/* ── Enumerations ─────────────────────────────────────────────────────────── */

typedef enum {
	ECHOSDK_TRANSPORT_UDP = 0,
	ECHOSDK_TRANSPORT_TCP,
	ECHOSDK_TRANSPORT_TLS,
	ECHOSDK_TRANSPORT_WS,
	ECHOSDK_TRANSPORT_WSS,
} echosdk_transport_t;

typedef enum {
	ECHOSDK_MEDIA_ENC_NONE = 0,
	ECHOSDK_MEDIA_ENC_SDES,
	ECHOSDK_MEDIA_ENC_DTLS_SRTP,
} echosdk_media_enc_t;

/* Only OPUS, PCMU and PCMA are compiled into the library — on every platform.
 * G722 and G726_32 are retained so the numeric values of the constants above
 * and below them stay put (removing them would shift the enum and break the
 * ABI for already-built callers), but no module registers those codecs:
 * selecting one contributes nothing to the SDP offer and logs a warning. */
typedef enum {
	ECHOSDK_CODEC_OPUS = 0,
	ECHOSDK_CODEC_PCMU,     /* G.711 µ-law */
	ECHOSDK_CODEC_PCMA,     /* G.711 A-law */
	ECHOSDK_CODEC_G722,     /* deprecated — not compiled in */
	ECHOSDK_CODEC_G726_32,  /* deprecated — not compiled in */
} echosdk_codec_t;

typedef enum {
	ECHOSDK_MOS_EMODEL = 0,
	ECHOSDK_MOS_SIMPLIFIED,
} echosdk_mos_method_t;

/**
 * AEC backend selection.
 * Stored as uint8_t to occupy exactly 1 byte (same as the former bool aec),
 * preserving echosdk_config_t memory layout.
 *
 * ECHOSDK_AEC_SUPPRESSOR (default): built-in half-duplex TX suppressor.
 *   Zero external dependencies.  Ignored on Android and iOS, where the
 *   platform driver captures through the OS voice path (VOICE_COMMUNICATION /
 *   VoiceProcessingIO) and has already cancelled the echo in hardware —
 *   ducking the mic on top of that only costs full duplex.
 * ECHOSDK_AEC_WEBRTC: full-duplex WebRTC acoustic echo cancellation.
 *   Desktop only; requires ECHOSDK_WITH_WEBRTC_AEC build option and
 *   libwebrtc-audio-processing-1.  Returns ENOTSUP on mobile builds and
 *   when the option is off.
 */
/* ECHOSDK_NO_PACKED_ENUM: for binding generators (e.g. ffigen) that ignore
 * __attribute__((packed)) and would widen the field to 4 bytes, silently
 * shifting every later struct member. The uint8_t typedef keeps the ABI. */
#if defined(_MSC_VER) || defined(ECHOSDK_NO_PACKED_ENUM)
typedef uint8_t echosdk_aec_mode_t;
#  define ECHOSDK_AEC_OFF        ((echosdk_aec_mode_t)0)
#  define ECHOSDK_AEC_SUPPRESSOR ((echosdk_aec_mode_t)1)
#  define ECHOSDK_AEC_WEBRTC     ((echosdk_aec_mode_t)2)
#else
typedef enum __attribute__((packed)) {
	ECHOSDK_AEC_OFF        = 0,
	ECHOSDK_AEC_SUPPRESSOR = 1,
	ECHOSDK_AEC_WEBRTC     = 2,
} echosdk_aec_mode_t;
#endif

typedef enum {
	ECHOSDK_MEDIA_DIR_RX = 0,
	ECHOSDK_MEDIA_DIR_TX,
} echosdk_media_dir_t;

typedef enum {
	ECHOSDK_DTMF_RFC4733  = 0, /* RFC 4733 RTP telephony-event (default) */
	ECHOSDK_DTMF_SIP_INFO = 1, /* SIP INFO application/dtmf-relay */
	ECHOSDK_DTMF_AUTO     = 2, /* prefer RFC 4733, fall back to SIP INFO */
} echosdk_dtmf_mode_t;

typedef enum {
	ECHOSDK_JBUF_ADAPTIVE = 0, /* adaptive jitter buffer (default) */
	ECHOSDK_JBUF_FIXED    = 1, /* fixed-depth jitter buffer */
} echosdk_jbuf_type_t;

/**
 * What to do with an ICE call whose candidates could not be re-gathered on
 * handover.
 *
 * A network handover invalidates every gathered ICE candidate, so the SDK
 * migrates an ICE call with a full RFC 8445 §9 ICE restart: new ice-ufrag /
 * ice-pwd, a fresh gather on the interface that now carries the default route,
 * and a re-INVITE built from the result.  That is the normal path and it needs
 * nothing from the app.
 *
 * This setting applies to the calls it cannot be done for — no ICE session left
 * to restart, or a replacement that could not be allocated.  Those get the plain
 * re-INVITE, which carries the old candidate set: it recovers the call when the
 * peer is reachable directly or through a still-valid TURN relay, and cannot
 * recover it otherwise.  ECHOSDK_NET_CALL_ICE_STALE marks such a call.
 */
typedef enum {
	/** Send the re-INVITE anyway and let media verification decide.
	 *  Recovers relay/direct paths; wastes net_verify_ms × net_max_attempts
	 *  before failing when it cannot. */
	ECHOSDK_ICE_HANDOVER_BEST_EFFORT = 0,
	/** Try once, then fail immediately.  Gives the app a prompt
	 *  CALL_MIGRATION_FAILED it can answer by re-placing the call, instead
	 *  of a long silence.  Recommended when calls are ICE+TURN over
	 *  cellular.
	 *
	 *  Does not apply to a call whose ICE was restarted: that call is not
	 *  offering the wrong candidates any more, so it is held to the same
	 *  retry budget as a direct-RTP call. */
	ECHOSDK_ICE_HANDOVER_FAIL_FAST,
} echosdk_ice_handover_t;

/**
 * ICE liveness of a call's media path — the agent's own verdict, from its
 * connectivity checks (RFC 8445) and consent freshness (RFC 7675), not an
 * inference from RTP counters.  Mirrors the browser's RTCIceConnectionState.
 *
 *   NEW → GATHERING → CHECKING → CONNECTED → COMPLETED
 *
 * CONNECTED: a pair is writable; COMPLETED: that pair is also nominated.
 * DISCONNECTED: the selected pair stopped answering or receiving while
 * checks continue (the path is in trouble but may recover on its own).
 * FAILED: every pair timed out and nothing is left to gather; the SDK does
 * not end the call — redialling is the app's decision.
 * Delivered as ECHOSDK_EV_ICE_STATE and readable via echosdk_call_ice_state().
 */
typedef enum {
	ECHOSDK_ICE_NEW = 0,
	ECHOSDK_ICE_GATHERING,
	ECHOSDK_ICE_CHECKING,
	ECHOSDK_ICE_CONNECTED,
	ECHOSDK_ICE_COMPLETED,
	ECHOSDK_ICE_DISCONNECTED,
	ECHOSDK_ICE_FAILED,
	ECHOSDK_ICE_CLOSED,
} echosdk_ice_state_t;

/** Candidate type of one side of the selected pair (RFC 8445 §5.1.1). */
typedef enum {
	ECHOSDK_ICE_CAND_HOST = 0,
	ECHOSDK_ICE_CAND_SRFLX,
	ECHOSDK_ICE_CAND_PRFLX,
	ECHOSDK_ICE_CAND_RELAY,
	ECHOSDK_ICE_CAND_NONE,
} echosdk_ice_cand_type_t;

typedef struct {
	int  bitrate;    /* 0 = auto/VBR; otherwise bps, e.g. 32000 */
	int  complexity; /* 0-10 CPU trade-off; -1 = opus default (9) */
	bool cbr;        /* constant bitrate; false = VBR (default) */
	bool dtx;        /* discontinuous transmission (silence suppression) */
	bool fec;        /* in-band forward error correction */
	bool stereo;     /* stereo output; false = mono (default) */
} echosdk_opus_config_t;

/* ── Error codes ──────────────────────────────────────────────────────────── */

typedef enum {
	ECHOSDK_OK                       =  0,
	ECHOSDK_ERR_INVAL                = -1,
	ECHOSDK_ERR_NOMEM                = -2,
	ECHOSDK_ERR_STATE                = -3,  /* wrong lifecycle state */
	ECHOSDK_ERR_DNS                  = -4,
	ECHOSDK_ERR_TRANSPORT            = -5,
	ECHOSDK_ERR_AUTH                 = -6,
	ECHOSDK_ERR_SERVER_5XX           = -7,
	ECHOSDK_ERR_WS_PROTOCOL_REJECTED = -8,
	ECHOSDK_ERR_TIMEOUT              = -9,
	ECHOSDK_ERR_ALREADY              = -10,
	/* 403 Forbidden: the request was understood and the credentials were
	 * fine — the server refused this particular request.  On a call that
	 * is a dialplan or policy refusal ("you may not dial that"), which is
	 * not a reason to ask anyone for new credentials.  Registration keeps
	 * reporting 403 as ECHOSDK_ERR_AUTH, where it does mean the account
	 * itself was rejected.  Appended at the end of the enum so an app
	 * built against an older header keeps its own numbering. */
	ECHOSDK_ERR_FORBIDDEN            = -11,
} echosdk_error_t;

/* ── Call states ──────────────────────────────────────────────────────────── */

typedef enum {
	ECHOSDK_CALL_CALLING = 0,
	ECHOSDK_CALL_RINGING,
	ECHOSDK_CALL_ESTABLISHED,
	ECHOSDK_CALL_HELD,
	ECHOSDK_CALL_ENDED,
	ECHOSDK_CALL_CANCELLED,
	ECHOSDK_CALL_FAILED,
} echosdk_call_state_t;

/* ── Registration states ──────────────────────────────────────────────────── */

typedef enum {
	ECHOSDK_REG_UNREGISTERED = 0,
	ECHOSDK_REG_REGISTERING,
	ECHOSDK_REG_REGISTERED,
	/**
	 * Terminal: the registration is down and the SDK has stopped trying.
	 * Either nothing it can retry will help (ECHOSDK_ERR_AUTH — wrong
	 * credentials), the retry budget (`reg_retry_max_attempts`) ran out, or
	 * the app cancelled the retry itself.  Recovering needs the app:
	 * echosdk_account_retry_now(), or new credentials.
	 */
	ECHOSDK_REG_FAILED,
	ECHOSDK_REG_UNREGISTERING,
	/**
	 * Transient: the registration is not usable right now, and the SDK is
	 * getting it back on its own.  Nothing for the app to do but say so —
	 * "Reconnecting…", not "Registration failed".
	 *
	 * Appended after UNREGISTERING deliberately: the values above it are
	 * ABI, so this one had to take 5 rather than sit next to FAILED.
	 *
	 * It is reported for every recovery the SDK drives itself:
	 *
	 *   - a REGISTER that failed on transport, timeout or 5xx, with a retry
	 *     armed — `retry_attempt` and `retry_delay_ms` say which attempt is
	 *     next and how long until it goes out,
	 *   - a keepalive probe that went unanswered (cfg.keepalive_interval):
	 *     the binding is registered on paper but the path to the proxy is
	 *     gone,
	 *   - a network handover or a link that dropped entirely — Wi-Fi ↔
	 *     cellular, VPN up/down, dock/undock — from the moment the change is
	 *     detected until the REGISTER lands on the new path.  The matching
	 *     ECHOSDK_EV_NETWORK events carry the detail.
	 *
	 * The state does not flicker back to REGISTERING for each retry: an
	 * account stays RECONNECTING for the whole recovery, and leaves it only
	 * for REGISTERED (recovered) or FAILED (given up).  REGISTERING keeps
	 * its narrower meaning — a registration the app asked for, going out
	 * for the first time.
	 */
	ECHOSDK_REG_RECONNECTING,
} echosdk_reg_state_t;

/* ── Presence enums ────────────────────────────────────────────────────────── */

typedef enum {
	ECHOSDK_PRESENCE_UNKNOWN = 0,
	ECHOSDK_PRESENCE_OPEN,     /* available */
	ECHOSDK_PRESENCE_CLOSED,   /* offline / DND */
	ECHOSDK_PRESENCE_BUSY,     /* on a call */
} echosdk_presence_status_t;

typedef enum {
	ECHOSDK_100REL_DISABLED = 0, /* never send/require 100rel */
	ECHOSDK_100REL_ENABLED  = 1, /* support 100rel if peer offers */
	ECHOSDK_100REL_REQUIRED = 2, /* require 100rel; reject if unsupported */
} echosdk_100rel_mode_t;

/**
 * Push notification provider — determines the pn-provider URI parameter
 * value placed in the SIP REGISTER Contact header per RFC 8599, or the
 * server-side push dispatch type for non-RFC-8599 servers.
 */
typedef enum {
	ECHOSDK_PUSH_PROVIDER_NONE         = 0, /* no push params (default) */
	ECHOSDK_PUSH_PROVIDER_APNS         = 1, /* Apple APNs production    */
	ECHOSDK_PUSH_PROVIDER_APNS_SANDBOX = 2, /* Apple APNs development   */
	ECHOSDK_PUSH_PROVIDER_FCM          = 3, /* Firebase Cloud Messaging  */
} echosdk_push_provider_t;

/* ── Event types ──────────────────────────────────────────────────────────── */

typedef enum {
	ECHOSDK_EV_LOG = 0,
	ECHOSDK_EV_REG_STATE,
	ECHOSDK_EV_INCOMING_CALL,
	ECHOSDK_EV_CALL_STATE,
	ECHOSDK_EV_CALL_DTMF,
	ECHOSDK_EV_SDP_NEGOTIATION,
	ECHOSDK_EV_SIP_TRACE,
	ECHOSDK_EV_MEDIA_STATS,
	ECHOSDK_EV_REGISTRAR_WARNING,
	ECHOSDK_EV_TRANSFER_REQUEST,
	ECHOSDK_EV_MWI,
	ECHOSDK_EV_MESSAGE,
	ECHOSDK_EV_PRESENCE_STATE,
	ECHOSDK_EV_QUALITY_ALERT,
	ECHOSDK_EV_NETWORK,
	/** An outgoing REFER was refused.  The call named in the payload is STILL
	 *  ESTABLISHED — this is deliberately not a call-state change, so a failed
	 *  transfer never tears down the leg the user is still talking on.  A REFER
	 *  that succeeds needs no event of its own: the far end takes the call over
	 *  and our leg closes, which arrives as ECHOSDK_EV_CALL_STATE / ENDED. */
	ECHOSDK_EV_TRANSFER_FAILED,
	/** The ICE liveness state of a call's media path changed — see
	 *  echosdk_ice_state_t.  Payload: echosdk_ev_ice_state_t. */
	ECHOSDK_EV_ICE_STATE,
} echosdk_event_type_t;

/* ── Network handover ─────────────────────────────────────────────────────── */

/**
 * Stages of a network handover (Wi-Fi ↔ 4G/5G, VPN up/down, dock/undock).
 *
 * A typical Wi-Fi → cellular handover emits, in order:
 *   CHANGE_DETECTED → TRANSPORT_RESET → REREGISTERING
 *                   → CALL_MIGRATING → CALL_MIGRATED
 *
 * When the device is briefly off-network the sequence starts with DOWN and
 * resumes at UP once a usable address appears.
 */
typedef enum {
	ECHOSDK_NET_CHANGE_DETECTED = 0,   /* local address set changed          */
	ECHOSDK_NET_DOWN,                  /* no usable local address            */
	ECHOSDK_NET_UP,                    /* usable local address again         */
	ECHOSDK_NET_TRANSPORT_RESET,       /* SIP transports flushed + re-bound  */
	ECHOSDK_NET_REREGISTERING,         /* REGISTER re-sent on the new path   */
	ECHOSDK_NET_CALL_MIGRATING,        /* re-INVITE sent to move the media   */
	ECHOSDK_NET_CALL_MIGRATE_ACCEPTED, /* peer answered; awaiting audio      */
	ECHOSDK_NET_CALL_MIGRATED,         /* RTP confirmed on the new path      */
	ECHOSDK_NET_CALL_MIGRATION_FAILED, /* gave up; see `error`               */
	ECHOSDK_NET_CALL_DEFERRED,         /* not refreshable yet; will retry    */
	ECHOSDK_NET_HANDOVER_FAILED,       /* transport reset failed; retrying   */
	/** ICE call whose candidates could not be re-gathered: the migration
	 *  falls back to a re-INVITE carrying the pre-handover candidate set,
	 *  which recovers direct and TURN-relayed paths only.  Emitted once per
	 *  call per handover, before that re-INVITE.  An ICE call that *was*
	 *  restarted does not emit this — it migrates like any other call.  See
	 *  echosdk_ice_handover_t and cfg.net_ice_handover. */
	ECHOSDK_NET_CALL_ICE_STALE,
	/** ICE finished its connectivity checks with no usable candidate pair,
	 *  so nothing was nominated and the media path was never validated.
	 *
	 *  Distinct from CALL_ICE_STALE, which is "the candidates could not be
	 *  re-gathered": here they were gathered and offered, the peer answered,
	 *  and every pair in the check list failed — a peer that rejected the
	 *  checks (STUN 401 after it rebuilt its own ICE session across our
	 *  restart offer) or a path that stopped carrying UDP.
	 *
	 *  The SDK does not end the call: where a pair had been validated
	 *  earlier the stream is held on it and audio usually survives, and the
	 *  decision to redial belongs to the app.  Emitted once per failed ICE
	 *  generation, so a call that restarts twice can emit it twice. */
	ECHOSDK_NET_CALL_ICE_FAILED,
} echosdk_net_event_t;

/**
 * Network handover progress.
 *
 * `attempt` / `max_attempts` render a progress counter:
 *     "link settled — rebuilding the media path %u/%u"
 * `elapsed_ms` on CALL_MIGRATED is how long audio was interrupted:
 *     "media recovered after %.1f s"
 */
typedef struct {
	echosdk_net_event_t      event;
	echosdk_call_handle_t    call;     /* CALL_* events only; else NULL    */
	echosdk_account_handle_t account;  /* REREGISTERING only; else NULL    */
	const char              *local_addr; /* new local IP, "" when unknown  */
	uint32_t                 attempt;  /* 1-based retry counter            */
	/**
	 * The offer budget this call is held to — net_max_attempts normally, or 1
	 * for a call under ECHOSDK_ICE_HANDOVER_FAIL_FAST whose ICE could not be
	 * restarted.
	 *
	 * Render it as a progress counter ("%u/%u"), not as an invariant:
	 * `attempt` also advances while a call waits for a default route to
	 * appear (CALL_DEFERRED), and those waits keep the full net_max_attempts
	 * budget, so on a fail-fast call `attempt` can exceed this value.
	 */
	uint32_t                 max_attempts;
	uint32_t                 elapsed_ms;   /* ms since this migration began */
	/**
	 * True when the call has a live ICE media-NAT (the negotiated truth, not
	 * cfg.ice_enabled).  Such a call is migrated with an RFC 8445 §9 ICE
	 * restart — new credentials and a fresh gather, so the re-INVITE carries
	 * candidates for the network the device is on now.  Recovery is only
	 * best-effort for one that also carries CALL_ICE_STALE, meaning the
	 * restart could not be performed.
	 */
	bool                     ice;
	echosdk_error_t          error;    /* ECHOSDK_OK unless *_FAILED       */
} echosdk_ev_network_t;

/**
 * ICE liveness change (ECHOSDK_EV_ICE_STATE).
 *
 * `local_addr` / `remote_addr` are the selected candidate pair as
 * "ip:port" ("" until a pair is selected), `generation` counts ICE
 * restarts (1 = the initial session), `rtt_ms` is the STUN round trip on
 * the selected pair.  The strings are valid for the callback's duration
 * (or for the event's life with cfg.deliver_owned_events).
 */
typedef struct {
	echosdk_call_handle_t    call;
	echosdk_ice_state_t      state;
	echosdk_ice_state_t      prev;
	uint32_t                 generation;
	echosdk_ice_cand_type_t  local_type;
	echosdk_ice_cand_type_t  remote_type;
	const char              *local_addr;
	const char              *remote_addr;
	float                    rtt_ms;
	echosdk_error_t          error;      /* ECHOSDK_OK unless FAILED */
} echosdk_ev_ice_state_t;

/* ── Event payload structs ────────────────────────────────────────────────── */
/**
 * Registration state change.
 *
 * `retry_attempt` / `retry_delay_ms` are non-zero on the RECONNECTING event
 * that announces an armed retry, and render a status line directly:
 *     "reconnecting — attempt %u in %.1f s"
 * They are 0 on every other event, including the RECONNECTING that a handover
 * or a dead keepalive raises before any backoff is involved.
 *
 * Two events can describe one lost registration: the failure itself, then the
 * retry that was armed for it.  Both carry the same state, so an app that only
 * renders `state` shows "Reconnecting…" once and needs no de-duplication.
 */
typedef struct {
	echosdk_account_handle_t account;
	echosdk_reg_state_t      state;
	echosdk_error_t          error;         /* ECHOSDK_OK when REGISTERED */
	/**
	 * Which recovery attempt this is, and how long until it goes out.
	 *
	 * Both are carried on every RECONNECTING event, so an app that renders
	 * only the latest one always has a coherent pair: retry_attempt counts
	 * up 1, 2, 3… for as long as the outage lasts and retry_delay_ms is the
	 * backoff actually armed.  Exactly one RECONNECTING event is emitted per
	 * attempt — the failure, the countdown and the retry's own REGISTER are
	 * one event, not three.
	 *
	 * On a terminal FAILED, retry_attempt is how many attempts were spent
	 * and retry_delay_ms is 0: nothing further is armed.  Both are 0 on
	 * REGISTERED and on a first REGISTERING.
	 */
	uint32_t                 retry_attempt;
	uint32_t                 retry_delay_ms;
	const char              *error_str;     /* human-readable; NULL on OK */
} echosdk_ev_reg_state_t;

typedef struct {
	echosdk_account_handle_t account;
	echosdk_call_handle_t    call;
	const char              *from_uri;
	const char              *display_name;
} echosdk_ev_incoming_call_t;

typedef struct {
	echosdk_account_handle_t account;
	echosdk_call_handle_t    call;
	echosdk_call_state_t     state;
	echosdk_error_t          error;
	const char              *reason;
} echosdk_ev_call_state_t;

typedef struct {
	echosdk_call_handle_t call;
	char                  digit;
} echosdk_ev_call_dtmf_t;

typedef struct {
	echosdk_call_handle_t  call;
	const char            *local_sdp;
	const char            *remote_sdp;
	const char            *negotiated_codec;
	const char            *negotiated_crypto; /* "NONE", "SDES", "DTLS-SRTP" */
	const char * const    *rejected_codecs;   /* NULL-terminated array */
	const char * const    *warnings;          /* NULL-terminated array */
} echosdk_ev_sdp_negotiation_t;

typedef struct {
	echosdk_media_dir_t  dir;
	const char          *transport;    /* "UDP", "TCP", "TLS", "WS", "WSS" */
	const char          *remote_addr;  /* "1.2.3.4:5060" */
	const char          *raw_message;
	uint64_t             timestamp_us;
} echosdk_ev_sip_trace_t;

typedef struct {
	echosdk_call_handle_t call;

	/* ── Packet counters ───────────────────────────────────────────────── */
	uint32_t packets_sent;
	uint32_t packets_received;
	uint32_t packets_lost;      /* TX-side: packets remote did not receive
	                               (cumulative for the call, per RFC 3550) */
	uint32_t packets_lost_rx;   /* RX-side: packets we did not receive
	                               (cumulative for the call, per RFC 3550) */
	uint32_t bytes_sent;        /* total RTP bytes sent */
	uint32_t bytes_received;    /* total RTP bytes received */
	uint32_t tx_errors;         /* RTP transmit errors */
	uint32_t rx_errors;         /* RTP receive errors */

	/* ── Loss ──────────────────────────────────────────────────────────── */
	/* Rates over the last poll window (stats_interval_ms), NOT lifetime
	 * averages — a rate computed over the whole call can never recover
	 * from an early burst.  The packets_lost* counters above stay
	 * cumulative; these are the derived rates. */
	float    loss_pct;          /* TX-side loss % this window */
	float    loss_pct_rx;       /* RX-side loss % this window */

	/* ── Delay / jitter ────────────────────────────────────────────────── */
	float    jitter_ms;         /* RX interarrival jitter ms (what we observe),
	                               RFC 3550 6.4.1 */
	float    tx_jitter_ms;      /* TX interarrival jitter ms (what remote
	                               reports in its RR) */
	float    rtt_ms;            /* round-trip time ms */

	/* ── Jitter buffer ─────────────────────────────────────────────────── */
	uint32_t jitter_buffer_ms;         /* current adaptive buffer depth (ms) */
	uint32_t jitter_buffer_load;       /* packets currently held in buffer */
	uint32_t late_packets;             /* packets that arrived too late */
	uint32_t discarded_packets;        /* packets discarded (overflow / flush) */
	uint32_t jitter_buffer_target_ms;  /* JB current jitter estimate (ms) */
	bool     jitter_buffer_adaptive;   /* true when JB is in adaptive mode */

	/* ── PLC (packet loss concealment) ─────────────────────────────────── */
	uint32_t plc_frames;  /* frames lost at jitter buffer (PLC trigger count) */
	float    plc_ratio;   /* plc_frames / total_rx_frames (0.0–1.0) */

	/* ── Bandwidth ─────────────────────────────────────────────────────── */
	uint32_t bandwidth_kbps_tx;     /* current TX bitrate (kbps) */
	uint32_t bandwidth_kbps_rx;     /* current RX bitrate (kbps) */
	uint32_t avg_bandwidth_kbps_tx; /* session-average TX bitrate (kbps) */
	uint32_t avg_bandwidth_kbps_rx; /* session-average RX bitrate (kbps) */

	/* ── MOS scores — zero when RTCP not yet available ─────────────────── */
	/* LQ is listening quality: the received signal alone, no delay term.
	 * CQ is conversational quality: LQ plus the ITU-T G.107 delay
	 * impairment Id, computed from RTT/2 plus jitter buffer depth.
	 * CQ <= LQ always.  Each direction is scored from its own loss and
	 * its own jitter.  The RX scores use effective loss (network loss
	 * plus jitter buffer discards); the TX scores cannot, because the
	 * peer's jitter buffer is not visible from here. */
	float    mos_lq;            /* TX-path listening quality  (1.0–4.5) */
	float    mos_cq;            /* TX-path conversational quality (1.0–4.5) */
	float    mos_lq_rx;         /* RX-path listening quality (patient → you) */
	float    mos_cq_rx;         /* RX-path conversational quality */
	echosdk_mos_method_t mos_method;

	/* ── Codec ─────────────────────────────────────────────────────────── */
	const char *codec_name;     /* e.g. "opus", "PCMU" */
	uint32_t    codec_clock_rate; /* RTP clock rate Hz */
	uint32_t    codec_sample_rate;/* audio sample rate Hz */
	uint8_t     codec_channels; /* 1=mono, 2=stereo */
	int         payload_type;   /* RTP payload type number (0-127) */

	/* ── Audio level ───────────────────────────────────────────────────── */
	float    audio_level_dbov;  /* speaker (RX) level dBov (0=max, -127=silent);
	                               NaN when unavailable */
	float    mic_level_dbov;    /* microphone (TX) level dBov (0=max, -127=silent);
	                               NaN when unavailable */

	/* ── Stream identity ───────────────────────────────────────────────── */
	uint32_t ssrc_tx;           /* our SSRC */
	uint32_t ssrc_rx;           /* remote SSRC (0 if not yet received) */
	char     remote_addr[64];   /* remote RTP address "ip:port\0" */

	/* ── Session history — populated after first stats tick ────────────── */
	float    mos_lq_min;        /* worst mos_lq tick this call */
	float    mos_lq_avg;        /* session average mos_lq — averaged in the
	                               R-factor domain and converted once, since
	                               MOS is non-linear in R */
	uint32_t stats_tick;        /* which poll this is (1-based) */
	uint64_t call_duration_ms;  /* elapsed ms since CALL_ESTABLISHED */
	bool     is_final;          /* true on the last event before teardown */

	/* ── ICE ───────────────────────────────────────────────────────────── */
	/* The media path as the ICE agent sees it.  `ice_state` is
	 * ECHOSDK_ICE_NEW and the rest zero on a call without ICE.
	 * `remote_addr` above is the same address as `ice_remote_addr` on an
	 * ICE call, and the signalled peer on a plain-RTP one. */
	echosdk_ice_state_t     ice_state;
	echosdk_ice_cand_type_t ice_local_type;
	echosdk_ice_cand_type_t ice_remote_type;
	float    ice_rtt_ms;        /* STUN RTT on the selected pair */
	uint32_t ice_generation;    /* ICE restarts so far; 1 = initial session */
	char     ice_local_addr[64];  /* selected local candidate "ip:port" */
	char     ice_remote_addr[64]; /* selected remote candidate "ip:port" */
} echosdk_ev_media_stats_t;

typedef struct {
	const char *message;
} echosdk_ev_log_t;

typedef struct {
	const char *message;
} echosdk_ev_registrar_warning_t;

/* ── Transfer, MWI, MESSAGE, Presence payload structs ─────────────────────── */

/**
 * Incoming REFER request — blind transfer, or attended when has_replaces.
 *
 * The SDK has already answered `202 Accepted` and sent `NOTIFY 100 Trying`;
 * per RFC 3515 the transferor is now waiting for a final `message/sipfrag`
 * NOTIFY saying what became of the reference.  Nothing sends it until the app
 * calls echosdk_call_transfer_accept() or echosdk_call_transfer_reject() —
 * exactly one of the two, on the call this event names.  Ignoring the event
 * leaves the transferor waiting out the 60 s subscription.
 */
typedef struct {
	echosdk_account_handle_t account;
	echosdk_call_handle_t    call;          /* call receiving the REFER */
	const char              *refer_to_uri;  /* Refer-To header value */
	bool                     has_replaces;  /* true = attended transfer */
	/**
	 * Whether the SDK already followed this transfer by itself.
	 *
	 * Always false today — the decision is the app's, because following a
	 * transfer means placing a call and no SDK should do that unasked.
	 * Present so an app written now keeps working if a future policy knob
	 * (auto-follow for a trusted PBX, say) makes it true, rather than
	 * placing a second call on top of the SDK's.
	 */
	bool                     auto_followed;
} echosdk_ev_transfer_req_t;

/**
 * An outgoing REFER was refused (blind or attended).
 *
 * The call is still up.  baresip does not close a call whose transfer failed,
 * and neither does EchoSDK: the caller is still on the line, usually on hold,
 * and the app's remedy is to resume them and report the failure — not to hang
 * up.  `reason` is the status line or cause text when the stack supplied one.
 */
typedef struct {
	echosdk_account_handle_t account;
	echosdk_call_handle_t    call;    /* the call we tried to transfer away */
	const char              *reason;  /* status line / cause, may be NULL */
} echosdk_ev_transfer_failed_t;

/** MWI NOTIFY — raw body is parsed into counters; all fields may be 0. */
typedef struct {
	echosdk_account_handle_t account;
	bool                     messages_waiting;
	uint32_t                 new_voice;
	uint32_t                 old_voice;
	uint32_t                 new_urgent;
	uint32_t                 old_urgent;
	const char              *raw_body;      /* full NOTIFY body */
} echosdk_ev_mwi_t;

/** Incoming SIP MESSAGE (instant message). */
typedef struct {
	echosdk_account_handle_t account;
	const char              *from_uri;
	const char              *body;
	const char              *content_type;  /* e.g. "text/plain" */
} echosdk_ev_message_t;

/** Buddy / contact presence state changed. */
typedef struct {
	echosdk_account_handle_t  account;
	const char               *target_uri;
	echosdk_presence_status_t status;
} echosdk_ev_presence_state_t;

typedef enum {
	ECHOSDK_QUALITY_MOS    = 0,
	ECHOSDK_QUALITY_LOSS,
	ECHOSDK_QUALITY_JITTER,
	ECHOSDK_QUALITY_RTT,
	/** No inbound RTP for cfg.media_stall_ms while the call is not on hold.
	 *  `value` is the stall duration in ms, `threshold` is media_stall_ms.
	 *  Non-fatal: fires again with `recovering` = true when RTP resumes.
	 *  See also cfg.rtp_timeout_s, which *ends* the call instead. */
	ECHOSDK_QUALITY_MEDIA_STALL,
} echosdk_quality_issue_t;

typedef struct {
	echosdk_call_handle_t   call;
	echosdk_quality_issue_t issue;
	float                   value;       /* current metric value */
	float                   threshold;   /* threshold that was crossed */
	bool                    recovering;  /* true = value returned above threshold */
} echosdk_ev_quality_alert_t;

/* ── Master event union ───────────────────────────────────────────────────── */

typedef struct {
	echosdk_event_type_t type;
	union {
		echosdk_ev_log_t               log;
		echosdk_ev_reg_state_t         reg;
		echosdk_ev_incoming_call_t     incoming;
		echosdk_ev_call_state_t        call_state;
		echosdk_ev_call_dtmf_t         dtmf;
		echosdk_ev_sdp_negotiation_t   sdp;
		echosdk_ev_sip_trace_t         sip_trace;
		echosdk_ev_media_stats_t       stats;
		echosdk_ev_registrar_warning_t reg_warn;
		echosdk_ev_transfer_req_t      transfer_req;
		echosdk_ev_transfer_failed_t   transfer_failed;
		echosdk_ev_mwi_t               mwi;
		echosdk_ev_message_t           msg;
		echosdk_ev_presence_state_t    presence;
		echosdk_ev_quality_alert_t     quality_alert;
		echosdk_ev_network_t           network;
		echosdk_ev_ice_state_t         ice;
	} u;
} echosdk_event_t;

/* ── Callbacks ────────────────────────────────────────────────────────────── */

/**
 * Event callback — fired from EchoSDK's event dispatch thread, never from
 * re_main. Calling EchoSDK APIs from inside the callback is safe: the
 * dispatch thread exists precisely so a consumer can re-enter without
 * deadlocking the SIP loop. Keep it fast (< 10 ms) — the callback runs
 * inline with the event queue, so a slow handler backs the queue up. Hand
 * heavy work (recording, transcription, UI) to your own thread.
 */
typedef void (*echosdk_event_cb_t)(const echosdk_event_t *ev, void *userdata);

/**
 * Release an event delivered with cfg.deliver_owned_events = true.
 * Must be called exactly once per delivered event; may be called from any
 * thread, at any time after delivery.  No-op on NULL.  Passing an event
 * that was NOT delivered in owned mode is undefined behavior.
 */
ECHOSDK_EXPORT void echosdk_event_release(const echosdk_event_t *ev);

/**
 * Media tap callback — fired from the audio thread on each audio frame.
 * Must be non-blocking. Copy PCM data if you need it beyond the callback.
 */
typedef void (*echosdk_media_tap_cb_t)(
	echosdk_call_handle_t  call,
	echosdk_media_dir_t    direction,
	const int16_t         *pcm,
	size_t                 samples,
	uint32_t               sample_rate,
	uint8_t                channels,
	uint64_t               timestamp_us,
	void                  *userdata);

/* ── Config ───────────────────────────────────────────────────────────────── */

#define ECHOSDK_CONFIG_VERSION 1

typedef struct {
	/* Forward-compat guard — MUST be set by echosdk_config_init(). */
	uint32_t  version;      /* ECHOSDK_CONFIG_VERSION */
	size_t    struct_size;  /* sizeof(echosdk_config_t) at compile time */

	/* ── Transport ────────────────────────────────────────────────── */
	echosdk_transport_t  transport;
	const char          *local_ip;        /* NULL = auto */
	uint16_t             local_port;      /* 0 = OS-assigned */
	const char          *bind_interface;  /* NULL = any, e.g. "wlan0" */
	bool                 prefer_ipv6;

	const char          *sip_domain;      /* AOR domain, e.g. "pbx.example.com" */

	/**
	 * Server endpoint — two ways to specify, pick one:
	 *
	 * Simple form (UDP/TCP/TLS, default ports):
	 *   transport   = ECHOSDK_TRANSPORT_TLS
	 *   server_host = "pbx.example.com"
	 *   server_port = 0  → uses 5061 for TLS
	 *
	 * Full URL form (required for WS/WSS, custom paths):
	 *   server_url = "wss://pbx.example.com/ws"
	 *   server_url = "wss://pbx.example.com:8089/ws"
	 *   server_url = "ws://pbx.internal:8088/ws"
	 *
	 * server_url overrides transport/server_host/server_port when set.
	 */
	const char  *server_url;    /* full URL, takes precedence */
	const char  *server_host;   /* simple form */
	uint16_t     server_port;   /* 0 = transport default */

	const char  *outbound_proxy; /* NULL = auto from server info */

	/* ── TLS / WSS ────────────────────────────────────────────────── */
	/**
	 * CA bundle for server verification.  NULL falls back to the
	 * platform trust store: the concatenated system CAs on Android,
	 * the keychain anchors on macOS, the ROOT store on Windows, the
	 * distribution bundle on Linux, and — because iOS exposes no API
	 * to read its own anchors — a Mozilla root bundle shipped inside
	 * the SDK on iOS.
	 *
	 * Set it explicitly for a private or enterprise CA, and on iOS
	 * whenever the device is meant to trust an MDM-installed root:
	 * the bundle compiled into the SDK cannot see those.
	 */
	const char  *ca_cert_path;
	const char  *client_cert;
	const char  *client_key;
	bool         verify_server;
	/**
	 * SNI hostname — NULL derives from server_url host.
	 * Set explicitly when the reverse-proxy cert SNI differs from
	 * the SIP domain (e.g. proxy cert for "proxy.example.com" but
	 * SIP domain is "pbx.internal").
	 */
	const char  *sni_hostname;
	const char  *user_agent;    /* SIP User-Agent header value */

	/* ── WebSocket-specific ───────────────────────────────────────── */
	const char  *ws_origin;         /* NULL = auto-derived from server_url */
	const char **ws_extra_headers;  /* NULL-terminated "Header: value" strings */
	uint32_t     ws_keepalive_ms;   /* WS ping interval; 0 = libre default (15s); recommended 20000-30000 */

	/* ── NAT ──────────────────────────────────────────────────────── */
	const char  *stun_server;
	const char  *turn_server;
	const char  *turn_user;
	const char  *turn_pass;
	bool         ice_enabled;
	bool         rtcp_mux;    /* multiplex RTCP on RTP port (RFC 5761); default true */

	/* ── Media ────────────────────────────────────────────────────── */
	echosdk_media_enc_t  media_enc;
	/* Ordered preference list. Overridden per account by
	 * echosdk_account_config_t.audio_codec[_name]s, and by the
	 * string list in audio_codec_names[] below.
	 * Leave at 0 for the cross-platform default: Opus, PCMU, PCMA. */
	echosdk_codec_t      audio_codecs[8];
	int                  audio_codec_count;
	uint8_t              dscp_sip;  /* 0 = OS default; 24 = AF31 */
	uint8_t              dscp_rtp;  /* 0 = OS default; 46 = EF */
	bool                 enable_video; /* reserved for future video support */

	/* ── Audio processing ─────────────────────────────────────────── */
	echosdk_aec_mode_t aec_mode;  /* echo cancellation backend; default SUPPRESSOR */
	bool  ns;   /* noise suppression */
	bool  agc;  /* automatic gain control */
	float aec_suppression_level;  /* 0=no suppression .. 1=maximum; default 1.0; SUPPRESSOR only */
	float mic_gain_db;            /* TX manual gain dB, clamped [-20,+20]; 0=unity */
	float speaker_gain_db;        /* RX manual gain dB, clamped [-20,+20]; 0=unity */

	/* ── Opus tuning ──────────────────────────────────────────────── */
	echosdk_opus_config_t opus; /* all fields zero/false = use opus defaults */

	/* ── Jitter buffer ────────────────────────────────────────────── */
	echosdk_jbuf_type_t jbuf_type;          /* adaptive (default) or fixed */
	uint32_t jitter_buffer_min_ms; /* JB min depth (default: 40 ms) */
	uint32_t jitter_buffer_max_ms; /* JB max depth (default: 400 ms) */

	/* ── Registration ─────────────────────────────────────────────── */
	uint32_t  reg_expires;           /* seconds; default 3600 */
	uint32_t  reg_refresh_pct;       /* refresh at N% of expires; default 75 */
	/**
	 * SIP keepalive / reachability probe interval in ms.  0 disables.
	 * Default 30000.
	 *
	 * Every `keepalive_interval` ms of registered idle time the SDK sends an
	 * OPTIONS request to the account's proxy.  Two things depend on it:
	 *
	 *  - the UDP NAT binding is refreshed, so inbound INVITEs keep arriving
	 *    between REGISTER refreshes (a `reg_expires` of 3600 s is far longer
	 *    than a typical carrier-NAT UDP timeout of 30–180 s);
	 *  - a black-holed path is detected within one interval instead of at
	 *    the next refresh.  When `keepalive_reregister` is set, a failed
	 *    probe re-REGISTERs immediately rather than waiting.
	 *
	 * The probe is suppressed while a call is up on that account: RTP is
	 * already holding the binding open, and the request would compete with
	 * media for a congested uplink.
	 */
	uint32_t  keepalive_interval;

	/* Registration retry policy */
	uint32_t  reg_retry_initial_ms;  /* default 2000 */
	uint32_t  reg_retry_max_ms;      /* default 300000 (5 min) */
	float     reg_retry_backoff;     /* multiplier; default 2.0 */
	uint32_t  reg_retry_max_attempts;/* 0 = retry forever */
	/**
	 * Randomisation applied to each retry delay, as a fraction of it.
	 * Default 0.2; 0 disables.  The delay actually used is drawn uniformly
	 * from [d·(1−jitter), d·(1+jitter)].
	 *
	 * Without it every device that lost the same network re-REGISTERs on
	 * the same schedule and arrives at the registrar in one burst — the
	 * outage becomes a thundering herd on recovery.  Clamped to [0, 1].
	 */
	float     reg_retry_jitter;

	/* ── SIP timers (RFC 3261 §17) ─────────────────────────────────
	 *
	 * T1 and T2 govern retransmission intervals inside libre's transaction
	 * layer, where they are compile-time constants (SIP_T1 / SIP_T2).  The
	 * two fields below are reported by echosdk_config_get() for
	 * completeness and are otherwise informational — setting them has no
	 * effect.
	 */
	uint32_t  sip_t1_ms;        /* informational; libre constant, 500 */
	uint32_t  sip_t2_ms;        /* informational; libre constant, 4000 */
	/**
	 * INVITE transaction timeout, ms.  Default 32000 (RFC 3261 Timer B).
	 *
	 * An outgoing call whose INVITE is black-holed — the usual outcome of a
	 * link that is up but not passing traffic — produces no response at
	 * all, and libre's Timer B is a compile-time 64·T1 = 32 s.  This field
	 * arms an SDK-side watchdog instead: an outgoing call still in
	 * ECHOSDK_CALL_CALLING after `sip_timer_b_ms` is cancelled with 408 and
	 * reported as ECHOSDK_CALL_FAILED / ECHOSDK_ERR_TIMEOUT.  Set below
	 * 32000 to fail fast (8000–12000 is usual on mobile); 0 disables the
	 * watchdog and leaves the 32 s transaction timeout as the only bound.
	 *
	 * Only the CALLING state is watched — the state where nothing at all has
	 * come back.  Once any provisional response arrives the call moves to
	 * RINGING, the far end is demonstrably reachable, and how long the user
	 * is willing to let it ring is a product decision rather than a
	 * transport timeout.
	 */
	uint32_t  sip_timer_b_ms;
	/**
	 * Non-INVITE transaction timeout, ms.  Default 32000 (Timer F).
	 *
	 * Bounds how long the registration watchdog waits for any answer to a
	 * REGISTER before reporting ECHOSDK_ERR_TIMEOUT and handing over to the
	 * retry policy.  0 disables the watchdog.
	 */
	uint32_t  sip_timer_f_ms;

	/* ── Session timers (RFC 4028) ───────────────────────────────── */
	bool      session_timer_enabled; /* default true */
	uint32_t  session_expires_s;     /* default 1800 */
	uint32_t  session_min_se_s;      /* default 90 */

	/* ── Quality / observability ─────────────────────────────────── */
	/**
	 * ECHOSDK_EV_MEDIA_STATS poll interval in ms.  Default 2000; 0 disables.
	 *
	 * This is also the master switch for RTCP accounting
	 * (baresip `avt.rtp_stats`), so with 0 the loss/jitter/RTT/MOS fields
	 * read back as zero from echosdk_call_get_stats() too, and every
	 * feature derived from them — quality alerts, media-stall detection and
	 * adaptive bitrate — is inert.
	 */
	uint32_t              stats_interval_ms;
	echosdk_mos_method_t  mos_method;
	/* Quality alert thresholds — 0 disables each alert */
	float  mos_alert_threshold;     /* fire QUALITY_ALERT when mos_lq < this (recommended 3.5) */
	float  loss_alert_threshold;    /* fire QUALITY_ALERT when loss_pct > this (recommended 5.0) */
	float  jitter_alert_threshold;  /* fire QUALITY_ALERT when jitter_ms > this (recommended 40.0) */

	/* ── Platform ────────────────────────────────────────────────── */
	/**
	 * Writable temporary directory for internal SDK state (e.g. uuid cache).
	 * NULL = auto-detect: $TMPDIR on POSIX/iOS, GetTempPath() on Windows,
	 * /tmp on Linux.  Android callers MUST set this to the app cache dir
	 * (e.g. context.getCacheDir().getAbsolutePath()) because /tmp does not
	 * exist on Android and $TMPDIR is not reliably set from native code.
	 */
	const char *tmp_dir;

	/* ── Tracing ─────────────────────────────────────────────────── */
	bool        trace_sip;      /* emit ECHOSDK_EV_SIP_TRACE per message */
	bool        trace_sdp_diff; /* emit ECHOSDK_EV_SDP_NEGOTIATION */
	const char *pcap_path;      /* NULL = no pcap; path = live capture */

	/* ── Logging & events ────────────────────────────────────────── */
	int                 log_level;      /* 0=err, 1=warn, 2=info, 3=debug */
	echosdk_event_cb_t  event_cb;       /* required */
	void               *event_userdata;

	/* ── Network handover (Wi-Fi ↔ 4G/5G roaming) ────────────────
	 *
	 * On a network change the SDK re-binds its SIP transports,
	 * re-REGISTERs, and re-INVITEs every active call onto the new local
	 * address.  On mobile, drive this from the OS connectivity callback
	 * by calling echosdk_network_changed(); the built-in poller below is
	 * a safety net for platforms with no such callback.
	 */

	/** Interface poll interval in seconds; 0 disables polling.
	 *  Default 10.  Set 0 on mobile and call echosdk_network_changed(). */
	uint32_t  net_monitor_interval_s;

	/** Debounce window: how long the address set must stay stable before
	 *  the handover is applied.  Default 1500 ms.  Prevents thrashing
	 *  while an interface is still coming up. */
	uint32_t  net_settle_ms;

	/** Re-INVITE active calls onto the new local address. Default true. */
	bool      net_reinvite_calls;

	/** Hang up a call whose media could not be migrated instead of
	 *  leaving it up with (probably) dead audio.  Default false. */
	bool      net_hangup_on_migration_failure;

	/** How long to wait for RTP on the new path before declaring the
	 *  migration failed and retrying.  Default 4000 ms; 0 disables the
	 *  media check (the re-INVITE is then assumed to have worked). */
	uint32_t  net_verify_ms;

	/** Maximum handover / re-INVITE attempts before giving up. Default 6.
	 *
	 *  Each attempt costs up to net_verify_ms, so this is a time budget as
	 *  much as a count: the default 6 x 4000 ms bounds a migration at ~24 s.
	 *  Values above a few dozen are not useful — a path that has not come
	 *  back by then needs a new call, not more re-INVITEs. */
	uint32_t  net_max_attempts;

	/* ── Event delivery ownership ─────────────────────────────────
	 *
	 * false (default): event_cb receives a borrowed event, valid only
	 * for the duration of the callback (the historical contract).
	 *
	 * true: event_cb receives a heap-owned clone of the event; the
	 * consumer MUST call echosdk_event_release(ev) exactly once when
	 * done — which may be after the callback has returned.  Required
	 * for bindings that dispatch events asynchronously, e.g. Dart's
	 * NativeCallable.listener, where the callback body runs on the
	 * isolate's event loop after the C call has already returned. */
	bool      deliver_owned_events;

	/* ── Global audio codec list by name ─────────────────────────
	 *
	 * String form of audio_codecs[] above; takes precedence over it
	 * when audio_codec_name_count > 0.  Names are matched
	 * case-insensitively and an unrecognized name is passed through
	 * to baresip as-is, so a codec registered by a module added to the
	 * build can be selected even without a echosdk_codec_t constant.
	 * Same accepted spellings as the per-account
	 * echosdk_account_config_t.audio_codec_names: "opus", "ulaw"/"pcmu",
	 * "alaw"/"pcma".
	 *
	 * Precedence, highest first:
	 *   account audio_codec_names → account audio_codecs →
	 *   global audio_codec_names  → global audio_codecs.
	 *
	 * When all four are empty the SDK offers a fixed default that is the
	 * same on every platform: opus/48000/2, PCMU/8000/1, PCMA/8000/1.
	 *
	 * Example — prefer µ-law globally, fall back to Opus:
	 *   strcpy(cfg.audio_codec_names[0], "ulaw");
	 *   strcpy(cfg.audio_codec_names[1], "opus");
	 *   cfg.audio_codec_name_count = 2;
	 */
	char  audio_codec_names[8][32];  /* codec name strings, each ≤ 31 chars */
	int   audio_codec_name_count;    /* 0 = fall back to audio_codecs[] */

	/* ── Platform audio session activation (iOS) ──────────────────
	 *
	 * Whether echosdk_init() activates the platform audio session it
	 * configures.  Only iOS has one; every other platform ignores this.
	 *
	 * true (default): configure the AVAudioSession (PlayAndRecord +
	 * VoiceChat) and activate it during echosdk_init().  Right for an
	 * app that owns audio outright and has no CallKit.
	 *
	 * false: configure category, mode and options, but do NOT call
	 * -setActive:.  Required for CallKit apps: CXProvider owns
	 * activation and Apple requires the session be activated only from
	 * -provider:didActivateAudioSession:.  Activating anywhere else
	 * takes the exclusive PlayAndRecord route out from under CallKit —
	 * and since starting the SDK at app launch (or on a PushKit wake,
	 * while CallKit is still mid-report) is exactly "anywhere else",
	 * such an app must set this to false.  Nothing else changes: the
	 * category is still in place, so audio works as soon as CallKit
	 * activates the session.
	 */
	bool      platform_audio_activate;

	/* ── Degraded-link handling ───────────────────────────────────
	 *
	 * Handover (the net_* fields above) covers the case where the local
	 * address changes.  The fields here cover the other one: the address
	 * stays put and the link itself goes bad — a phone at one bar, a
	 * congested uplink, a cell that stops passing packets without ever
	 * dropping the PDP context.  Nothing in SIP notices that on its own.
	 */

	/**
	 * Terminate a call after this many seconds without inbound RTP.
	 * 0 (default) = never.
	 *
	 * Maps to baresip's `avt.rtp_timeout`.  This is the hard, fatal bound:
	 * the stream is closed with ETIMEDOUT and the call ends with
	 * ECHOSDK_ERR_TIMEOUT.  Only sendrecv streams are checked, so a held
	 * call is never torn down by it.
	 *
	 * Left off by default because ending a call is destructive and some
	 * deployments legitimately run one-way media.  30–60 s is the usual
	 * choice when it is wanted; prefer `media_stall_ms` for a warning that
	 * keeps the call up.
	 */
	uint32_t  rtp_timeout_s;

	/**
	 * Warn after this many ms without inbound RTP.  Default 4000; 0 = off.
	 *
	 * Non-fatal counterpart to `rtp_timeout_s`, evaluated on the stats tick:
	 * fires ECHOSDK_QUALITY_MEDIA_STALL when inbound RTP stops advancing and
	 * again with `recovering` = true when it resumes.  This is what turns
	 * "the user says they can't hear anything" into an event — a stall that
	 * is not accompanied by an address change is invisible to handover and,
	 * with rtp_timeout_s at 0, invisible to baresip.
	 *
	 * Requires stats_interval_ms > 0.  Values below one stats interval
	 * cannot be detected any sooner than that interval.
	 */
	uint32_t  media_stall_ms;

	/**
	 * Reduce the audio encoder's bitrate when the link degrades, and raise
	 * it again when the link recovers.  Default false.
	 *
	 * Evaluated on the stats tick against the remote's RTCP receiver report
	 * (`loss_pct`, i.e. what the peer is actually losing).  A step down is
	 * taken when loss exceeds `adapt_loss_down_pct`; a step up after
	 * `adapt_recover_ticks` consecutive ticks below `adapt_loss_up_pct`.
	 * Steps are halve-down / +25%-up between the bounds below.
	 *
	 * Applied through the codec's encoder-update path (no re-INVITE, no
	 * renegotiation, no audio gap), so it only does anything for codecs
	 * with a variable bitrate — Opus in practice.  A G.711 call is left
	 * alone; there is nothing to vary.  A deployment pinned to G.711 has no
	 * concealment available in this build (see the `plc` note in
	 * src/modules_init.c) — offering Opus is what makes a lossy link
	 * survivable.
	 */
	bool      adaptive_bitrate;
	uint32_t  adapt_min_bitrate;    /* bps; 0 = 12000  */
	uint32_t  adapt_max_bitrate;    /* bps; 0 = 32000  */
	float     adapt_loss_down_pct;  /* step down above this; 0 = 5.0  */
	float     adapt_loss_up_pct;    /* step up below this;   0 = 1.0  */
	uint32_t  adapt_recover_ticks;  /* clean ticks before a step up; 0 = 5 */

	/**
	 * Expected packet loss handed to the Opus encoder, percent.  0 = off.
	 *
	 * Turns on Opus in-band FEC (LBRR) at both ends — the encoder spends
	 * part of its budget on a redundant low-bitrate copy of the previous
	 * frame, and the decoder uses it to reconstruct a lost one.  Costs
	 * bitrate and quality even on a clean link, which is why it is off by
	 * default; 10–20 is a reasonable setting for mobile.
	 *
	 * `opus.fec` only enables the mechanism.  This value is what tells the
	 * encoder how much redundancy to actually spend, and the decoder to
	 * look for it, so set both.
	 */
	uint32_t  opus_expected_loss_pct;

	/**
	 * Re-REGISTER immediately when a keepalive probe fails.  Default true.
	 *
	 * A failed OPTIONS means the path to the proxy is gone even though the
	 * local address never changed — the case handover cannot see.  Without
	 * this the account stays nominally REGISTERED until the next refresh,
	 * which with the default `reg_expires` is up to an hour of missed
	 * inbound calls.  Requires keepalive_interval > 0.
	 */
	bool      keepalive_reregister;

	/**
	 * Rotate through the RFC 3263 SRV target list on registration retry.
	 * Default true.
	 *
	 * Without it every retry re-sends to the same host the last attempt
	 * timed out on, so a down primary proxy is never failed over to the
	 * secondary the SRV records exist to provide.  With it the SDK resolves
	 * _sip._<transport>.<domain> once per account and advances the outbound
	 * proxy one target per failed attempt, in (priority, weight) order,
	 * wrapping at the end.
	 *
	 * Ignored when the account or global config pins an explicit
	 * `outbound_proxy` — an operator-chosen proxy is not second-guessed —
	 * and when the server is reached by IP literal or WS/WSS URL, neither
	 * of which has SRV records to consult.
	 */
	bool      dns_srv_failover;

	/**
	 * How to treat an ICE call that could not be re-gathered on handover.
	 * Default BEST_EFFORT.  See echosdk_ice_handover_t — an ICE call is
	 * normally migrated with a full ICE restart, and this does not apply to
	 * those.
	 */
	echosdk_ice_handover_t net_ice_handover;

	/**
	 * Deadline for ICE candidate gathering on an outgoing call, ms.
	 * Default 2000; 0 disables the deadline (wait indefinitely).
	 *
	 * With a media-NAT configured, the INVITE is not sent when the call is
	 * placed — it is sent later, when the ICE stack reports that it has
	 * finished gathering candidates.  Nothing in that stack bounds how long
	 * that takes, and one path never reports at all, so an outgoing call
	 * could sit in ECHOSDK_CALL_CALLING forever with no SIP message ever
	 * reaching the wire and no event to say so.
	 *
	 * This is the bound.  When it expires the offer is released with
	 * whatever candidates were gathered by then, exactly as a browser-based
	 * SIP client does (JsSIP/SIP.js/dart-sip-ua all cap the same wait, at
	 * 0.5–5 s) and as pjsua's PJSUA_ICE_TRANSPORT_INIT_TIMEOUT does.  A
	 * degraded candidate set makes for a degraded call; a call that is
	 * never offered is no call at all.
	 *
	 * Gathering is not cancelled — if it completes afterwards the fuller
	 * set is offered again in a re-INVITE.  A *failure* arriving after the
	 * deadline is dropped rather than ending the call, since by then the
	 * offer is already on the wire.
	 *
	 * The same bound applies to the re-gather of an ICE restart on network
	 * handover, where it decides how long a migrating call stays without
	 * audio before an offer goes out.  There, 0 does *not* mean "wait
	 * forever" — the call is already live and silent, so a 3 s default
	 * applies instead.
	 *
	 * Set 0 only if a slow TURN allocation must never be pre-empted on dial
	 * and an unbounded dial is acceptable.
	 */
	uint32_t  ice_gathering_timeout_ms;

} echosdk_config_t;

/* ── Account config ───────────────────────────────────────────────────────── */

typedef struct {
	/* ── Required ─────────────────────────────────────────────────────── */

	/**
	 * Login URI — who you are and where the server is.
	 * Accepted forms:
	 *   "user@host"            →  AOR sip:user@host, server = host:default_port
	 *   "user@host:port"       →  AOR sip:user@host, server = host:port
	 *   "sip:user@host"        →  same as "user@host"
	 * Required.
	 */
	const char          *uri;

	const char          *password;    /* digest auth password — required */

	/* ── Transport & server ────────────────────────────────────────────── */

	/**
	 * SIP transport protocol.  Defaults to ECHOSDK_TRANSPORT_UDP (0).
	 * Ignored when server_url is set (transport is derived from the URL scheme).
	 */
	echosdk_transport_t  transport;

	/**
	 * Override the server address.  NULL = use the host:port from uri.
	 * Useful when the SIP domain differs from the physical server address.
	 */
	const char          *server_host;  /* NULL = host from uri */
	uint16_t             server_port;  /* 0    = port from uri, or transport default */

	/**
	 * Full server URL for WebSocket transports or non-standard paths.
	 *   "wss://pbx.example.com:443/ws"
	 *   "ws://pbx.internal:8088/"
	 * When set, overrides transport, server_host, and server_port.
	 */
	const char          *server_url;

	/* ── Identity ──────────────────────────────────────────────────────── */

	const char          *auth_user;    /* NULL = user part of uri */
	const char          *display_name; /* NULL = omit */

	/* ── Media / NAT ──────────────────────────────────────────────────── */

	echosdk_media_enc_t  media_enc;   /* ECHOSDK_MEDIA_ENC_NONE / SDES / DTLS_SRTP */
	bool                 ice_enabled; /* false by default */
	bool                 rtcp_mux;     /* used when rtcp_mux_set is true */
	bool                 rtcp_mux_set; /* false = inherit global; true = use rtcp_mux above */
	const char          *stun_server; /* NULL = no STUN, e.g. "stun:stun.l.google.com:19302" */
	const char          *turn_server; /* NULL = no TURN, e.g. "turn:turn.example.com:3478" */
	const char          *turn_user;
	const char          *turn_pass;

	/* ── Advanced overrides ────────────────────────────────────────────── */

	const char          *outbound;   /* NULL = auto-derived from server */
	const char          *outbound_proxy; /* alias for outbound; NULL = auto */
	bool                 verify_tls; /* false = skip TLS cert verification */

	/* ── Push notifications ─────────────────────────────────────────────── */

	/**
	 * Push notification provider.  NONE (0) disables push params (default).
	 *
	 * When set, pn-provider/pn-prid/pn-param URI parameters are added to
	 * the Contact header of every REGISTER per RFC 8599.  The server reads
	 * these and sends an APNs/FCM push when a SIP INVITE arrives for an
	 * offline device.
	 *
	 * The host app is responsible for obtaining the OS push token
	 * (PKPushRegistry on iOS, FirebaseMessaging on Android) and passing it
	 * here before calling echosdk_account_register().
	 */
	echosdk_push_provider_t  push_provider;

	/**
	 * Device push token string.
	 * APNs: hex-encoded device token (128 hex chars, 64 bytes).
	 * FCM:  registration token (~152 chars).
	 * NULL or empty → push params omitted from Contact URI.
	 */
	const char              *push_token;

	/**
	 * Provider-specific parameter (pn-param).
	 * APNs: app bundle ID, e.g. "com.example.MyApp".
	 * FCM:  app package name, e.g. "com.example.myapp".
	 * NULL → pn-param omitted.
	 */
	const char              *push_param;

	/* ── Audio codec override ──────────────────────────────────────────────── */

	/**
	 * Per-account audio codec preference list (ordered, highest priority first).
	 * When audio_codec_count > 0, this list overrides the global cfg.audio_codecs.
	 * When audio_codec_count == 0 (default), the global list is used.
	 *
	 * Example — prefer PCMU, fall back to Opus:
	 *   cfg.audio_codecs[0]   = ECHOSDK_CODEC_PCMU;
	 *   cfg.audio_codecs[1]   = ECHOSDK_CODEC_OPUS;
	 *   cfg.audio_codec_count = 2;
	 */
	echosdk_codec_t  audio_codecs[8];
	int              audio_codec_count; /* 0 = use global cfg codecs */

	/**
	 * String-based codec list — takes precedence over audio_codecs[] when
	 * audio_codec_name_count > 0.  Names are matched case-insensitively.
	 *
	 * Accepted values (aliases accepted) — the full set of compiled-in
	 * codecs, identical on desktop and mobile:
	 *   "opus"              — Opus 48 kHz stereo
	 *   "ulaw" / "pcmu"     — G.711 µ-law
	 *   "alaw" / "pcma"     — G.711 A-law
	 *
	 * Any other name is passed through to baresip unchanged, so a codec
	 * registered by a module added to the build can be selected too.  A name
	 * nothing registers is dropped from the offer with a warning.
	 *
	 * Example — prefer µ-law, fall back to Opus:
	 *   strcpy(cfg.audio_codec_names[0], "ulaw");
	 *   strcpy(cfg.audio_codec_names[1], "opus");
	 *   cfg.audio_codec_name_count = 2;
	 */
	char  audio_codec_names[8][32];  /* codec name strings, each ≤ 31 chars */
	int   audio_codec_name_count;    /* 0 = fall back to audio_codecs[] / global */

	/* ── DTMF ──────────────────────────────────────────────────────────── */
	echosdk_dtmf_mode_t dtmf_mode; /* default ECHOSDK_DTMF_RFC4733 (0) */

} echosdk_account_config_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/** Zero-fill cfg and set version/struct_size correctly. Call before populating. */
ECHOSDK_EXPORT void echosdk_config_init(echosdk_config_t *cfg);

/**
 * Initialize the EchoSDK stack. Call once per process before any other API.
 * Spawns re_main thread and event dispatch thread internally.
 * Returns ECHOSDK_OK or a negative ECHOSDK_ERR_* code.
 */
ECHOSDK_EXPORT int echosdk_init(const echosdk_config_t *cfg);

/**
 * True while the stack is initialized (between a successful echosdk_init()
 * and echosdk_shutdown()).  Thread-safe.
 *
 * The stack lives in the process, not in the caller's runtime.  A host that
 * can lose and rebuild its own runtime while the process stays alive — an
 * Android headless Flutter engine tearing down the Dart isolate between push
 * wakeups, a re-loaded plugin/scripting VM — comes back to a stack that is
 * still up and still registered.  Calling echosdk_init() again then returns
 * ECHOSDK_ERR_ALREADY; the correct recovery is to re-point the event sink at
 * the new runtime with echosdk_set_event_handler() and re-discover the live
 * accounts and calls with echosdk_account_foreach() / echosdk_call_foreach().
 */
ECHOSDK_EXPORT bool echosdk_is_initialized(void);

/**
 * Re-point the event sink at a new callback on an already-initialized stack.
 *
 * Use when the process outlives the consumer that installed cfg.event_cb (see
 * echosdk_is_initialized()).  Once this returns the old callback is neither
 * running nor reachable — a delivery already inside it is waited out — so the
 * caller may free it immediately (close a Dart NativeCallable, unload a
 * plugin).  Calling from inside the event callback skips that wait and returns
 * without blocking, since the delivery being waited for would be the caller.
 * A callback that blocks forever blocks this call with it.
 *
 * Events are NOT buffered across the gap — whatever fired while the old
 * consumer was gone is dropped, including an INVITE that arrived during the
 * gap.  Recover the resulting state, not the missed events: the call itself is
 * still live in the stack, so a reattaching consumer enumerates
 * echosdk_call_foreach() and reads echosdk_call_get_state() to find a call
 * still RINGING, and echosdk_account_foreach() /
 * echosdk_account_get_reg_state() for registration state.
 *
 * @param cb                    New callback; NULL parks delivery (events keep
 *                              being dequeued and dropped, never queued up)
 * @param userdata              Passed to cb as-is
 * @param deliver_owned_events  Ownership mode for the new callback — same
 *                              contract as cfg.deliver_owned_events.  A
 *                              handler installed with true MUST release every
 *                              event it receives.
 * @return ECHOSDK_OK, or ECHOSDK_ERR_STATE if the stack is not initialized
 */
ECHOSDK_EXPORT int echosdk_set_event_handler(echosdk_event_cb_t cb,
                                             void *userdata,
                                             bool deliver_owned_events);

/**
 * Tear down the stack. Blocks until all internal threads have exited.
 * All active accounts and calls are forcibly terminated first.
 */
ECHOSDK_EXPORT void echosdk_shutdown(void);

/* ── Accounts ─────────────────────────────────────────────────────────────── */

/**
 * Create a SIP account. Does NOT register — call echosdk_account_register().
 * Thread-safe; may be called from any thread after echosdk_init().
 */
ECHOSDK_EXPORT int echosdk_account_create(const echosdk_account_config_t *cfg,
                             echosdk_account_handle_t *out);

/** Destroy account and all associated calls. Unregisters first if registered. */
ECHOSDK_EXPORT void echosdk_account_destroy(echosdk_account_handle_t acct);

/** Begin registration. Fires ECHOSDK_EV_REG_STATE events on state changes.
 *  REGISTERING → REGISTERED on success; a failure the SDK will retry reports
 *  ECHOSDK_REG_RECONNECTING, and only a failure it has given up on reports
 *  ECHOSDK_REG_FAILED. */
ECHOSDK_EXPORT int echosdk_account_register(echosdk_account_handle_t acct);

/** Unregister (sends REGISTER with Expires: 0). */
ECHOSDK_EXPORT int echosdk_account_unregister(echosdk_account_handle_t acct);

/**
 * Override the retry policy for this account. Takes effect on the next retry.
 * Overrides the global reg_retry_* fields in echosdk_config_t for this account only.
 * @param initial_ms   First retry delay in ms (e.g. 2000)
 * @param max_ms       Maximum retry delay cap in ms (e.g. 300000)
 * @param backoff      Delay multiplier per attempt (e.g. 2.0)
 * @param max_attempts Max attempts before giving up; 0 = retry forever
 */
ECHOSDK_EXPORT int echosdk_account_set_retry_policy(echosdk_account_handle_t acct,
                                                     uint32_t initial_ms,
                                                     uint32_t max_ms,
                                                     float    backoff,
                                                     uint32_t max_attempts);

/** Cancel a pending retry timer and reset the attempt counter.
 *  The app has taken over, so the recovery stops being the SDK's: an account
 *  that was RECONNECTING reports FAILED once the retry is cancelled — leaving
 *  it on "Reconnecting…" would promise an attempt that is never coming.
 *  Call echosdk_account_register() to restart. */
ECHOSDK_EXPORT int echosdk_account_cancel_retry(echosdk_account_handle_t acct);

/** Skip the current backoff delay and re-register immediately.
 *  Resets the attempt counter. No-op if the account is not in a retry loop. */
ECHOSDK_EXPORT int echosdk_account_retry_now(echosdk_account_handle_t acct);

/**
 * Update the push token for an account at runtime (e.g. on OS token rotation).
 *
 * The new token is stored and the Contact URI params are updated.  A new
 * REGISTER is sent immediately unless the account is mid-call, mid-transaction,
 * or in retry backoff — in those cases the update is deferred to the next
 * natural re-registration.  Callers that need the token applied before the
 * next call should wait for ECHOSDK_EV_CALL_STATE ENDED first.
 *
 * Pass NULL to clear push params and re-register without pn-* Contact params.
 *
 * @param acct        Account handle (must have push_provider set in config)
 * @param push_token  New device token string, or NULL to clear
 * @return ECHOSDK_OK on success, ECHOSDK_ERR_STATE if account has no UA yet
 */
ECHOSDK_EXPORT int echosdk_account_set_push_token(echosdk_account_handle_t acct,
                                                   const char *push_token);

/**
 * Add a custom SIP header that is sent on REGISTER requests only.
 *
 * Use this for vendor push schemes that use non-standard headers (e.g.
 * "X-Push-Token", "X-Apple-Push-Bundle-Id") on hosted servers where you
 * cannot configure server-side push dispatch.  Unlike
 * echosdk_account_add_header(), this header is NOT included on INVITE,
 * BYE, or REFER — the token is not leaked to call peers.
 *
 * Multiple calls accumulate headers.  The list is append-only for the
 * lifetime of the account; recreate the account to clear it.
 *
 * @param name   Header field name  (e.g. "X-Push-Token")
 * @param value  Header field value (e.g. "a1b2c3...")
 */
ECHOSDK_EXPORT int echosdk_account_add_register_header(
        echosdk_account_handle_t acct,
        const char *name, const char *value);

/**
 * Add a custom SIP header to all outgoing requests for this account.
 * @param name   Header field name  (e.g. "X-Tenant-Id")
 * @param value  Header field value (e.g. "12345")
 */
ECHOSDK_EXPORT int echosdk_account_add_header(echosdk_account_handle_t acct,
                                const char *name, const char *value);

/**
 * Subscribe to presence state changes for a contact (SUBSCRIBE/NOTIFY).
 * Presence updates are delivered via ECHOSDK_EV_PRESENCE_STATE events.
 * @param target_uri  SIP URI of the contact to subscribe to
 * @return ECHOSDK_OK on success, or negative error code
 */
ECHOSDK_EXPORT int echosdk_account_subscribe_presence(echosdk_account_handle_t acct,
                                        const char *target_uri);

/**
 * Unsubscribe from presence state changes for a contact.
 * @param target_uri  SIP URI of the contact to unsubscribe from
 * @return ECHOSDK_OK on success, or negative error code
 */
ECHOSDK_EXPORT int echosdk_account_unsubscribe_presence(echosdk_account_handle_t acct,
                                          const char *target_uri);

/** Called once per live account by echosdk_account_foreach(). */
typedef void (*echosdk_account_iter_fn)(echosdk_account_handle_t acct, void *arg);

/**
 * Iterate all live accounts. Safe to call from any thread.
 * Do not create or destroy accounts from inside fn.
 *
 * Together with echosdk_account_get_aor() this lets a consumer that lost its
 * own handles — a rebuilt runtime attaching to a stack that is still up, see
 * echosdk_is_initialized() — re-derive them instead of creating duplicates.
 */
ECHOSDK_EXPORT void echosdk_account_foreach(echosdk_account_iter_fn fn, void *arg);

/**
 * Write the account's AOR ("sip:user@host") into buf as a NUL-terminated
 * string.  Identifies an account handle obtained from
 * echosdk_account_foreach().
 * @return ECHOSDK_OK, ECHOSDK_ERR_INVAL on bad args, or ECHOSDK_ERR_NOMEM
 *         if buf is too small (buf is left empty).
 */
ECHOSDK_EXPORT int echosdk_account_get_aor(echosdk_account_handle_t acct,
                                            char *buf, size_t sz);

/** Current registration state, as last reported by ECHOSDK_EV_REG_STATE.
 *  Useful on a cold start or after re-attaching, where no event has been seen
 *  yet.  Can return ECHOSDK_REG_RECONNECTING — see the enum. */
ECHOSDK_EXPORT echosdk_reg_state_t echosdk_account_get_reg_state(
        echosdk_account_handle_t acct);

/* ── Calls ────────────────────────────────────────────────────────────────── */

/**
 * Initiate an outgoing call. Returns immediately; fires ECHOSDK_EV_CALL_STATE
 * CALLING, then RINGING, then ESTABLISHED (or FAILED).
 */
ECHOSDK_EXPORT int echosdk_call_invite(echosdk_account_handle_t acct,
                         const char *uri,
                         echosdk_call_handle_t *out);

/** Answer an incoming call (received via ECHOSDK_EV_INCOMING_CALL). */
ECHOSDK_EXPORT int echosdk_call_answer(echosdk_call_handle_t call);

/** Terminate a call with BYE. */
ECHOSDK_EXPORT int echosdk_call_hangup(echosdk_call_handle_t call);

/**
 * Terminate a call with an explicit SIP status code.
 * For an unanswered incoming call this sends the given final response
 * (486 "Busy Here", 603 "Decline", ...); for an established call the
 * dialog is ended as usual (the code applies where the SIP state allows).
 * @param scode   SIP status code, e.g. 486 or 603. 0 = default behavior.
 * @param reason  Reason phrase; NULL = derived from scode.
 */
ECHOSDK_EXPORT int echosdk_call_reject(echosdk_call_handle_t call,
                                        uint16_t scode, const char *reason);

/** Put call on hold (re-INVITE with sendonly). */
ECHOSDK_EXPORT int echosdk_call_hold(echosdk_call_handle_t call);

/** Resume a held call (re-INVITE with sendrecv). */
ECHOSDK_EXPORT int echosdk_call_resume(echosdk_call_handle_t call);

/** Return true if the call is currently on local hold. */
ECHOSDK_EXPORT bool echosdk_call_is_held(echosdk_call_handle_t call);

/** Send DTMF digit via RFC 4733 RTP events. digit: '0'-'9', '*', '#', 'A'-'D'. */
ECHOSDK_EXPORT int echosdk_call_send_dtmf(echosdk_call_handle_t call, char digit);

/** Blind transfer via REFER. */
ECHOSDK_EXPORT int echosdk_call_transfer(echosdk_call_handle_t call, const char *uri);

/**
 * Add a custom SIP header to a specific call/dialog.
 * Headers are applied to subsequent re-INVITEs, BYE, and REFER messages
 * within this dialog. Must be called while the call is active.
 * @param name   Header field name  (e.g. "X-Call-Id")
 * @param value  Header field value (e.g. "12345")
 */
ECHOSDK_EXPORT int echosdk_call_add_header(echosdk_call_handle_t call,
                             const char *name, const char *value);

/**
 * Attended transfer: send REFER w/ Replaces on call_a, bridging it to call_b.
 * call_a is the call to transfer away; call_b is the already-established
 * consultation call whose dialog info is embedded in Replaces.
 */
/**
 * Follow an incoming REFER (see ECHOSDK_EV_TRANSFER_REQUEST).
 *
 * Places the call to the Refer-To target and links it to `call`, so the SDK
 * reports the outcome to the transferor automatically: `NOTIFY 200 OK` when
 * the new call is established, or the failure status if it is not.
 *
 * Do NOT implement this by hanging up and dialling. That breaks the REFER
 * subscription the transferor is waiting on, and it never learns the transfer
 * worked.
 *
 * The original call is left up; end it when the new one connects, or keep both
 * and let the user choose. On success `*out` receives the new call handle.
 *
 * @return ECHOSDK_OK, ECHOSDK_ERR_INVAL for a NULL handle,
 *         ECHOSDK_ERR_STATE when no transfer is pending on this call, or a
 *         positive errno if the call could not be placed (the transferor is
 *         sent the failure NOTIFY in that case).
 */
ECHOSDK_EXPORT int echosdk_call_transfer_accept(echosdk_call_handle_t  call,
                                                 echosdk_call_handle_t *out);

/**
 * Refuse an incoming REFER (see ECHOSDK_EV_TRANSFER_REQUEST).
 *
 * Sends the terminating NOTIFY so the transferor stops waiting, and leaves
 * this call established — the user is still on the line.
 *
 * @param scode   SIP status to report, 400-699. 603 Decline is the usual
 *                answer for "the user said no"; 486 for "busy".
 * @param reason  Reason phrase, or NULL for "Declined".
 *
 * @return ECHOSDK_OK, ECHOSDK_ERR_INVAL for a NULL handle or an out-of-range
 *         status, or ECHOSDK_ERR_STATE when no transfer is pending.
 */
ECHOSDK_EXPORT int echosdk_call_transfer_reject(echosdk_call_handle_t call,
                                                 uint16_t scode,
                                                 const char *reason);

ECHOSDK_EXPORT int echosdk_call_attended_transfer(echosdk_call_handle_t call_a,
                                    echosdk_call_handle_t call_b);

/** Called once per active call by echosdk_call_foreach(). */
typedef void (*echosdk_call_iter_fn)(echosdk_call_handle_t call, void *arg);

/** Iterate all active calls. Safe to call from any thread. */
ECHOSDK_EXPORT void echosdk_call_foreach(echosdk_call_iter_fn fn, void *arg);

/** The account this call belongs to, or NULL if it has none. */
ECHOSDK_EXPORT echosdk_account_handle_t echosdk_call_get_account(
        echosdk_call_handle_t call);

/** Current call state. Returns ECHOSDK_CALL_ENDED for a NULL handle. */
ECHOSDK_EXPORT echosdk_call_state_t echosdk_call_get_state(
        echosdk_call_handle_t call);

/**
 * The call's current ICE liveness state (see echosdk_ice_state_t).  Safe from
 * any thread; ECHOSDK_ICE_NEW for a call without ICE or before gathering.
 */
ECHOSDK_EXPORT echosdk_ice_state_t echosdk_call_ice_state(
        echosdk_call_handle_t call);

/* ── SIP MESSAGE ─────────────────────────────────────────────────────────── */

/**
 * Send a SIP MESSAGE (instant message) out of dialog.
 * content_type defaults to "text/plain" if NULL.
 * Fires ECHOSDK_EV_MESSAGE on the remote end when received.
 */
ECHOSDK_EXPORT int echosdk_message_send(echosdk_account_handle_t account,
                          const char *to_uri,
                          const char *body,
                          const char *content_type);

/* ── Presence ────────────────────────────────────────────────────────────── */

/**
 * Publish presence status for the account (PUBLISH request).
 * Fires ECHOSDK_EV_PRESENCE_STATE on subscribed watchers.
 */
ECHOSDK_EXPORT int echosdk_account_publish_presence(echosdk_account_handle_t account,
                                      echosdk_presence_status_t status);

/* ── 100rel / PRACK ──────────────────────────────────────────────────────── */

/**
 * Set the RFC 3262 100rel mode for an account.
 * Must be called before echosdk_call_invite / echosdk_call_answer.
 */
ECHOSDK_EXPORT int echosdk_account_set_100rel(echosdk_account_handle_t account,
                                echosdk_100rel_mode_t mode);

/* ── Audio ────────────────────────────────────────────────────────────────── */

/* ── App-owned audio device ───────────────────────────────────────────────── */

/**
 * Hand the microphone and speaker to the app.
 *
 * With this on the SDK opens no capture or playback device of its own — no
 * OpenSL ES on Android, no AudioUnit on iOS — and the app supplies and consumes
 * PCM itself, from whatever the platform gives it (AudioRecord/AudioTrack,
 * AVAudioEngine, a WebRTC AudioDeviceModule, a file):
 *
 *   app capture thread   -> echosdk_audio_external_push()  -> far end
 *   app playback thread  <- echosdk_audio_external_pull()  <- far end
 *
 * Turning it off restores the platform device.  Takes effect immediately,
 * including on calls that are already up, so it is safe to switch mid-call.
 *
 * Not sticky across a restart: echosdk_init() re-derives the device from the
 * platform, so an app that shuts the stack down and brings it back up has to
 * ask for the app-owned device again.
 *
 * One microphone, one speaker: baresip opens a device per call, but the app
 * has only one of each.  The most recently opened call owns them; a second
 * concurrent call gets silence rather than a share.
 *
 * Note this replaces the device only.  Echo cancellation follows the device:
 * the platform cancellers the SDK relies on (Android's VOICE_COMMUNICATION
 * capture preset, iOS's VoiceProcessingIO) belong to the drivers being
 * displaced, so an app taking this over owns AEC too — capture through
 * VOICE_COMMUNICATION / VoiceProcessingIO on its own side, or expect echo.
 * The SDK's own half-duplex suppressor becomes available as a fallback while
 * the app owns the device, but stays off unless asked for with
 * echosdk_set_aec_mode(ECHOSDK_AEC_SUPPRESSOR); it is switched off again when
 * the platform device comes back, so the two cancellers never stack.
 *
 * Returns 0, or an errno on failure.
 */
ECHOSDK_EXPORT int echosdk_audio_use_external(bool enable);

/**
 * Give the stack captured microphone audio.  This is what the far end hears.
 *
 * [pcm] is S16LE interleaved, [nsamp] total samples (frames x channels), at the
 * rate and channel count from echosdk_audio_external_format().  Any buffer size
 * is accepted; the stack re-frames internally.
 *
 * Call from the app's capture thread.  Returns 0, ENODEV when no call is
 * capturing (push between calls is not an error worth acting on), or an errno.
 */
ECHOSDK_EXPORT int echosdk_audio_external_push(const int16_t *pcm, size_t nsamp);

/**
 * Take decoded audio to play.  This is what the local user hears.
 *
 * Fills [pcm] with [nsamp] S16LE interleaved samples, always completely:
 * silence when no call is up, so the app can hand the buffer straight to the
 * speaker without checking.
 *
 * Call from the app's playback thread.  Returns 0, or ENODEV when no call is
 * playing (the buffer is still zeroed).
 */
ECHOSDK_EXPORT int echosdk_audio_external_pull(int16_t *pcm, size_t nsamp);

/**
 * The format the current call negotiated, for sizing the app's own device.
 * Any of the out-params may be NULL.  Returns 0, or ENODEV when no call has
 * media yet — poll after the call is established, or just re-check on each
 * call, since a different codec can mean a different rate.
 *
 * There is no event for "media is up": CALL_ESTABLISHED is a SIP state and
 * races the device by a few ms, and a mid-call re-INVITE can renegotiate the
 * codec without any call-state change at all.  Polling this is the way to
 * learn both that the device opened and that its format changed.
 */
ECHOSDK_EXPORT int echosdk_audio_external_format(uint32_t *srate, uint8_t *ch,
                                                  uint32_t *ptime);

/** True while a call is capturing or playing through the app-owned device. */
ECHOSDK_EXPORT bool echosdk_audio_external_is_active(void);

/* ── Audio device enumeration ─────────────────────────────────────────────── */

typedef struct {
	char name[128];         /* device name passed to set_input/output_device */
	char description[256];  /* human-readable label (may be empty) */
	bool is_default;        /* true for the platform default device */
} echosdk_audio_device_t;

/**
 * Fill devices[] with available audio input devices, up to max_count entries.
 * Returns the number of entries written (0 if no devices found yet).
 * Call after echosdk_init(); device lists may be empty until the audio
 * module has finished async enumeration.
 */
ECHOSDK_EXPORT int echosdk_audio_list_input_devices(echosdk_audio_device_t *devices,
                                                     int max_count);

/** Same for audio output (speaker/playback) devices. */
ECHOSDK_EXPORT int echosdk_audio_list_output_devices(echosdk_audio_device_t *devices,
                                                      int max_count);

/* ── Audio processing — runtime toggles ──────────────────────────────────── */

/**
 * Enable or disable echo cancellation (takes effect on next audio frame).
 * Re-enables the aec_mode that was configured at echosdk_init().
 * set_aec(false) → AEC_OFF; set_aec(true) → restores init mode.
 * Back-compat: behavior is identical to the former bool aec API.
 */
ECHOSDK_EXPORT void echosdk_set_aec(bool enable);

/**
 * Switch the AEC backend.  Only AEC_OFF ↔ init_mode transitions are valid
 * at runtime — module_load is one-way.  Switching between SUPPRESSOR and
 * WEBRTC at runtime returns EINVAL.  WEBRTC returns ENOTSUP on mobile builds
 * and when ECHOSDK_WITH_WEBRTC_AEC was not set at build time.
 * Requires bsdk_dispatch_sync — NOT safe to call from the audio thread.
 */
ECHOSDK_EXPORT int echosdk_set_aec_mode(echosdk_aec_mode_t mode);

/**
 * Tune the built-in TX echo suppressor aggressiveness.  SUPPRESSOR mode only.
 * 0.0 = no TX suppression (passes through even when far end is loud).
 * 1.0 = maximum suppression (default — −16.5 dB floor on TX when RX active).
 * Takes effect on the next audio frame via atomic store; safe from any thread.
 */
ECHOSDK_EXPORT void echosdk_set_aec_suppression_level(float level);

/** Enable/disable noise suppression globally (takes effect next frame). */
ECHOSDK_EXPORT void echosdk_set_ns(bool enable);

/** Enable/disable automatic gain control globally (takes effect next frame). */
ECHOSDK_EXPORT void echosdk_set_agc(bool enable);

/**
 * Set manual microphone (TX) gain in dB.  Clamped to [-20, +20].
 * 0.0 = unity — fast-path bypass, no per-sample work.
 * Applied before NS/AGC/AEC in the encode chain (raw pre-boost).
 * Takes effect on the next audio frame via atomic store; safe from any thread.
 */
ECHOSDK_EXPORT void echosdk_set_mic_gain_db(float db);

/**
 * Set manual speaker (RX) gain in dB.  Clamped to [-20, +20].
 * 0.0 = unity — fast-path bypass.
 * Applied after the jitter buffer, before playback.
 * Takes effect on the next audio frame via atomic store; safe from any thread.
 */
ECHOSDK_EXPORT void echosdk_set_speaker_gain_db(float db);

/**
 * Change DSCP/TOS on the RTP socket of an active call.
 * Common values: 46 (EF — voice), 34 (AF41 — video), 0 (best-effort).
 * Takes effect immediately on the next outgoing RTP packet.
 */
ECHOSDK_EXPORT int echosdk_call_set_dscp_rtp(echosdk_call_handle_t call,
                                              uint8_t dscp);

/**
 * Update jitter buffer bounds.  Takes effect on calls established after
 * this call — existing streams keep their current adaptive depth.
 * Pass 0 for either bound to restore the baresip default (~0 / 150 ms).
 */
ECHOSDK_EXPORT void echosdk_set_jitter_buffer(uint32_t min_ms, uint32_t max_ms);

/** Set jitter buffer type (adaptive or fixed). Takes effect on new calls. */
ECHOSDK_EXPORT void echosdk_set_jitter_buffer_type(echosdk_jbuf_type_t type);

/* ── Degraded-link control ───────────────────────────────────────────────── */

/**
 * Set the no-inbound-RTP timeout for one established call, in seconds.
 *
 * Per-call override of cfg.rtp_timeout_s.  0 disables the timeout for this
 * call.  Takes effect immediately on every stream of the call.
 *
 * Useful where the policy is per-call rather than global: an attended
 * transfer or a call parked against music-on-hold may legitimately go quiet,
 * while a normal two-party call should not.
 *
 * @return 0, or ECHOSDK_ERR_INVAL / ECHOSDK_ERR_STATE.
 */
ECHOSDK_EXPORT int echosdk_call_set_rtp_timeout(echosdk_call_handle_t call,
                                                 uint32_t seconds);

/**
 * Set the audio encoder bitrate for one call, in bits/s.
 *
 * Goes through the codec's encoder-update path: no re-INVITE and no audio
 * gap, but also no effect for a fixed-rate codec such as G.711.  Pass 0 to
 * return the encoder to its negotiated automatic rate.
 *
 * Calling this on a call that the adaptive controller is managing is
 * honoured, and then overridden by the controller on its next decision — set
 * cfg.adaptive_bitrate to false, or use echosdk_set_adaptive_bitrate(), if
 * the app wants to own the rate.
 *
 * @return 0, or ECHOSDK_ERR_INVAL / ECHOSDK_ERR_STATE.
 */
ECHOSDK_EXPORT int echosdk_call_set_bitrate(echosdk_call_handle_t call,
                                             uint32_t bitrate_bps);

/**
 * Enable or disable link-adaptive bitrate at runtime, with bounds.
 *
 * Pass 0 for either bound to keep the value already configured.  Disabling
 * leaves every call at whatever rate it currently has; call
 * echosdk_call_set_bitrate(call, 0) to restore the negotiated rate.
 */
ECHOSDK_EXPORT void echosdk_set_adaptive_bitrate(bool enabled,
                                                  uint32_t min_bps,
                                                  uint32_t max_bps);

/**
 * Send a keepalive/reachability probe (SIP OPTIONS) for one account now.
 *
 * The same probe the keepalive_interval timer sends, on demand — useful from
 * an app-foreground or push-wake handler, where the question "is my
 * registration still reachable?" is worth answering before the user places a
 * call.  The result arrives as ECHOSDK_EV_REG_STATE: nothing changes on
 * success, and on failure the account goes to FAILED and (when
 * cfg.keepalive_reregister is set) re-REGISTERs.
 *
 * @return 0, or ECHOSDK_ERR_INVAL / ECHOSDK_ERR_STATE.
 */
ECHOSDK_EXPORT int echosdk_account_keepalive_now(
                                        echosdk_account_handle_t account);

/* ── Audio mute / device control ─────────────────────────────────────────── */

/** Mute/unmute the microphone (TX path) for a call. */
ECHOSDK_EXPORT int echosdk_audio_mute(echosdk_call_handle_t call, bool mute);

/** Return true if TX audio is currently muted on this call. */
ECHOSDK_EXPORT bool echosdk_audio_is_muted(echosdk_call_handle_t call);

/** Mute/unmute the speaker (RX path) for a call — silences incoming audio. */
ECHOSDK_EXPORT int echosdk_audio_mute_rx(echosdk_call_handle_t call, bool mute);

/** Set the system audio input device by name. NULL = platform default. */
ECHOSDK_EXPORT int echosdk_audio_set_input_device(const char *name);

/** Set the system audio output device by name. NULL = platform default. */
ECHOSDK_EXPORT int echosdk_audio_set_output_device(const char *name);

/* ── Media tap ────────────────────────────────────────────────────────────── */

/**
 * Install a PCM tap on an active call.
 * cb is called from the audio thread for both RX (decoded) and TX (pre-encode)
 * directions. Must be non-blocking — heavy work must go to your own thread.
 * Pass NULL cb to remove the tap.
 */
ECHOSDK_EXPORT int echosdk_call_set_media_tap(echosdk_call_handle_t   call,
                                echosdk_media_tap_cb_t  cb,
                                void                   *userdata);

/* ── Audio recording ──────────────────────────────────────────────────────── */

/**
 * Start recording call audio to a single mixed WAV file (PCM S16LE).
 * Both the received (RX) and sent (TX) audio are clip-summed into one stream.
 * The WAV header is written on the first audio frame and finalized on stop.
 * Returns EALREADY if recording is already active.
 */
ECHOSDK_EXPORT int echosdk_call_record_start(echosdk_call_handle_t call,
                                              const char *path);

/** Stop recording and finalize WAV headers. Idempotent. */
ECHOSDK_EXPORT int echosdk_call_record_stop(echosdk_call_handle_t call);

/* ── Call info ────────────────────────────────────────────────────────────── */

/**
 * Static and slow-moving facts about a call, as opposed to the per-tick media
 * numbers in echosdk_call_get_stats().
 *
 * Strings are fixed arrays rather than pointers so the whole struct is a value
 * the caller owns: nothing here can dangle when the call ends, and every
 * binding can copy it without a lifetime rule. Unavailable fields read as an
 * empty string or 0.
 */
typedef struct {
	char     peer_uri[256];          /* far end AoR                        */
	char     peer_display_name[128]; /* From display-name; may be empty    */
	char     local_uri[256];         /* our AoR on this dialog             */
	char     contact_uri[256];       /* far end Contact                    */
	char     call_id[128];           /* SIP Call-ID                        */
	/** Diversion / History-Info URI when the call was forwarded to us.
	 *  Empty otherwise. This is NOT Referred-By: a call that reached us by
	 *  transfer carries no diverter. */
	char     diverter_uri[256];
	bool     is_outgoing;            /* we placed it                       */
	/** True when the PEER has put us on hold. Local hold — the hold this
	 *  app asked for — is echosdk_call_is_held(); the two are independent
	 *  and can both be true. */
	bool     is_remote_hold;
	uint16_t sip_status;             /* last SIP status; 0 while up        */
	uint64_t duration_ms;            /* since ESTABLISHED; 0 before that   */
	/** INVITE → answer. Whole seconds scaled to ms: the stack measures
	 *  this to second granularity, so it steps in 1000s. */
	uint32_t setup_duration_ms;
	uint32_t line_number;            /* 1-based; stable for the call       */
	echosdk_transport_t  transport;  /* transport this dialog signals over */
	echosdk_call_state_t state;      /* same value as the last CALL_STATE  */
} echosdk_call_info_t;

/**
 * Fill `out` with the current facts about a call.
 *
 * Safe to call from any thread and at any point in the call's life, including
 * after it has ended, for as long as the handle is valid.
 *
 * @return ECHOSDK_OK, or ECHOSDK_ERR_INVAL for a NULL argument.
 */
ECHOSDK_EXPORT int echosdk_call_get_info(echosdk_call_handle_t  call,
                                          echosdk_call_info_t   *out);

/* ── Stats ────────────────────────────────────────────────────────────────── */

/**
 * Synchronously retrieve current stats for a call.
 * Also delivered automatically via ECHOSDK_EV_MEDIA_STATS if
 * cfg.stats_interval_ms > 0.
 *
 * `out` is zeroed before anything else, so it is fully defined whatever this
 * returns: on an error every field reads 0, never uninitialised garbage.
 *
 * Note which numbers need RTCP to exist at all.  rtt_ms comes only from the
 * peer's receiver report (RFC 3550 LSR/DLSR), so it reads 0.0 until the first
 * RR carrying both arrives — typically one RTCP interval into the call, and
 * for the whole call against a peer that sends no RTCP.  0.0 there means
 * "not measured yet", not "zero delay".  The only fields that are ever NaN
 * are audio_level_dbov and mic_level_dbov, which use NaN for "unavailable".
 */
ECHOSDK_EXPORT int echosdk_call_get_stats(echosdk_call_handle_t     call,
                            echosdk_ev_media_stats_t *out);

/* ── Network handover ─────────────────────────────────────────────────────── */

/**
 * Tell the SDK that the underlying network may have changed.
 *
 * Call this from the platform's connectivity callback:
 *   Android — ConnectivityManager.NetworkCallback onAvailable / onLost /
 *             onCapabilitiesChanged
 *   iOS     — NWPathMonitor pathUpdateHandler
 *   Desktop — NetworkManager / SCNetworkReachability / NotifyAddrChange, or
 *             leave it to the built-in poller (cfg.net_monitor_interval_s).
 *
 * Safe to call from any thread and as often as the OS fires.  Calls are
 * coalesced: the handover runs once the address set has been stable for
 * cfg.net_settle_ms.  Returns immediately — progress is reported through
 * ECHOSDK_EV_NETWORK events.
 *
 * A report that changed nothing costs nothing.  When the local address set
 * is the one the transports are already bound to, every account is
 * REGISTERED over a connection-oriented transport (WS/WSS/TCP/TLS) and every
 * live call runs ICE, the SDK first sends one OPTIONS per account over its
 * existing connection.  If they all answer within 3 s the connections
 * survived, and no handover runs — no NET event, no RECONNECTING, no
 * re-REGISTER.  Any failure or timeout and the report is handled as the
 * handover below.  A browser behaves the same way: it keeps its WebSocket
 * until the WebSocket itself fails.  Reports that the platform monitors
 * repeat on every engine attach, a VPN coming up next to an unchanged
 * primary network, or a host calling this defensively on resume therefore no
 * longer tear down a working registration.
 *
 * The handover performs, in order:
 *   1. re-scan local addresses and refresh the DNS resolver list
 *   2. flush and re-bind all SIP transports (drops dead TCP/TLS/WS sockets)
 *   3. re-REGISTER every account the app asked to be registered
 *   4. re-INVITE every active call with the new local address in the SDP
 *   5. verify RTP resumes on the new path, retrying the re-INVITE if not
 *
 * @return ECHOSDK_OK, or ECHOSDK_ERR_STATE if the SDK is not initialized.
 */
ECHOSDK_EXPORT int echosdk_network_changed(void);

/**
 * Change the built-in interface poll interval at runtime.
 * @param seconds  Poll period; 0 disables polling entirely.
 */
ECHOSDK_EXPORT int echosdk_network_set_monitor_interval(uint32_t seconds);

/**
 * Adjust handover behaviour at runtime (overrides the cfg.net_* fields).
 * @param reinvite_calls    Re-INVITE active calls onto the new address.
 * @param hangup_on_failure Hang up calls whose media could not be migrated.
 */
ECHOSDK_EXPORT int echosdk_network_set_handover_policy(bool reinvite_calls,
                                                        bool hangup_on_failure);

/**
 * Copy the local IP the SDK is currently using into buf.
 * Writes an empty string when no usable address exists.
 * @return ECHOSDK_OK, or ECHOSDK_ERR_INVAL / ECHOSDK_ERR_STATE.
 */
ECHOSDK_EXPORT int echosdk_network_local_addr(char *buf, size_t sz);

/** False while the device has no usable (non-loopback) local address. */
ECHOSDK_EXPORT bool echosdk_network_is_up(void);

/* ── pcap ─────────────────────────────────────────────────────────────────── */

/**
 * Start capturing SIP + RTP to a Wireshark-compatible pcap file.
 * Writes synthetic Ethernet/IP/UDP headers around each SIP message.
 */
ECHOSDK_EXPORT int echosdk_pcap_start(const char *path);

/** Stop capture and flush/close the pcap file. */
ECHOSDK_EXPORT int echosdk_pcap_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* ECHOSDK_H — embedded C declarations end */

/* ── C++ wrapper ────────────────────────────────────────────────────────── */
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace EchoSDK {

namespace detail {
inline void check(int rc, const char* what) {
    if (rc != ECHOSDK_OK)
        throw std::runtime_error(std::string(what) + " failed (code " + std::to_string(rc) + ")");
}
}

using EventCallback = std::function<void(const echosdk_event_t&)>;

class Call {
public:
    Call() = default;
    explicit Call(echosdk_call_handle_t h) : h_(h) {}

    void answer()                        { detail::check(echosdk_call_answer(h_),    "answer"); }
    void hangup()                        { echosdk_call_hangup(h_); }
    void hold()                          { detail::check(echosdk_call_hold(h_),      "hold"); }
    void resume()                        { detail::check(echosdk_call_resume(h_),    "resume"); }
    void send_dtmf(char digit)           { echosdk_call_send_dtmf(h_, digit); }
    void transfer(const std::string& u)  { detail::check(echosdk_call_transfer(h_, u.c_str()), "transfer"); }
    void mute(bool m)                    { echosdk_audio_mute(h_, m); }

    void add_header(const std::string& name, const std::string& value) {
        detail::check(echosdk_call_add_header(h_, name.c_str(), value.c_str()), "add_header");
    }

    /* Incoming REFER (ECHOSDK_EV_TRANSFER_REQUEST). Answer with exactly one
     * of these, or the transferor never learns the outcome. */
    Call transfer_accept() {
        echosdk_call_handle_t nh = nullptr;
        detail::check(echosdk_call_transfer_accept(h_, &nh), "transfer_accept");
        return Call(nh);
    }
    void transfer_reject(uint16_t scode = 603, const std::string& reason = "Declined") {
        detail::check(echosdk_call_transfer_reject(h_, scode, reason.c_str()),
                      "transfer_reject");
    }

    echosdk_call_info_t info() const {
        echosdk_call_info_t i{};
        detail::check(echosdk_call_get_info(h_, &i), "get_info");
        return i;
    }

    echosdk_ev_media_stats_t stats() const {
        echosdk_ev_media_stats_t s{};
        echosdk_call_get_stats(h_, &s);
        return s;
    }

    void attended_transfer(Call& consult) {
        detail::check(echosdk_call_attended_transfer(h_, consult.h_), "attended_transfer");
    }

    void set_media_tap(echosdk_media_tap_cb_t cb, void* userdata = nullptr) {
        detail::check(echosdk_call_set_media_tap(h_, cb, userdata), "set_media_tap");
    }

    echosdk_call_handle_t handle() const { return h_; }
    explicit operator bool()       const { return h_ != nullptr; }

private:
    echosdk_call_handle_t h_ = nullptr;
};

class Account {
public:
    Account() = default;

    Account(echosdk_account_handle_t h, EventCallback* global_cb)
        : h_(h), global_cb_(global_cb) {}

    ~Account() { if (h_) echosdk_account_destroy(h_); }

    Account(const Account&)            = delete;
    Account& operator=(const Account&) = delete;
    Account(Account&& o) noexcept
        : h_(o.h_), global_cb_(o.global_cb_) { o.h_ = nullptr; }

    Account& operator=(Account&& o) noexcept {
        if (this != &o) {
            if (h_) echosdk_account_destroy(h_);
            h_         = o.h_;
            global_cb_ = o.global_cb_;
            o.h_       = nullptr;
        }
        return *this;
    }

    void register_account() {
        detail::check(echosdk_account_register(h_), "register_account");
    }

    void unregister() { echosdk_account_unregister(h_); }

    Call call(const std::string& uri) {
        echosdk_call_handle_t ch = nullptr;
        detail::check(echosdk_call_invite(h_, uri.c_str(), &ch), "call_invite");
        return Call(ch);
    }

    void send_message(const std::string& to, const std::string& body,
                      const std::string& content_type = "text/plain") {
        detail::check(
            echosdk_message_send(h_, to.c_str(), body.c_str(), content_type.c_str()),
            "message_send");
    }

    void publish_presence(echosdk_presence_status_t status) {
        echosdk_account_publish_presence(h_, status);
    }

    void subscribe_presence(const std::string& target_uri) {
        detail::check(echosdk_account_subscribe_presence(h_, target_uri.c_str()),
                      "subscribe_presence");
    }

    void unsubscribe_presence(const std::string& target_uri) {
        detail::check(echosdk_account_unsubscribe_presence(h_, target_uri.c_str()),
                      "unsubscribe_presence");
    }

    void add_header(const std::string& name, const std::string& value) {
        detail::check(echosdk_account_add_header(h_, name.c_str(), value.c_str()), "add_header");
    }

    /** Add a custom SIP header sent on REGISTER requests only (not on INVITE/BYE). */
    void add_register_header(const std::string& name, const std::string& value) {
        detail::check(
            echosdk_account_add_register_header(h_, name.c_str(), value.c_str()),
            "add_register_header");
    }

    /** Update the push token at runtime. Pass empty string to clear push params. */
    int set_push_token(const std::string& push_token) {
        return echosdk_account_set_push_token(
            h_, push_token.empty() ? nullptr : push_token.c_str());
    }

    void set_100rel(echosdk_100rel_mode_t mode) {
        detail::check(echosdk_account_set_100rel(h_, mode), "set_100rel");
    }

    echosdk_account_handle_t handle() const { return h_; }
    explicit operator bool()          const { return h_ != nullptr; }

private:
    echosdk_account_handle_t h_      = nullptr;
    EventCallback*           global_cb_ = nullptr;
};

class SDK {
public:
    SDK() { echosdk_config_init(&cfg_); }

    ~SDK() { if (initialized_) echosdk_shutdown(); }

    SDK(const SDK&)            = delete;
    SDK& operator=(const SDK&) = delete;

    echosdk_config_t& config() { return cfg_; }
    const echosdk_config_t& config() const { return cfg_; }

    void on_event(EventCallback cb) { event_cb_ = std::move(cb); }

    void init() {
        cfg_.event_cb       = &SDK::dispatch_;
        cfg_.event_userdata = this;
        detail::check(echosdk_init(&cfg_), "echosdk_init");
        initialized_ = true;
    }

    Account create_account(const std::string& uri,
                           const std::string& password,
                           echosdk_transport_t transport = ECHOSDK_TRANSPORT_UDP,
                           std::initializer_list<echosdk_codec_t> codecs = {}) {
        if (!initialized_) init();

        echosdk_account_config_t acfg{};
        acfg.uri       = uri.c_str();
        acfg.password  = password.c_str();
        acfg.transport = transport;
        int ci = 0;
        for (auto c : codecs) { if (ci >= 8) break; acfg.audio_codecs[ci++] = c; }
        acfg.audio_codec_count = ci;

        echosdk_account_handle_t h = nullptr;
        detail::check(echosdk_account_create(&acfg, &h), "account_create");
        return Account(h, &event_cb_);
    }

    Account create_account(const std::string& uri,
                           const std::string& password,
                           echosdk_push_provider_t push_provider,
                           const std::string& push_token,
                           const std::string& push_param = {},
                           echosdk_transport_t transport = ECHOSDK_TRANSPORT_UDP,
                           std::initializer_list<echosdk_codec_t> codecs = {}) {
        if (!initialized_) init();

        echosdk_account_config_t acfg{};
        acfg.uri           = uri.c_str();
        acfg.password      = password.c_str();
        acfg.transport     = transport;
        acfg.push_provider = push_provider;
        acfg.push_token    = push_token.empty() ? nullptr : push_token.c_str();
        acfg.push_param    = push_param.empty()  ? nullptr : push_param.c_str();
        int ci = 0;
        for (auto c : codecs) { if (ci >= 8) break; acfg.audio_codecs[ci++] = c; }
        acfg.audio_codec_count = ci;

        echosdk_account_handle_t h = nullptr;
        detail::check(echosdk_account_create(&acfg, &h), "account_create");
        return Account(h, &event_cb_);
    }

    Account create_account(const echosdk_account_config_t& acfg) {
        if (!initialized_) init();

        echosdk_account_handle_t h = nullptr;
        detail::check(echosdk_account_create(&acfg, &h), "account_create");
        return Account(h, &event_cb_);
    }

    void pcap_start(const std::string& path) {
        detail::check(echosdk_pcap_start(path.c_str()), "pcap_start");
    }

    void pcap_stop() { echosdk_pcap_stop(); }

    const char* version() const { return echosdk_version(); }

    // ── Echo cancellation ────────────────────────────────────────────────
    /** Enable or disable echo cancellation. Re-enables the aec_mode set at init. */
    void set_aec(bool enable) { echosdk_set_aec(enable); }

    /** Switch AEC backend at runtime. Only AEC_OFF ↔ init_mode transitions allowed. */
    int set_aec_mode(echosdk_aec_mode_t mode) { return echosdk_set_aec_mode(mode); }

    /** Tune suppressor aggressiveness: 0=none, 1=maximum (default). SUPPRESSOR only. */
    void set_aec_suppression_level(float level) { echosdk_set_aec_suppression_level(level); }

    // ── Noise suppression / AGC ──────────────────────────────────────────
    void set_ns(bool enable)  { echosdk_set_ns(enable); }
    void set_agc(bool enable) { echosdk_set_agc(enable); }

    // ── Gain control ─────────────────────────────────────────────────────
    /** Set microphone (TX) gain in dB. Range: -20 to +20. 0 = unity. */
    void set_mic_gain(float db) { echosdk_set_mic_gain_db(db); }

    /** Set speaker (RX) gain in dB. Range: -20 to +20. 0 = unity. */
    void set_speaker_gain(float db) { echosdk_set_speaker_gain_db(db); }

    void set_jitter_buffer(uint32_t min_ms, uint32_t max_ms) {
        echosdk_set_jitter_buffer(min_ms, max_ms);
    }

    // ── App-owned audio device ───────────────────────────────────────────
    //
    // Takes the SDK off the platform capture/playback device; SIP, ICE, SRTP,
    // codecs and the jitter buffer stay here. Owning the device means owning
    // echo cancellation too — capture through the platform voice path.

    /** Hand the mic and speaker to the app; false restores the platform
     *  device. Takes effect immediately, including mid-call. Not sticky
     *  across a restart. */
    void use_external_audio(bool enable) {
        detail::check(echosdk_audio_use_external(enable), "use_external_audio");
    }

    /** Captured mic PCM, S16LE interleaved, [nsamp] total samples.
     *
     *  Returns 0, or ENODEV between calls. Deliberately does NOT throw: this
     *  runs on the app's realtime capture thread, and pushing between calls is
     *  the normal steady state, not an error worth unwinding for. */
    int audio_push(const int16_t* pcm, size_t nsamp) {
        return echosdk_audio_external_push(pcm, nsamp);
    }

    /** Decoded audio to play. Always fills the buffer — silence when idle — so
     *  it can go straight to the speaker. Returns 0 or ENODEV; see
     *  audio_push() for why neither throws. */
    int audio_pull(int16_t* pcm, size_t nsamp) {
        return echosdk_audio_external_pull(pcm, nsamp);
    }

    struct AudioFormat {
        uint32_t srate;
        uint8_t  ch;
        uint32_t ptime;
    };

    /** The negotiated format, for sizing the app's device. False until the
     *  call has media — there is no media-up event, so poll it. */
    bool audio_format(AudioFormat& out) {
        return 0 == echosdk_audio_external_format(&out.srate, &out.ch,
                                                  &out.ptime);
    }

    /** True while a call is capturing or playing through the app-owned
     *  device — false between calls, even with the mode on. */
    bool audio_external_active() {
        return echosdk_audio_external_is_active();
    }

    /* ── Network handover (Wi-Fi ↔ 4G/5G) ──────────────────────────────── */

    /** Tell the SDK the network may have changed.  Progress arrives as
     *  ECHOSDK_EV_NETWORK events.  Safe to call from any thread. */
    void network_changed() {
        detail::check(echosdk_network_changed(), "network_changed");
    }

    /** Interface poll period in seconds; 0 disables polling (use on mobile
     *  and drive network_changed() from the OS connectivity callback). */
    void network_set_monitor_interval(uint32_t seconds) {
        detail::check(echosdk_network_set_monitor_interval(seconds),
                      "network_set_monitor_interval");
    }

    void network_set_handover_policy(bool reinvite_calls,
                                     bool hangup_on_failure) {
        detail::check(echosdk_network_set_handover_policy(reinvite_calls,
                                                          hangup_on_failure),
                      "network_set_handover_policy");
    }

    /** Local IP currently in use, or "" when the device has no address. */
    std::string network_local_addr() {
        char buf[64] = {0};
        if (echosdk_network_local_addr(buf, sizeof(buf)) != ECHOSDK_OK)
            return {};
        return buf;
    }

    bool network_is_up() { return echosdk_network_is_up(); }

private:
    static void dispatch_(const echosdk_event_t* ev, void* ud) {
        auto* self = static_cast<SDK*>(ud);
        if (self->event_cb_) self->event_cb_(*ev);
    }

    echosdk_config_t cfg_{};
    EventCallback    event_cb_;
    bool             initialized_ = false;
};

} // namespace EchoSDK
