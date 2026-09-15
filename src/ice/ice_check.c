/**
 * @file ice_check.c  echo-ice: connectivity checks and the scheduler
 *
 * One timer per generation.  Every tick prunes the ping table, refreshes
 * pair states and the selection (ice_select.c), then sends at most one
 * Binding Request, chosen in libwebrtc's order: the selected pair when it
 * is due, triggered checks, never-pinged pairs by priority (unfreezing one
 * foundation at a time), backup pairs every 25 s while strong, and the
 * least recently pinged pair otherwise.  The tick runs at 48 ms while the
 * generation is weak and 480 ms once it is strong.
 *
 * Pings are single-shot: no RFC 5389 retransmission ladder, the scheduler
 * is the retry.  Each carries a fresh transaction ID recorded in the
 * generation's ping table with its pair; responses are matched there,
 * never through libre's ctrans (which times out after one RTO and would
 * orphan a late response).  A success response is also the RFC 7675
 * consent refresh for that pair.
 *
 * Error responses never move a pair to a terminal state.  A 401 during a
 * restart is the peer still rebuilding its ICE session (Asterisk does this
 * for ~1 s); the pair is backed off exponentially and tried again.
 */

#include <string.h>
#include "ice_internal.h"



struct icea_cand *icea_lcand_on_port(const struct icea_gen *gen,
				     unsigned compid,
				     const struct icea_port *port)
{
	struct icea_cand *any = NULL;
	struct le *le;

	LIST_FOREACH(&gen->lcandl, le) {
		struct icea_cand *c = le->data;

		if (c->compid != compid || c->port != port)
			continue;
		if (c->type == ICE_CAND_TYPE_HOST)
			return c;
		if (!any)
			any = c;
	}
	return any;
}


/* ── Sending ─────────────────────────────────────────────────────────────── */

int icea_send_stun(struct icea_port *port, const struct sa *dst,
		   struct mbuf *mb)
{
	if (!port || !dst || !mb)
		return EINVAL;

	/* Stream sockets: enter the helper chain below our own layer, so
	 * neither DTLS/SRTP above nor our dst-rewriting send helper touch
	 * the packet, while a TURN client below still can.  Own ports run
	 * their full (short) chain. */
	if (port->is_stream_sock && port->bypass_uh)
		return udp_send_helper(port->us, dst, mb, port->bypass_uh);

	return udp_send(port->us, dst, mb);
}


static void ping_destructor(void *arg)
{
	struct icea_ping *ping = arg;

	list_unlink(&ping->le);
}


static unsigned count_unanswered(const struct icea_gen *gen,
				 const struct icea_pair *p)
{
	struct le *le;
	unsigned n = 0;

	LIST_FOREACH(&gen->pingl, le) {
		const struct icea_ping *ping = le->data;
		if (ping->pair == p)
			n++;
	}
	return n;
}


static int send_ping(struct icea_gen *gen, struct icea_pair *p, bool use_cand)
{
	struct icea_ping *ping;
	struct mbuf *mb;
	char username[80];
	uint32_t prio_prflx;
	uint16_t ctrl_attr;
	size_t presz;
	int err;

	if (!gen->rufrag || !gen->rpwd)
		return EPROTO;

	/* Only the controlling agent nominates (RFC 8445 §7.2.5.3.4). */
	if (gen->role != ICE_ROLE_CONTROLLING)
		use_cand = false;
	ctrl_attr = gen->role == ICE_ROLE_CONTROLLING ? STUN_ATTR_CONTROLLING
						      : STUN_ATTR_CONTROLLED;

	re_snprintf(username, sizeof(username), "%s:%s", gen->rufrag,
		    gen->lufrag);

	/* PRIORITY carries what a peer-reflexive candidate learned from
	 * this check would have (§7.1.1), computed from our base. */
	prio_prflx = ice_cand_calc_prio(ICE_CAND_TYPE_PRFLX,
					icea_cand_lpref(p->lc->port),
					p->lc->compid);

	ping = mem_zalloc(sizeof(*ping), ping_destructor);
	if (!ping)
		return ENOMEM;

	stun_generate_tid(ping->tid);
	ping->pair       = p;
	ping->sent       = icea_now();
	ping->nominating = use_cand;

	/* Headroom for a TURN channel header on relay pairs; unused else. */
	presz = p->lc->type == ICE_CAND_TYPE_RELAY ? 4 : 0;

	mb = mbuf_alloc(presz + 128);
	if (!mb) {
		mem_deref(ping);
		return ENOMEM;
	}
	mb->pos = mb->end = presz;

	err = stun_msg_encode(mb, STUN_METHOD_BINDING, STUN_CLASS_REQUEST,
			      ping->tid, NULL,
			      (const uint8_t *)gen->rpwd, str_len(gen->rpwd),
			      true, 0, 4,
			      STUN_ATTR_USERNAME, username,
			      STUN_ATTR_PRIORITY, &prio_prflx,
			      ctrl_attr, &gen->tiebrk,
			      STUN_ATTR_USE_CAND, use_cand ? &use_cand : NULL);
	if (err)
		goto out;

	if (p->lc->type == ICE_CAND_TYPE_RELAY && p->lc->port->turnc) {
		/* Relay pairs always use a channel: Send indications carry
		 * no headroom and cost 36 bytes on every packet. */
		(void)turnc_add_chan(p->lc->port->turnc, &p->rc->addr,
				     NULL, NULL);
	}

	/* Record before sending: a peer on the same host answers inside
	 * udp_send(), and the response must find its transaction. */
	list_append(&gen->pingl, &ping->le, ping);
	p->unanswered++;
	p->last_ping_sent = ping->sent;
	p->triggered      = false;
	if (p->state == ICE_CANDPAIR_WAITING || p->state == ICE_CANDPAIR_FROZEN)
		p->state = ICE_CANDPAIR_INPROGRESS;

	mb->pos = presz;
	err = icea_send_stun(p->lc->port, &p->rc->addr, mb);
	if (err) {
		p->unanswered--;
		goto out;
	}

	ICEA_DEBUG(gen, "ice[gen %u]: tx check {%u} %s %J -> %s %J%s\n",
		   gen->seq, p->lc->compid, ice_cand_type2name(p->lc->type),
		   &p->lc->addr, ice_cand_type2name(p->rc->type), &p->rc->addr,
		   use_cand ? " [USE]" : "");

 out:
	mem_deref(mb);
	if (err)
		mem_deref(ping);
	return err;
}


/* ── Receiving ───────────────────────────────────────────────────────────── */

uint64_t icea_pair_last_received(const struct icea_pair *p)
{
	uint64_t t = p->last_ping_recv;

	if (p->last_data_recv > t)
		t = p->last_data_recv;
	if (p->last_resp_recv > t)
		t = p->last_resp_recv;
	return t;
}


/**
 * Stamp receipt on every pair the arriving packet is evidence for.
 *
 * A datagram proves liveness for a 5-tuple, and a 5-tuple is (socket,
 * remote) — not (candidate, remote).  Every local candidate on one port
 * shares that port's socket, so host, server-reflexive and peer-reflexive
 * pairs to the same remote are all the same path on the wire and the packet
 * is evidence for all of them.
 *
 * Attributing it to one pair instead — the first match, which is the
 * highest-priority one — is wrong in exactly the case that matters.  Behind
 * a NAT the *selected* pair is reflexive while the host pair to the same
 * remote still exists and outranks it, so the media went on stamping the
 * host pair while the selected pair's clock ran out: `receiving` lapsed on
 * the ping cadence alone (2500 ms window, 2500 ms cadence) and the call
 * reported COMPLETED → DISCONNECTED → COMPLETED for ever.  Found against
 * FreeSWITCH on 2026-09-15, where the selected local candidate is reflexive;
 * Asterisk on the same office LAN had hidden it by selecting a host pair.
 *
 * That flapping is not cosmetic: `disconnected` puts the scheduler into its
 * weak cadence, and netmon reads the state to decide whether a media stall
 * is the far end's problem or its own.
 */
static void note_receipt(struct icea_gen *gen, unsigned compid,
			 const struct icea_port *port, const struct sa *src,
			 bool is_data)
{
	uint64_t now = icea_now();
	struct le *le;

	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;

		if (p->lc->compid != compid)
			continue;
		if (port && p->lc->port != port)
			continue;
		if (!sa_cmp(&p->rc->addr, src, SA_ALL))
			continue;

		if (is_data)
			p->last_data_recv = now;
		else
			p->last_ping_recv = now;
		p->receiving = true;
	}
}


void icea_pair_note_data(struct icea_gen *gen, unsigned compid,
			 const struct icea_port *port, const struct sa *src)
{
	note_receipt(gen, compid, port, src, true);
}


void icea_pair_note_ping_recv(struct icea_gen *gen, unsigned compid,
			      const struct icea_port *port, const struct sa *src)
{
	note_receipt(gen, compid, port, src, false);
}


static struct icea_ping *ping_find(const struct icea_gen *gen,
				   const uint8_t *tid)
{
	struct le *le;

	LIST_FOREACH(&gen->pingl, le) {
		struct icea_ping *ping = le->data;
		if (0 == memcmp(ping->tid, tid, STUN_TID_SIZE))
			return ping;
	}
	return NULL;
}


/* §7.2.5.3.3: a success unfreezes the other pairs of its foundation. */
static void unfreeze_foundation(struct icea_gen *gen, const struct icea_pair *p)
{
	struct le *le;

	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *q = le->data;

		if (q->state != ICE_CANDPAIR_FROZEN)
			continue;
		if (str_cmp(q->lc->foundation, p->lc->foundation) ||
		    str_cmp(q->rc->foundation, p->rc->foundation))
			continue;

		q->state = ICE_CANDPAIR_WAITING;
	}
}


static void handle_success(struct icea_gen *gen, struct icea_pair *p,
			   struct icea_ping *ping, const struct sa *mapped,
			   uint64_t now)
{
	struct icea_cand *lc;
	struct icea_pair *vp;
	struct le *le, *tmp;
	uint32_t rtt = (uint32_t)(now - ping->sent);
	bool nominating = ping->nominating;

	/* Credit this response and every older ping on the same pair:
	 * they were not lost, the answers just crossed (libwebrtc's
	 * ReceivedPingResponse semantics). */
	LIST_FOREACH_SAFE(&gen->pingl, le, tmp) {
		struct icea_ping *q = le->data;
		if (q->pair == p && q->sent <= ping->sent)
			mem_deref(q);
	}
	ping = NULL;
	p->unanswered = count_unanswered(gen, p);

	if (p->rtt_samples++ == 0)
		p->rtt_ms = rtt;
	else
		p->rtt_ms = (3 * p->rtt_ms + rtt) / 4;

	p->last_resp_recv  = now;
	p->last_consent_ok = now;
	p->receiving       = true;
	p->ws              = ICEA_WS_WRITABLE;
	p->err_count       = 0;
	p->state           = ICE_CANDPAIR_SUCCEEDED;

	/* §7.2.5.3.1: the mapped address may be a peer-reflexive local
	 * candidate we did not know about (per-destination NAT mapping —
	 * measured against this PBX: STUN saw 213.212.207.242, Asterisk saw
	 * 41.33.94.42 for the same socket). */
	lc = icea_cand_find(&gen->lcandl, p->lc->compid, mapped);
	if (!lc) {
		lc = icea_cand_add_local(gen, ICE_CAND_TYPE_PRFLX, p->lc->compid,
					 p->lc->port, p->lc->base, mapped,
					 icea_cand_lpref(p->lc->port));
		if (lc)
			ICEA_DEBUG(gen, "ice[gen %u]: peer-reflexive local"
				   " {%u} %J\n", gen->seq, p->lc->compid,
				   mapped);
	}

	/* §7.2.5.3.2: the valid pair is (candidate for mapped, remote). */
	vp = (lc && lc != p->lc) ? icea_pair_create(gen, lc, p->rc) : p;
	if (!vp)
		vp = p;

	vp->valid = true;
	vp->state = ICE_CANDPAIR_SUCCEEDED;
	if (vp != p) {
		vp->ws              = ICEA_WS_WRITABLE;
		vp->last_resp_recv  = now;
		vp->last_consent_ok = now;
		vp->receiving       = true;
		vp->rtt_ms          = p->rtt_ms;
		vp->rtt_samples     = p->rtt_samples;
		vp->err_count       = 0;
	}
	p->valid = p->valid || (vp == p);

	/* §7.2.5.3.4: a nominating check that succeeded nominates the
	 * valid pair; the controlled side's flag is remote_nominated. */
	if (nominating && gen->role == ICE_ROLE_CONTROLLING)
		vp->nominated = true;
	if (vp->use_cand_pending) {
		vp->remote_nominated = true;
		vp->use_cand_pending = false;
	}

	unfreeze_foundation(gen, p);

	ICEA_DEBUG(gen, "ice[gen %u]: check ok {%u} %s %J <-> %s %J rtt=%u%s\n",
		   gen->seq, vp->lc->compid, ice_cand_type2name(vp->lc->type),
		   &vp->lc->addr, ice_cand_type2name(vp->rc->type),
		   &vp->rc->addr, rtt, vp->nominated ? " NOMINATED" : "");
}


static void handle_error(struct icea_gen *gen, struct icea_pair *p,
			 struct icea_ping *ping, uint16_t scode, uint64_t now)
{
	uint32_t backoff;

	mem_deref(ping);
	p->unanswered = count_unanswered(gen, p);
	p->last_scode = scode;

	if (scode == 487) {
		/* §7.2.5.1: role conflict.  Switch, re-prioritise at the
		 * next tick, and repeat this check right away. */
		gen->role = gen->role == ICE_ROLE_CONTROLLING
			  ? ICE_ROLE_CONTROLLED : ICE_ROLE_CONTROLLING;
		gen->role_switch_pending = true;
		p->triggered = true;
		p->next_allowed_ping = now;
		ICEA_DEBUG(gen, "ice[gen %u]: 487 role conflict, now %s\n",
			   gen->seq, ice_role2name(gen->role));
		return;
	}

	/* 401/400/438/500: the peer answered, so the path is alive, but
	 * it does not accept this check yet.  Back off and retry; the pair
	 * is not Failed (see the file header). */
	p->err_count++;
	backoff = gen->conf->err_retry_base_ms
		  << (p->err_count > 4 ? 4 : p->err_count - 1);
	if (backoff > gen->conf->err_retry_cap_ms)
		backoff = gen->conf->err_retry_cap_ms;
	p->next_allowed_ping = now + backoff;
	p->last_ping_recv = now;   /* the peer is there; it just said no */

	ICEA_DEBUG(gen, "ice[gen %u]: check {%u} -> %J answered %u"
		   " (%u in a row, retry in %u ms)\n", gen->seq,
		   p->lc->compid, &p->rc->addr, scode, p->err_count, backoff);
}


bool icea_check_recv_response(struct icea_gen *gen, struct icea_port *port,
			      const struct sa *src, const struct stun_msg *msg)
{
	struct icea_ping *ping;
	struct icea_pair *p;
	struct stun_attr *attr;
	uint64_t now = icea_now();

	if (!gen || !msg)
		return false;

	ping = ping_find(gen, stun_msg_tid(msg));
	if (!ping)
		return false;
	p = ping->pair;

	/* §7.2.5.2.1: the response must come from where the request went,
	 * on the socket it left from.  Anything else is treated as lost. */
	if (!sa_cmp(src, &p->rc->addr, SA_ALL) || (port && p->lc->port != port)) {
		ICEA_DEBUG(gen, "ice[gen %u]: response from %J, expected %J"
			   " — ignored\n", gen->seq, src, &p->rc->addr);
		return true;
	}

	switch (stun_msg_class(msg)) {

	case STUN_CLASS_SUCCESS_RESP:
		/* A response without our password's integrity is noise. */
		if (stun_msg_chk_mi(msg, (const uint8_t *)gen->rpwd,
				    str_len(gen->rpwd)))
			return true;
		attr = stun_msg_attr(msg, STUN_ATTR_XOR_MAPPED_ADDR);
		if (!attr)
			attr = stun_msg_attr(msg, STUN_ATTR_MAPPED_ADDR);
		if (!attr) {
			handle_error(gen, p, ping, 400, now);
			return true;
		}
		handle_success(gen, p, ping, &attr->v.sa, now);
		break;

	case STUN_CLASS_ERROR_RESP:
		attr = stun_msg_attr(msg, STUN_ATTR_ERR_CODE);
		handle_error(gen, p, ping, attr ? attr->v.err_code.code : 500,
			     now);
		break;

	default:
		return true;
	}

	/* Something changed: let selection and nomination act on it now
	 * rather than at the next 480 ms tick. */
	icea_check_tick_now(gen);
	return true;
}


/* ── Scheduler ───────────────────────────────────────────────────────────── */

static void prune_pings(struct icea_gen *gen, uint64_t now)
{
	struct le *le, *tmp;

	LIST_FOREACH_SAFE(&gen->pingl, le, tmp) {
		struct icea_ping *ping = le->data;
		struct icea_pair *p = ping->pair;

		if (now - ping->sent < gen->conf->ping_table_expiry_ms)
			break;   /* oldest first */

		mem_deref(ping);
		p->unanswered = count_unanswered(gen, p);
	}
}


static bool pair_stable(const struct icea_pair *p)
{
	return p->rtt_samples >= 4 && p->unanswered == 0;
}


static bool pingable(const struct icea_gen *gen, const struct icea_pair *p,
		     uint64_t now)
{
	if (p->state == ICE_CANDPAIR_FAILED)
		return false;
	if (icea_pair_dead(p, now))
		return false;
	if (now < p->next_allowed_ping)
		return false;
	if (p->err_count >= gen->conf->err_failed_count)
		return false;
	return true;
}


static struct icea_pair *pick_ping(struct icea_gen *gen, uint64_t now)
{
	struct icea_pair *best;
	struct le *le;
	bool weak = icea_gen_weak(gen);
	uint64_t oldest;

	/* 1. The selected pair of each component, at its own cadence. */
	for (unsigned i = 0; i < gen->ncomp; i++) {
		struct icea_pair *s = gen->selected[i];
		uint32_t interval;

		if (!s || !pingable(gen, s, now))
			continue;
		interval = (weak || !pair_stable(s))
			 ? gen->conf->pair_stabilizing_ms
			 : gen->conf->pair_stable_ms;
		if (now - s->last_ping_sent >= interval)
			return s;
	}

	/* 2. Triggered checks (§7.3.1.4), highest priority first. */
	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;
		if (p->triggered && pingable(gen, p, now))
			return p;
	}

	/* 3. Ordinary checks: the highest-priority Waiting pair never
	 *    pinged, else unfreeze the highest-priority Frozen one. */
	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;
		if (p->state == ICE_CANDPAIR_WAITING && !p->last_ping_sent &&
		    pingable(gen, p, now))
			return p;
	}
	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;
		if (p->state == ICE_CANDPAIR_FROZEN && pingable(gen, p, now)) {
			p->state = ICE_CANDPAIR_WAITING;
			return p;
		}
	}

	/* 4. Strong: keep backup (writable, not selected) pairs warm. */
	if (!weak) {
		LIST_FOREACH(&gen->pairl, le) {
			struct icea_pair *p = le->data;
			bool is_sel = false;

			for (unsigned i = 0; i < gen->ncomp; i++)
				if (gen->selected[i] == p)
					is_sel = true;
			if (is_sel || p->ws != ICEA_WS_WRITABLE)
				continue;
			if (now - p->last_ping_sent >= gen->conf->backup_ping_ms &&
			    pingable(gen, p, now))
				return p;
		}
		return NULL;
	}

	/* 5. Weak: the least recently pinged pair that is due. */
	best = NULL;
	oldest = UINT64_MAX;
	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;

		if (!pingable(gen, p, now))
			continue;
		if (now - p->last_ping_sent < gen->conf->ping_weak_ms)
			continue;
		if (p->last_ping_sent < oldest) {
			oldest = p->last_ping_sent;
			best = p;
		}
	}
	return best;
}


static void tick(void *arg)
{
	struct icea_gen *gen = arg;
	struct icea_pair *p;
	uint64_t now = icea_now();

	if (gen->role_switch_pending) {
		icea_pair_reprio(gen);
		gen->role_switch_pending = false;
	}

	prune_pings(gen, now);
	icea_select_tick(gen, now);

	if (gen->remote_ready) {
		p = pick_ping(gen, now);
		if (p)
			(void)send_ping(gen, p, icea_nominate_decision(gen, p));
	}

	if (gen->checking)
		tmr_start(&gen->tmr_tick,
			  icea_gen_weak(gen) ? gen->conf->ping_weak_ms
					     : gen->conf->ping_strong_ms,
			  tick, gen);
}


void icea_check_tick_now(struct icea_gen *gen)
{
	if (gen && gen->checking)
		tmr_start(&gen->tmr_tick, 0, tick, gen);
}


int icea_gen_start_checks(struct icea_gen *gen)
{
	int err;

	if (!gen)
		return EINVAL;
	if (!gen->remote_ready)
		return EPROTO;

	err = icea_pair_form(gen);
	if (err)
		return err;
	icea_pair_prune(gen);
	icea_pair_set_states(gen);

	if (!gen->checking) {
		gen->checking = true;
		if (!gen->checks_started)
			gen->checks_started = icea_now();
		ICEA_DEBUG(gen, "ice[gen %u]: starting checks with %u pairs"
			   " as %s\n", gen->seq, list_count(&gen->pairl),
			   ice_role2name(gen->role));
	}
	if (gen->state < ICEA_STATE_CHECKING)
		icea_gen_set_state(gen, ICEA_STATE_CHECKING);

	tmr_start(&gen->tmr_tick, 0, tick, gen);
	return 0;
}


void icea_gen_hold(struct icea_gen *gen)
{
	if (!gen)
		return;

	gen->checking = false;
	tmr_cancel(&gen->tmr_tick);
}
