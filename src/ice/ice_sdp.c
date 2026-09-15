/**
 * @file ice_sdp.c  echo-ice: SDP grammar (RFC 8839)
 *
 * Writes a=ice-ufrag / a=ice-pwd at session level, one a=candidate per local
 * candidate, a=remote-candidates once we are controlling and concluded, and
 * refreshes the media-level connection address to the default candidate
 * (before checks) or the selected local candidate (after).  Reads the
 * peer's credentials (media level overriding session level), ice-lite,
 * ice-options, candidates and ice-mismatch.
 *
 * The candidate line is parsed by a tokenizer rather than libre's regex:
 * browsers append "generation 0 ufrag x network-id 1 network-cost 10",
 * rtpengine puts a slash in the foundation, Asterisk writes "UDP" in
 * capitals, and mDNS ".local" addresses must be skipped, not rejected.
 */

#include <string.h>
#include "ice_internal.h"


static const char attr_options[] = "ice-options";


/* ── Candidate attribute ─────────────────────────────────────────────────── */

/* Advance past spaces, then return the next space-delimited token in `tok`
 * and move `pl` past it.  False when nothing is left. */
static bool next_tok(struct pl *pl, struct pl *tok)
{
	while (pl->l && *pl->p == ' ') {
		pl->p++;
		pl->l--;
	}
	if (!pl->l)
		return false;

	tok->p = pl->p;
	tok->l = 0;
	while (pl->l && *pl->p != ' ') {
		pl->p++;
		pl->l--;
		tok->l++;
	}
	return true;
}


int icea_cand_attr_decode(struct ice_cand_attr *cand, const char *val)
{
	struct pl pl, fnd, compid, transp, prio, addr, port, typ, type;
	int err;

	if (!cand || !val)
		return EINVAL;

	memset(cand, 0, sizeof(*cand));
	pl_set_str(&pl, val);

	if (!next_tok(&pl, &fnd)    || !next_tok(&pl, &compid) ||
	    !next_tok(&pl, &transp) || !next_tok(&pl, &prio)   ||
	    !next_tok(&pl, &addr)   || !next_tok(&pl, &port)   ||
	    !next_tok(&pl, &typ)    || !next_tok(&pl, &type))
		return EBADMSG;

	if (pl_strcasecmp(&typ, "typ"))
		return EBADMSG;

	(void)pl_strcpy(&fnd, cand->foundation, sizeof(cand->foundation));
	cand->compid = pl_u32(&compid);
	cand->prio   = pl_u32(&prio);

	if (0 == pl_strcasecmp(&transp, "udp"))
		cand->proto = IPPROTO_UDP;
	else if (0 == pl_strcasecmp(&transp, "tcp"))
		cand->proto = IPPROTO_TCP;
	else
		cand->proto = 0;

	/* A hostname here is an mDNS candidate (draft-ietf-mmusic-mdns-ice).
	 * We do not resolve it: EAFNOSUPPORT tells the caller to skip. */
	err = sa_set(&cand->addr, &addr, pl_u32(&port));
	if (err)
		return EAFNOSUPPORT;

	if (0 == pl_strcasecmp(&type, "host"))
		cand->type = ICE_CAND_TYPE_HOST;
	else if (0 == pl_strcasecmp(&type, "srflx"))
		cand->type = ICE_CAND_TYPE_SRFLX;
	else if (0 == pl_strcasecmp(&type, "prflx"))
		cand->type = ICE_CAND_TYPE_PRFLX;
	else if (0 == pl_strcasecmp(&type, "relay"))
		cand->type = ICE_CAND_TYPE_RELAY;
	else
		return EBADMSG;

	/* Optional key/value extensions, any order, unknown ones ignored. */
	{
		struct pl key, v, raddr = PL_INIT, rport = PL_INIT;

		while (next_tok(&pl, &key) && next_tok(&pl, &v)) {
			if (0 == pl_strcasecmp(&key, "raddr"))
				raddr = v;
			else if (0 == pl_strcasecmp(&key, "rport"))
				rport = v;
			else if (0 == pl_strcasecmp(&key, "tcptype")) {
				if (0 == pl_strcasecmp(&v, "active"))
					cand->tcptype = ICE_TCP_ACTIVE;
				else if (0 == pl_strcasecmp(&v, "passive"))
					cand->tcptype = ICE_TCP_PASSIVE;
				else
					cand->tcptype = ICE_TCP_SO;
			}
		}

		if (pl_isset(&raddr)) {
			/* rtpengine writes "raddr 0.0.0.0 rport 0" — harmless */
			(void)sa_set(&cand->rel_addr, &raddr,
				     pl_isset(&rport) ? pl_u32(&rport) : 0);
		}
	}

	return 0;
}


static int cand_encode(struct re_printf *pf, const struct icea_cand *cand)
{
	struct ice_cand_attr attr;

	memset(&attr, 0, sizeof(attr));
	str_ncpy(attr.foundation, cand->foundation, sizeof(attr.foundation));
	attr.compid   = cand->compid;
	attr.proto    = IPPROTO_UDP;
	attr.prio     = cand->prio;
	attr.addr     = cand->addr;
	attr.type     = cand->type;
	attr.rel_addr = cand->rel;

	return ice_cand_attr_encode(pf, &attr);
}


/* ── Local side ──────────────────────────────────────────────────────────── */

static int remote_cands_encode(struct re_printf *pf, void *arg)
{
	const struct icea_gen *gen = arg;
	int err = 0;
	bool first = true;

	for (unsigned i = 0; i < gen->ncomp; i++) {
		const struct icea_pair *p = gen->selected[i];

		if (!p)
			continue;

		err |= re_hprintf(pf, "%s%u %j %u", first ? "" : " ",
				  i + 1, &p->rc->addr, sa_port(&p->rc->addr));
		first = false;
	}

	return err;
}


void icea_sdp_clear_local(struct sdp_media *sdpm)
{
	if (!sdpm)
		return;

	sdp_media_del_lattr(sdpm, ice_attr_cand);
	sdp_media_del_lattr(sdpm, ice_attr_remote_cand);
	sdp_media_del_lattr(sdpm, ice_attr_mismatch);
}


int icea_sdp_write_local(struct icea_gen *gen, struct sdp_session *sdp,
			 struct sdp_media *sdpm, bool session_creds)
{
	struct le *le;
	int err = 0;

	if (!gen || !sdpm)
		return EINVAL;

	if (session_creds && sdp) {
		err |= sdp_session_set_lattr(sdp, true, ice_attr_ufrag, "%s",
					     gen->lufrag);
		err |= sdp_session_set_lattr(sdp, true, ice_attr_pwd, "%s",
					     gen->lpwd);
	}

	/* RFC 8445 §5.1: the peer's connection address was not one of its
	 * candidates — tell it so and offer nothing to pair against. */
	if (gen->mismatch) {
		sdp_media_del_lattr(sdpm, ice_attr_cand);
		sdp_media_del_lattr(sdpm, ice_attr_remote_cand);
		return sdp_media_set_lattr(sdpm, true, ice_attr_mismatch, NULL);
	}
	sdp_media_del_lattr(sdpm, ice_attr_mismatch);

	sdp_media_del_lattr(sdpm, ice_attr_cand);
	LIST_FOREACH(&gen->lcandl, le) {
		err |= sdp_media_set_lattr(sdpm, false, ice_attr_cand, "%H",
					   cand_encode, le->data);
	}

	/* a=remote-candidates: the controlling agent's conclusion, one
	 * (the selected) pair per component — not every remote we know. */
	if (gen->role == ICE_ROLE_CONTROLLING &&
	    gen->state == ICEA_STATE_COMPLETED && gen->selected[0]) {
		err |= sdp_media_set_lattr(sdpm, true, ice_attr_remote_cand,
					   "%H", remote_cands_encode, gen);
	}
	else {
		sdp_media_del_lattr(sdpm, ice_attr_remote_cand);
	}

	/* Connection address: what the peer should send to before ICE has
	 * concluded (the default candidate), the selected local candidate
	 * after.  Written at media level, as baresip's ice module did, so it
	 * overrides the session-level c= (RFC 4566 §5.7). */
	for (unsigned i = 0; i < gen->ncomp; i++) {
		const struct icea_cand *c = gen->selected[i]
					  ? gen->selected[i]->lc
					  : icea_cand_default(gen, i + 1);
		if (!c)
			continue;

		if (i == 0)
			sdp_media_set_laddr(sdpm, &c->addr);
		else
			sdp_media_set_laddr_rtcp(sdpm, &c->addr);
	}

	return err;
}


/* ── Remote side ─────────────────────────────────────────────────────────── */

struct rd {
	struct icea_gen *gen;
	const char *ufrag;   /* highest-precedence value seen so far */
	const char *pwd;
	unsigned cands;
	unsigned skipped;
};


static void options_apply(struct icea_gen *gen, const char *value)
{
	struct pl pl, tok;

	pl_set_str(&pl, value);
	while (next_tok(&pl, &tok)) {
		if (0 == pl_strcasecmp(&tok, "renomination"))
			gen->renomination = true;
	}
}


static bool sess_attr_h(const char *name, const char *value, void *arg)
{
	struct rd *rd = arg;

	if (0 == str_casecmp(name, ice_attr_lite))
		rd->gen->rlite = true;
	else if (0 == str_casecmp(name, attr_options))
		options_apply(rd->gen, value);
	else if (0 == str_casecmp(name, ice_attr_ufrag))
		rd->ufrag = value;
	else if (0 == str_casecmp(name, ice_attr_pwd))
		rd->pwd = value;

	return false;
}


static bool media_attr_h(const char *name, const char *value, void *arg)
{
	struct rd *rd = arg;

	if (0 == str_casecmp(name, ice_attr_cand)) {
		struct ice_cand_attr attr;
		int err = icea_cand_attr_decode(&attr, value);

		if (err) {
			rd->skipped++;
			ICEA_DEBUG(rd->gen, "ice[gen %u]: skipped candidate"
				   " '%s' (%m)\n", rd->gen->seq, value, err);
		}
		else if (icea_cand_add_remote(rd->gen, &attr))
			rd->cands++;
		else
			rd->skipped++;
	}
	else if (0 == str_casecmp(name, attr_options))
		options_apply(rd->gen, value);
	else if (0 == str_casecmp(name, ice_attr_lite))
		rd->gen->rlite = true;
	else if (0 == str_casecmp(name, ice_attr_ufrag))
		rd->ufrag = value;              /* media overrides session */
	else if (0 == str_casecmp(name, ice_attr_pwd))
		rd->pwd = value;
	else if (0 == str_casecmp(name, ice_attr_mismatch))
		rd->gen->mismatch = true;

	return false;
}


int icea_sdp_read_remote(struct icea_gen *gen, struct sdp_session *sdp,
			 struct sdp_media *sdpm, bool *ufrag_changed)
{
	struct rd rd;
	bool changed = false;
	int err = 0;

	if (!gen || !sdpm)
		return EINVAL;

	memset(&rd, 0, sizeof(rd));
	rd.gen = gen;

	/* Session level first so media-level values win by overwriting. */
	if (sdp)
		(void)sdp_session_rattr_apply(sdp, NULL, sess_attr_h, &rd);
	(void)sdp_media_rattr_apply(sdpm, NULL, media_attr_h, &rd);

	if (rd.ufrag) {
		if (gen->rufrag && str_cmp(gen->rufrag, rd.ufrag))
			changed = true;
		if (!gen->rufrag || changed) {
			gen->rufrag = mem_deref(gen->rufrag);
			err |= str_dup(&gen->rufrag, rd.ufrag);
		}
	}
	if (rd.pwd && (!gen->rpwd || str_cmp(gen->rpwd, rd.pwd))) {
		gen->rpwd = mem_deref(gen->rpwd);
		err |= str_dup(&gen->rpwd, rd.pwd);
	}

	gen->remote_ready = gen->rufrag && gen->rpwd &&
			    !list_isempty(&gen->rcandl);

	if (ufrag_changed)
		*ufrag_changed = changed;

	ICEA_DEBUG(gen, "ice[gen %u]: remote SDP: %u candidates (%u skipped)"
		   " ufrag=%s%s%s%s\n", gen->seq, rd.cands, rd.skipped,
		   gen->rufrag ? gen->rufrag : "(none)",
		   changed ? " CHANGED" : "",
		   gen->rlite ? " lite" : "",
		   gen->renomination ? " renomination" : "");

	return err;
}


bool icea_sdp_peer_has_ice(const struct icea_gen *gen)
{
	return gen && gen->rufrag && gen->rpwd;
}


bool icea_sdp_peer_supports_ice(struct icea_gen *gen,
				const struct sdp_media *sdpm)
{
	const struct sa *raddr;

	if (!gen || !sdpm || !icea_sdp_peer_has_ice(gen))
		return false;

	/* RFC 8445 §5.1 (ICE mismatch): the default destination in c=/m=
	 * has to be one of the candidates, unless it is the trickle
	 * placeholder 0.0.0.0:9 which promises nothing. */
	raddr = sdp_media_raddr(sdpm);
	if (!raddr || !sa_isset(raddr, SA_ADDR))
		return true;

	if (icea_cand_find(&gen->rcandl, 1, raddr))
		return true;

	ICEA_DEBUG(gen, "ice[gen %u]: ICE mismatch: c= %J is not a remote"
		   " candidate\n", gen->seq, raddr);
	gen->mismatch = true;
	return false;
}
