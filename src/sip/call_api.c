/**
 * @file call_api.c  Calls, as core/mdt.h offers them to the SDK
 *
 * Creating calls for a user agent (dial, accept an INVITE, the call that
 * follows a REFER), answering and ending them, the call's identity, and the
 * translation of what a call reports (mdt_call_event_h) into the SDK's
 * typed hooks (mdt_events.h).
 *
 * Adapted from baresip's src/ua.c and src/account.c (BSD-3-Clause,
 * Copyright (C) 2010 Alfred E. Heggestad); see THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include "mdt_call.h"
#include "../api/sip_status.h"

/* ── Events: call → SDK ──────────────────────────────────────────────────── */

/* A close reason from the socket layer has no SIP status and ends in
 * "[<errno>]" (e.g. "Connection reset by peer [104]"). */
static bool reason_is_transport_err(const char *reason, int scode)
{
	return scode == 0 && reason &&
	       strrchr(reason, '[') && strrchr(reason, ']') > strrchr(reason, '[');
}

static void call_closed(struct mdt_call *call, const char *reason)
{
	int scode = bsdk_sip_reason_code(reason);
	struct bsdk_close_cause cause = {
		.scode         = (uint16_t)scode,
		.transport_err = reason_is_transport_err(reason, scode),
		.peer_cancel   = mdt_call_peer_cancelled(call),
		.reason        = reason,
	};

	bsdk_ev_call_closed(bsdk_call_find(call),
			    bsdk_account_find(mdt_call_get_ua(call)), &cause);
}

static void call_event_handler(struct mdt_call *call, enum mdt_call_event ev,
			       const char *str, void *arg)
{
	struct mdt_ua *ua = arg;
	struct medut_account *acct = bsdk_account_find(ua);
	struct medut_call *lc = bsdk_call_find(call);
	const char *peeruri = mdt_call_peeruri(call);

	if (!peeruri)
		return;

	switch (ev) {

	case MDT_CALL_EVENT_INCOMING:
		bsdk_ev_call_incoming(acct, call, peeruri,
				      mdt_call_peername(call));
		/* The INVITE's offer was applied before the SDK had the call:
		 * note it now, so our answer completes a negotiation (and
		 * MEDUT_EV_SDP_NEGOTIATION reports the first one too). */
		if (mdt_call_got_offer(call))
			bsdk_ev_call_sdp(bsdk_call_find(call), true, false);
		break;

	/* 183 with SDP is reported as ringing, as 180 is. */
	case MDT_CALL_EVENT_RINGING:
	case MDT_CALL_EVENT_PROGRESS:
		bsdk_ev_call_ringing(lc, acct);
		break;

	/* OUTGOING fires inside mdt_call_connect(), before the SDK has a
	 * wrapper; the invite returns the handle in CALLING instead.
	 * ANSWERED is the 2xx before its SDP is applied: ESTABLISHED follows
	 * in the same response, and reporting both gave the app two. */
	case MDT_CALL_EVENT_OUTGOING:
	case MDT_CALL_EVENT_ANSWERED:
		break;

	case MDT_CALL_EVENT_ESTABLISHED:
		MDT_INFO("%s: call established: %s\n", ua->aor, peeruri);
		bsdk_ev_call_established(lc, acct);
		break;

	case MDT_CALL_EVENT_CLOSED:
		call_closed(call, str);
		mem_deref(call);
		break;

	case MDT_CALL_EVENT_TRANSFER:
		bsdk_ev_transfer_request(lc, acct, str);
		break;

	case MDT_CALL_EVENT_TRANSFER_FAILED:
		bsdk_ev_transfer_failed(lc, acct, str);
		break;

	case MDT_CALL_EVENT_MENC:
		bsdk_ev_call_menc(lc);
		break;
	}
}

/* DTMF from the peer.  Only the start of a digit goes to the SDK, as it
 * always has: the end carries no digit. */
static void call_dtmf_handler(struct mdt_call *call, char key, void *arg)
{
	(void)arg;

	if (key != 0 && key != KEYCODE_REL)
		bsdk_ev_call_dtmf(bsdk_call_find(call), key, false);
}

/* ── Creating calls ──────────────────────────────────────────────────────── */

/* The connection address of an SDP offer (c=IN IP4/IP6 ...), with the port
 * of its audio line. */
static int sdp_connection(const struct mbuf *mb, int *af, struct sa *sa)
{
	struct pl pl1, pl2;
	int err;

	*af = AF_UNSPEC;

	err = re_regex((char *)mbuf_buf(mb), mbuf_get_left(mb),
		       "c=IN IP[46]1 [^ \r\n]+", &pl1, &pl2);
	if (err)
		return err;

	switch (pl1.p[0]) {
	case '4': *af = AF_INET;  break;
	case '6': *af = AF_INET6; break;
	default:  return EAFNOSUPPORT;
	}

	err = re_regex((char *)mbuf_buf(mb), mbuf_get_left(mb),
		       "m=audio [0-9]+ ", &pl1);
	if (err)
		return err;

	err = sa_set(sa, &pl2, pl_u32(&pl1));
	if (!err && !sa_isset(sa, SA_ADDR))
		return EINVAL;

	return err;
}

/* A call of `ua`: the address family and local address its media uses
 * follow the peer (an incoming offer, the dialled host), else the
 * registration's flow, else the default route. */
static int call_alloc(struct mdt_call **callp, struct mdt_ua *ua,
		      const struct sip_msg *msg, struct mdt_call *xcall,
		      const char *local_uri, const struct sa *dst_in)
{
	struct mdt_call_prm cprm;
	const struct sa *laddr = NULL;
	struct sa dst;
	int af = AF_UNSPEC;
	int err;

	sa_init(&dst, AF_UNSPEC);
	if (dst_in)
		sa_cpy(&dst, dst_in);

	if (msg && !sdp_connection(msg->mb, &af, &dst)) {
		MDT_INFO("sip: using connection-address %j of SDP offer\n",
			  &dst);
	}
	else if (sa_isset(&dst, SA_ADDR)) {
		af = sa_af(&dst);
	}
	else if (msg) {
		laddr = &msg->dst;
		af = sa_af(laddr);
	}
	else if (ua->regint) {
		laddr = mdt_ua_regladdr(ua);
		af = sa_af(laddr);
	}
	else {
		af = mdt_net_af();
	}

	if (af != AF_UNSPEC && !mdt_net_af_enabled(af)) {
		MDT_WARN("sip: address family %s not supported\n",
			  net_af2name(af));
		if (msg)
			(void)sip_treply(NULL, g_sip.sip, msg, 488,
					 "Not Acceptable Here");
		return EAFNOSUPPORT;
	}

	memset(&cprm, 0, sizeof(cprm));

	if (sa_isset(laddr, SA_ADDR)) {
		sa_cpy(&cprm.laddr, laddr);
	}
	else if (sa_isset(&dst, SA_ADDR)) {
		laddr = mdt_net_laddr_for(&dst);
		if (!sa_isset(laddr, SA_ADDR)) {
			MDT_WARN("sip: no local address for %j\n", &dst);
			return EINVAL;
		}
		sa_cpy(&cprm.laddr, laddr);
	}
	else {
		(void)net_default_source_addr_get(af, &cprm.laddr);
	}

	cprm.af      = af;
	cprm.use_rtp = true;

	err = mdt_call_alloc(callp, mdt_config(), &ua->calls,
			      ua->dispname,
			      local_uri ? local_uri : ua->aor,
			      ua, &cprm, msg, xcall, mdt_net_dnsc(),
			      call_event_handler, ua);
	if (err)
		return err;

	if (!list_isempty(&ua->custom_hdrs))
		mdt_call_set_custom_hdrs(*callp, &ua->custom_hdrs);

	mdt_call_set_handlers(*callp, NULL, call_dtmf_handler, ua);

	return 0;
}

/* The INVITE sip_stack.c routed to `ua`. */
int mdt_call_accept(struct mdt_ua *ua, const struct sip_msg *msg)
{
	struct mdt_call *call = NULL;
	char *to_uri = NULL;
	struct le *le;
	int err;

	if (!ua || !msg)
		return EINVAL;

	LIST_FOREACH(&ua->calls, le) {
		if (mdt_call_sess_cmp(le->data, msg)) {
			MDT_WARN("sip: call was already created\n");
			return EINVAL;
		}
	}

	err = pl_strdup(&to_uri, &msg->to.auri);
	if (err)
		goto error;

	err = call_alloc(&call, ua, msg, NULL, to_uri, NULL);
	if (err) {
		MDT_WARN("sip: call_alloc: %m\n", err);
		if (err == EAFNOSUPPORT) {
			mem_deref(to_uri);
			return err;
		}
		goto error;
	}

	err = mdt_call_sess_accept(call, g_sip.sock, msg);
	if (err)
		goto error;

	mem_deref(to_uri);
	return 0;

 error:
	mem_deref(call);
	mem_deref(to_uri);
	(void)sip_treply(NULL, g_sip.sip, msg, 500, "Call Error");
	return err;
}

static int connect_uri(struct mdt_call **callp, struct mdt_ua *ua,
		       const char *req_uri)
{
	struct mdt_call *call = NULL;
	struct sip_addr addr;
	struct sa dst;
	struct pl pl;
	int err;

	pl_set_str(&pl, req_uri);

	/* A request URI with an IP literal host is where the media goes. */
	sa_init(&dst, AF_UNSPEC);
	if (!sip_addr_decode(&addr, &pl))
		(void)sa_set(&dst, &addr.uri.host, addr.uri.port);
	if (sa_isset(&dst, SA_ADDR) && !sa_isset(&dst, SA_PORT))
		sa_set_port(&dst, SIP_PORT);

	err = call_alloc(&call, ua, NULL, NULL, NULL, &dst);
	if (err)
		return err;

	err = mdt_call_invite(call, &pl);
	if (err) {
		mem_deref(call);
		return err;
	}

	*callp = call;
	return 0;
}

int mdt_call_connect(struct mdt_call **ecp, struct mdt_ua *ea,
		      const char *dial)
{
	char *ruri = NULL;
	int err;

	if (!ecp || !ea || !dial)
		return EINVAL;

	/* "sip:*43": the scheme without a host is dropped so the domain gets
	 * added; "sips:" is left alone rather than downgraded. */
	if (!strchr(dial, '@') && strncasecmp(dial, "sip:", 4) == 0)
		dial += 4;

	err = mdt_ua_uri_complete(ea, &ruri, dial);
	if (err)
		return err;

	err = connect_uri(ecp, ea, ruri);
	if (err == EAFNOSUPPORT && ea->mnat) {
		/* ICE could not gather on an address family this host lacks:
		 * place the call without it, and give the account its ICE
		 * back for the next call. */
		const struct mdt_mnat *keep = ea->mnat;

		ea->mnat = NULL;
		err = connect_uri(ecp, ea, ruri);
		ea->mnat = keep;
	}

	mem_deref(ruri);
	return err;
}

/* Following a REFER: the new call inherits the old one's notifier, so its
 * progress is reported to the transferor as NOTIFY sipfrags. */
int mdt_call_xfer_alloc(struct mdt_call **ecp, struct mdt_ua *ea,
			 struct mdt_call *xcall)
{
	struct mdt_call *call2 = NULL;
	int err;

	if (!ecp || !ea || !xcall)
		return EINVAL;

	err = call_alloc(&call2, ea, NULL, xcall, mdt_call_localuri(xcall),
			 NULL);
	if (err)
		return err;

	{
		const struct pl *ud = mdt_call_user_data(xcall);
		if (ud)
			(void)mdt_call_set_user_data(call2, ud);
	}

	*ecp = call2;
	return 0;
}

int mdt_call_dial(struct mdt_call *ec, const char *uri)
{
	struct pl pl;

	if (!ec || !uri)
		return EINVAL;

	pl_set_str(&pl, uri);
	return mdt_call_invite(ec, &pl);
}

/* ── Answering and ending ────────────────────────────────────────────────── */

int mdt_call_answer(struct mdt_ua *ea, struct mdt_call *ec)
{
	struct le *le;

	if (!ea)
		return EINVAL;

	if (!ec) {
		LIST_FOREACH(&ea->calls, le) {
			if (mdt_call_state(le->data) == MDT_CALL_INCOMING) {
				ec = le->data;
				break;
			}
		}
		if (!ec)
			return ENOENT;
	}

	return mdt_call_respond(ec, 200);
}

void mdt_call_hangup(struct mdt_ua *ea, struct mdt_call *ec,
		      uint16_t scode, const char *reason)
{
	const char *why;

	if (!ea)
		return;

	/* No call: the current one, which is the last in the list. */
	if (!ec) {
		struct le *le = list_tail(&ea->calls);
		if (!le)
			return;
		ec = le->data;
	}

	/* Only a ring turned away was rejected; read before the state ends. */
	why = reason ? reason :
	      mdt_call_state(ec) == MDT_CALL_INCOMING ? "Rejected by user"
							: "Local hangup";

	mdt_call_end(ec, scode, reason);

	call_closed(ec, why);

	mem_deref(ec);
}

/* Every call of a UA that is going away: the current one hung up, the rest
 * reported closed. */
void mdt_ua_calls_close(struct mdt_ua *ua)
{
	struct le *le;

	mdt_call_hangup(ua, NULL, 0, NULL);

	while ((le = list_head(&ua->calls))) {
		struct mdt_call *call = le->data;

		call_closed(call, "User-Agent deleted");
		list_unlink(le);
		mem_deref(call);
	}
}

int mdt_call_notify_sipfrag(struct mdt_call *ec, uint16_t scode,
			     const char *reason)
{
	return mdt_call_sipfrag(ec, scode, "%s", reason);
}

void mdt_call_free(struct mdt_call *ec)
{
	mem_deref(ec);
}

/* ── Identity ────────────────────────────────────────────────────────────── */

static medut_transport_t transp_to_sdk(enum sip_transp tp)
{
	switch (tp) {
	case SIP_TRANSP_TCP: return MEDUT_TRANSPORT_TCP;
	case SIP_TRANSP_TLS: return MEDUT_TRANSPORT_TLS;
	case SIP_TRANSP_WS:  return MEDUT_TRANSPORT_WS;
	case SIP_TRANSP_WSS: return MEDUT_TRANSPORT_WSS;
	default:             return MEDUT_TRANSPORT_UDP;
	}
}

void mdt_call_info(const struct mdt_call *ec, medut_call_info_t *o)
{
	const char *sv;

	if (!ec || !o)
		return;

	sv = mdt_call_peeruri(ec);
	if (sv) str_ncpy(o->peer_uri, sv, sizeof(o->peer_uri));
	sv = mdt_call_peername(ec);
	if (sv) str_ncpy(o->peer_display_name, sv,
			 sizeof(o->peer_display_name));
	sv = mdt_call_localuri(ec);
	if (sv) str_ncpy(o->local_uri, sv, sizeof(o->local_uri));
	sv = mdt_call_contacturi(ec);
	if (sv) str_ncpy(o->contact_uri, sv, sizeof(o->contact_uri));
	sv = mdt_call_id(ec);
	if (sv) str_ncpy(o->call_id, sv, sizeof(o->call_id));
	sv = mdt_call_diverteruri(ec);
	if (sv) str_ncpy(o->diverter_uri, sv, sizeof(o->diverter_uri));

	o->is_outgoing       = mdt_call_is_outgoing(ec);
	o->is_remote_hold    = mdt_call_is_remote_hold(ec);
	o->sip_status        = mdt_call_scode(ec);
	o->setup_duration_ms = mdt_call_setup_duration(ec) * 1000u;
	o->line_number       = mdt_call_linenum(ec);
	o->transport         = transp_to_sdk(mdt_call_transp(ec));
}

struct mdt_ua *mdt_call_acct(const struct mdt_call *ec)
{
	return mdt_call_get_ua(ec);
}

const struct sa *mdt_call_raddr(const struct mdt_call *ec)
{
	struct mdt_stream *strm = mdt_audio_strm(mdt_call_audio(ec));
	const struct sa *raddr;

	if (!strm)
		return NULL;

	/* Where RTP actually goes (learned from the peer) before the SDP's. */
	raddr = mdt_stream_raddr(strm);
	if (!raddr || !sa_isset(raddr, SA_ADDR))
		raddr = sdp_media_raddr(mdt_stream_sdpmedia(strm));

	return raddr;
}
