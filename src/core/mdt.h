/**
 * @file mdt.h  The Medut stack: what it provides the SDK above it.
 *
 * Medut's public API, its policy (registration retry, SRV failover,
 * keepalive, network handover, degraded-link adaptation, stats/MOS) and its
 * event queue live above this line.  Below it, the Medut stack on libre
 * owns the SIP user agent (src/sip/), the media pipeline (src/media/), codecs
 * (src/codec/), media encryption (src/menc/), ICE (src/ice/) and audio
 * devices (src/audio/, platform/).
 *
 * Rules for this header:
 *   - only libre types, medut.h types and the opaque handles below;
 *   - every function runs on re_main unless its comment says otherwise;
 *   - the stack reports what happens through mdt_events.h, never by
 *     posting SDK events itself.
 *
 * Handles: `struct mdt_ua` is one SIP user agent (one per medut account),
 * `struct mdt_call` one call.  Both are opaque here; their identity is what
 * bsdk_account_find() / bsdk_call_find() match on.
 */

#ifndef MDT_H
#define MDT_H

#include <re.h>
#include "../../include/medut.h"

struct mdt_ua;
struct mdt_call;
struct dnsc;
struct sip;
struct mbuf;
struct sip_msg;

/* Response to an out-of-dialog request the SDK sent through the engine
 * (keepalive / reachability OPTIONS).  msg is NULL when err is set. */
typedef void (mdt_resp_h)(int err, const struct sip_msg *msg, void *arg);

/* ── Lifecycle (core.c) ────────────────────────────────────────────────────
 * Called in this order from medut_init(); each close mirrors its init.  The
 * split points are where the SDK interleaves its own subsystems, so the order
 * of operations is the same as before the seam. */

/* Bring the engine up as far as the SIP stack: configuration, network and
 * DNS state.  conf_dir is a writable directory the engine may keep state in
 * (instance UUID).  After this, mdt_net_dnsc() is valid. */
int  mdt_stack_init(const char *conf_dir);

/* SIP transports, TLS, the user-agent stack.  Runs after bsdk_dns_init(). */
int  mdt_sip_init(void);

/* Start/stop delivering engine events (core/mdt_events.h).  Start runs after
 * the SDK event thread exists. */
int  mdt_events_start(void);
void mdt_events_stop(void);

/* Codecs, media encryption, audio drivers.  Runs after mdt_events_start();
 * the ICE media-NAT (bsdk_ice_module_init) registers right after it. */
int  mdt_media_init(void);

/* Teardown, in the order medut_shutdown() calls them. */
void mdt_sip_quiesce(void);     /* re thread, UAs freed: listeners off */
bool mdt_sip_busy(void);        /* an un-REGISTER/-SUBSCRIBE still out */
void mdt_ua_close_all(void);        /* all UAs gone, SIP stack closed      */
void mdt_app_unload(void);      /* application-level modules unloaded  */
void mdt_media_close(void);     /* codecs / menc / drivers unloaded    */
void mdt_stack_close(void);           /* the rest of the engine              */

/* Logging: forward the engine's own log output to bsdk_log_deliver()
 * (core/mdt_events.h) — and nowhere else. */
void mdt_log_init(void);
void mdt_log_close(void);

/* ── SIP stack & network (trace.c, dns.c, netmon.c) ────────────────────── */

struct sip  *mdt_sip_stack(void);   /* for the SIP trace hook; may be NULL */
struct dnsc *mdt_net_dnsc(void);    /* borrowed; valid between init/close  */

/* Re-create every SIP transport on the current local addresses (handover). */
int  mdt_sip_reset_transp(void);

/* The engine's local-address list, which decides what transports bind to. */
bool mdt_net_ifaddr_ok(const char *ifname, const struct sa *sa);
bool mdt_net_laddr_apply(net_ifaddr_h *ifh, void *arg);
int  mdt_net_add_address(const struct sa *sa);
int  mdt_net_rm_address(const struct sa *sa);
int  mdt_net_af(void);
const struct sa *mdt_net_laddr_af(int af);
/* Reload nameservers from the system (only when the app configured none). */
void mdt_net_dns_refresh(void);

/* ── Accounts (account.c, headers.c, presence.c, message.c) ────────────── */

/* Everything an engine needs to build one UA, resolved by account.c from
 * account config > global config > what the server URL implies. */
struct mdt_ua_prm {
	const char *aor;           /* sip:user@host[:port];transport=…[;rwait=N] */
	const char *auth_user;
	const char *auth_pass;
	const char *display_name;
	uint32_t    reg_expires;
	const char *outbound;      /* outbound proxy URI, NULL for none        */
	medut_media_enc_t media_enc;
	bool        ice;           /* use the "ice" media-NAT                   */
	bool        sip_outbound;  /* RFC 5626 (reg-id, +sip.instance)          */
	bool        rtcp_mux;
	const char *stun_uri;      /* the one NAT server slot (TURN preferred) */
	const char *stun_user;
	const char *stun_pass;
	const char *codecs;        /* comma-separated codec names              */
	medut_dtmf_mode_t dtmf_mode;
};

/* Create a UA.  Returns MEDUT_ERR_STATE when the engine cannot honour the
 * parameters (codec / media encryption / ICE not available) rather than
 * silently degrading. */
int  mdt_ua_alloc(struct mdt_ua **eap, const struct mdt_ua_prm *prm);
/* Hang up every call of the UA and free it. */
void mdt_ua_free(struct mdt_ua *ea);

int  mdt_ua_register(struct mdt_ua *ea);
void mdt_ua_unregister(struct mdt_ua *ea);
bool mdt_ua_isregistered(const struct mdt_ua *ea);
/* Our end of the connection the live registration was last answered on (the
 * real local address, also for WS/WSS); NULL if not registered. */
const struct sa *mdt_ua_reg_conn_laddr(const struct mdt_ua *ea);
/* Refresh the binding in place (push re-registration).  0 or EALREADY when a
 * refresh went out or is already in flight; anything else means "not
 * possible, send a full REGISTER". */
int  mdt_ua_refresh_register(struct mdt_ua *ea);
int  mdt_ua_set_outbound(struct mdt_ua *ea, const char *uri);
/* OPTIONS to `uri` through the account's outbound route. */
int  mdt_ua_options_send(struct mdt_ua *ea, const char *uri,
                           mdt_resp_h *resph, void *arg);
bool mdt_ua_has_calls(const struct mdt_ua *ea);
/* Whether the UA's requests go to `host`: the host of its outbound route
 * when it has one, else the host of its AOR.  Case-insensitive, as host
 * names are.  Used to decide TLS verification per server. */
bool mdt_ua_sends_to(const struct mdt_ua *ea, const char *host);
int  mdt_ua_add_header(struct mdt_ua *ea, const char *name,
                         const char *value);
int  mdt_ua_set_100rel(struct mdt_ua *ea, medut_100rel_mode_t mode);

/* Presence and MESSAGE (sip/presence.c, sip/message.c).  MWI needs no
 * call: a registered UA subscribes by itself (sip/mwi.c). */
int  mdt_presence_publish(struct mdt_ua *ea, medut_presence_status_t st);
/* Watch `uri`'s presence from `ea` (BLF); changes come as bsdk_ev_presence. */
int  mdt_presence_subscribe(struct mdt_ua *ea, const char *uri);
int  mdt_presence_unsubscribe(struct mdt_ua *ea, const char *uri);
int  mdt_message_send(struct mdt_ua *ea, const char *to_uri,
                      const char *body);

/* ── Calls (call.c, transfer.c, netmon.c) ──────────────────────────────── */

/* Dial `dial` (an extension, user@host or a full URI) from `ea`.  The engine
 * completes it into a request URI.  On success *ecp is the new call. */
int  mdt_call_connect(struct mdt_call **ecp, struct mdt_ua *ea,
                      const char *dial);
int  mdt_call_answer(struct mdt_ua *ea, struct mdt_call *ec);
/* scode 0 = engine default (BYE / 486 / CANCEL as the state requires). */
void mdt_call_hangup(struct mdt_ua *ea, struct mdt_call *ec,
                     uint16_t scode, const char *reason);
int  mdt_call_hold(struct mdt_call *ec, bool hold);
int  mdt_call_send_digit(struct mdt_call *ec, char key);
int  mdt_call_transfer(struct mdt_call *ec, const char *uri);
int  mdt_call_replace_transfer(struct mdt_call *ec, struct mdt_call *target);

/* Following an incoming REFER: allocate a call that inherits `xcall`'s
 * transfer notifier, dial it, and report progress on the old call. */
int  mdt_call_xfer_alloc(struct mdt_call **ecp, struct mdt_ua *ea,
                         struct mdt_call *xcall);
int  mdt_call_dial(struct mdt_call *ec, const char *uri);
int  mdt_call_notify_sipfrag(struct mdt_call *ec, uint16_t scode,
                             const char *reason);
void mdt_call_free(struct mdt_call *ec);

void mdt_call_info(const struct mdt_call *ec, medut_call_info_t *o);
struct mdt_ua *mdt_call_acct(const struct mdt_call *ec);
bool mdt_call_is_onhold(const struct mdt_call *ec);       /* local  */
bool mdt_call_is_remote_hold(const struct mdt_call *ec);  /* peer's */
/* May a re-INVITE/UPDATE be sent now (dialog confirmed, nothing pending)? */
bool mdt_call_refresh_allowed(const struct mdt_call *ec);
/* Re-offer media from `laddr` (non-ICE handover). */
int  mdt_call_reset_transp(struct mdt_call *ec, const struct sa *laddr);
/* Withdraw our unanswered re-INVITE.  True when one was abandoned. */
bool mdt_call_reinvite_abandon(struct mdt_call *ec);
const struct sa *mdt_call_laddr(const struct mdt_call *ec);
/* Where audio is sent: the stream's remote address, else the SDP's. */
const struct sa *mdt_call_raddr(const struct mdt_call *ec);

/* ── Media (audio.c, stats.c, adapt.c, sdp.c) ──────────────────────────── */

int  mdt_call_mute(struct mdt_call *ec, bool mute);
bool mdt_call_is_muted(const struct mdt_call *ec);
int  mdt_call_mute_rx(struct mdt_call *ec, bool mute);
int  mdt_call_set_dscp_rtp(struct mdt_call *ec, uint8_t dscp);
int  mdt_call_set_rtp_timeout(struct mdt_call *ec, uint32_t ms);
/* True while a stream's media encryption has not come up (DTLS-SRTP). */
bool mdt_call_menc_pending(const struct mdt_call *ec);
uint32_t mdt_call_rx_packets(const struct mdt_call *ec);
/* The negotiated TX codec name, NULL before negotiation. */
const char *mdt_call_codec_name(const struct mdt_call *ec);

/* The negotiated remote format of the audio stream. */
struct mdt_rfmt {
	const char *codec;      /* "opus", "PCMU", …                          */
	const char *params;     /* the peer's fmtp, may be NULL               */
	bool        encupd;     /* the encoder can be re-parametrised live    */
};
int  mdt_call_rfmt(const struct mdt_call *ec, struct mdt_rfmt *rf);
/* Re-run the encoder update with `params` as the remote fmtp.  loss_pct >= 0
 * also sets the Opus expected-loss (FEC redundancy) the update applies. */
int  mdt_call_encoder_set(struct mdt_call *ec, const char *params,
                          int32_t loss_pct);
/* Reset the Opus expected-loss for encoders opened from now on. */
void mdt_media_expected_loss(int32_t loss_pct);

/* One consistent read of a call's audio-stream counters (stats.c). */
struct mdt_media_snap {
	uint32_t tx_packets, rx_packets, tx_bytes, rx_bytes, tx_err, rx_err;
	uint32_t tx_bitrate, rx_bitrate;          /* bps, current             */
	double   tx_avg_bitrate, rx_avg_bitrate;  /* bps, session             */

	const char *codec;                        /* TX codec, NULL if none   */
	uint32_t codec_srate, codec_crate;
	uint8_t  codec_ch;
	const char *rx_codec;                     /* RX codec, NULL if none   */

	bool     jb_valid;
	bool     jb_adaptive;
	uint32_t jb_delay_ms, jb_packets, jb_late, jb_overflow, jb_flush;
	uint32_t jb_jitter_ms, jb_lost, jb_get;

	bool     fec_valid;                       /* Opus decoder counters    */
	uint32_t fec_frames, plc_frames;

	bool     rtcp_valid;
	uint32_t rtt_us, rx_jit_us, tx_jit_us;
	uint32_t tx_ehsn, tx_sent, rx_sent;
	int32_t  tx_lost, rx_lost;

	uint32_t ssrc_tx, ssrc_rx;
	int      pt;
	bool     raddr_valid;                     /* the stream has one       */
	struct sa raddr;                          /* signalled / learned peer */
};
int  mdt_call_media_snap(const struct mdt_call *ec, struct mdt_media_snap *s);

/* Jitter buffer type and bounds for streams opened from now on. */
void mdt_jbuf_configure(medut_jbuf_type_t type, uint32_t min_ms,
                        uint32_t max_ms);

/* ── Audio devices (audio.c) ───────────────────────────────────────────── */

int  mdt_audio_set_device(bool input, const char *name);
/* Switch every stream (and future ones) to the app-owned device or back to
 * the platform one.  ENOENT when there is no platform device to go back to. */
int  mdt_audio_use_external(bool enable);
bool mdt_audio_is_external(void);
/* Select a registered audio driver (mdt_ausrc + mdt_auplay of that name) for every
 * stream and future ones.  ENOENT when no such driver is registered. */
int  mdt_audio_use_driver(const char *name);
int  mdt_audio_list_devices(bool input, medut_audio_device_t *devices,
                            int max_count);

#endif /* MDT_H */
