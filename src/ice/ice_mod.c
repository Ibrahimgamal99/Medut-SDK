/**
 * @file ice_mod.c  echo-ice: the baresip media-NAT module and SDK seams
 *
 * Registers as media-NAT "ice" (the id baresip matches to advertise
 * `Supported: ice`, and ftag "+sip.ice" for the Contact) and owns
 * everything between baresip and the engine in ice_*.c: session and media
 * wrappers with stable pointers, one generation per stream sharing the
 * session's credentials, gathering and its deadline, the release of the
 * deferred INVITE / re-INVITE, connectivity checks primed while an inbound
 * call rings, the virtual-peer addressing that keeps DTLS/SRTP/RTP blind to
 * pair changes, the DTLS-gated candidate re-offer, peer-restart detection,
 * and the netmon seams (bsdk_ice_restart, bsdk_ice_call_active/state).
 *
 * Every behaviour carried over from src/ice_shim.c is marked with the date
 * of the on-device incident that produced it; see the plan for the list.
 *
 * Threads: baresip calls sessh/mediah/updateh/attrh and our handlers on
 * re_main.  The one exception is the stream-socket *send* helper, which
 * runs on the audio encoder thread for RTP: it reads only the
 * mutex-guarded txsel snapshot and never touches agent state.
 */

#include <string.h>
#include "echosdk_internal.h"
#include "ice_internal.h"


/* ── Objects ─────────────────────────────────────────────────────────────── */

struct icea_media;

/** What baresip holds as `call->mnats`. */
struct icea_sess {
	struct le le;                 /* g_sessl                                */
	void *call;                   /* borrowed struct call * (estabh's arg)  */
	mnat_estab_h *estabh;
	struct sdp_session *sdp;      /* mem_ref'd: call_destructor frees the SDP
	                                 before the media-NAT                    */
	struct dnsc *dnsc;            /* borrowed; owned by the SIP stack        */
	int af;
	bool offerer;

	/* configured NAT server, resolved into serverl */
	bool srv_set, srv_turn;
	char *srv_host;
	uint16_t srv_port;
	char *user, *pass;
	struct stun_dns *dnsq;        /* owned                                  */
	struct list serverl;          /* owned struct icea_server              */
	bool servers_ready;           /* DNS done (or nothing to resolve)        */

	/* credentials shared by every stream of one generation */
	uint32_t gen_seq;
	char lufrag[ICEA_UFRAG_LEN + 1];
	char lpwd[ICEA_PWD_LEN + 1];
	uint64_t tiebrk;

	struct list medial;           /* owned struct icea_media               */
	struct tmr tmr_gather;        /* gathering deadline                      */
	struct tmr tmr_bounce;        /* leave baresip's call stack first        */
	struct tmr tmr_reoffer;       /* re-offer held for the DTLS handshake    */
	struct tmr tmr_media;         /* media-start watchdog, from the answer   */
	uint64_t reoffer_due;

	bool released;                /* the offer has been handed to baresip    */
	bool restarting;              /* a restart gather is outstanding         */
	bool in_estab;                /* inside estabh — see icea_updateh()      */
	bool await_answer;            /* a restart offer is on the wire          */
	bool reoffered;               /* the candidate re-offer was requested    */
	bool primed;                  /* checks started while ringing            */
	bool answered;                /* our SDP answer has been generated       */
	bool bounce_restart;          /* what tmr_bounce should do              */
	bool bounce_connh;
	bool bounce_gather;
	unsigned rrestarts;           /* peer restarts answered with our own     */

	struct icea_conf conf;
	enum icea_state state;
};

/** What baresip holds as `stream->mns`.  Its address never changes. */
struct icea_media {
	struct le le;                 /* sess->medial                           */
	struct icea_sess *sess;       /* borrowed; NULL once the session is gone */
	struct sdp_media *sdpm;       /* mem_ref'd                              */
	struct udp_sock *sock[ICEA_MAX_COMP];   /* mem_ref'd stream sockets     */
	struct udp_helper *uh[ICEA_MAX_COMP];   /* owned; LAYER_ICE            */
	struct { struct icea_media *m; unsigned compid; } hook[ICEA_MAX_COMP];
	unsigned ncomp;

	struct icea_gen *cur;         /* owned                                  */

	mtx_t *tx_mtx;                /* owned; guards txsel[]                   */
	struct icea_txsel txsel[ICEA_MAX_COMP];

	mnat_connected_h *connh;      /* borrowed: baresip's stream handler     */
	void *arg;

	struct sa vpeer[ICEA_MAX_COMP];  /* constant virtual peer per component */
	bool vpeer_set;
	bool connh_fired;
	bool held;                    /* connh deferred until our answer is out */
	bool nomedia_warned;          /* watchdog reported no writable pair      */
	bool plain;                   /* peer without ICE: pass through          */
	bool in_inject;               /* recv helper: this datagram is ours     */

	struct sa signalled;          /* laddr the peer was told (re-offer rule)*/
	enum icea_state state;
};

static struct list g_sessl = LIST_INIT;
static struct mnat mnat_ice;   /* registered struct; defined at the end */


/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void sdk_debug_h(const char *line)
{
	debug("%s", line);
}


static struct icea_sess *sess_find(const void *call)
{
	struct le *le;

	LIST_FOREACH(&g_sessl, le) {
		struct icea_sess *s = le->data;
		if (s->call == call)
			return s;
	}
	return NULL;
}


static bool call_is_incoming(const struct icea_sess *s)
{
	return s->call &&
	       call_state((const struct call *)s->call) == CALL_STATE_INCOMING;
}


/* Has the peer been given our answer — fingerprint, setup role, credentials —
 * so that media may start?  True once we are out of the incoming state (a
 * delayed-offer INVITE answers in the ACK), and from the moment baresip runs
 * updateh to build the answer itself: call_answer() calls us one step before
 * sdp_encode() and sipsess_answer(), so by the time anything else in this
 * module runs — a bounce, a socket callback — the answer is on the wire. */
static bool answer_is_out(const struct icea_sess *s)
{
	return s->answered || !call_is_incoming(s);
}


/* The address the call signals from: the session's own c= when set, else
 * the interface baresip would pick for the family. */
static const struct sa *sig_addr(const struct icea_sess *s)
{
	const struct sa *sig = s->sdp ? sdp_session_laddr(s->sdp) : NULL;

	if (sig && sa_isset(sig, SA_ADDR))
		return sig;
	return net_laddr_af(baresip_network(), s->af);
}


/* Is a DTLS handshake configured and not yet finished on any stream?
 * The re-INVITE hold keys on this (2026-08-31: 9/9 calls that released a
 * re-offer mid-handshake died; 10/11 that held it lived). */
static bool sess_awaiting_secure(const struct icea_sess *s)
{
	struct list *streaml;
	struct le *le;
	bool dtls = false;

	if (!s || !s->sdp || !s->call)
		return false;

	LIST_FOREACH(&s->medial, le) {
		const struct icea_media *m = le->data;
		if (m->sdpm &&
		    sdp_media_session_rattr(m->sdpm, s->sdp, "fingerprint")) {
			dtls = true;
			break;
		}
	}
	if (!dtls)
		return false;

	streaml = call_streaml((const struct call *)s->call);
	if (!streaml)
		return false;

	LIST_FOREACH(streaml, le) {
		if (!stream_is_secure(le->data))
			return true;
	}
	return false;
}


/* ── Virtual peer and connh ──────────────────────────────────────────────── */

static void vpeer_init(struct icea_media *m)
{
	int af = m->cur ? m->cur->af : AF_INET;

	for (unsigned i = 0; i < ICEA_MAX_COMP; i++) {
		sa_set_str(&m->vpeer[i], af == AF_INET6 ? ICEA_VPEER_V6
						       : ICEA_VPEER_V4,
			   (uint16_t)(ICEA_VPEER_PORT + i));
	}
	m->vpeer_set = true;
}


/* Deliver (or re-deliver) the connected handler with the virtual peer.
 * stream_mnat_connected() copies it into tx.raddr and starts DTLS the
 * first time; later calls undo stream_update()'s overwrite of tx.raddr
 * with the SDP address, so stream_is_ready() stays true even for a peer
 * that signals c=0.0.0.0. */
static void fire_connh(struct icea_media *m)
{
	if (!m->connh)
		return;
	if (!m->vpeer_set)
		vpeer_init(m);

	m->connh(&m->vpeer[0], m->ncomp > 1 ? &m->vpeer[1] : NULL, m->arg);
	m->connh_fired = true;
	m->held        = false;
}


/* Plain RTP (peer offers no ICE, or an ICE mismatch): the stream talks to
 * the signalled address and the helpers pass everything through. */
static void fire_connh_plain(struct icea_media *m)
{
	const struct sa *raddr = sdp_media_raddr(m->sdpm);
	struct sa rtcp;

	if (!m->connh || !raddr || !sa_isset(raddr, SA_ALL))
		return;

	sdp_media_raddr_rtcp(m->sdpm, &rtcp);
	m->plain = true;
	mtx_lock(m->tx_mtx);
	for (unsigned i = 0; i < ICEA_MAX_COMP; i++) {
		m->txsel[i].us = mem_deref(m->txsel[i].us);
		m->txsel[i].consent_ok = false;
	}
	mtx_unlock(m->tx_mtx);

	m->connh(raddr, m->ncomp > 1 ? &rtcp : NULL, m->arg);
	m->connh_fired = true;
}


/* Publish the selected pair of one component to the send helper. */
static void publish_txsel(struct icea_media *m, unsigned compid)
{
	struct icea_gen *gen = m->cur;
	struct icea_pair *p = gen ? gen->selected[compid - 1] : NULL;
	struct icea_txsel sel;
	struct udp_sock *old;

	memset(&sel, 0, sizeof(sel));
	if (p) {
		sel.dst            = p->rc->addr;
		sel.us             = mem_ref(p->lc->port->us);
		sel.is_stream_sock = p->lc->port->is_stream_sock;
		sel.consent_ok     = icea_pair_consent_ok(p, icea_now()) ||
				     p->last_consent_ok == 0;
	}

	mtx_lock(m->tx_mtx);
	old = m->txsel[compid - 1].us;
	m->txsel[compid - 1] = sel;
	mtx_unlock(m->tx_mtx);
	mem_deref(old);
}


/* ── Offer release and the deadline ──────────────────────────────────────── */

/* Hand the offer to baresip.  On success in_estab flags that any updateh
 * baresip runs from inside estabh belongs to *our* offer, not to an answer
 * (2026-09-02: checks against the peer's old credentials, 401 every time).
 * On failure `s` may be gone when estabh returns (CALL_EVENT_CLOSED). */
static void estab_release(struct icea_sess *s, int err, uint16_t scode,
			  const char *reason)
{
	if (err || scode) {
		s->estabh(err, scode, reason, s->call);
		return;
	}
	s->in_estab = true;
	s->estabh(0, 0, NULL, s->call);
	s->in_estab = false;
}


static void prime_conncheck(struct icea_sess *s);


static bool all_gathered(const struct icea_sess *s)
{
	struct le *le;

	LIST_FOREACH(&s->medial, le) {
		const struct icea_media *m = le->data;
		if (m->cur && !icea_gather_done(m->cur))
			return false;
	}
	return !list_isempty(&s->medial);
}


static bool relay_only_unmet(const struct icea_sess *s)
{
	struct le *le;

	if (s->conf.policy != ICEA_TRANSPORT_RELAY)
		return false;

	LIST_FOREACH(&s->medial, le) {
		const struct icea_media *m = le->data;
		const struct icea_cand *def =
			m->cur ? icea_cand_default(m->cur, 1) : NULL;
		if (!def || def->type != ICE_CAND_TYPE_RELAY)
			return true;
	}
	return false;
}


/**
 * Write this stream's ICE attributes, then re-baseline what the peer has been
 * told our media address is.
 *
 * `signalled` is seeded in icea_mediah() from the *session* address, which is
 * all there is before gathering produces anything — and on an SDP session that
 * is the placeholder `0.0.0.0`/port 9 or the private host address.  The offer
 * that actually goes out carries the reflexive address gathering just found,
 * so comparing a later selection against the seed reports "never signalled"
 * for an address the peer was told about, and the log says so in as many
 * words.  Take the baseline from what the SDP carries at the moment baresip is
 * about to build the offer or answer from it.
 */
static void media_write_sdp(struct icea_sess *s, struct icea_media *m)
{
	const struct sa *laddr;

	if (!m->cur || !m->sdpm)
		return;

	(void)icea_sdp_write_local(m->cur, s->sdp, m->sdpm, true);

	laddr = sdp_media_laddr(m->sdpm);
	if (laddr && sa_isset(laddr, SA_ADDR))
		m->signalled = *laddr;
}


static void write_all_sdp(struct icea_sess *s)
{
	struct le *le;

	LIST_FOREACH(&s->medial, le)
		media_write_sdp(s, le->data);
}


/* Every stream has gathered (or the deadline gave up waiting). */
static void release_offer(struct icea_sess *s, const char *why)
{
	tmr_cancel(&s->tmr_gather);
	write_all_sdp(s);

	if (relay_only_unmet(s)) {
		/* Correction 8: an offer with credentials but no relay
		 * candidates would make the peer fall back to c= — a policy
		 * violation.  Initial offer: fail the call fast.  Restart:
		 * no re-INVITE, report, leave the call as it is. */
		warning("EchoSDK/ice: relay-only policy and no relayed"
			" candidate (%s) — %s\n", why,
			s->restarting ? "not re-offering" : "failing the call");
		if (s->restarting) {
			s->restarting = false;
			bsdk_netmon_call_ice_failed((struct call *)s->call,
						    ETIMEDOUT);
			return;
		}
		s->released = true;
		estab_release(s, ETIMEDOUT, 0, NULL);
		return;
	}

	if (s->released && !s->restarting) {
		/* A late report on an offer already out (2026-08-31 lesson):
		 * never a re-INVITE mid-handshake.  The prflx rule in
		 * on_selected() decides whether the new address needs one. */
		debug("EchoSDK/ice: gathering reported again after the offer"
		      " went out (%s) — nothing to send\n", why);
		return;
	}

	bool was_restart = s->restarting;
	s->restarting = false;
	s->released   = true;

	info("EchoSDK/ice: %s — %s\n",
	     was_restart ? "restart gathered" : "gathered", why);

	estab_release(s, 0, 0, NULL);

	/* estabh may have ended the call; only continue if we still exist */
	if (!sess_find(s->call))
		return;

	if (!was_restart)
		prime_conncheck(s);
}


static void gather_deadline_h(void *arg)
{
	struct icea_sess *s = arg;
	struct le *le;

	if (s->released && !s->restarting)
		return;

	warning("EchoSDK/ice: %s gathering did not complete in %u ms —"
		" offering what exists\n",
		s->restarting ? "restart" : "candidate",
		g_bsdk.cfg.ice_gathering_timeout_ms
			? g_bsdk.cfg.ice_gathering_timeout_ms
			: BSDK_ICE_RESTART_DEADLINE_MS);

	/* A stream whose gather never reported still has its host
	 * candidate; the deadline itself concludes gathering for it. */
	LIST_FOREACH(&s->medial, le) {
		struct icea_media *m = le->data;
		if (m->cur && !m->cur->gathered) {
			m->cur->gather_pending = 0;
			m->cur->gathered = true;
			icea_cand_redund_elim(m->cur);
		}
	}

	release_offer(s, "deadline");
}


/* ── Engine callbacks (owner side) ───────────────────────────────────────── */

static void on_gathered(struct icea_gen *gen, void *arg)
{
	struct icea_media *m = arg;
	struct icea_sess *s = m->sess;
	(void)gen;

	if (!s)
		return;

	media_write_sdp(s, m);

	if (all_gathered(s))
		release_offer(s, "all streams gathered");
}


static enum icea_state sess_state(const struct icea_sess *s)
{
	enum icea_state worst = ICEA_STATE_COMPLETED;
	struct le *le;
	bool any = false;

	/* worse wins: failed < disconnected < checking < connected < completed */
	static const int rank[] = { 2, 2, 2, 3, 4, 1, 0, 2 };

	LIST_FOREACH(&s->medial, le) {
		const struct icea_media *m = le->data;
		if (!m->cur)
			continue;
		any = true;
		if (rank[m->cur->state] < rank[worst])
			worst = m->cur->state;
	}
	return any ? worst : ICEA_STATE_NEW;
}


/* ECHOSDK_EV_ICE_STATE, built the way netmon_emit() builds its events:
 * strings packed into the queued event's own buffer, the call handle borrowed
 * (the wrapper outlives every event queued before its CALL_ENDED). */
static void emit_ice_state(struct icea_sess *s, struct icea_media *m,
			   enum icea_state prev, enum icea_state now)
{
	struct echosdk_call *lc = bsdk_call_find((struct call *)s->call);
	struct echosdk_queued_event *qev;
	const struct icea_pair *p = m->cur ? m->cur->selected[0] : NULL;
	size_t off = 0;
	int n;

	if (!lc)
		return;

	atomic_store(&lc->ice_state, (int)now);

	qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type              = ECHOSDK_EV_ICE_STATE;
	qev->ev.u.ice.call        = (echosdk_call_handle_t)lc;
	qev->ev.u.ice.state       = (echosdk_ice_state_t)now;
	qev->ev.u.ice.prev        = (echosdk_ice_state_t)prev;
	qev->ev.u.ice.generation  = m->cur ? m->cur->seq : 0;
	qev->ev.u.ice.local_type  = p ? (echosdk_ice_cand_type_t)p->lc->type
				      : ECHOSDK_ICE_CAND_NONE;
	qev->ev.u.ice.remote_type = p ? (echosdk_ice_cand_type_t)p->rc->type
				      : ECHOSDK_ICE_CAND_NONE;
	qev->ev.u.ice.rtt_ms      = p ? (float)p->rtt_ms : 0.f;
	qev->ev.u.ice.error       = now == ICEA_STATE_FAILED ? ECHOSDK_ERR_TIMEOUT
							     : ECHOSDK_OK;

	qev->ev.u.ice.local_addr = qev->buf + off;
	n = p ? re_snprintf(qev->buf + off, sizeof(qev->buf) - off, "%J",
			    &p->lc->addr) : 0;
	if (n < 0) n = 0;
	off += (size_t)n + 1;
	qev->buf[off - 1] = 0;

	qev->ev.u.ice.remote_addr = qev->buf + off;
	n = p ? re_snprintf(qev->buf + off, sizeof(qev->buf) - off, "%J",
			    &p->rc->addr) : 0;
	if (n < 0) n = 0;
	qev->buf[off + (size_t)n] = 0;

	bsdk_event_post_qev(qev);
}


static void on_state(struct icea_gen *gen, enum icea_state prev,
		     enum icea_state now, void *arg)
{
	struct icea_media *m = arg;
	struct icea_sess *s = m->sess;
	enum icea_state agg;

	m->state = now;
	if (!s)
		return;

	info("EchoSDK/ice: '%s' gen %u: %s -> %s\n", sdp_media_name(m->sdpm),
	     gen->seq, icea_state_name(prev), icea_state_name(now));

	if (now == ICEA_STATE_FAILED) {
		/* Report, never tear down: the app decides (2026-09-10 —
		 * three silent calls that nobody had been told about). */
		bsdk_netmon_call_ice_failed((struct call *)s->call, ETIMEDOUT);
	}

	agg = sess_state(s);
	if (agg != s->state) {
		enum icea_state before = s->state;

		s->state = agg;
		/* netmon reads this to tell "our path died" from "the far end
		 * went quiet" when it is asked to repair a stall. */
		bsdk_netmon_call_ice_state((struct call *)s->call, (int)before,
					   (int)agg);
		emit_ice_state(s, m, before, agg);
	}
}


static void bounce_h(void *arg);


static void on_selected(struct icea_gen *gen, unsigned compid, void *arg)
{
	struct icea_media *m = arg;
	struct icea_sess *s = m->sess;
	struct icea_pair *p = gen->selected[compid - 1];

	publish_txsel(m, compid);

	if (!s || !p || m->plain)
		return;

	/* First writable pair: start media — unless the call is still
	 * ringing on our side.  dtls_srtp would send its ClientHello before
	 * the peer has our answer (fingerprint, setup role, ufrag), lose it,
	 * and burn its restart budget (2026-08-31). */
	if (!m->connh_fired && p->ws == ICEA_WS_WRITABLE) {
		if (!answer_is_out(s)) {
			if (!m->held)
				info("EchoSDK/ice: '%s' concluded on %J while"
				     " ringing — media starts with the answer\n",
				     sdp_media_name(m->sdpm), &p->rc->addr);
			m->held = true;
		}
		else {
			fire_connh(m);
		}
	}

	/* Candidate re-offer rule (kept through PR1, dropped last): the
	 * local candidate ICE settled on was never signalled — a
	 * peer-reflexive address behind a per-destination NAT — so the peer
	 * may filter our media until told.  One re-offer per session, held
	 * behind the DTLS handshake, abandoned rather than sent late. */
	if (compid == 1 && !s->reoffered && sa_isset(&m->signalled, SA_ADDR) &&
	    !sa_cmp(&p->lc->addr, &m->signalled, SA_ADDR) &&
	    p->lc->type == ICE_CAND_TYPE_PRFLX && s->released &&
	    !s->restarting) {
		s->reoffered = true;
		info("EchoSDK/ice: selected local candidate %J was never"
		     " signalled (offered %J) — re-offering it\n",
		     &p->lc->addr, &m->signalled);
		s->reoffer_due = icea_now() + ICEA_REOFFER_SECURE_DEADLINE_MS;
		tmr_start(&s->tmr_reoffer, ICEA_REOFFER_POLL_MS,
			  bounce_h, s);   /* bounce_h checks the DTLS gate */
	}
}


static void on_data(struct icea_gen *gen, unsigned compid,
		    const struct sa *src, struct mbuf *mb, void *arg)
{
	struct icea_media *m = arg;
	struct udp_sock *us = m->sock[compid - 1];
	(void)gen; (void)src;

	if (!us || !m->uh[compid - 1])
		return;
	if (!m->vpeer_set)
		vpeer_init(m);

	/* Data that arrived on one of the agent's own ports (relay): hand it
	 * to the stream socket above our layer, as coming from the vpeer. */
	m->in_inject = true;
	udp_recv_helper(us, &m->vpeer[compid - 1], mb, m->uh[compid - 1]);
	m->in_inject = false;
}


/* ── Stream-socket helpers (LAYER_ICE) ───────────────────────────────────── */

static struct icea_port *stream_port(struct icea_media *m, unsigned compid)
{
	return m->cur ? icea_port_find(m->cur, m->sock[compid - 1]) : NULL;
}


/* re_main */
static bool stream_recv_h(struct sa *src, struct mbuf *mb, void *arg)
{
	struct { struct icea_media *m; unsigned compid; } *hook = arg;
	struct icea_media *m = hook->m;
	unsigned compid = hook->compid;
	struct icea_gen *gen = m->cur;
	struct icea_port *port;

	if (m->in_inject)
		return false;              /* our own injection: pass up */
	if (!gen || m->plain)
		return false;

	port = stream_port(m, compid);
	if (!port)
		return false;

	if (icea_route_stun_demux(gen, port, src, mb))
		return true;

	if (icea_pair_by_src(gen, compid, port, src)) {
		icea_pair_note_data(gen, compid, port, src);
	}
	else if (gen->conf->drop_unknown_src) {
		return true;
	}

	if (m->vpeer_set)
		*src = m->vpeer[compid - 1];
	return false;
}


/* Any thread (audio encoder for RTP).  Reads only the txsel snapshot. */
static bool stream_send_h(int *err, struct sa *dst, struct mbuf *mb, void *arg)
{
	struct { struct icea_media *m; unsigned compid; } *hook = arg;
	struct icea_media *m = hook->m;
	struct icea_txsel sel;

	if (m->plain)
		return false;

	/* STUN is always addressed explicitly by the engine: never rewrite */
	if (icea_is_stun(mb))
		return false;

	mtx_lock(m->tx_mtx);
	sel = m->txsel[hook->compid - 1];
	if (sel.us)
		mem_ref(sel.us);
	mtx_unlock(m->tx_mtx);

	if (!sel.us)
		return false;          /* nothing selected yet: pass through */

	if (!sel.consent_ok) {
		/* RFC 7675 §5.1: consent expired — MUST cease sending */
		mem_deref(sel.us);
		*err = 0;
		return true;
	}

	*dst = sel.dst;

	if (sel.is_stream_sock) {
		/* the selected port is this very socket: fall through with
		 * the rewritten destination, no re-entry */
		mem_deref(sel.us);
		return false;
	}

	/* another socket (relay / interface port): send there instead.
	 * Relay channel data needs 4 bytes of headroom. */
	if (mb->pos < 4) {
		struct mbuf *mb2 = mbuf_alloc(mbuf_get_left(mb) + 4);
		if (!mb2) {
			mem_deref(sel.us);
			*err = ENOMEM;
			return true;
		}
		mb2->pos = mb2->end = 4;
		mbuf_write_mem(mb2, mbuf_buf(mb), mbuf_get_left(mb));
		mb2->pos = 4;
		*err = udp_send(sel.us, dst, mb2);
		mem_deref(mb2);
	}
	else {
		*err = udp_send(sel.us, dst, mb);
	}
	mem_deref(sel.us);
	return true;
}


/* ── Generations ─────────────────────────────────────────────────────────── */

static void sess_new_credentials(struct icea_sess *s)
{
	s->gen_seq++;
	rand_str(s->lufrag, sizeof(s->lufrag));
	rand_str(s->lpwd, sizeof(s->lpwd));
	s->tiebrk = rand_u64();
}


/* A generation for `m`: the session's current credentials, the stream
 * sockets attached as ports on the signalling address (PR1: one wildcard
 * socket per component, labelled with the interface the call signals from
 * — the ice-ifsel lesson, 2026-08-31 / 2026-09-02). */
static int gen_new(struct icea_media *m, const struct sa *laddr)
{
	struct icea_sess *s = m->sess;
	struct icea_gen_cb cb = {
		.gathered = on_gathered, .state = on_state,
		.selected = on_selected, .data = on_data, .arg = m,
	};
	struct icea_gen *gen = NULL;
	const struct sa *sig = laddr && sa_isset(laddr, SA_ADDR) ? laddr
							       : sig_addr(s);
	char ifname[16] = "";
	int err;

	err = icea_gen_alloc(&gen, &s->conf, s->gen_seq,
			     s->offerer ? ICE_ROLE_CONTROLLING
					: ICE_ROLE_CONTROLLED,
			     s->af, m->ncomp, &cb);
	if (err)
		return err;

	str_ncpy(gen->lufrag, s->lufrag, sizeof(gen->lufrag));
	str_ncpy(gen->lpwd, s->lpwd, sizeof(gen->lpwd));
	gen->tiebrk = s->tiebrk;

	if (sig)
		(void)net_if_getname(ifname, sizeof(ifname), sa_af(sig), sig);

	for (unsigned i = 0; i < m->ncomp; i++) {
		struct icea_port *port = NULL;
		struct sa local, paddr;

		if (!m->sock[i])
			continue;
		if (udp_local_get(m->sock[i], &local))
			continue;

		/* the wildcard socket, labelled with the signalling address */
		paddr = sig && sa_isset(sig, SA_ADDR) ? *sig : local;
		sa_set_port(&paddr, sa_port(&local));

		err = icea_port_attach(gen, i + 1, m->sock[i], &paddr, ifname,
				       0, true, &port);
		if (err)
			break;
		port->bypass_uh = m->uh[i];
	}
	if (err) {
		mem_deref(gen);
		return err;
	}

	mem_deref(m->cur);
	m->cur = gen;
	return 0;
}


static int media_start_gather(struct icea_media *m)
{
	struct icea_sess *s = m->sess;
	const struct config *cfg = conf_config();
	uint16_t pmin = cfg ? (uint16_t)cfg->avt.rtp_ports.min : 0;
	uint16_t pmax = cfg ? (uint16_t)cfg->avt.rtp_ports.max : 0;

	if (!m->cur)
		return EINVAL;

	return icea_gather_start(m->cur, &s->serverl, pmin, pmax);
}


static void sess_start_gathering(struct icea_sess *s)
{
	struct le *le;

	s->servers_ready = true;

	LIST_FOREACH(&s->medial, le) {
		struct icea_media *m = le->data;
		int err;

		if (!m->cur || m->cur->gathered || m->cur->gather_pending)
			continue;
		err = media_start_gather(m);
		if (err)
			warning("EchoSDK/ice: '%s': gathering failed to start"
				" (%m)\n", sdp_media_name(m->sdpm), err);
	}
}


/* ── Checks: start, prime, remote updates ────────────────────────────────── */

static void media_maybe_start_checks(struct icea_media *m)
{
	struct icea_gen *gen = m->cur;

	if (!gen || !gen->remote_ready || gen->checking || m->plain)
		return;

	if (icea_gen_start_checks(gen))
		warning("EchoSDK/ice: '%s': could not start checks\n",
			sdp_media_name(m->sdpm));
}


/* Inbound call: the peer's offer is already in the SDP when we gather, so
 * run the checks while the phone rings instead of after the answer
 * (2026-08-31: 5.9 s of dead air on cellular otherwise). */
static void prime_conncheck(struct icea_sess *s)
{
	struct le *le;

	if (s->primed || !call_is_incoming(s))
		return;

	LIST_FOREACH(&s->medial, le) {
		struct icea_media *m = le->data;
		if (!m->cur)
			continue;
		(void)icea_sdp_read_remote(m->cur, s->sdp, m->sdpm, NULL);
		/* the caller cannot answer these before it has our answer */
		icea_gen_set_early(m->cur, !s->answered);
		if (icea_sdp_peer_supports_ice(m->cur, m->sdpm))
			media_maybe_start_checks(m);
	}
	s->primed = true;
	info("EchoSDK/ice: running the connectivity checks while the call"
	     " rings\n");
}


/* ── Media-start watchdog ────────────────────────────────────────────────── */

/* Has this stream had its answer, and is it still waiting for ICE to hand
 * the media a path?  A stream whose peer has not answered yet (an outbound
 * call still ringing) is not waiting on us. */
static bool media_start_pending(const struct icea_sess *s,
				const struct icea_media *m)
{
	return answer_is_out(s) && m->cur && m->cur->remote_ready &&
	       !m->connh_fired && !m->plain;
}


/* dtls_srtp's handshake watchdog starts with the handshake, and the
 * handshake starts only when we fire connh.  A path ICE settled on without
 * telling the stream therefore left the call silent with nothing in the log
 * to say so (2026-09-29: ICE completed, no `start DTLS`, no warning).  This
 * covers the gap before that, from the answer on.  A writable pair with no
 * media is our own bug: say so, and start it.  No writable pair yet is
 * reported once and watched until there is one. */
static void media_watchdog_h(void *arg)
{
	struct icea_sess *s = arg;
	struct le *le;
	bool pending = false;

	LIST_FOREACH(&s->medial, le) {
		struct icea_media *m = le->data;
		const struct icea_pair *p;

		if (!media_start_pending(s, m))
			continue;

		p = m->cur->selected[0];
		if (p && p->ws == ICEA_WS_WRITABLE) {
			warning("EchoSDK/ice: '%s': ICE is %s on %J but media"
				" never started — starting it now\n",
				sdp_media_name(m->sdpm),
				icea_state_name(m->cur->state), &p->rc->addr);
			fire_connh(m);
			continue;
		}

		pending = true;
		if (!m->nomedia_warned) {
			m->nomedia_warned = true;
			warning("EchoSDK/ice: '%s': no writable ICE pair %u ms"
				" after the answer (ICE %s) — no audio until"
				" there is one\n", sdp_media_name(m->sdpm),
				ICEA_MEDIA_START_DEADLINE_MS,
				icea_state_name(m->cur->state));
		}
	}

	if (pending)
		tmr_start(&s->tmr_media, ICEA_MEDIA_START_POLL_MS,
			  media_watchdog_h, s);
}


static void arm_media_watchdog(struct icea_sess *s)
{
	struct le *le;

	if (tmr_isrunning(&s->tmr_media))
		return;

	LIST_FOREACH(&s->medial, le) {
		if (media_start_pending(s, le->data)) {
			tmr_start(&s->tmr_media, ICEA_MEDIA_START_DEADLINE_MS,
				  media_watchdog_h, s);
			return;
		}
	}
}


static void bounce_h(void *arg)
{
	struct icea_sess *s = arg;
	struct le *le;

	/* the held re-offer: release once secure, abandon at the deadline */
	if (tmr_isrunning(&s->tmr_reoffer) || s->reoffer_due) {
		if (sess_awaiting_secure(s)) {
			if (icea_now() < s->reoffer_due) {
				tmr_start(&s->tmr_reoffer, ICEA_REOFFER_POLL_MS,
					  bounce_h, s);
				return;
			}
			s->reoffer_due = 0;
			warning("EchoSDK/ice: media encryption not secure after"
				" %u ms — dropping the candidate re-offer\n",
				ICEA_REOFFER_SECURE_DEADLINE_MS);
			return;
		}
		s->reoffer_due = 0;
		write_all_sdp(s);
		info("EchoSDK/ice: re-offering the selected candidate\n");
		estab_release(s, 0, 0, NULL);
		return;
	}

	if (s->bounce_gather) {
		s->bounce_gather = false;
		sess_start_gathering(s);
	}

	if (s->bounce_connh) {
		s->bounce_connh = false;
		LIST_FOREACH(&s->medial, le) {
			struct icea_media *m = le->data;
			if (m->held) {
				m->held = false;
				info("EchoSDK/ice: '%s': answer is out —"
				     " starting media\n", sdp_media_name(m->sdpm));
				fire_connh(m);
			}
		}
	}

	if (s->bounce_restart) {
		s->bounce_restart = false;
		int err = bsdk_ice_restart(s->call, NULL);
		if (err && err != EALREADY)
			warning("EchoSDK/ice: restart in answer to the peer's"
				" failed (%m)\n", err);
	}
}


/* ── The media-NAT vtable ────────────────────────────────────────────────── */

static void media_destructor(void *data)
{
	struct icea_media *m = data;

	list_unlink(&m->le);

	/* generation first: it stops the tick and drops the ping table,
	 * and its ports hold references to the sockets below */
	m->cur = mem_deref(m->cur);

	if (m->tx_mtx) {
		mtx_lock(m->tx_mtx);
		for (unsigned i = 0; i < ICEA_MAX_COMP; i++)
			m->txsel[i].us = mem_deref(m->txsel[i].us);
		mtx_unlock(m->tx_mtx);
	}

	/* helpers before their sockets: the helper destructor locks us->lock */
	for (unsigned i = 0; i < ICEA_MAX_COMP; i++) {
		mem_deref(m->uh[i]);
		mem_deref(m->sock[i]);
	}
	mem_deref(m->sdpm);
	mem_deref(m->tx_mtx);
}


static void sess_destructor(void *data)
{
	struct icea_sess *s = data;
	struct le *le, *tmp;

	tmr_cancel(&s->tmr_gather);
	tmr_cancel(&s->tmr_bounce);
	tmr_cancel(&s->tmr_reoffer);
	tmr_cancel(&s->tmr_media);
	list_unlink(&s->le);

	/* the media wrappers belong to baresip's streams, which may outlive
	 * us — cut them loose (2026-08 shim lesson: nothing guarantees the
	 * teardown order on every path) */
	LIST_FOREACH_SAFE(&s->medial, le, tmp) {
		struct icea_media *m = le->data;
		m->sess = NULL;
		m->cur  = mem_deref(m->cur);
		list_unlink(&m->le);
	}

	mem_deref(s->dnsq);
	list_flush(&s->serverl);
	mem_deref(s->sdp);
	mem_deref(s->srv_host);
	mem_deref(s->user);
	mem_deref(s->pass);
}


static void dns_h(int err, const struct sa *srv, void *arg)
{
	struct icea_sess *s = arg;
	struct icea_server *server = NULL;

	if (err || !srv) {
		warning("EchoSDK/ice: NAT server %s could not be resolved (%m)"
			" — host candidates only\n",
			s->srv_host ? s->srv_host : "?", err);
	}
	else if (0 == icea_server_alloc(&server, s->srv_turn, IPPROTO_UDP,
					false, srv, s->user, s->pass)) {
		list_append(&s->serverl, &server->le, server);
		info("EchoSDK/ice: %s server %J\n",
		     s->srv_turn ? "TURN" : "STUN", srv);
	}

	sess_start_gathering(s);
}


static void conf_from_sdk(struct icea_conf *c)
{
	static const struct icea_conf def = ICEA_CONF_DEFAULT;

	*c = def;
	c->debug   = g_bsdk.cfg.log_level >= 3;
	c->debug_h = sdk_debug_h;
	/* transport policy and server arrays arrive with the PR4 config */
}


static int icea_sessh(struct mnat_sess **sessp, const struct mnat *mnat,
		      struct dnsc *dnsc, int af, const struct stun_uri *srv,
		      const char *user, const char *pass,
		      struct sdp_session *sdp, bool offerer,
		      mnat_estab_h *estabh, void *arg)
{
	struct icea_sess *s;
	int err = 0;
	(void)mnat;

	if (!sessp || !sdp || !estabh)
		return EINVAL;

	s = mem_zalloc(sizeof(*s), sess_destructor);
	if (!s)
		return ENOMEM;

	tmr_init(&s->tmr_gather);
	tmr_init(&s->tmr_bounce);
	tmr_init(&s->tmr_reoffer);
	tmr_init(&s->tmr_media);
	list_init(&s->medial);
	list_init(&s->serverl);

	s->call    = arg;
	s->estabh  = estabh;
	s->sdp     = mem_ref(sdp);
	s->dnsc    = dnsc;
	s->af      = af;
	s->offerer = offerer;
	conf_from_sdk(&s->conf);
	sess_new_credentials(s);

	/* the account's one NAT server (arrays: PR4) — copied, the account
	 * can be reconfigured under a live call */
	if (srv && srv->host) {
		s->srv_set  = true;
		s->srv_turn = srv->scheme == STUN_SCHEME_TURN;
		s->srv_port = srv->port;
		err  = str_dup(&s->srv_host, srv->host);
		if (user) err |= str_dup(&s->user, user);
		if (pass) err |= str_dup(&s->pass, pass);
	}
	if (err)
		goto out;

	/* session-level credentials go out with the very first SDP */
	err  = sdp_session_set_lattr(sdp, true, ice_attr_ufrag, "%s", s->lufrag);
	err |= sdp_session_set_lattr(sdp, true, ice_attr_pwd, "%s", s->lpwd);
	if (err)
		goto out;

	if (s->srv_set && dnsc) {
		err = stun_server_discover(&s->dnsq, dnsc,
					   s->srv_turn ? stun_usage_relay
						       : stun_usage_binding,
					   stun_proto_udp, af, s->srv_host,
					   s->srv_port, dns_h, s);
		if (err) {
			warning("EchoSDK/ice: NAT server lookup failed (%m)\n",
				err);
			err = 0;
			s->bounce_gather = true;
			tmr_start(&s->tmr_bounce, 0, bounce_h, s);
		}
	}
	else {
		/* nothing to resolve — but never gather from inside sessh:
		 * baresip has not attached the streams yet */
		s->bounce_gather = true;
		tmr_start(&s->tmr_bounce, 0, bounce_h, s);
	}

	/* A bound on every gather (the empty-medial hang: an INVITE that
	 * never left).  Zero = wait for baresip's own conclusion. */
	if (g_bsdk.cfg.ice_gathering_timeout_ms)
		tmr_start(&s->tmr_gather, g_bsdk.cfg.ice_gathering_timeout_ms,
			  gather_deadline_h, s);

	list_append(&g_sessl, &s->le, s);

 out:
	if (err)
		mem_deref(s);
	else
		*sessp = (struct mnat_sess *)s;
	return err;
}


static int icea_mediah(struct mnat_media **mp, struct mnat_sess *sess,
		       struct udp_sock *sock1, struct udp_sock *sock2,
		       struct sdp_media *sdpm, mnat_connected_h *connh,
		       void *arg)
{
	struct icea_sess *s = (struct icea_sess *)sess;
	struct icea_media *m;
	const struct sa *laddr;
	int err;

	if (!mp || !s || !sock1 || !sdpm)
		return EINVAL;

	m = mem_zalloc(sizeof(*m), media_destructor);
	if (!m)
		return ENOMEM;

	m->sess    = s;
	m->sdpm    = mem_ref(sdpm);
	m->sock[0] = mem_ref(sock1);
	m->sock[1] = sock2 ? mem_ref(sock2) : NULL;
	m->ncomp   = sock2 ? 2 : 1;
	m->connh   = connh;
	m->arg     = arg;

	err = mutex_alloc(&m->tx_mtx);
	if (err)
		goto out;

	for (unsigned i = 0; i < m->ncomp; i++) {
		m->hook[i].m = m;
		m->hook[i].compid = i + 1;
		err = udp_register_helper(&m->uh[i], m->sock[i], ICEA_LAYER_ICE,
					  stream_send_h, stream_recv_h,
					  &m->hook[i]);
		if (err)
			goto out;
	}

	/* what the peer is told before gathering says otherwise: the
	 * session address (a stream's own laddr is 0.0.0.0:port until ICE
	 * picks one — the re-offer rule compares against this) */
	laddr = sdp_media_laddr(sdpm);
	if (!laddr || !sa_isset(laddr, SA_ADDR))
		laddr = sdp_session_laddr(s->sdp);
	if (laddr)
		m->signalled = *laddr;

	list_append(&s->medial, &m->le, m);

	err = gen_new(m, NULL);
	if (err)
		goto out;

	/* Host candidates only so far, so this is also the first honest
	 * baseline for `signalled`: what the peer would be told if the offer
	 * went out right now. */
	media_write_sdp(s, m);

	if (s->servers_ready)
		(void)media_start_gather(m);

 out:
	if (err)
		mem_deref(m);
	else
		*mp = (struct mnat_media *)m;
	return err;
}


static int icea_updateh(struct mnat_sess *sess)
{
	struct icea_sess *s = (struct icea_sess *)sess;
	struct le *le;
	bool any_changed = false, answer_to_restart = false;
	int err = 0;

	if (!s)
		return EINVAL;

	/* baresip runs this from inside estabh while building OUR offer, with
	 * the peer's previous answer still in the SDP.  Reading it would pair
	 * new credentials with old ones (2026-09-02).  Re-assert the vpeer
	 * and do nothing else. */
	if (s->in_estab && s->await_answer) {
		LIST_FOREACH(&s->medial, le) {
			struct icea_media *m = le->data;
			if (m->connh_fired && !m->held && !m->plain)
				fire_connh(m);
		}
		return 0;
	}

	/* Incoming and being asked to update: baresip is building our answer
	 * (call_answer() -> call_apply_sdp()).  Whatever runs after this call
	 * stack may start media, and from here on the caller can answer our
	 * checks, so an unanswered one counts again. */
	if (call_is_incoming(s)) {
		s->answered = true;
		LIST_FOREACH(&s->medial, le) {
			struct icea_media *m = le->data;
			icea_gen_set_early(m->cur, false);
		}
	}

	LIST_FOREACH(&s->medial, le) {
		struct icea_media *m = le->data;
		bool changed = false;

		if (!m->cur || !sdp_media_has_media(m->sdpm))
			continue;

		err |= icea_sdp_read_remote(m->cur, s->sdp, m->sdpm, &changed);
		any_changed |= changed;

		if (!icea_sdp_peer_has_ice(m->cur)) {
			/* RFC 8445 §5.1: no ICE on the far side.  baresip's
			 * module left the stream waiting for ever here;
			 * we start plain media on the signalled address. */
			if (!m->plain)
				info("EchoSDK/ice: '%s': peer offers no ICE —"
				     " plain RTP to the signalled address\n",
				     sdp_media_name(m->sdpm));
			fire_connh_plain(m);
			continue;
		}
		if (!icea_sdp_peer_supports_ice(m->cur, m->sdpm)) {
			warning("EchoSDK/ice: '%s': ICE mismatch — using the"
				" signalled address\n", sdp_media_name(m->sdpm));
			fire_connh_plain(m);
			media_write_sdp(s, m);
			continue;
		}
	}

	if (s->await_answer) {
		/* RFC 8445 §9: the peer re-mints credentials in its answer to
		 * our restart.  Not a peer-initiated restart (2026-09-02:
		 * three re-INVITEs in a second reading it as one). */
		s->await_answer = false;
		answer_to_restart = true;
	}

	if (any_changed && !answer_to_restart) {
		/* A credential change is the only peer-restart signal —
		 * a=connection:new is not (2026-08-31, reverted within the
		 * hour: a storm).  Only worth a restart of our own while the
		 * media is not up; with consent freshness this cap is a
		 * backstop. */
		if (sess_awaiting_secure(s)) {
			if (s->restarting || s->bounce_restart) {
				info("EchoSDK/ice: peer restarted ICE; ours is"
				     " already under way\n");
			}
			else if (s->rrestarts < ICEA_MAX_REMOTE_RESTARTS) {
				s->rrestarts++;
				info("EchoSDK/ice: peer restarted ICE (%u/%u) —"
				     " restarting ours\n", s->rrestarts,
				     ICEA_MAX_REMOTE_RESTARTS);
				s->bounce_restart = true;
				tmr_start(&s->tmr_bounce, 0, bounce_h, s);
			}
			else {
				warning("EchoSDK/ice: peer has restarted ICE %u"
					" times — not answering again\n",
					s->rrestarts);
			}
		}
		else {
			info("EchoSDK/ice: peer changed its ICE credentials"
			     " while media is up — checks continue against"
			     " the new ones\n");
		}
	}

	LIST_FOREACH(&s->medial, le) {
		struct icea_media *m = le->data;

		if (!m->cur || m->plain)
			continue;

		media_maybe_start_checks(m);

		/* refresh what the SDP carries (laddr, candidates) */
		media_write_sdp(s, m);

		/* stream_update() has just reset tx.raddr to the signalled
		 * address; put the virtual peer back (the 172.16.11.52
		 * incident: a call aimed at an RFC 1918 address for good) */
		if (m->connh_fired && !m->held)
			fire_connh(m);

		/* held during ringing: deliver once the answer is out.  The
		 * bounce is what makes this safe while we are still inside
		 * call_answer(): it runs after the 200 has been sent. */
		if (m->held && answer_is_out(s)) {
			s->bounce_connh = true;
			tmr_start(&s->tmr_bounce, 0, bounce_h, s);
		}
	}

	arm_media_watchdog(s);

	return err;
}


static void icea_attrh(struct mnat_media *mm, const char *name,
		       const char *value)
{
	struct icea_media *m = (struct icea_media *)mm;
	struct ice_cand_attr attr;

	/* Only peerconn.c calls this in baresip; SIP calls go through
	 * updateh.  Kept for completeness. */
	if (!m || !m->cur || !name)
		return;

	if (0 == str_casecmp(name, ice_attr_cand) && value) {
		if (0 == icea_cand_attr_decode(&attr, value))
			(void)icea_cand_add_remote(m->cur, &attr);
	}
	else if (0 == str_casecmp(name, ice_attr_ufrag) && value) {
		m->cur->rufrag = mem_deref(m->cur->rufrag);
		(void)str_dup(&m->cur->rufrag, value);
	}
	else if (0 == str_casecmp(name, ice_attr_pwd) && value) {
		m->cur->rpwd = mem_deref(m->cur->rpwd);
		(void)str_dup(&m->cur->rpwd, value);
	}

	m->cur->remote_ready = m->cur->rufrag && m->cur->rpwd &&
			       !list_isempty(&m->cur->rcandl);
	media_maybe_start_checks(m);
}


static struct mnat mnat_ice = {
	.id             = "ice",
	.ftag           = "+sip.ice",
	.wait_connected = true,
	.sessh          = icea_sessh,
	.mediah         = icea_mediah,
	.updateh        = icea_updateh,
	.attrh          = icea_attrh,
};


/* ── SDK seams ───────────────────────────────────────────────────────────── */

int bsdk_ice_restart(void *call, const struct sa *laddr)
{
	struct icea_sess *s = sess_find(call);
	struct le *le;
	uint32_t deadline;
	int err = 0;

	if (!s || list_isempty(&s->medial))
		return ENOENT;
	if (s->restarting)
		return EALREADY;

	sess_new_credentials(s);

	if (laddr && sa_isset(laddr, SA_ADDR))
		sdp_session_set_laddr(s->sdp, laddr);

	LIST_FOREACH(&s->medial, le) {
		struct icea_media *m = le->data;
		struct sa local;

		/* PR1: the previous generation goes at once; PR3 keeps it
		 * carrying media until the new one is nominated */
		err = gen_new(m, laddr);
		if (err) {
			warning("EchoSDK/ice: restart: '%s' failed (%m)\n",
				sdp_media_name(m->sdpm), err);
			continue;
		}

		/* stale candidates out; the new interface with the socket's
		 * port in, so a gather that never reports still offers a
		 * working address (degraded to plain RTP by the peer) */
		icea_sdp_clear_local(m->sdpm);
		if (laddr && sa_isset(laddr, SA_ADDR) &&
		    !udp_local_get(m->sock[0], &local)) {
			struct sa media = *laddr;
			sa_set_port(&media, sa_port(&local));
			sdp_media_set_laddr(m->sdpm, &media);
			m->signalled = media;
		}
		m->held = false;

		for (unsigned i = 0; i < m->ncomp; i++)
			publish_txsel(m, i + 1);
	}

	err = sdp_session_set_lattr(s->sdp, true, ice_attr_ufrag, "%s", s->lufrag);
	err |= sdp_session_set_lattr(s->sdp, true, ice_attr_pwd, "%s", s->lpwd);

	s->released     = false;
	s->restarting   = true;
	s->reoffered    = false;
	s->await_answer = true;
	s->reoffer_due  = 0;
	tmr_cancel(&s->tmr_reoffer);

	/* Off this call stack: a host-only gather concludes at once, and the
	 * re-INVITE must follow from the estab handler, not from inside the
	 * netmon timer that asked for the restart. */
	s->bounce_gather = true;
	tmr_start(&s->tmr_bounce, 0, bounce_h, s);

	deadline = g_bsdk.cfg.ice_gathering_timeout_ms
		 ? g_bsdk.cfg.ice_gathering_timeout_ms
		 : BSDK_ICE_RESTART_DEADLINE_MS;
	tmr_start(&s->tmr_gather, deadline, gather_deadline_h, s);

	if (laddr && sa_isset(laddr, SA_ADDR))
		info("EchoSDK/ice: restarting ICE on %j — new credentials,"
		     " re-gathering, re-INVITE within %u ms\n", laddr, deadline);
	else
		info("EchoSDK/ice: restarting ICE — new credentials,"
		     " re-gathering, re-INVITE within %u ms\n", deadline);

	return err;
}


bool bsdk_ice_call_active(void *call)
{
	struct icea_sess *s = sess_find(call);
	struct le *le;

	if (!s || list_isempty(&s->medial))
		return false;

	LIST_FOREACH(&s->medial, le) {
		const struct icea_media *m = le->data;
		if (m->cur && !m->plain)
			return true;
	}
	return false;
}


int bsdk_ice_call_state(void *call)
{
	struct icea_sess *s = sess_find(call);

	return s ? (int)sess_state(s) : (int)ICEA_STATE_NEW;
}


bool bsdk_ice_call_selected(void *call, struct sa *local, struct sa *remote,
			    int *ltype, int *rtype, uint32_t *rtt_ms,
			    uint32_t *generation)
{
	struct icea_sess *s = sess_find(call);
	const struct icea_media *m;
	const struct icea_pair *p;

	if (!s || list_isempty(&s->medial))
		return false;
	m = s->medial.head->data;
	if (!m->cur || !m->cur->selected[0])
		return false;
	p = m->cur->selected[0];

	if (local)      *local  = p->lc->addr;
	if (remote)     *remote = p->rc->addr;
	if (ltype)      *ltype  = p->lc->type;
	if (rtype)      *rtype  = p->rc->type;
	if (rtt_ms)     *rtt_ms = p->rtt_ms;
	if (generation) *generation = m->cur->seq;
	return true;
}


bool bsdk_ice_call_laddr(void *call, struct sa *base)
{
	struct icea_sess *s = sess_find(call);
	const struct icea_media *m;
	const struct icea_pair *p;

	if (!s || !base || list_isempty(&s->medial))
		return false;
	m = s->medial.head->data;
	if (!m->cur || !m->cur->selected[0])
		return false;

	p = m->cur->selected[0];
	/* The base of a srflx/prflx candidate is the host candidate it was
	 * discovered through — the address the packets actually leave from. */
	*base = p->lc->base ? p->lc->base->addr : p->lc->addr;
	return sa_isset(base, SA_ADDR);
}


bool bsdk_ice_setup_pending(void *call)
{
	struct icea_sess *s = sess_find(call);
	struct le *le;

	if (!s)
		return false;

	LIST_FOREACH(&s->medial, le) {
		const struct icea_media *m = le->data;

		if (!m->cur || m->plain)
			continue;
		if (m->cur->state < ICEA_STATE_CONNECTED)
			return true;
	}
	return false;
}


void bsdk_ice_network_changed(void)
{
	struct le *le;

	LIST_FOREACH(&g_sessl, le) {
		struct icea_sess *s = le->data;
		struct le *mle;

		if (!s->servers_ready || list_isempty(&s->serverl))
			continue;

		/* Re-run server-reflexive gathering on the ports we already
		 * have.  The sockets are wildcard-bound and follow the new
		 * default route by themselves, so nothing needs re-creating —
		 * but the NAT mapping behind them is usually gone, and the
		 * candidate we would otherwise re-offer is the old one.
		 *
		 * Per-interface sockets, and with them gathering on an
		 * interface that is not carrying the signalling, are a
		 * separate change; this is what can be refreshed without
		 * new credentials or a new generation. */
		LIST_FOREACH(&s->medial, mle) {
			struct icea_media *m = mle->data;
			struct le *ple;

			if (!m->cur || m->plain)
				continue;

			LIST_FOREACH(&m->cur->portl, ple) {
				struct icea_port *port = ple->data;
				port->gathered = false;
			}
			m->cur->gathered = false;
			(void)media_start_gather(m);
		}

		info("EchoSDK/ice: network changed — re-gathering reflexive"
		     " candidates\n");
	}
}


int bsdk_ice_module_init(void)
{
	if (mnat_ice.le.list)
		return 0;             /* re-init after shutdown: already there */

	mnat_register(baresip_mnatl(), &mnat_ice);
	return 0;
}


void bsdk_ice_module_close(void)
{
	if (!mnat_ice.le.list)
		return;

	mnat_unregister(&mnat_ice);
	memset(&mnat_ice.le, 0, sizeof(mnat_ice.le));
}
