/**
 * @file ice_internal.h  echo-ice: EchoSDK's own ICE agent (private header)
 *
 * Replaces baresip's `ice` media-NAT module and src/ice_shim.c with an agent
 * shaped like libwebrtc's: RFC 7675 consent freshness, continual connectivity
 * checks, one socket per interface, several STUN/TURN servers, relay-only
 * policy, and generation-based ICE restart with the previous pair carrying
 * media until the new one is proven.
 *
 * Layering rule: every engine file (ice_cand.c ice_sdp.c ice_pair.c
 * ice_check.c ice_stund.c ice_select.c ice_route.c ice_gather.c ice_port.c)
 * includes only libre headers and this file — no baresip.h, no g_bsdk, no
 * bsdk_*.  Tunables arrive through struct icea_conf.  Only ice_mod.c speaks
 * to baresip and the rest of the SDK.
 *
 * Names are prefixed icea_ because <re.h> pulls re_trice.h, whose
 * struct ice_cand / ice_candpair / ice_lcand would otherwise collide.
 *
 * Threads: all agent state lives on re_main.  The one exception is the wire
 * send helper (ice_route.c), which baresip runs on the audio encoder thread
 * for RTP; it reads only the mutex-guarded icea_txsel snapshot.
 */

#ifndef ECHOSDK_ICE_INTERNAL_H
#define ECHOSDK_ICE_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <re.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ─────────────────────────────────────────────────────────────
 *
 * Timing follows libwebrtc (p2p/base/p2p_constants.h) unless the RFC number
 * is stricter.  Every value is reachable through struct icea_conf so the
 * harness can compress time.
 */

/* udp_helper layers on the RTP/RTCP sockets.  baresip's dtls_srtp sits at
 * 20 (LAYER_DTLS / LAYER_SRTP), bundle at 40; receive runs low → high, send
 * runs high → low, so 0 puts us first after the wire and last before it. */
#define ICEA_LAYER_ICE      0
#define ICEA_LAYER_TURN   (-10)   /* libre turnc below us, as modules/ice did */

#define ICEA_PING_WEAK_MS               48     /* transport tick while weak     */
#define ICEA_PING_STRONG_MS            480     /* transport tick while strong   */
#define ICEA_PAIR_STABILIZING_MS       900     /* selected pair, not yet stable */
/* Keepalive on a stable selected pair.  Its response is what refreshes
 * `receiving` while the stream carries no media, so it has to sit clear of
 * ICEA_RECEIVING_TIMEOUT_MS by more than one rtt — libwebrtc's own 2500
 * leaves no margin at all once a timer fires late. */
#define ICEA_PAIR_STABLE_MS           2000     /* selected pair, stable         */
#define ICEA_BACKUP_PING_MS          25000     /* non-selected writable pairs   */
#define ICEA_WRITE_CONNECT_FAILURES      5     /* unanswered pings ...          */
#define ICEA_WRITE_CONNECT_TIMEOUT_MS 5000     /* ... over this long → unreliable */
#define ICEA_WRITE_TIMEOUT_MS        15000     /* → timeout                     */
#define ICEA_RECEIVING_TIMEOUT_MS     2500     /* no STUN/data → not receiving  */
#define ICEA_DEAD_MS                 30000     /* timeout + silence → dead      */
#define ICEA_CONSENT_EXPIRY_MS       30000     /* RFC 7675 §5.1 hard ceiling    */
#define ICEA_RTT_SWITCH_MARGIN_MS       10
#define ICEA_SWITCHING_DELAY_MS       1000
#define ICEA_ERR_RETRY_BASE_MS         500     /* 401/400 during a restart: ×2, cap */
#define ICEA_ERR_RETRY_CAP_MS         8000
#define ICEA_ERR_FAILED_COUNT            8     /* error-only pair counts as dead */
#define ICEA_STUN_KEEPALIVE_MS       10000     /* srflx mapping refresh         */
#define ICEA_NOMINATION_DELAY_MS       500     /* RFC 8863 PAC: let better pairs answer */
#define ICEA_PING_TABLE_EXPIRY_MS    30000     /* forget a tid after this       */

/* Gather bound for a restart when cfg.ice_gathering_timeout_ms is 0.  The
 * initial INVITE may wait for ever (baresip's own behaviour, no call yet);
 * a restart runs under a live, already-silent call and must not. */
#ifndef BSDK_ICE_RESTART_DEADLINE_MS
#define BSDK_ICE_RESTART_DEADLINE_MS  3000
#endif

/* Peer-initiated restarts we answer with one of our own, per call.  With
 * consent freshness this is a backstop, not policy. */
#define ICEA_MAX_REMOTE_RESTARTS         2

/* How long a candidate re-offer waits for the DTLS handshake before being
 * abandoned (never sent).  Spans dtls_srtp's restart ladder: 1200 + 4×2500. */
#define ICEA_REOFFER_SECURE_DEADLINE_MS 12000
#define ICEA_REOFFER_POLL_MS               50

/* How long after the answer a stream may go without ICE handing it a path
 * before the media-start watchdog speaks up, and how often it looks again.
 * ICE against the PBX completes 6-15 ms after the 200 OK. */
#define ICEA_MEDIA_START_DEADLINE_MS     3000
#define ICEA_MEDIA_START_POLL_MS         1000

/* Virtual peer address handed upward (DTLS/SRTP/RTP see only this).  RFC 5737
 * / RFC 3849 documentation ranges: never a real host, so no collision with a
 * LAN peer's source.  Port 9 (discard) keeps sa_isset(SA_ALL) true. */
#define ICEA_VPEER_V4  "192.0.2.1"
#define ICEA_VPEER_V6  "2001:db8::1"
#define ICEA_VPEER_PORT 9

#define ICEA_UFRAG_LEN   8
#define ICEA_PWD_LEN    32
#define ICEA_MAX_COMP    2

/* ── Enums ───────────────────────────────────────────────────────────────── */

/* Mirrors RTCIceConnectionState.  The public echosdk_ice_state_t (added to
 * echosdk.h at the flip) uses identical values; ice_mod.c asserts that. */
enum icea_state {
	ICEA_STATE_NEW = 0,
	ICEA_STATE_GATHERING,
	ICEA_STATE_CHECKING,
	ICEA_STATE_CONNECTED,
	ICEA_STATE_COMPLETED,
	ICEA_STATE_DISCONNECTED,
	ICEA_STATE_FAILED,
	ICEA_STATE_CLOSED,
};

enum icea_nomination {
	ICEA_NOMINATE_REGULAR = 0,      /* RFC 8445 §8.1.1: once, the selected pair */
	ICEA_NOMINATE_RENOMINATION,     /* peer negotiated a=ice-options:renomination */
};

enum icea_transport_policy {
	ICEA_TRANSPORT_ALL = 0,
	ICEA_TRANSPORT_RELAY,
};

/* libwebrtc Connection::WriteState */
enum icea_write_state {
	ICEA_WS_INIT = 0,      /* no response yet                       */
	ICEA_WS_WRITABLE,      /* recent response                       */
	ICEA_WS_UNRELIABLE,    /* a few misses                          */
	ICEA_WS_TIMEOUT,       /* many misses                           */
};

/* ── Configuration (filled by ice_mod.c from g_bsdk.cfg, by tests by hand) ─ */

struct icea_conf {
	uint32_t ping_weak_ms, ping_strong_ms;
	uint32_t pair_stabilizing_ms, pair_stable_ms;
	uint32_t backup_ping_ms;
	uint32_t write_connect_failures;
	uint32_t write_connect_timeout_ms;
	uint32_t write_timeout_ms;
	uint32_t receiving_timeout_ms;
	uint32_t dead_ms;
	uint32_t consent_expiry_ms;
	uint32_t rtt_switch_margin_ms;
	uint32_t switching_delay_ms;
	uint32_t err_retry_base_ms, err_retry_cap_ms;
	uint32_t err_failed_count;
	uint32_t stun_keepalive_ms;
	uint32_t ping_table_expiry_ms;
	uint32_t stun_rto_ms, stun_rc;    /* gathering ladder (RFC 5389)       */
	uint32_t nomination_delay_ms;     /* hold nomination for better pairs  */
	enum icea_nomination       nomination;
	enum icea_transport_policy policy;
	bool     drop_unknown_src;  /* PR1: false — accept and log             */
	bool     family_filter;     /* offer only the signalling family        */
	bool     debug;
	void   (*debug_h)(const char *line);  /* NULL → re_printf             */
	/* Pin a freshly created socket to the network behind `ifname` before
	 * it is bound (Android: android_setsocknetwork; Darwin: IP_BOUND_IF;
	 * Linux: SO_BINDTODEVICE).  NULL = ports bind to the interface address
	 * only.  Pinned ports bind to the any-address, as libwebrtc does: the
	 * kernel then picks the source from the network's own route. */
	int    (*pin_fd)(re_sock_t fd, const char *ifname, int af);
	bool   pin_bind_any;        /* pinned ports bind the any-address (Android)
	                               rather than the interface address        */
};

#define ICEA_CONF_DEFAULT { \
	.ping_weak_ms = ICEA_PING_WEAK_MS, .ping_strong_ms = ICEA_PING_STRONG_MS, \
	.pair_stabilizing_ms = ICEA_PAIR_STABILIZING_MS, \
	.pair_stable_ms = ICEA_PAIR_STABLE_MS, .backup_ping_ms = ICEA_BACKUP_PING_MS, \
	.write_connect_failures = ICEA_WRITE_CONNECT_FAILURES, \
	.write_connect_timeout_ms = ICEA_WRITE_CONNECT_TIMEOUT_MS, \
	.write_timeout_ms = ICEA_WRITE_TIMEOUT_MS, \
	.receiving_timeout_ms = ICEA_RECEIVING_TIMEOUT_MS, .dead_ms = ICEA_DEAD_MS, \
	.consent_expiry_ms = ICEA_CONSENT_EXPIRY_MS, \
	.rtt_switch_margin_ms = ICEA_RTT_SWITCH_MARGIN_MS, \
	.switching_delay_ms = ICEA_SWITCHING_DELAY_MS, \
	.err_retry_base_ms = ICEA_ERR_RETRY_BASE_MS, .err_retry_cap_ms = ICEA_ERR_RETRY_CAP_MS, \
	.err_failed_count = ICEA_ERR_FAILED_COUNT, \
	.stun_keepalive_ms = ICEA_STUN_KEEPALIVE_MS, \
	.ping_table_expiry_ms = ICEA_PING_TABLE_EXPIRY_MS, \
	.stun_rto_ms = STUN_DEFAULT_RTO, .stun_rc = STUN_DEFAULT_RC, \
	.nomination_delay_ms = ICEA_NOMINATION_DELAY_MS, \
	.nomination = ICEA_NOMINATE_REGULAR, .policy = ICEA_TRANSPORT_ALL, \
	.drop_unknown_src = false, .family_filter = true, .debug = false }

/* ── Objects ─────────────────────────────────────────────────────────────── */

struct icea_gen;
struct icea_pair;

/** One socket a generation can send and receive on. */
struct icea_port {
	struct le le;                /* gen->portl                              */
	struct icea_gen *gen;        /* borrowed parent                         */
	unsigned compid;             /* 1 = RTP, 2 = RTCP                       */
	struct udp_sock *us;         /* mem_ref'd; stream sock (shared) or ours */
	struct udp_helper *uh;       /* owned; NULL on the stream sockets, whose
	                                helper belongs to the media (ice_route)  */
	struct sa laddr;             /* bound address, supplied by the creator:
	                                udp_local_get() needs a real fd and the
	                                harness sockets have none               */
	char ifname[16];
	int  cost;                   /* interface cost: lower is better         */
	bool is_stream_sock;         /* send via udp_send_helper, not udp_send  */
	struct udp_helper *bypass_uh;/* borrowed; stream sockets: the media's ICE
	                                helper, so STUN leaves below DTLS/SRTP  */
	struct turnc *turnc;         /* owned; relay ports only                 */
	struct stun_ctrans *ct_gath; /* owned; outstanding srflx request        */
	struct stun_keepalive *ska;  /* owned; mapping refresh (PR3)            */
	bool gathered;               /* srflx/relay attempt concluded           */
	bool primary;                /* on the signalling interface: one gathered
	                                primary port is enough to release the offer */
	bool carried;                /* moved over from the previous generation  */
	struct sa srflx;             /* reflexive mapping learned on this port;
	                                reused when the port is carried over       */
};

/** A local or remote candidate. */
struct icea_cand {
	struct le le;                /* gen->lcandl or gen->rcandl              */
	bool local;
	enum ice_cand_type type;
	unsigned compid;
	uint32_t prio;
	struct sa addr;
	struct sa rel;               /* related address (base) for non-host     */
	char foundation[33];
	struct icea_port *port;      /* local only; borrowed                    */
	struct icea_cand *base;      /* local only; borrowed within lcandl      */
};

/** One Binding Request we sent — the transaction table entry. */
struct icea_ping {
	struct le le;                /* gen->pingl, oldest first                */
	uint8_t tid[STUN_TID_SIZE];
	struct icea_pair *pair;      /* borrowed; pairs outlive pingl           */
	uint64_t sent;
	bool nominating;             /* USE-CANDIDATE was set                   */
	enum ice_role role;          /* our role when it was sent (§7.2.5.1)   */
	bool early;                  /* sent before the peer could answer it:
	                                never counted as lost, no rtt sample    */
};

/** A candidate pair with libwebrtc's per-connection bookkeeping. */
struct icea_pair {
	struct le le;                /* gen->pairl, sorted by prio desc         */
	struct icea_gen *gen;
	struct icea_cand *lc, *rc;   /* borrowed (same generation)              */
	uint64_t prio;               /* RFC 8445 §6.1.2.3                       */
	enum ice_candpair_state state;
	bool valid, nominated, remote_nominated, triggered, use_cand_pending;
	uint64_t created;
	uint64_t last_ping_sent, last_ping_recv, last_resp_recv;
	uint64_t last_data_recv, last_consent_ok, next_allowed_ping;
	unsigned unanswered;         /* pings in pingl for this pair            */
	unsigned err_count;          /* consecutive STUN error responses        */
	uint16_t last_scode;
	uint32_t rtt_ms;
	unsigned rtt_samples;
	enum icea_write_state ws;
	bool receiving;
};

/** What the send helper reads.  Guarded by icea_media.tx_mtx. */
struct icea_txsel {
	struct sa dst;
	struct udp_sock *us;         /* mem_ref'd while published               */
	bool is_stream_sock;
	bool consent_ok;             /* false → drop (RFC 7675 cease)           */
};

/** Owner callbacks: ice_mod.c in the SDK, the harness in tests. */
struct icea_gen_cb {
	void (*gathered)(struct icea_gen *gen, void *arg);
	void (*state)(struct icea_gen *gen, enum icea_state prev,
		      enum icea_state now, void *arg);
	/* The selected pair changed, or the selected pair became writable. */
	void (*selected)(struct icea_gen *gen, unsigned compid, void *arg);
	/* Deliver application data that arrived on one of the generation's own
	 * ports; the owner injects it into the stream socket above ICE. */
	void (*data)(struct icea_gen *gen, unsigned compid,
		     const struct sa *src, struct mbuf *mb, void *arg);
	void *arg;
};

/** One ICE credential set: the initial one, or one restart. */
struct icea_gen {
	const struct icea_conf *conf;   /* borrowed                             */
	uint32_t seq;                   /* 1 = initial, 2.. = restarts          */
	char lufrag[ICEA_UFRAG_LEN + 1];
	char lpwd[ICEA_PWD_LEN + 1];
	char *rufrag, *rpwd;            /* owned                                */
	uint64_t tiebrk;
	enum ice_role role;
	int  af;                        /* signalling family                    */
	bool rlite;                     /* peer is ICE-lite                     */
	bool renomination;              /* peer negotiated renomination         */
	bool remote_ready;              /* rufrag, rpwd and ≥1 remote candidate */
	bool gathered;
	bool checking;
	bool mismatch;                  /* peer's c= not among its candidates   */
	unsigned ncomp;                 /* 1 with rtcp-mux, else 2              */
	struct list portl, lcandl, rcandl, pairl, pingl;   /* owned            */
	struct icea_pair *selected[ICEA_MAX_COMP];          /* borrowed         */
	uint64_t selected_since[ICEA_MAX_COMP];
	bool sel_writable[ICEA_MAX_COMP]; /* selected pair writable when last
	                                     reported to the owner            */
	bool early_checks;              /* checks go to a peer that cannot
	                                   answer yet (see icea_gen_set_early) */
	struct stun *stun;              /* owned; gathering transactions only   */
	unsigned gather_pending;
	struct tmr tmr_tick;            /* the scheduler                        */
	uint64_t checks_started;        /* first icea_gen_start_checks()        */
	bool role_switch_pending;       /* 487 seen; re-prioritise at next tick */
	bool was_connected[ICEA_MAX_COMP];/* ever had a writable pair           */
	bool failed_reported;           /* FAILED emitted for this generation   */
	enum icea_state state;
	struct icea_gen_cb cb;
};

/* ── Engine API (implemented across the ice_*.c files) ────────────────────── */

/* ice_gen.c / lifecycle */
int  icea_gen_alloc(struct icea_gen **genp, const struct icea_conf *conf,
		    uint32_t seq, enum ice_role role, int af, unsigned ncomp,
		    const struct icea_gen_cb *cb);
void icea_gen_set_state(struct icea_gen *gen, enum icea_state st);
const char *icea_state_name(enum icea_state st);
uint64_t icea_now(void);

/* ice_cand.c */
struct icea_cand *icea_cand_add_local(struct icea_gen *gen,
				      enum ice_cand_type type, unsigned compid,
				      struct icea_port *port,
				      struct icea_cand *base,
				      const struct sa *addr, uint16_t lpref);
struct icea_cand *icea_cand_add_remote(struct icea_gen *gen,
				       const struct ice_cand_attr *attr);
struct icea_cand *icea_cand_add_prflx_remote(struct icea_gen *gen,
					     unsigned compid, uint32_t prio,
					     const struct sa *addr);
struct icea_cand *icea_cand_find(const struct list *lst, unsigned compid,
				 const struct sa *addr);
void icea_cand_redund_elim(struct icea_gen *gen);
const struct icea_cand *icea_cand_default(const struct icea_gen *gen,
					  unsigned compid);
uint16_t icea_cand_lpref(const struct icea_port *port);

/* ice_sdp.c */
int  icea_sdp_write_local(struct icea_gen *gen, struct sdp_session *sdp,
			  struct sdp_media *sdpm, bool session_creds);
int  icea_sdp_read_remote(struct icea_gen *gen, struct sdp_session *sdp,
			  struct sdp_media *sdpm, bool *ufrag_changed);
bool icea_sdp_peer_supports_ice(struct icea_gen *gen,
				const struct sdp_media *sdpm);
bool icea_sdp_peer_has_ice(const struct icea_gen *gen);
int  icea_cand_attr_decode(struct ice_cand_attr *cand, const char *val);
void icea_sdp_clear_local(struct sdp_media *sdpm);

/* ice_pair.c */
int  icea_pair_form(struct icea_gen *gen);
void icea_pair_prune(struct icea_gen *gen);
void icea_pair_set_states(struct icea_gen *gen);
void icea_pair_reprio(struct icea_gen *gen);
struct icea_pair *icea_pair_find(const struct icea_gen *gen,
				 const struct icea_cand *lc,
				 const struct icea_cand *rc);
struct icea_pair *icea_pair_by_src(const struct icea_gen *gen, unsigned compid,
				   const struct icea_port *port,
				   const struct sa *src);
struct icea_pair *icea_pair_create(struct icea_gen *gen, struct icea_cand *lc,
				   struct icea_cand *rc);
uint64_t icea_pair_calc_prio(enum ice_role role, uint32_t lprio, uint32_t rprio);

/* ice_check.c */
int  icea_gen_start_checks(struct icea_gen *gen);
void icea_gen_hold(struct icea_gen *gen);
void icea_gen_set_early(struct icea_gen *gen, bool on);
bool icea_check_recv_response(struct icea_gen *gen, struct icea_port *port,
			      const struct sa *src, const struct stun_msg *msg);
void icea_check_tick_now(struct icea_gen *gen);
void icea_pair_note_data(struct icea_gen *gen, unsigned compid,
			 const struct icea_port *port, const struct sa *src);
void icea_pair_note_ping_recv(struct icea_gen *gen, unsigned compid,
			      const struct icea_port *port, const struct sa *src);
int  icea_send_stun(struct icea_port *port, const struct sa *dst,
		    struct mbuf *mb);
struct icea_cand *icea_lcand_on_port(const struct icea_gen *gen,
				     unsigned compid,
				     const struct icea_port *port);

/* ice_stund.c */
void icea_stund_recv(struct icea_gen *gen, struct icea_port *port,
		     const struct sa *src, const struct stun_msg *msg);
bool icea_stund_owns(const struct icea_gen *gen, const struct stun_msg *msg);

/* ice_select.c */
void icea_select_tick(struct icea_gen *gen, uint64_t now);
bool icea_gen_weak(const struct icea_gen *gen);
bool icea_nominate_decision(struct icea_gen *gen, const struct icea_pair *p);
uint64_t icea_pair_last_received(const struct icea_pair *p);
bool icea_pair_dead(const struct icea_pair *p, uint64_t now);
bool icea_pair_consent_ok(const struct icea_pair *p, uint64_t now);

/* ice_port.c */
int  icea_port_attach(struct icea_gen *gen, unsigned compid,
		      struct udp_sock *us, const struct sa *laddr,
		      const char *ifname, int cost, bool is_stream_sock,
		      struct icea_port **portp);
int  icea_port_alloc(struct icea_gen *gen, unsigned compid,
		     const struct sa *laddr, const char *ifname, int cost,
		     uint16_t port_min, uint16_t port_max,
		     struct icea_port **portp);
struct icea_port *icea_port_find(const struct icea_gen *gen,
				 const struct udp_sock *us);

/* ice_gather.c */
struct icea_server {
	struct le le;
	bool turn;                   /* false = STUN only, true = TURN         */
	int proto;                   /* IPPROTO_UDP / IPPROTO_TCP (+tls flag)   */
	bool tls;
	struct sa addr;              /* resolved by the owner                   */
	char *user, *pass;
};
int  icea_server_alloc(struct icea_server **srvp, bool turn, int proto,
		       bool tls, const struct sa *addr, const char *user,
		       const char *pass);
int  icea_gather_start(struct icea_gen *gen, const struct list *serverl,
		       uint16_t port_min, uint16_t port_max);
bool icea_gather_done(const struct icea_gen *gen);
/* A port learned its mapping some other way (peer-reflexive): re-evaluate
 * whether the generation can be offered. */
void icea_gather_port_concluded(struct icea_gen *gen);

/* ice_route.c — receive path of the agent's own ports, and the STUN demux
 * the media's stream-socket helper (ice_mod.c) shares with it */
void icea_route_port_recv(const struct sa *src, struct mbuf *mb, void *arg);
bool icea_route_stun_demux(struct icea_gen *gen, struct icea_port *port,
			   const struct sa *src, struct mbuf *mb);
bool icea_is_stun(const struct mbuf *mb);

/* Debug */
int  icea_gen_debug(struct re_printf *pf, const struct icea_gen *gen);

void icea_debugf(const struct icea_conf *conf, const char *fmt, ...);
#define ICEA_DEBUG(gen, ...) do { if ((gen)->conf->debug) \
	icea_debugf((gen)->conf, __VA_ARGS__); } while (0)

#ifdef __cplusplus
}
#endif

#endif /* ECHOSDK_ICE_INTERNAL_H */
