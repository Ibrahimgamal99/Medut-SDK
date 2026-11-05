/**
 * @file reg.c  Registration client: one binding of a user agent
 *
 * A wrapper around libre's sipreg that reports to the SDK
 * (bsdk_ev_reg_*), learns the public GRUU, and carries the RFC 8599 push
 * binding across re-REGISTERs: the push params the SDK keeps for the account
 * go into the Contact, and a re-created client takes over the old one's
 * binding instead of removing it (libre's sipreg_handoff_push() /
 * sipreg_takeover() / sipreg_release(), Medut patches to libre).
 *
 * Adapted from baresip's src/reg.c (BSD-3-Clause, Copyright (C) 2010 Alfred
 * E. Heggestad); see THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include "mdt_sip.h"

struct mdt_reg {
	struct le       le;
	struct mdt_ua *ua;
	struct sipreg  *sipreg;
	int             id;       /* reg-id (RFC 5626), 0 for none */
	struct sa       paddr;    /* where the last 2xx came from  */
	struct sa       claddr;   /* our end of the socket it came in on */
	uint16_t        scode;    /* last final status, 999 = error */
	char           *srv;      /* the registrar's Server header */
	int             af;       /* address family of the flow     */
	bool            unreg;    /* an un-REGISTER is what is out  */
};

static void destructor(void *arg)
{
	struct mdt_reg *reg = arg;

	list_unlink(&reg->le);
	mem_deref(reg->sipreg);
	mem_deref(reg->srv);
}

/* Our end of the socket or connection msg arrived on.  For WS/WSS this is the
 * connection's real local address — what the REGISTER itself carries is the
 * transport's listen address (libre's sip_transp_send()). */
static int msg_laddr(const struct sip_msg *msg, struct sa *laddr)
{
	switch (msg->tp) {

	case SIP_TRANSP_UDP:
		return udp_local_get(msg->sock, laddr);

	case SIP_TRANSP_TCP:
	case SIP_TRANSP_TLS:
	case SIP_TRANSP_WS:
	case SIP_TRANSP_WSS:
		return tcp_conn_local_get(sip_msg_tcpconn(msg), laddr);

	default:
		return EPROTONOSUPPORT;
	}
}

static int msg_af(const struct sip_msg *msg)
{
	struct sa laddr;

	return msg_laddr(msg, &laddr) ? AF_UNSPEC : sa_af(&laddr);
}

static const char *af_name(int af)
{
	switch (af) {
	case AF_INET:  return "v4";
	case AF_INET6: return "v6";
	default:       return "v?";
	}
}

/* The Contact of the 2xx that is ours. */
static bool contact_handler(const struct sip_hdr *hdr,
			    const struct sip_msg *msg, void *arg)
{
	struct mdt_reg *reg = arg;
	struct sip_addr addr;
	(void)msg;

	if (sip_addr_decode(&addr, &hdr->val))
		return false;

	return 0 == pl_strcasecmp(&addr.uri.user, reg->ua->cuser);
}

static void pub_gruu_set(struct mdt_ua *ua, const struct pl *pval)
{
	ua->pub_gruu = mem_deref(ua->pub_gruu);
	(void)pl_strdup(&ua->pub_gruu, pval);
}

static void register_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct mdt_reg *reg = arg;
	struct mdt_ua *ua = reg->ua;
	struct medut_account *acct = bsdk_account_find(ua);
	const struct sip_hdr *hdr;
	char reason[128];

	/* The 2xx to an un-REGISTER is the binding gone, not a registration.
	 * (A failed one falls through: event.c reports an account the app
	 * unregistered as UNREGISTERED whichever way its un-REGISTER went.) */
	if (reg->unreg && !err && msg->scode >= 200 && msg->scode < 300) {
		reg->scode = 0;
		bsdk_ev_reg_unregistered(acct);
		return;
	}

	if (err) {
		MDT_WARN("reg: %s: register: %m\n", ua->aor, err);
		reg->scode = 999;
		(void)re_snprintf(reason, sizeof(reason), "%m", err);
		bsdk_ev_reg_fail(acct, 0, reason);
		return;
	}

	hdr = sip_msg_hdr(msg, SIP_HDR_SERVER);
	if (hdr) {
		reg->srv = mem_deref(reg->srv);
		(void)pl_strdup(&reg->srv, &hdr->val);
	}

	if (msg->scode >= 200 && msg->scode <= 299) {
		uint32_t n = sip_msg_hdr_count(msg, SIP_HDR_CONTACT);

		reg->paddr = msg->src;
		reg->af    = msg_af(msg);
		if (msg_laddr(msg, &reg->claddr))
			sa_init(&reg->claddr, AF_UNSPEC);

		if (msg->scode != reg->scode) {
			MDT_INFO("reg: %s {%d/%s/%s} %u %r (%s) "
				  "[%u binding%s]\n", ua->aor, reg->id,
				  sip_transp_name(msg->tp), af_name(reg->af),
				  msg->scode, &msg->reason, reg->srv, n,
				  n == 1 ? "" : "s");
		}

		reg->scode = msg->scode;

		hdr = sip_msg_hdr_apply(msg, true, SIP_HDR_CONTACT,
					contact_handler, reg);
		if (hdr) {
			struct sip_addr addr;
			struct pl pval;

			if (0 == sip_addr_decode(&addr, &hdr->val) &&
			    0 == msg_param_decode(&addr.params, "pub-gruu",
						  &pval))
				pub_gruu_set(ua, &pval);
		}

		mdt_mwi_reg_ok(ua);
		bsdk_ev_reg_ok(acct);
	}
	else if (msg->scode >= 300) {
		MDT_WARN("reg: %s: %u %r (%s)\n", ua->aor, msg->scode,
			  &msg->reason, reg->srv);

		reg->scode = msg->scode;

		(void)re_snprintf(reason, sizeof(reason), "%u %r",
				  msg->scode, &msg->reason);
		bsdk_ev_reg_fail(acct, msg->scode, reason);
	}
}

int mdt_reg_add(struct list *lst, struct mdt_ua *ua, int regid)
{
	struct mdt_reg *reg;

	if (!lst || !ua)
		return EINVAL;

	reg = mem_zalloc(sizeof(*reg), destructor);
	if (!reg)
		return ENOMEM;

	reg->ua = ua;
	reg->id = regid;

	list_append(lst, &reg->le, reg);
	return 0;
}

int mdt_reg_register(struct mdt_reg *reg, const char *reg_uri,
		      const char *params, const char *outbound)
{
	struct mdt_ua *ua;
	struct sipreg *old;
	const char *routev[1];
	const char *cparams;
	bool failed;
	int err;

	if (!reg || !reg_uri || !params)
		return EINVAL;

	ua = reg->ua;
	reg->scode = 0;
	reg->unreg = false;
	routev[0] = outbound;

	/* RFC 8599: the same push binding is handed over, not removed. */
	cparams = bsdk_ua_contact_params(ua);
	sipreg_handoff_push(reg->sipreg, cparams);

	/* The new client keeps counting the old one's failures, so the retry
	 * backs off across the swap. */
	old = reg->sipreg;
	reg->sipreg = NULL;
	failed = sipreg_failed(old);

	err = sipreg_alloc(&reg->sipreg, g_sip.sip, reg_uri,
			   ua->aor, ua->dispname, ua->aor,
			   ua->regint, ua->cuser,
			   routev[0] ? routev : NULL, routev[0] ? 1 : 0,
			   reg->id,
			   mdt_ua_auth, ua, true,
			   register_handler, reg,
			   params[0] ? &params[1] : NULL,
			   "Allow: %H\r\n%H",
			   mdt_ua_print_allowed, ua,
			   mdt_hdrs_print, &ua->custom_hdrs);

	if (!err && ua->rwait)
		err = sipreg_set_rwait(reg->sipreg, ua->rwait);

	if (!err && failed)
		sipreg_incfailc(reg->sipreg);

	if (!err && str_isset(cparams))
		err = sipreg_set_contact_params(reg->sipreg, cparams);

	if (err) {
		reg->sipreg = mem_deref(reg->sipreg);
		(void)sipreg_release(old);
		return err;
	}

	/* The new client takes over the old one's binding: the old one sends
	 * no un-REGISTER, and the new REGISTER removes the old Contact if a
	 * handover changed it (libre sipreg_takeover(), a Medut patch).
	 * An un-REGISTER from the old client would go out from the new
	 * sockets, naming the new Contact — the binding about to be made. */
	sipreg_takeover(reg->sipreg, old);
	(void)sipreg_release(old);

	return sipreg_send(reg->sipreg);
}

void mdt_reg_unregister(struct mdt_reg *reg)
{
	if (!reg)
		return;

	/* Before the send: a send error is reported from inside it. */
	reg->unreg = true;
	sipreg_unregister(reg->sipreg);
}

bool mdt_reg_isok(const struct mdt_reg *reg)
{
	if (!reg || !reg->sipreg)
		return false;

	return sipreg_registered(reg->sipreg) && reg->scode == 200;
}

int mdt_reg_refresh(struct mdt_reg *reg)
{
	return reg ? sipreg_refresh(reg->sipreg) : EINVAL;
}

const struct sa *mdt_reg_conn_laddr(const struct mdt_reg *reg)
{
	return reg && sa_isset(&reg->claddr, SA_ADDR) ? &reg->claddr : NULL;
}

const struct sa *mdt_reg_laddr(const struct mdt_reg *reg)
{
	return reg ? sipreg_laddr(reg->sipreg) : NULL;
}
