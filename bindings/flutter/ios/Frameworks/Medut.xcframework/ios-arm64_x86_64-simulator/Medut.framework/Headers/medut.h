/**
 * @file medut.h  Medut public API
 *
 * Single header for all platforms. Include this and link medut.a.
 * Never include re.h or baresip.h directly from consumer code.
 *
 * ABI stability contract:
 *   - Fields are only appended to structs; never reordered or removed.
 *   - medut_config_t carries version + struct_size for forward compat.
 *   - Opaque handle types are stable across minor versions.
 */

#ifndef MEDUT_H
#define MEDUT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── DLL export macros (Windows shared library build) ──────────────────── */
#ifdef _WIN32
#  ifdef MEDUT_SHARED_BUILD
#    define MEDUT_EXPORT __declspec(dllexport)
#  elif defined(MEDUT_SHARED)
#    define MEDUT_EXPORT __declspec(dllimport)
#  else
#    define MEDUT_EXPORT
#  endif
#else
#  define MEDUT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────────────────── */

#define MEDUT_VERSION_MAJOR 1
#define MEDUT_VERSION_MINOR 0
#define MEDUT_VERSION_PATCH 0

MEDUT_EXPORT const char *medut_version(void);
MEDUT_EXPORT const char *medut_strerror(int err);

/* ── Opaque handles ───────────────────────────────────────────────────────── */

typedef struct medut_account  *medut_account_handle_t;
typedef struct medut_call     *medut_call_handle_t;

/* ── Enumerations ─────────────────────────────────────────────────────────── */

typedef enum {
	MEDUT_TRANSPORT_UDP = 0,
	MEDUT_TRANSPORT_TCP,
	MEDUT_TRANSPORT_TLS,
	MEDUT_TRANSPORT_WS,
	MEDUT_TRANSPORT_WSS,
} medut_transport_t;

typedef enum {
	MEDUT_MEDIA_ENC_NONE = 0,
	MEDUT_MEDIA_ENC_SDES,
	MEDUT_MEDIA_ENC_DTLS_SRTP,
} medut_media_enc_t;

/* Only OPUS, PCMU and PCMA are compiled into the library — on every platform.
 * G722 and G726_32 are retained so the numeric values of the constants above
 * and below them stay put (removing them would shift the enum and break the
 * ABI for already-built callers), but no module registers those codecs:
 * selecting one contributes nothing to the SDP offer and logs a warning. */
typedef enum {
	MEDUT_CODEC_OPUS = 0,
	MEDUT_CODEC_PCMU,     /* G.711 µ-law */
	MEDUT_CODEC_PCMA,     /* G.711 A-law */
	MEDUT_CODEC_G722,     /* deprecated — not compiled in */
	MEDUT_CODEC_G726_32,  /* deprecated — not compiled in */
} medut_codec_t;

typedef enum {
	MEDUT_MOS_EMODEL = 0,
	MEDUT_MOS_SIMPLIFIED,
} medut_mos_method_t;

/**
 * AEC backend selection.
 * Stored as uint8_t to occupy exactly 1 byte (same as the former bool aec),
 * preserving medut_config_t memory layout.
 *
 * MEDUT_AEC_SUPPRESSOR (default): built-in half-duplex TX suppressor.
 *   Zero external dependencies.  Ignored on Android and iOS, where the
 *   platform driver captures through the OS voice path (VOICE_COMMUNICATION /
 *   VoiceProcessingIO) and has already cancelled the echo in hardware —
 *   ducking the mic on top of that only costs full duplex.  On an Android
 *   device that refuses the VOICE_COMMUNICATION preset there is no hardware
 *   canceller, and the suppressor runs there.
 * MEDUT_AEC_WEBRTC: full-duplex WebRTC acoustic echo cancellation (AEC3).
 *   Desktop Linux only; requires the MEDUT_WITH_WEBRTC_AEC build option
 *   and webrtc-audio-processing 1.x (2.x is not supported).  The library is
 *   loaded at runtime: when it is missing, or the build does not have it,
 *   init falls back to the suppressor and logs it.  With ns on, WebRTC's
 *   noise suppressor runs instead of the built-in gate.
 *   medut_set_aec_mode(WEBRTC) returns ENOTSUP when the build does not
 *   have it.
 */
/* MEDUT_NO_PACKED_ENUM: for binding generators (e.g. ffigen) that ignore
 * __attribute__((packed)) and would widen the field to 4 bytes, silently
 * shifting every later struct member. The uint8_t typedef keeps the ABI. */
#if defined(_MSC_VER) || defined(MEDUT_NO_PACKED_ENUM)
typedef uint8_t medut_aec_mode_t;
#  define MEDUT_AEC_OFF        ((medut_aec_mode_t)0)
#  define MEDUT_AEC_SUPPRESSOR ((medut_aec_mode_t)1)
#  define MEDUT_AEC_WEBRTC     ((medut_aec_mode_t)2)
#else
typedef enum __attribute__((packed)) {
	MEDUT_AEC_OFF        = 0,
	MEDUT_AEC_SUPPRESSOR = 1,
	MEDUT_AEC_WEBRTC     = 2,
} medut_aec_mode_t;
#endif

typedef enum {
	MEDUT_MEDIA_DIR_RX = 0,
	MEDUT_MEDIA_DIR_TX,
} medut_media_dir_t;

typedef enum {
	MEDUT_DTMF_RFC4733  = 0, /* RFC 4733 RTP telephony-event (default) */
	MEDUT_DTMF_SIP_INFO = 1, /* SIP INFO application/dtmf-relay */
	MEDUT_DTMF_AUTO     = 2, /* prefer RFC 4733, fall back to SIP INFO */
} medut_dtmf_mode_t;

typedef enum {
	MEDUT_JBUF_ADAPTIVE = 0, /* adaptive jitter buffer (default) */
	MEDUT_JBUF_FIXED    = 1, /* fixed-depth jitter buffer */
} medut_jbuf_type_t;

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
 * recover it otherwise.  MEDUT_NET_CALL_ICE_STALE marks such a call.
 */
typedef enum {
	/** Send the re-INVITE anyway and let media verification decide.
	 *  Recovers relay/direct paths; wastes net_verify_ms × net_max_attempts
	 *  before failing when it cannot. */
	MEDUT_ICE_HANDOVER_BEST_EFFORT = 0,
	/** Try once, then fail immediately.  Gives the app a prompt
	 *  CALL_MIGRATION_FAILED it can answer by re-placing the call, instead
	 *  of a long silence.  Recommended when calls are ICE+TURN over
	 *  cellular.
	 *
	 *  Does not apply to a call whose ICE was restarted: that call is not
	 *  offering the wrong candidates any more, so it is held to the same
	 *  retry budget as a direct-RTP call. */
	MEDUT_ICE_HANDOVER_FAIL_FAST,
} medut_ice_handover_t;

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
 * Delivered as MEDUT_EV_ICE_STATE and readable via medut_call_ice_state().
 */
typedef enum {
	MEDUT_ICE_NEW = 0,
	MEDUT_ICE_GATHERING,
	MEDUT_ICE_CHECKING,
	MEDUT_ICE_CONNECTED,
	MEDUT_ICE_COMPLETED,
	MEDUT_ICE_DISCONNECTED,
	MEDUT_ICE_FAILED,
	MEDUT_ICE_CLOSED,
} medut_ice_state_t;

/** Candidate type of one side of the selected pair (RFC 8445 §5.1.1). */
typedef enum {
	MEDUT_ICE_CAND_HOST = 0,
	MEDUT_ICE_CAND_SRFLX,
	MEDUT_ICE_CAND_PRFLX,
	MEDUT_ICE_CAND_RELAY,
	MEDUT_ICE_CAND_NONE,
} medut_ice_cand_type_t;

/**
 * One STUN or TURN server, in the URL grammar browsers accept (RFC 7064 /
 * RFC 7065): "stun:host[:port]", "stuns:host[:port]", "turn:host[:port]
 * [?transport=udp|tcp]", "turns:host[:port][?transport=tcp]".  Default
 * ports 3478 (5349 for the TLS schemes).  `user`/`pass` are the TURN
 * long-term credentials and ignored for STUN.  TURN over TCP/TLS entries are
 * accepted and currently skipped at gathering time with a warning.
 */
typedef struct {
	const char *url;
	const char *user;
	const char *pass;
} medut_ice_server_t;

/** Which candidates a call may use (WebRTC's iceTransportPolicy). */
typedef enum {
	MEDUT_ICE_TRANSPORT_ALL = 0,   /**< host, reflexive and relayed */
	MEDUT_ICE_TRANSPORT_RELAY,     /**< relayed only: offer and accept
	                                      nothing but TURN candidates.  A
	                                      call with no usable TURN server
	                                      fails fast instead of leaking a
	                                      direct path */
} medut_ice_transport_policy_t;

typedef struct {
	int  bitrate;    /* target bps (VBR); default 32000; 0 = Opus picks (~50 kbps mono) */
	int  complexity; /* 0-10 CPU trade-off; -1 = opus default (9) */
	bool cbr;        /* constant bitrate; false = VBR (default) */
	bool dtx;        /* discontinuous transmission: the encoder sends nothing
	                    in silence and the offer carries usedtx=1 (RFC 7587
	                    3.1.3); default false.  Our own switch: it does not
	                    wait for the peer to ask. */
	bool fec;        /* in-band FEC; default true (medut_config_init) */
	bool stereo;     /* send and ask for stereo; false = mono (default) */
} medut_opus_config_t;

/* ── Error codes ──────────────────────────────────────────────────────────── */

typedef enum {
	MEDUT_OK                       =  0,
	MEDUT_ERR_INVAL                = -1,
	MEDUT_ERR_NOMEM                = -2,
	MEDUT_ERR_STATE                = -3,  /* wrong lifecycle state */
	MEDUT_ERR_DNS                  = -4,
	MEDUT_ERR_TRANSPORT            = -5,
	MEDUT_ERR_AUTH                 = -6,
	MEDUT_ERR_SERVER_5XX           = -7,
	MEDUT_ERR_WS_PROTOCOL_REJECTED = -8,
	MEDUT_ERR_TIMEOUT              = -9,
	MEDUT_ERR_ALREADY              = -10,
	/* 403 Forbidden: the request was understood and the credentials were
	 * fine — the server refused this particular request.  On a call that
	 * is a dialplan or policy refusal ("you may not dial that"), which is
	 * not a reason to ask anyone for new credentials.  Registration keeps
	 * reporting 403 as MEDUT_ERR_AUTH, where it does mean the account
	 * itself was rejected.  Appended at the end of the enum so an app
	 * built against an older header keeps its own numbering. */
	MEDUT_ERR_FORBIDDEN            = -11,
} medut_error_t;

/* ── Call states ──────────────────────────────────────────────────────────── */

typedef enum {
	MEDUT_CALL_CALLING = 0,
	MEDUT_CALL_RINGING,
	MEDUT_CALL_ESTABLISHED,
	MEDUT_CALL_HELD,
	MEDUT_CALL_ENDED,
	MEDUT_CALL_CANCELLED,
	MEDUT_CALL_FAILED,
} medut_call_state_t;

/* ── Registration states ──────────────────────────────────────────────────── */

typedef enum {
	MEDUT_REG_UNREGISTERED = 0,
	MEDUT_REG_REGISTERING,
	MEDUT_REG_REGISTERED,
	/**
	 * Terminal: the registration is down and the SDK has stopped trying.
	 * Either nothing it can retry will help (MEDUT_ERR_AUTH — wrong
	 * credentials), the retry budget (`reg_retry_max_attempts`) ran out, or
	 * the app cancelled the retry itself.  Recovering needs the app:
	 * medut_account_retry_now(), or new credentials.
	 */
	MEDUT_REG_FAILED,
	MEDUT_REG_UNREGISTERING,
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
	 *     MEDUT_EV_NETWORK events carry the detail.
	 *
	 * The state does not flicker back to REGISTERING for each retry: an
	 * account stays RECONNECTING for the whole recovery, and leaves it only
	 * for REGISTERED (recovered) or FAILED (given up).  REGISTERING keeps
	 * its narrower meaning — a registration the app asked for, going out
	 * for the first time.
	 */
	MEDUT_REG_RECONNECTING,
} medut_reg_state_t;

/* ── Presence enums ────────────────────────────────────────────────────────── */

typedef enum {
	MEDUT_PRESENCE_UNKNOWN = 0,
	MEDUT_PRESENCE_OPEN,     /* available */
	MEDUT_PRESENCE_CLOSED,   /* offline / DND */
	MEDUT_PRESENCE_BUSY,     /* on a call */
} medut_presence_status_t;

typedef enum {
	MEDUT_100REL_DISABLED = 0, /* never send/require 100rel */
	MEDUT_100REL_ENABLED  = 1, /* support 100rel if peer offers */
	MEDUT_100REL_REQUIRED = 2, /* require 100rel; reject if unsupported */
} medut_100rel_mode_t;

/**
 * Push notification provider — determines the pn-provider URI parameter
 * value placed in the SIP REGISTER Contact header per RFC 8599, or the
 * server-side push dispatch type for non-RFC-8599 servers.
 */
typedef enum {
	MEDUT_PUSH_PROVIDER_NONE         = 0, /* no push params (default) */
	MEDUT_PUSH_PROVIDER_APNS         = 1, /* Apple APNs production    */
	MEDUT_PUSH_PROVIDER_APNS_SANDBOX = 2, /* Apple APNs development   */
	MEDUT_PUSH_PROVIDER_FCM          = 3, /* Firebase Cloud Messaging  */
} medut_push_provider_t;

/* ── Event types ──────────────────────────────────────────────────────────── */

typedef enum {
	MEDUT_EV_LOG = 0,
	MEDUT_EV_REG_STATE,
	MEDUT_EV_INCOMING_CALL,
	MEDUT_EV_CALL_STATE,
	MEDUT_EV_CALL_DTMF,
	MEDUT_EV_SDP_NEGOTIATION,
	MEDUT_EV_SIP_TRACE,
	MEDUT_EV_MEDIA_STATS,
	MEDUT_EV_REGISTRAR_WARNING,
	MEDUT_EV_TRANSFER_REQUEST,
	MEDUT_EV_MWI,
	MEDUT_EV_MESSAGE,
	MEDUT_EV_PRESENCE_STATE,
	MEDUT_EV_QUALITY_ALERT,
	MEDUT_EV_NETWORK,
	/** An outgoing REFER was refused.  The call named in the payload is STILL
	 *  ESTABLISHED — this is deliberately not a call-state change, so a failed
	 *  transfer never tears down the leg the user is still talking on.  A REFER
	 *  that succeeds needs no event of its own: the far end takes the call over
	 *  and our leg closes, which arrives as MEDUT_EV_CALL_STATE / ENDED. */
	MEDUT_EV_TRANSFER_FAILED,
	/** The ICE liveness state of a call's media path changed — see
	 *  medut_ice_state_t.  Payload: medut_ev_ice_state_t. */
	MEDUT_EV_ICE_STATE,
} medut_event_type_t;

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
	MEDUT_NET_CHANGE_DETECTED = 0,   /* local address set changed          */
	MEDUT_NET_DOWN,                  /* no usable local address            */
	MEDUT_NET_UP,                    /* usable local address again         */
	MEDUT_NET_TRANSPORT_RESET,       /* SIP transports flushed + re-bound  */
	MEDUT_NET_REREGISTERING,         /* REGISTER re-sent on the new path   */
	MEDUT_NET_CALL_MIGRATING,        /* re-INVITE sent to move the media   */
	MEDUT_NET_CALL_MIGRATE_ACCEPTED, /* peer answered; awaiting audio      */
	MEDUT_NET_CALL_MIGRATED,         /* RTP confirmed on the new path      */
	MEDUT_NET_CALL_MIGRATION_FAILED, /* gave up; see `error`               */
	MEDUT_NET_CALL_DEFERRED,         /* not refreshable yet; will retry    */
	MEDUT_NET_HANDOVER_FAILED,       /* transport reset failed; retrying   */
	/** ICE call whose candidates could not be re-gathered: the migration
	 *  falls back to a re-INVITE carrying the pre-handover candidate set,
	 *  which recovers direct and TURN-relayed paths only.  Emitted once per
	 *  call per handover, before that re-INVITE.  An ICE call that *was*
	 *  restarted does not emit this — it migrates like any other call.  See
	 *  medut_ice_handover_t and cfg.net_ice_handover. */
	MEDUT_NET_CALL_ICE_STALE,
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
	MEDUT_NET_CALL_ICE_FAILED,
} medut_net_event_t;

/**
 * Network handover progress.
 *
 * `attempt` / `max_attempts` render a progress counter:
 *     "link settled — rebuilding the media path %u/%u"
 * `elapsed_ms` on CALL_MIGRATED is how long audio was interrupted:
 *     "media recovered after %.1f s"
 */
typedef struct {
	medut_net_event_t      event;
	medut_call_handle_t    call;     /* CALL_* events only; else NULL    */
	medut_account_handle_t account;  /* REREGISTERING only; else NULL    */
	const char              *local_addr; /* new local IP, "" when unknown  */
	uint32_t                 attempt;  /* 1-based retry counter            */
	/**
	 * The offer budget this call is held to — net_max_attempts normally, or 1
	 * for a call under MEDUT_ICE_HANDOVER_FAIL_FAST whose ICE could not be
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
	medut_error_t          error;    /* MEDUT_OK unless *_FAILED       */
} medut_ev_network_t;

/**
 * ICE liveness change (MEDUT_EV_ICE_STATE).
 *
 * `local_addr` / `remote_addr` are the selected candidate pair as
 * "ip:port" ("" until a pair is selected), `generation` counts ICE
 * restarts (1 = the initial session), `rtt_ms` is the STUN round trip on
 * the selected pair.  The strings are valid for the callback's duration
 * (or for the event's life with cfg.deliver_owned_events).
 */
typedef struct {
	medut_call_handle_t    call;
	medut_ice_state_t      state;
	medut_ice_state_t      prev;
	uint32_t                 generation;
	medut_ice_cand_type_t  local_type;
	medut_ice_cand_type_t  remote_type;
	const char              *local_addr;
	const char              *remote_addr;
	float                    rtt_ms;
	medut_error_t          error;      /* MEDUT_OK unless FAILED */
} medut_ev_ice_state_t;

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
	medut_account_handle_t account;
	medut_reg_state_t      state;
	medut_error_t          error;         /* MEDUT_OK when REGISTERED */
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
} medut_ev_reg_state_t;

typedef struct {
	medut_account_handle_t account;
	medut_call_handle_t    call;
	const char              *from_uri;
	const char              *display_name;
} medut_ev_incoming_call_t;

typedef struct {
	medut_account_handle_t account;
	medut_call_handle_t    call;
	medut_call_state_t     state;
	medut_error_t          error;
	const char              *reason;
} medut_ev_call_state_t;

typedef struct {
	medut_call_handle_t call;
	char                  digit;
} medut_ev_call_dtmf_t;

typedef struct {
	medut_call_handle_t  call;
	const char            *local_sdp;
	const char            *remote_sdp;
	const char            *negotiated_codec;
	const char            *negotiated_crypto; /* "NONE", "SDES", "DTLS-SRTP" */
	const char * const    *rejected_codecs;   /* NULL-terminated array */
	const char * const    *warnings;          /* NULL-terminated array */
} medut_ev_sdp_negotiation_t;

typedef struct {
	medut_media_dir_t  dir;
	const char          *transport;    /* "UDP", "TCP", "TLS", "WS", "WSS" */
	const char          *remote_addr;  /* "1.2.3.4:5060" */
	const char          *raw_message;
	uint64_t             timestamp_us;
} medut_ev_sip_trace_t;

typedef struct {
	medut_call_handle_t call;

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
	medut_mos_method_t mos_method;

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
	 * MEDUT_ICE_NEW and the rest zero on a call without ICE.
	 * `remote_addr` above is the same address as `ice_remote_addr` on an
	 * ICE call, and the signalled peer on a plain-RTP one. */
	medut_ice_state_t     ice_state;
	medut_ice_cand_type_t ice_local_type;
	medut_ice_cand_type_t ice_remote_type;
	float    ice_rtt_ms;        /* STUN RTT on the selected pair */
	uint32_t ice_generation;    /* ICE restarts so far; 1 = initial session */
	char     ice_local_addr[64];  /* selected local candidate "ip:port" */
	char     ice_remote_addr[64]; /* selected remote candidate "ip:port" */

	/* ── Opus FEC we send ──────────────────────────────────────────────── */
	/* Whether our encoder is coding in-band FEC, and the expected loss it
	 * is sized for.  Switched on and off from the peer's receiver reports:
	 * on at 4% loss (2% in two reports running), off after 10 s under 1%
	 * (see opus_expected_loss_pct).
	 * False / 0 for G.711, or a peer that did not ask for FEC.  Also false
	 * while the link is congested and the bitrate is below the FEC floor,
	 * where the encoder codes none — with a pinned opus_expected_loss_pct
	 * too, whose level fec_level_pct still reports. */
	bool     fec_active;
	uint32_t fec_level_pct;

	/* ── Opus FEC we receive ───────────────────────────────────────────── */
	/* How each frame the jitter buffer reported lost was filled in,
	 * cumulative for the call: rebuilt from the redundant copy the next
	 * packet carried (fec_recovered), or concealed by the decoder alone
	 * (plc_concealed).  plc_frames counts the gaps the buffer saw; these
	 * two say what became of them.  0 for a codec other than Opus. */
	uint32_t fec_recovered;
	uint32_t plc_concealed;
} medut_ev_media_stats_t;

typedef struct {
	const char *message;
} medut_ev_log_t;

typedef struct {
	const char *message;
} medut_ev_registrar_warning_t;

/* ── Transfer, MWI, MESSAGE, Presence payload structs ─────────────────────── */

/**
 * Incoming REFER request — blind transfer, or attended when has_replaces.
 *
 * The SDK has already answered `202 Accepted` and sent `NOTIFY 100 Trying`;
 * per RFC 3515 the transferor is now waiting for a final `message/sipfrag`
 * NOTIFY saying what became of the reference.  Nothing sends it until the app
 * calls medut_call_transfer_accept() or medut_call_transfer_reject() —
 * exactly one of the two, on the call this event names.  Ignoring the event
 * leaves the transferor waiting out the 60 s subscription.
 */
typedef struct {
	medut_account_handle_t account;
	medut_call_handle_t    call;          /* call receiving the REFER */
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
} medut_ev_transfer_req_t;

/**
 * An outgoing REFER was refused (blind or attended).
 *
 * The call is still up.  baresip does not close a call whose transfer failed,
 * and neither does Medut: the caller is still on the line, usually on hold,
 * and the app's remedy is to resume them and report the failure — not to hang
 * up.  `reason` is the status line or cause text when the stack supplied one.
 */
typedef struct {
	medut_account_handle_t account;
	medut_call_handle_t    call;    /* the call we tried to transfer away */
	const char              *reason;  /* status line / cause, may be NULL */
} medut_ev_transfer_failed_t;

/** MWI NOTIFY — raw body is parsed into counters; all fields may be 0. */
typedef struct {
	medut_account_handle_t account;
	bool                     messages_waiting;
	uint32_t                 new_voice;
	uint32_t                 old_voice;
	uint32_t                 new_urgent;
	uint32_t                 old_urgent;
	const char              *raw_body;      /* full NOTIFY body */
} medut_ev_mwi_t;

/** Incoming SIP MESSAGE (instant message). */
typedef struct {
	medut_account_handle_t account;
	const char              *from_uri;
	const char              *body;
	const char              *content_type;  /* e.g. "text/plain" */
} medut_ev_message_t;

/** Buddy / contact presence state changed. */
typedef struct {
	medut_account_handle_t  account;
	const char               *target_uri;
	medut_presence_status_t status;
} medut_ev_presence_state_t;

typedef enum {
	MEDUT_QUALITY_MOS    = 0,
	MEDUT_QUALITY_LOSS,
	MEDUT_QUALITY_JITTER,
	MEDUT_QUALITY_RTT,
	/** No inbound RTP for cfg.media_stall_ms while the call is not on hold.
	 *  `value` is the stall duration in ms, `threshold` is media_stall_ms.
	 *  Non-fatal: fires again with `recovering` = true when RTP resumes.
	 *  See also cfg.rtp_timeout_s, which *ends* the call instead. */
	MEDUT_QUALITY_MEDIA_STALL,
} medut_quality_issue_t;

typedef struct {
	medut_call_handle_t   call;
	medut_quality_issue_t issue;
	float                   value;       /* current metric value */
	float                   threshold;   /* threshold that was crossed */
	bool                    recovering;  /* true = value returned above threshold */
} medut_ev_quality_alert_t;

/* ── Master event union ───────────────────────────────────────────────────── */

typedef struct {
	medut_event_type_t type;
	union {
		medut_ev_log_t               log;
		medut_ev_reg_state_t         reg;
		medut_ev_incoming_call_t     incoming;
		medut_ev_call_state_t        call_state;
		medut_ev_call_dtmf_t         dtmf;
		medut_ev_sdp_negotiation_t   sdp;
		medut_ev_sip_trace_t         sip_trace;
		medut_ev_media_stats_t       stats;
		medut_ev_registrar_warning_t reg_warn;
		medut_ev_transfer_req_t      transfer_req;
		medut_ev_transfer_failed_t   transfer_failed;
		medut_ev_mwi_t               mwi;
		medut_ev_message_t           msg;
		medut_ev_presence_state_t    presence;
		medut_ev_quality_alert_t     quality_alert;
		medut_ev_network_t           network;
		medut_ev_ice_state_t         ice;
	} u;
} medut_event_t;

/* ── Callbacks ────────────────────────────────────────────────────────────── */

/**
 * Event callback — fired from Medut's event dispatch thread, never from
 * re_main. Calling Medut APIs from inside the callback is safe: the
 * dispatch thread exists precisely so a consumer can re-enter without
 * deadlocking the SIP loop. Keep it fast (< 10 ms) — the callback runs
 * inline with the event queue, so a slow handler backs the queue up. Hand
 * heavy work (recording, transcription, UI) to your own thread.
 */
typedef void (*medut_event_cb_t)(const medut_event_t *ev, void *userdata);

/**
 * Release an event delivered with cfg.deliver_owned_events = true.
 * Must be called exactly once per delivered event; may be called from any
 * thread, at any time after delivery.  No-op on NULL.  Passing an event
 * that was NOT delivered in owned mode is undefined behavior.
 */
MEDUT_EXPORT void medut_event_release(const medut_event_t *ev);

/**
 * Media tap callback — fired from the audio thread on each audio frame.
 * Must be non-blocking. Copy PCM data if you need it beyond the callback.
 */
typedef void (*medut_media_tap_cb_t)(
	medut_call_handle_t  call,
	medut_media_dir_t    direction,
	const int16_t         *pcm,
	size_t                 samples,
	uint32_t               sample_rate,
	uint8_t                channels,
	uint64_t               timestamp_us,
	void                  *userdata);

/* ── Config ───────────────────────────────────────────────────────────────── */

#define MEDUT_CONFIG_VERSION 1

typedef struct {
	/* Forward-compat guard — MUST be set by medut_config_init(). */
	uint32_t  version;      /* MEDUT_CONFIG_VERSION */
	size_t    struct_size;  /* sizeof(medut_config_t) at compile time */

	/* ── Transport ────────────────────────────────────────────────── */
	medut_transport_t  transport;
	const char          *local_ip;        /* NULL = auto */
	uint16_t             local_port;      /* 0 = OS-assigned */
	const char          *bind_interface;  /* NULL = any, e.g. "wlan0" */
	bool                 prefer_ipv6;

	const char          *sip_domain;      /* AOR domain, e.g. "pbx.example.com" */

	/**
	 * Server endpoint — two ways to specify, pick one:
	 *
	 * Simple form (UDP/TCP/TLS, default ports):
	 *   transport   = MEDUT_TRANSPORT_TLS
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
	/** Verify the server's certificate: SIP/WSS over TLS, and the
	 *  TURN servers given as `turns:` URLs. */
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
	medut_media_enc_t  media_enc;
	/* Ordered preference list. Overridden per account by
	 * medut_account_config_t.audio_codec[_name]s, and by the
	 * string list in audio_codec_names[] below.
	 * Leave at 0 for the cross-platform default: Opus, PCMU, PCMA. */
	medut_codec_t      audio_codecs[8];
	int                  audio_codec_count;
	uint8_t              dscp_sip;  /* 0 = OS default; 24 = AF31 */
	uint8_t              dscp_rtp;  /* 0 = OS default; 46 = EF */
	bool                 enable_video; /* reserved for future video support */

	/* ── Audio processing ─────────────────────────────────────────── */
	medut_aec_mode_t aec_mode;  /* echo cancellation backend; default SUPPRESSOR */
	bool  ns;   /* noise suppression; off on the mobile voice path, see medut_set_ns() */
	bool  agc;  /* automatic gain control; off on the mobile voice path, see medut_set_agc() */
	float aec_suppression_level;  /* 0=no suppression .. 1=maximum; default 1.0; SUPPRESSOR only */
	float mic_gain_db;            /* TX manual gain dB, clamped [-20,+20]; 0=unity */
	float speaker_gain_db;        /* RX manual gain dB, clamped [-20,+20]; 0=unity */

	/* ── Opus tuning ──────────────────────────────────────────────── */
	medut_opus_config_t opus; /* see medut_config_init() for defaults */

	/* ── Jitter buffer ────────────────────────────────────────────── */
	medut_jbuf_type_t jbuf_type;          /* adaptive (default) or fixed */
	uint32_t jitter_buffer_min_ms; /* JB min depth; 0 = 40 ms (fixed: 100 ms,
	                                  the depth a fixed buffer holds) */
	uint32_t jitter_buffer_max_ms; /* JB max depth; 0 = 400 ms (fixed: 200 ms) */

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
	 * two fields below are reported by medut_config_get() for
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
	 * MEDUT_CALL_CALLING after `sip_timer_b_ms` is cancelled with 408 and
	 * reported as MEDUT_CALL_FAILED / MEDUT_ERR_TIMEOUT.  Set below
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
	 * REGISTER before reporting MEDUT_ERR_TIMEOUT and handing over to the
	 * retry policy.  0 disables the watchdog.
	 */
	uint32_t  sip_timer_f_ms;

	/* ── Session timers (RFC 4028) ───────────────────────────────── */
	bool      session_timer_enabled; /* default true */
	uint32_t  session_expires_s;     /* default 1800 */
	uint32_t  session_min_se_s;      /* default 90 */

	/* ── Quality / observability ─────────────────────────────────── */
	/**
	 * MEDUT_EV_MEDIA_STATS poll interval in ms.  Default 2000; 0 disables.
	 *
	 * This is also the master switch for RTCP accounting
	 * (baresip `avt.rtp_stats`), so with 0 the loss/jitter/RTT/MOS fields
	 * read back as zero from medut_call_get_stats() too, and every
	 * feature derived from them — quality alerts, media-stall detection and
	 * adaptive bitrate — is inert.
	 */
	uint32_t              stats_interval_ms;
	medut_mos_method_t  mos_method;
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
	bool        trace_sip;      /* emit MEDUT_EV_SIP_TRACE per message */
	bool        trace_sdp_diff; /* emit MEDUT_EV_SDP_NEGOTIATION */
	const char *pcap_path;      /* NULL = no pcap; path = live capture */

	/* ── Logging & events ────────────────────────────────────────── */
	int                 log_level;      /* 0=err, 1=warn, 2=info, 3=debug */
	medut_event_cb_t  event_cb;       /* required */
	void               *event_userdata;

	/* ── Network handover (Wi-Fi ↔ 4G/5G roaming) ────────────────
	 *
	 * On a network change the SDK re-binds its SIP transports,
	 * re-REGISTERs, and re-INVITEs every active call onto the new local
	 * address.  On mobile, drive this from the OS connectivity callback
	 * by calling medut_network_changed(); the built-in poller below is
	 * a safety net for platforms with no such callback.
	 */

	/** Interface poll interval in seconds; 0 disables polling.
	 *  Default 10.  Set 0 on mobile and call medut_network_changed(). */
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
	 * consumer MUST call medut_event_release(ev) exactly once when
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
	 * build can be selected even without a medut_codec_t constant.
	 * Same accepted spellings as the per-account
	 * medut_account_config_t.audio_codec_names: "opus", "ulaw"/"pcmu",
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
	 * Whether medut_init() activates the platform audio session it
	 * configures.  Only iOS has one; every other platform ignores this.
	 *
	 * true (default): configure the AVAudioSession (PlayAndRecord +
	 * VoiceChat) and activate it during medut_init().  Right for an
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
	 * MEDUT_ERR_TIMEOUT.  Only sendrecv streams are checked, so a held
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
	 * fires MEDUT_QUALITY_MEDIA_STALL when inbound RTP stops advancing and
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
	 * it again when the link recovers.  Default true.
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
	uint32_t  adapt_min_bitrate;    /* bps; 0 = 12000; 20000 while FEC is on, unless congested */
	uint32_t  adapt_max_bitrate;    /* bps; 0 = 32000  */
	float     adapt_loss_down_pct;  /* step down above this; 0 = 5.0  */
	float     adapt_loss_up_pct;    /* step up below this;   0 = 1.0  */
	uint32_t  adapt_recover_ticks;  /* clean ticks before a step up; 0 = 5 */

	/**
	 * Expected packet loss handed to the Opus encoder, percent.
	 * 0 (default) = switched on and off with the link.
	 *
	 * Sizes Opus in-band FEC (LBRR): the encoder spends part of its budget
	 * on a redundant low-bitrate copy of the previous frame, and the peer
	 * rebuilds a lost frame from the packet after it.  Received FEC is
	 * always used, and a lost frame is concealed either way.
	 *
	 * With `opus.fec` on (the default) and this left at 0, the SDK switches
	 * our FEC on and off from the loss in the peer's RTCP receiver reports:
	 * on at once for a report of 4% or more, or when two reports in a row
	 * show 2% (one alone can be chance on a 1–2% link); off once no report
	 * has shown 1% for 10 s — so a clean link carries none and pays
	 * nothing.  While
	 * on, the redundancy follows the loss: under 4% → 5, 4–10% → 10,
	 * 10–15% → 15, 15–20% → 20, above → 25.  The state is on every stats
	 * tick as `fec_active` / `fec_level_pct`.  A non-zero value pins it
	 * instead.  `opus.fec = false` turns FEC off: we neither ask the peer
	 * for it nor send it.
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
	 * Default BEST_EFFORT.  See medut_ice_handover_t — an ICE call is
	 * normally migrated with a full ICE restart, and this does not apply to
	 * those.
	 */
	medut_ice_handover_t net_ice_handover;

	/**
	 * Deadline for ICE candidate gathering on an outgoing call, ms.
	 * Default 2000; 0 disables the deadline (wait indefinitely).
	 *
	 * With a media-NAT configured, the INVITE is not sent when the call is
	 * placed — it is sent later, when the ICE stack reports that it has
	 * finished gathering candidates.  Nothing in that stack bounds how long
	 * that takes, and one path never reports at all, so an outgoing call
	 * could sit in MEDUT_CALL_CALLING forever with no SIP message ever
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

	/* ── ICE servers (append-only ABI) ─────────────────────────────────── */
	/**
	 * Every STUN/TURN server to gather from, in preference order; the
	 * array is deep-copied at init.  NULL or 0 entries = fold in the legacy
	 * stun_server / turn_server fields above (TURN first).  Each server is
	 * resolved and asked on every gather, including the re-gather of an
	 * ICE restart, so credentials changed through
	 * medut_account_set_ice_servers() reach the next restart.
	 */
	const medut_ice_server_t     *ice_servers;
	uint32_t                        ice_server_count;
	medut_ice_transport_policy_t  ice_transport_policy; /* default ALL */

	/**
	 * Gather on every interface the platform can pin a socket to, not only
	 * the one the call signals from — cellular next to Wi-Fi on a phone, a
	 * VPN on a desktop (at most two extra; loopback, bridges and container
	 * interfaces are never used).  A handover then offers candidates that
	 * already exist instead of waiting on a fresh gather.  On Android the
	 * extra interface must be known through
	 * medut_network_set_interface_handle(); the Flutter plugin does that
	 * when MedutConfig.iceMultipath is set.  Experimental: verified on
	 * Linux; on Android the pinned socket got no replies on the device it
	 * was tried on (docs/guides/network_handover.md).  Default false.
	 */
	bool                            ice_multipath;

} medut_config_t;

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
	 * SIP transport protocol.  Defaults to MEDUT_TRANSPORT_UDP (0).
	 * Ignored when server_url is set (transport is derived from the URL scheme).
	 */
	medut_transport_t  transport;

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

	medut_media_enc_t  media_enc;   /* MEDUT_MEDIA_ENC_NONE / SDES / DTLS_SRTP */
	bool                 ice_enabled; /* false by default; set stun_server or
	                                     turn_server too, or calls offer only
	                                     local addresses (logged at create) */
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
	 * here before calling medut_account_register().
	 */
	medut_push_provider_t  push_provider;

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
	 *   cfg.audio_codecs[0]   = MEDUT_CODEC_PCMU;
	 *   cfg.audio_codecs[1]   = MEDUT_CODEC_OPUS;
	 *   cfg.audio_codec_count = 2;
	 */
	medut_codec_t  audio_codecs[8];
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
	medut_dtmf_mode_t dtmf_mode; /* default MEDUT_DTMF_RFC4733 (0) */

	/* ── ICE servers (append-only ABI) ─────────────────────────────────── */
	/** Per-account STUN/TURN list and policy; NULL / 0 = inherit the global
	 *  list, then the legacy single-server fields.  See medut_config_t. */
	const medut_ice_server_t     *ice_servers;
	uint32_t                        ice_server_count;
	medut_ice_transport_policy_t  ice_transport_policy;

} medut_account_config_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/** Zero-fill cfg and set version/struct_size correctly. Call before populating. */
MEDUT_EXPORT void medut_config_init(medut_config_t *cfg);

/**
 * Initialize the Medut stack. Call once per process before any other API.
 * Spawns re_main thread and event dispatch thread internally.
 * Returns MEDUT_OK or a negative MEDUT_ERR_* code.
 */
MEDUT_EXPORT int medut_init(const medut_config_t *cfg);

/**
 * True while the stack is initialized (between a successful medut_init()
 * and medut_shutdown()).  Thread-safe.
 *
 * The stack lives in the process, not in the caller's runtime.  A host that
 * can lose and rebuild its own runtime while the process stays alive — an
 * Android headless Flutter engine tearing down the Dart isolate between push
 * wakeups, a re-loaded plugin/scripting VM — comes back to a stack that is
 * still up and still registered.  Calling medut_init() again then returns
 * MEDUT_ERR_ALREADY; the correct recovery is to re-point the event sink at
 * the new runtime with medut_set_event_handler() and re-discover the live
 * accounts and calls with medut_account_foreach() / medut_call_foreach().
 */
MEDUT_EXPORT bool medut_is_initialized(void);

/**
 * Re-point the event sink at a new callback on an already-initialized stack.
 *
 * Use when the process outlives the consumer that installed cfg.event_cb (see
 * medut_is_initialized()).  Once this returns the old callback is neither
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
 * medut_call_foreach() and reads medut_call_get_state() to find a call
 * still RINGING, and medut_account_foreach() /
 * medut_account_get_reg_state() for registration state.
 *
 * @param cb                    New callback; NULL parks delivery (events keep
 *                              being dequeued and dropped, never queued up)
 * @param userdata              Passed to cb as-is
 * @param deliver_owned_events  Ownership mode for the new callback — same
 *                              contract as cfg.deliver_owned_events.  A
 *                              handler installed with true MUST release every
 *                              event it receives.
 * @return MEDUT_OK, or MEDUT_ERR_STATE if the stack is not initialized
 */
MEDUT_EXPORT int medut_set_event_handler(medut_event_cb_t cb,
                                             void *userdata,
                                             bool deliver_owned_events);

/**
 * Tear down the stack. Blocks until all internal threads have exited.
 * All active accounts and calls are forcibly terminated first.
 */
MEDUT_EXPORT void medut_shutdown(void);

/* ── Accounts ─────────────────────────────────────────────────────────────── */

/**
 * Create a SIP account. Does NOT register — call medut_account_register().
 * Thread-safe; may be called from any thread after medut_init().
 */
MEDUT_EXPORT int medut_account_create(const medut_account_config_t *cfg,
                             medut_account_handle_t *out);

/** Destroy account and all associated calls. Unregisters first if registered. */
MEDUT_EXPORT void medut_account_destroy(medut_account_handle_t acct);

/** Begin registration. Fires MEDUT_EV_REG_STATE events on state changes.
 *  REGISTERING → REGISTERED on success; a failure the SDK will retry reports
 *  MEDUT_REG_RECONNECTING, and only a failure it has given up on reports
 *  MEDUT_REG_FAILED. */
MEDUT_EXPORT int medut_account_register(medut_account_handle_t acct);

/** Unregister (sends REGISTER with Expires: 0). */
MEDUT_EXPORT int medut_account_unregister(medut_account_handle_t acct);

/**
 * Override the retry policy for this account. Takes effect on the next retry.
 * Overrides the global reg_retry_* fields in medut_config_t for this account only.
 * @param initial_ms   First retry delay in ms (e.g. 2000)
 * @param max_ms       Maximum retry delay cap in ms (e.g. 300000)
 * @param backoff      Delay multiplier per attempt (e.g. 2.0)
 * @param max_attempts Max attempts before giving up; 0 = retry forever
 */
MEDUT_EXPORT int medut_account_set_retry_policy(medut_account_handle_t acct,
                                                     uint32_t initial_ms,
                                                     uint32_t max_ms,
                                                     float    backoff,
                                                     uint32_t max_attempts);

/** Cancel a pending retry timer and reset the attempt counter.
 *  The app has taken over, so the recovery stops being the SDK's: an account
 *  that was RECONNECTING reports FAILED once the retry is cancelled — leaving
 *  it on "Reconnecting…" would promise an attempt that is never coming.
 *  Call medut_account_register() to restart. */
MEDUT_EXPORT int medut_account_cancel_retry(medut_account_handle_t acct);

/** Skip the current backoff delay and re-register immediately.
 *  Resets the attempt counter. No-op if the account is not in a retry loop. */
MEDUT_EXPORT int medut_account_retry_now(medut_account_handle_t acct);

/**
 * Replace the account's STUN/TURN server list and transport policy.
 *
 * Takes effect on the next gather: a new call, or the re-gather of an ICE
 * restart on a live call (network handover, media stall), which is how
 * short-lived TURN credentials (RFC 7635 / TURN REST) are refreshed without
 * re-creating the account.  `count` 0 clears the per-account list, so the
 * global list and then the legacy single-server fields apply again.  The
 * array is copied; the caller keeps ownership of its memory.
 */
MEDUT_EXPORT int medut_account_set_ice_servers(
	medut_account_handle_t acct,
	const medut_ice_server_t *servers, uint32_t count,
	medut_ice_transport_policy_t policy);

/**
 * Update the push token for an account at runtime (e.g. on OS token rotation).
 *
 * The new token is stored and the Contact URI params are updated.  A new
 * REGISTER is sent immediately unless the account is mid-call, mid-transaction,
 * or in retry backoff — in those cases the update is deferred to the next
 * natural re-registration.  Callers that need the token applied before the
 * next call should wait for MEDUT_EV_CALL_STATE ENDED first.
 *
 * Pass NULL to clear push params and re-register without pn-* Contact params.
 *
 * @param acct        Account handle (must have push_provider set in config)
 * @param push_token  New device token string, or NULL to clear
 * @return MEDUT_OK on success, MEDUT_ERR_STATE if account has no UA yet
 */
MEDUT_EXPORT int medut_account_set_push_token(medut_account_handle_t acct,
                                                   const char *push_token);

/**
 * Add a custom SIP header that is sent on REGISTER requests only.
 *
 * Use this for vendor push schemes that use non-standard headers (e.g.
 * "X-Push-Token", "X-Apple-Push-Bundle-Id") on hosted servers where you
 * cannot configure server-side push dispatch.  Unlike
 * medut_account_add_header(), this header is NOT included on INVITE,
 * BYE, or REFER — the token is not leaked to call peers.
 *
 * Multiple calls accumulate headers.  The list is append-only for the
 * lifetime of the account; recreate the account to clear it.
 *
 * @param name   Header field name  (e.g. "X-Push-Token")
 * @param value  Header field value (e.g. "a1b2c3...")
 */
MEDUT_EXPORT int medut_account_add_register_header(
        medut_account_handle_t acct,
        const char *name, const char *value);

/**
 * Add a custom SIP header to all outgoing requests for this account.
 * @param name   Header field name  (e.g. "X-Tenant-Id")
 * @param value  Header field value (e.g. "12345")
 */
MEDUT_EXPORT int medut_account_add_header(medut_account_handle_t acct,
                                const char *name, const char *value);

/**
 * Subscribe to presence state changes for a contact (SUBSCRIBE/NOTIFY).
 * Presence updates are delivered via MEDUT_EV_PRESENCE_STATE events.
 * @param target_uri  SIP URI of the contact to subscribe to
 * @return MEDUT_OK on success, or negative error code
 */
MEDUT_EXPORT int medut_account_subscribe_presence(medut_account_handle_t acct,
                                        const char *target_uri);

/**
 * Unsubscribe from presence state changes for a contact.
 * @param target_uri  SIP URI of the contact to unsubscribe from
 * @return MEDUT_OK on success, or negative error code
 */
MEDUT_EXPORT int medut_account_unsubscribe_presence(medut_account_handle_t acct,
                                          const char *target_uri);

/** Called once per live account by medut_account_foreach(). */
typedef void (*medut_account_iter_fn)(medut_account_handle_t acct, void *arg);

/**
 * Iterate all live accounts. Safe to call from any thread.
 * Do not create or destroy accounts from inside fn.
 *
 * Together with medut_account_get_aor() this lets a consumer that lost its
 * own handles — a rebuilt runtime attaching to a stack that is still up, see
 * medut_is_initialized() — re-derive them instead of creating duplicates.
 */
MEDUT_EXPORT void medut_account_foreach(medut_account_iter_fn fn, void *arg);

/**
 * Write the account's AOR ("sip:user@host") into buf as a NUL-terminated
 * string.  Identifies an account handle obtained from
 * medut_account_foreach().
 * @return MEDUT_OK, MEDUT_ERR_INVAL on bad args, or MEDUT_ERR_NOMEM
 *         if buf is too small (buf is left empty).
 */
MEDUT_EXPORT int medut_account_get_aor(medut_account_handle_t acct,
                                            char *buf, size_t sz);

/** Current registration state, as last reported by MEDUT_EV_REG_STATE.
 *  Useful on a cold start or after re-attaching, where no event has been seen
 *  yet.  Can return MEDUT_REG_RECONNECTING — see the enum. */
MEDUT_EXPORT medut_reg_state_t medut_account_get_reg_state(
        medut_account_handle_t acct);

/* ── Calls ────────────────────────────────────────────────────────────────── */

/**
 * Initiate an outgoing call. Returns immediately; fires MEDUT_EV_CALL_STATE
 * CALLING, then RINGING, then ESTABLISHED (or FAILED).
 */
MEDUT_EXPORT int medut_call_invite(medut_account_handle_t acct,
                         const char *uri,
                         medut_call_handle_t *out);

/** Answer an incoming call (received via MEDUT_EV_INCOMING_CALL). */
MEDUT_EXPORT int medut_call_answer(medut_call_handle_t call);

/** Terminate a call with BYE. */
MEDUT_EXPORT int medut_call_hangup(medut_call_handle_t call);

/**
 * Terminate a call with an explicit SIP status code.
 * For an unanswered incoming call this sends the given final response
 * (486 "Busy Here", 603 "Decline", ...); for an established call the
 * dialog is ended as usual (the code applies where the SIP state allows).
 * @param scode   SIP status code, e.g. 486 or 603. 0 = default behavior.
 * @param reason  Reason phrase; NULL = derived from scode.
 */
MEDUT_EXPORT int medut_call_reject(medut_call_handle_t call,
                                        uint16_t scode, const char *reason);

/** Put call on hold (re-INVITE with sendonly). */
MEDUT_EXPORT int medut_call_hold(medut_call_handle_t call);

/** Resume a held call (re-INVITE with sendrecv). */
MEDUT_EXPORT int medut_call_resume(medut_call_handle_t call);

/** Return true if the call is currently on local hold. */
MEDUT_EXPORT bool medut_call_is_held(medut_call_handle_t call);

/** Send DTMF digit via RFC 4733 RTP events. digit: '0'-'9', '*', '#', 'A'-'D'. */
MEDUT_EXPORT int medut_call_send_dtmf(medut_call_handle_t call, char digit);

/** Blind transfer via REFER. */
MEDUT_EXPORT int medut_call_transfer(medut_call_handle_t call, const char *uri);

/**
 * Add a custom SIP header to a specific call/dialog.
 * Headers are applied to subsequent re-INVITEs, BYE, and REFER messages
 * within this dialog. Must be called while the call is active.
 * @param name   Header field name  (e.g. "X-Call-Id")
 * @param value  Header field value (e.g. "12345")
 */
MEDUT_EXPORT int medut_call_add_header(medut_call_handle_t call,
                             const char *name, const char *value);

/**
 * Attended transfer: send REFER w/ Replaces on call_a, bridging it to call_b.
 * call_a is the call to transfer away; call_b is the already-established
 * consultation call whose dialog info is embedded in Replaces.
 */
/**
 * Follow an incoming REFER (see MEDUT_EV_TRANSFER_REQUEST).
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
 * @return MEDUT_OK, MEDUT_ERR_INVAL for a NULL handle,
 *         MEDUT_ERR_STATE when no transfer is pending on this call, or a
 *         positive errno if the call could not be placed (the transferor is
 *         sent the failure NOTIFY in that case).
 */
MEDUT_EXPORT int medut_call_transfer_accept(medut_call_handle_t  call,
                                                 medut_call_handle_t *out);

/**
 * Refuse an incoming REFER (see MEDUT_EV_TRANSFER_REQUEST).
 *
 * Sends the terminating NOTIFY so the transferor stops waiting, and leaves
 * this call established — the user is still on the line.
 *
 * @param scode   SIP status to report, 400-699. 603 Decline is the usual
 *                answer for "the user said no"; 486 for "busy".
 * @param reason  Reason phrase, or NULL for "Declined".
 *
 * @return MEDUT_OK, MEDUT_ERR_INVAL for a NULL handle or an out-of-range
 *         status, or MEDUT_ERR_STATE when no transfer is pending.
 */
MEDUT_EXPORT int medut_call_transfer_reject(medut_call_handle_t call,
                                                 uint16_t scode,
                                                 const char *reason);

MEDUT_EXPORT int medut_call_attended_transfer(medut_call_handle_t call_a,
                                    medut_call_handle_t call_b);

/** Called once per active call by medut_call_foreach(). */
typedef void (*medut_call_iter_fn)(medut_call_handle_t call, void *arg);

/** Iterate all active calls. Safe to call from any thread. */
MEDUT_EXPORT void medut_call_foreach(medut_call_iter_fn fn, void *arg);

/** The account this call belongs to, or NULL if it has none. */
MEDUT_EXPORT medut_account_handle_t medut_call_get_account(
        medut_call_handle_t call);

/** Current call state. Returns MEDUT_CALL_ENDED for a NULL handle. */
MEDUT_EXPORT medut_call_state_t medut_call_get_state(
        medut_call_handle_t call);

/**
 * The call's current ICE liveness state (see medut_ice_state_t).  Safe from
 * any thread; MEDUT_ICE_NEW for a call without ICE or before gathering.
 */
MEDUT_EXPORT medut_ice_state_t medut_call_ice_state(
        medut_call_handle_t call);

/* ── SIP MESSAGE ─────────────────────────────────────────────────────────── */

/**
 * Send a SIP MESSAGE (instant message) out of dialog.
 * content_type defaults to "text/plain" if NULL.
 * Fires MEDUT_EV_MESSAGE on the remote end when received.
 */
MEDUT_EXPORT int medut_message_send(medut_account_handle_t account,
                          const char *to_uri,
                          const char *body,
                          const char *content_type);

/* ── Presence ────────────────────────────────────────────────────────────── */

/**
 * Publish presence status for the account (PUBLISH request).
 * Fires MEDUT_EV_PRESENCE_STATE on subscribed watchers.
 */
MEDUT_EXPORT int medut_account_publish_presence(medut_account_handle_t account,
                                      medut_presence_status_t status);

/* ── 100rel / PRACK ──────────────────────────────────────────────────────── */

/**
 * Set the RFC 3262 100rel mode for an account.
 * Must be called before medut_call_invite / medut_call_answer.
 */
MEDUT_EXPORT int medut_account_set_100rel(medut_account_handle_t account,
                                medut_100rel_mode_t mode);

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
 *   app capture thread   -> medut_audio_external_push()  -> far end
 *   app playback thread  <- medut_audio_external_pull()  <- far end
 *
 * Turning it off restores the platform device.  Takes effect immediately,
 * including on calls that are already up, so it is safe to switch mid-call.
 *
 * Not sticky across a restart: medut_init() re-derives the device from the
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
 * medut_set_aec_mode(MEDUT_AEC_SUPPRESSOR) — on every platform, desktop
 * included, and medut_set_aec(true) is not that request.  It is switched
 * off again when the platform device comes back, so the two cancellers never
 * stack.  NS and AGC follow cfg.ns / cfg.agc while the app owns the device,
 * unless its engine reports the platform's voice path under it
 * (medut_audio_external_set_voice_path), and stop again when the
 * platform's own come back.  All of it applies to calls already up, from
 * their next frame.
 *
 * Returns 0, or an errno on failure.
 */
MEDUT_EXPORT int medut_audio_use_external(bool enable);

/**
 * Give the stack captured microphone audio.  This is what the far end hears.
 *
 * [pcm] is S16LE interleaved, [nsamp] total samples (frames x channels), at the
 * rate and channel count from medut_audio_external_format().  Any buffer size
 * is accepted; the stack re-frames internally.
 *
 * Call from the app's capture thread.  Returns 0, ENODEV when no call is
 * capturing (push between calls is not an error worth acting on), or an errno.
 */
MEDUT_EXPORT int medut_audio_external_push(const int16_t *pcm, size_t nsamp);

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
MEDUT_EXPORT int medut_audio_external_pull(int16_t *pcm, size_t nsamp);

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
MEDUT_EXPORT int medut_audio_external_format(uint32_t *srate, uint8_t *ch,
                                                  uint32_t *ptime);

/** True while a call is capturing or playing through the app-owned device. */
MEDUT_EXPORT bool medut_audio_external_is_active(void);

/**
 * Say whether the app-owned device captures through the platform's voice
 * path — iOS VoiceProcessingIO, Android's VOICE_COMMUNICATION source — so
 * the device has already cancelled echo and run its own NS and AGC before
 * medut_audio_external_push() sees a sample.
 *
 * While true, the SDK's noise gate and AGC stay off, as they do on its own
 * mobile drivers: stacked on the platform's they only cost speech.  The
 * suppressor is not affected; on an app-owned device it runs only when asked
 * for with medut_set_aec_mode(MEDUT_AEC_SUPPRESSOR).  False, the default,
 * means a raw microphone.
 *
 * Call it from the device's start and stop: true once capture is running,
 * false when it stops.  Never from the audio callback — it logs.  Safe from
 * any other thread, and does not wait on the SIP thread.  Reset to false by
 * medut_audio_use_external(false) and by medut_init(); ignored while the
 * SDK's own device is in use.
 */
MEDUT_EXPORT void medut_audio_external_set_voice_path(bool active);

/* ── Audio device enumeration ─────────────────────────────────────────────── */

typedef struct {
	char name[128];         /* device name passed to set_input/output_device */
	char description[256];  /* human-readable label (may be empty) */
	bool is_default;        /* true for the platform default device */
} medut_audio_device_t;

/**
 * Fill devices[] with available audio input devices, up to max_count entries.
 * Returns the number of entries written (0 if no devices found yet).
 * Call after medut_init(); device lists may be empty until the audio
 * module has finished async enumeration.
 */
MEDUT_EXPORT int medut_audio_list_input_devices(medut_audio_device_t *devices,
                                                     int max_count);

/** Same for audio output (speaker/playback) devices. */
MEDUT_EXPORT int medut_audio_list_output_devices(medut_audio_device_t *devices,
                                                      int max_count);

/* ── Audio processing — runtime toggles ──────────────────────────────────── */

/*
 * medut_set_aec, _aec_mode, _ns and _agc reach calls that are already up,
 * on their next audio frame — the filters stay in each call's chain and read
 * the setting per frame.  They dispatch to the SIP thread, so none of the
 * four is for an audio thread.
 */

/**
 * Enable or disable echo cancellation.
 * Re-enables the echo control medut_init() settled on: the configured
 * aec_mode, or the suppressor if WEBRTC fell back.
 * set_aec(false) → AEC_OFF; set_aec(true) → restores init mode.
 * Back-compat: behavior is identical to the former bool aec API.
 * Not a request for the suppressor on an app-owned device; that is
 * medut_set_aec_mode(MEDUT_AEC_SUPPRESSOR).
 */
MEDUT_EXPORT void medut_set_aec(bool enable);

/**
 * Switch the AEC backend.  Only AEC_OFF ↔ init_mode transitions are valid
 * at runtime.  Switching between SUPPRESSOR and WEBRTC at runtime returns
 * EINVAL.  WEBRTC returns ENOTSUP on mobile builds and when
 * MEDUT_WITH_WEBRTC_AEC was not set at build time.
 *
 * WEBRTC mode with AEC off keeps WebRTC's noise suppressor running if ns is
 * on.  While the app owns the device (medut_audio_use_external), on any
 * platform, SUPPRESSOR here is also the app asking for the suppressor there —
 * the only way it runs on an app-owned device.  The request lapses when the
 * platform device comes back.
 */
MEDUT_EXPORT int medut_set_aec_mode(medut_aec_mode_t mode);

/**
 * Tune the built-in TX echo suppressor aggressiveness.  SUPPRESSOR mode only.
 * 0.0 = no TX suppression (passes through even when far end is loud).
 * 1.0 = maximum suppression (default — −16.5 dB floor on TX when RX active).
 * The floor is reached with the far end at −21 dBFS; below that the
 * attenuation eases off in dB, to none at −44 dBFS.
 * Takes effect on the next audio frame via atomic store; safe from any thread.
 */
MEDUT_EXPORT void medut_set_aec_suppression_level(float level);

/**
 * Enable/disable noise suppression globally (takes effect next frame).
 *
 * In WEBRTC mode this is WebRTC's noise suppressor.  Otherwise it is the
 * built-in gate, which turns the pauses down by 20 dB and leaves speech and
 * the noise under it alone.
 *
 * Never on top of the phone's own: while Android captures through the
 * VOICE_COMMUNICATION preset, or iOS through VoiceProcessingIO, the platform
 * already suppresses noise and this stays off (logged).  It applies again
 * while the app owns the device — unless the app reports its own voice path
 * (medut_audio_external_set_voice_path) — or on an Android device that
 * refuses the preset.
 */
MEDUT_EXPORT void medut_set_ns(bool enable);

/**
 * Enable/disable automatic gain control globally (takes effect next frame).
 * Like medut_set_ns(), never on top of the phone's own voice path.
 */
MEDUT_EXPORT void medut_set_agc(bool enable);

/**
 * Set manual microphone (TX) gain in dB.  Clamped to [-20, +20].
 * 0.0 = unity — fast-path bypass, no per-sample work.
 * Applied before NS/AGC/AEC in the encode chain (raw pre-boost).
 * Takes effect on the next audio frame via atomic store; safe from any thread.
 */
MEDUT_EXPORT void medut_set_mic_gain_db(float db);

/**
 * Set manual speaker (RX) gain in dB.  Clamped to [-20, +20].
 * 0.0 = unity — fast-path bypass.
 * Applied after the jitter buffer, before playback.
 * Takes effect on the next audio frame via atomic store; safe from any thread.
 */
MEDUT_EXPORT void medut_set_speaker_gain_db(float db);

/**
 * Change DSCP/TOS on the RTP socket of an active call.
 * Common values: 46 (EF — voice), 34 (AF41 — video), 0 (best-effort).
 * Takes effect immediately on the next outgoing RTP packet.
 */
MEDUT_EXPORT int medut_call_set_dscp_rtp(medut_call_handle_t call,
                                              uint8_t dscp);

/**
 * Update jitter buffer bounds.  Takes effect on calls established after
 * this call — existing streams keep their current adaptive depth.
 * Pass 0 for either bound to restore its default (see jitter_buffer_min_ms
 * and jitter_buffer_max_ms).  Keeps the current jbuf_type.
 */
MEDUT_EXPORT void medut_set_jitter_buffer(uint32_t min_ms, uint32_t max_ms);

/** Set jitter buffer type (adaptive or fixed). Takes effect on new calls. */
MEDUT_EXPORT void medut_set_jitter_buffer_type(medut_jbuf_type_t type);

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
 * @return 0, or MEDUT_ERR_INVAL / MEDUT_ERR_STATE.
 */
MEDUT_EXPORT int medut_call_set_rtp_timeout(medut_call_handle_t call,
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
 * cfg.adaptive_bitrate to false, or use medut_set_adaptive_bitrate(), if
 * the app wants to own the rate.
 *
 * @return 0, or MEDUT_ERR_INVAL / MEDUT_ERR_STATE.
 */
MEDUT_EXPORT int medut_call_set_bitrate(medut_call_handle_t call,
                                             uint32_t bitrate_bps);

/**
 * Enable or disable link-adaptive bitrate at runtime, with bounds.
 *
 * Pass 0 for either bound to keep the value already configured.  Disabling
 * leaves every call at whatever rate it currently has; call
 * medut_call_set_bitrate(call, 0) to restore the negotiated rate.
 */
MEDUT_EXPORT void medut_set_adaptive_bitrate(bool enabled,
                                                  uint32_t min_bps,
                                                  uint32_t max_bps);

/**
 * Send a keepalive/reachability probe (SIP OPTIONS) for one account now.
 *
 * The same probe the keepalive_interval timer sends, on demand — useful from
 * an app-foreground or push-wake handler, where the question "is my
 * registration still reachable?" is worth answering before the user places a
 * call.  The result arrives as MEDUT_EV_REG_STATE: nothing changes on
 * success, and on failure the account goes to FAILED and (when
 * cfg.keepalive_reregister is set) re-REGISTERs.
 *
 * @return 0, or MEDUT_ERR_INVAL / MEDUT_ERR_STATE.
 */
MEDUT_EXPORT int medut_account_keepalive_now(
                                        medut_account_handle_t account);

/* ── Audio mute / device control ─────────────────────────────────────────── */

/** Mute/unmute the microphone (TX path) for a call. */
MEDUT_EXPORT int medut_audio_mute(medut_call_handle_t call, bool mute);

/** Return true if TX audio is currently muted on this call. */
MEDUT_EXPORT bool medut_audio_is_muted(medut_call_handle_t call);

/** Mute/unmute the speaker (RX path) for a call — silences incoming audio. */
MEDUT_EXPORT int medut_audio_mute_rx(medut_call_handle_t call, bool mute);

/** Set the system audio input device by name. NULL = platform default. */
MEDUT_EXPORT int medut_audio_set_input_device(const char *name);

/** Set the system audio output device by name. NULL = platform default. */
MEDUT_EXPORT int medut_audio_set_output_device(const char *name);

/* ── Media tap ────────────────────────────────────────────────────────────── */

/**
 * Install a PCM tap on an active call.
 * cb is called from the audio thread for both RX (decoded) and TX (pre-encode)
 * directions. Must be non-blocking — heavy work must go to your own thread.
 * Pass NULL cb to remove the tap.
 */
MEDUT_EXPORT int medut_call_set_media_tap(medut_call_handle_t   call,
                                medut_media_tap_cb_t  cb,
                                void                   *userdata);

/* ── Audio recording ──────────────────────────────────────────────────────── */

/**
 * Start recording call audio to a single mixed WAV file (PCM S16LE).
 * Both the received (RX) and sent (TX) audio are clip-summed into one stream.
 * The WAV header is written on the first audio frame and finalized on stop.
 * Returns EALREADY if recording is already active.
 */
MEDUT_EXPORT int medut_call_record_start(medut_call_handle_t call,
                                              const char *path);

/** Stop recording and finalize WAV headers. Idempotent. */
MEDUT_EXPORT int medut_call_record_stop(medut_call_handle_t call);

/* ── Call info ────────────────────────────────────────────────────────────── */

/**
 * Static and slow-moving facts about a call, as opposed to the per-tick media
 * numbers in medut_call_get_stats().
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
	 *  app asked for — is medut_call_is_held(); the two are independent
	 *  and can both be true. */
	bool     is_remote_hold;
	uint16_t sip_status;             /* last SIP status; 0 while up        */
	uint64_t duration_ms;            /* since ESTABLISHED; 0 before that   */
	/** INVITE → answer. Whole seconds scaled to ms: the stack measures
	 *  this to second granularity, so it steps in 1000s. */
	uint32_t setup_duration_ms;
	uint32_t line_number;            /* 1-based; stable for the call       */
	medut_transport_t  transport;  /* transport this dialog signals over */
	medut_call_state_t state;      /* same value as the last CALL_STATE  */
} medut_call_info_t;

/**
 * Fill `out` with the current facts about a call.
 *
 * Safe to call from any thread and at any point in the call's life, including
 * after it has ended, for as long as the handle is valid.
 *
 * @return MEDUT_OK, or MEDUT_ERR_INVAL for a NULL argument.
 */
MEDUT_EXPORT int medut_call_get_info(medut_call_handle_t  call,
                                          medut_call_info_t   *out);

/* ── Stats ────────────────────────────────────────────────────────────────── */

/**
 * Synchronously retrieve current stats for a call.
 * Also delivered automatically via MEDUT_EV_MEDIA_STATS if
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
MEDUT_EXPORT int medut_call_get_stats(medut_call_handle_t     call,
                            medut_ev_media_stats_t *out);

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
 * MEDUT_EV_NETWORK events.
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
 * @return MEDUT_OK, or MEDUT_ERR_STATE if the SDK is not initialized.
 */
MEDUT_EXPORT int medut_network_changed(void);

/**
 * Tell the SDK that interface `ifname` belongs to platform network `handle`
 * (Android: Network.getNetworkHandle()), or forget it with
 * medut_network_clear_interface_handle().
 *
 * With a handle on file for a second interface (typically cellular while
 * Wi-Fi is the default), every new ICE generation gathers on that interface
 * too, with its sockets pinned to that network, so a handover offers
 * candidates that already exist instead of waiting on a fresh gather.  The
 * plugin feeds this from a ConnectivityManager NetworkRequest; no-op on
 * platforms that pin by interface name (iOS) or cannot pin at all.
 */
MEDUT_EXPORT int medut_network_set_interface_handle(const char *ifname,
                                                         uint64_t handle);
MEDUT_EXPORT int medut_network_clear_interface_handle(const char *ifname);

/**
 * Change the built-in interface poll interval at runtime.
 * @param seconds  Poll period; 0 disables polling entirely.
 */
MEDUT_EXPORT int medut_network_set_monitor_interval(uint32_t seconds);

/**
 * Adjust handover behaviour at runtime (overrides the cfg.net_* fields).
 * @param reinvite_calls    Re-INVITE active calls onto the new address.
 * @param hangup_on_failure Hang up calls whose media could not be migrated.
 */
MEDUT_EXPORT int medut_network_set_handover_policy(bool reinvite_calls,
                                                        bool hangup_on_failure);

/**
 * Copy the local IP the SDK is currently using into buf.
 * Writes an empty string when no usable address exists.
 * @return MEDUT_OK, or MEDUT_ERR_INVAL / MEDUT_ERR_STATE.
 */
MEDUT_EXPORT int medut_network_local_addr(char *buf, size_t sz);

/** False while the device has no usable (non-loopback) local address. */
MEDUT_EXPORT bool medut_network_is_up(void);

/* ── pcap ─────────────────────────────────────────────────────────────────── */

/**
 * Start capturing SIP + RTP to a Wireshark-compatible pcap file.
 * Writes synthetic Ethernet/IP/UDP headers around each SIP message.
 */
MEDUT_EXPORT int medut_pcap_start(const char *path);

/** Stop capture and flush/close the pcap file. */
MEDUT_EXPORT int medut_pcap_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDUT_H */
