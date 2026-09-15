/**
 * @file ice_stund.c  echo-ice: the STUN server side of connectivity checks
 *
 * RFC 8445 §7.3.  A Binding Request from the peer is verified against
 * *our* password (the request's MESSAGE-INTEGRITY is keyed on the
 * receiver's credentials), which is why it can be answered before the
 * peer's answer has told us its own credentials: Asterisk sends its first
 * check the instant it sends 200 OK, ahead of our processing of it.  Such
 * an early request is answered, its source recorded as a peer-reflexive
 * remote candidate, and the pair marked for a triggered check once the
 * remote credentials arrive.
 *
 * Replies are built with stun_msg_encode() and sent below the media's ICE
 * helper (ice_check.c: icea_send_stun), never with stun_reply(): its
 * internal udp_send would re-enter our send helper and have its
 * destination rewritten to the selected pair.
 */

#include <string.h>
#include "ice_internal.h"


static const char sw[] = "EchoSDK echo-ice";


/* Does the USERNAME of this request name our ufrag? */
bool icea_stund_owns(const struct icea_gen *gen, const struct stun_msg *msg)
{
	struct stun_attr *attr;
	struct pl lu, ru;

	if (!gen || !msg)
		return false;

	attr = stun_msg_attr(msg, STUN_ATTR_USERNAME);
	if (!attr || !attr->v.username)
		return false;

	if (re_regex(attr->v.username, str_len(attr->v.username),
		     "[^:]+:[^]+", &lu, &ru))
		return false;

	return 0 == pl_strcmp(&lu, gen->lufrag);
}


static int send_reply(struct icea_gen *gen, struct icea_port *port,
		      const struct sa *dst, const struct stun_msg *req,
		      uint16_t scode, const char *reason)
{
	struct stun_errcode ec = { scode, (char *)reason };
	struct mbuf *mb;
	size_t presz = port->turnc ? 4 : 0;
	int err;

	mb = mbuf_alloc(presz + 128);
	if (!mb)
		return ENOMEM;
	mb->pos = mb->end = presz;

	if (scode == 0) {
		err = stun_msg_encode(mb, STUN_METHOD_BINDING,
				      STUN_CLASS_SUCCESS_RESP, stun_msg_tid(req),
				      NULL, (const uint8_t *)gen->lpwd,
				      str_len(gen->lpwd), true, 0, 2,
				      STUN_ATTR_XOR_MAPPED_ADDR, dst,
				      STUN_ATTR_SOFTWARE, sw);
	}
	else {
		err = stun_msg_encode(mb, STUN_METHOD_BINDING,
				      STUN_CLASS_ERROR_RESP, stun_msg_tid(req),
				      &ec, (const uint8_t *)gen->lpwd,
				      str_len(gen->lpwd), true, 0, 1,
				      STUN_ATTR_SOFTWARE, sw);
	}
	if (!err) {
		mb->pos = presz;
		err = icea_send_stun(port, dst, mb);
	}

	mem_deref(mb);
	return err;
}


void icea_stund_recv(struct icea_gen *gen, struct icea_port *port,
		     const struct sa *src, const struct stun_msg *req)
{
	struct stun_attr *attr;
	struct icea_cand *rc, *lc;
	struct icea_pair *p;
	struct pl lu, ru;
	enum ice_role rrole = ICE_ROLE_UNKNOWN;
	uint64_t tiebrk = 0, now = icea_now();
	uint32_t prio;
	bool use_cand;

	if (!gen || !port || !src || !req)
		return;

	/* Not for us at all (bad fingerprint): drop silently. */
	if (stun_msg_chk_fingerprint(req))
		return;

	/* §7.3: integrity is keyed on the receiver's — our — password. */
	if (stun_msg_chk_mi(req, (const uint8_t *)gen->lpwd,
			    str_len(gen->lpwd))) {
		(void)send_reply(gen, port, src, req, 401, "Unauthorized");
		return;
	}

	attr = stun_msg_attr(req, STUN_ATTR_USERNAME);
	if (!attr || !attr->v.username ||
	    re_regex(attr->v.username, str_len(attr->v.username),
		     "[^:]+:[^]+", &lu, &ru)) {
		(void)send_reply(gen, port, src, req, 400, "Bad Request");
		return;
	}
	if (pl_strcmp(&lu, gen->lufrag)) {
		(void)send_reply(gen, port, src, req, 401, "Unauthorized");
		return;
	}
	/* The peer names its own ufrag too.  Once we know it, a different
	 * one means the peer has restarted and this check belongs to the
	 * generation that will replace us — 401 it (correction 4). */
	if (gen->rufrag && pl_strcmp(&ru, gen->rufrag)) {
		(void)send_reply(gen, port, src, req, 401, "Unauthorized");
		return;
	}

	attr = stun_msg_attr(req, STUN_ATTR_CONTROLLED);
	if (attr) {
		rrole = ICE_ROLE_CONTROLLED;
		tiebrk = attr->v.uint64;
	}
	attr = stun_msg_attr(req, STUN_ATTR_CONTROLLING);
	if (attr) {
		rrole = ICE_ROLE_CONTROLLING;
		tiebrk = attr->v.uint64;
	}

	/* §7.3.1.1: both sides in the same role. */
	if (rrole != ICE_ROLE_UNKNOWN && rrole == gen->role) {
		if (gen->tiebrk >= tiebrk) {
			gen->role = gen->role == ICE_ROLE_CONTROLLING
				  ? ICE_ROLE_CONTROLLED : ICE_ROLE_CONTROLLING;
			gen->role_switch_pending = true;
			ICEA_DEBUG(gen, "ice[gen %u]: role conflict, switching"
				   " to %s\n", gen->seq,
				   ice_role2name(gen->role));
		}
		else {
			(void)send_reply(gen, port, src, req, 487,
					 "Role Conflict");
			return;
		}
	}

	attr = stun_msg_attr(req, STUN_ATTR_PRIORITY);
	if (!attr) {
		(void)send_reply(gen, port, src, req, 400, "Bad Request");
		return;
	}
	prio = attr->v.uint32;
	use_cand = stun_msg_attr(req, STUN_ATTR_USE_CAND) != NULL;

	/* §7.3.1.3: learn a peer-reflexive remote candidate. */
	rc = icea_cand_find(&gen->rcandl, port->compid, src);
	if (!rc)
		rc = icea_cand_add_prflx_remote(gen, port->compid, prio, src);

	/* §7.3.1.4: the pair this check arrived on, created if missing. */
	lc = icea_lcand_on_port(gen, port->compid, port);
	p = (lc && rc) ? icea_pair_create(gen, lc, rc) : NULL;
	if (p) {
		p->last_ping_recv = now;
		p->receiving      = true;
		if (p->state != ICE_CANDPAIR_SUCCEEDED)
			p->triggered = true;

		if (use_cand && gen->role == ICE_ROLE_CONTROLLED) {
			/* Correction 5: a second nomination is honoured
			 * only from a peer that negotiated renomination. */
			bool already = false;
			struct le *le;
			LIST_FOREACH(&gen->pairl, le) {
				const struct icea_pair *q = le->data;
				if (q != p && q->remote_nominated &&
				    q->lc->compid == p->lc->compid)
					already = true;
			}
			if (!already || gen->renomination) {
				if (p->valid)
					p->remote_nominated = true;
				else
					p->use_cand_pending = true;
			}
		}
	}

	(void)send_reply(gen, port, src, req, 0, NULL);

	ICEA_DEBUG(gen, "ice[gen %u]: rx check {%u} from %J%s%s\n", gen->seq,
		   port->compid, src, use_cand ? " [USE]" : "",
		   gen->remote_ready ? "" : " (before the answer)");

	icea_check_tick_now(gen);
}
