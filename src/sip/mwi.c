/**
 * @file mwi.c  Message Waiting Indication (RFC 3842)
 *
 * Once a user agent is registered it subscribes to message-summary for its
 * own address; each NOTIFY body goes to the SDK (bsdk_ev_mwi), which parses
 * it into MEDUT_EV_MWI.  One subscription per user agent: a REGISTER
 * refresh does not add another, and one that closed is made again at the
 * next successful REGISTER.
 *
 * Servers that send message-summary NOTIFYs without a subscription
 * (Asterisk with mailboxes on the endpoint refuses the SUBSCRIBE and does
 * only that; FreeSWITCH sends one at REGISTER) are heard too.
 *
 * Adapted from baresip's modules/mwi/mwi.c (BSD-3-Clause, Copyright (C)
 * 2010 - 2015 Alfred E. Heggestad); see THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include "mdt_sip.h"

struct mwi {
	struct le       le;      /* in mwil                                */
	struct sipsub  *sub;
	struct mdt_ua *ua;      /* not a reference: mdt_mwi_ua_close()   */
};

static struct list mwil;

static void destructor(void *arg)
{
	struct mwi *mwi = arg;

	list_unlink(&mwi->le);
	mem_deref(mwi->sub);  /* libre un-SUBSCRIBEs a live subscription */
}

static void notify_handler(struct sip *sip, const struct sip_msg *msg,
			   void *arg)
{
	struct mwi *mwi = arg;

	if (mbuf_get_left(msg->mb)) {
		char *body = NULL;

		if (0 == re_sdprintf(&body, "%b", mbuf_buf(msg->mb),
				     mbuf_get_left(msg->mb)))
			bsdk_ev_mwi(bsdk_account_find(mwi->ua), body);
		mem_deref(body);
	}

	(void)sip_treply(NULL, sip, msg, 200, "OK");
}

static void close_handler(int err, const struct sip_msg *msg,
			  const struct sipevent_substate *substate, void *arg)
{
	struct mwi *mwi = arg;
	(void)substate;

	if (err)
		MDT_INFO("mwi: %s: subscription closed: %m\n",
			  mwi->ua->aor, err);
	else
		MDT_INFO("mwi: %s: subscription closed: %u %r\n",
			  mwi->ua->aor, msg ? msg->scode : 0,
			  msg ? &msg->reason : &pl_null);

	mem_deref(mwi);
}

static int mwi_subscribe(struct mdt_ua *ua)
{
	const char *routev[1];
	struct mwi *mwi;
	int err;

	mwi = mem_zalloc(sizeof(*mwi), destructor);
	if (!mwi)
		return ENOMEM;

	mwi->ua = ua;
	list_append(&mwil, &mwi->le, mwi);

	routev[0] = ua->outbound;

	MDT_INFO("mwi: subscribing to messages for %s\n", ua->aor);

	err = sipevent_subscribe(&mwi->sub, g_sip.evsock, ua->aor, NULL,
				 ua->aor, "message-summary", NULL, 600,
				 mdt_ua_cuser(ua),
				 routev, routev[0] ? 1 : 0,
				 mdt_ua_auth, ua, true, NULL,
				 notify_handler, close_handler, mwi,
				 "Accept: application/simple-message-summary"
				 "\r\n");
	if (err) {
		MDT_WARN("mwi: %s: subscribe: %m\n", ua->aor, err);
		mem_deref(mwi);
	}

	return err;
}

static struct mwi *mwi_find(const struct mdt_ua *ua)
{
	struct le *le;

	LIST_FOREACH(&mwil, le) {
		struct mwi *mwi = le->data;

		if (mwi->ua == ua)
			return mwi;
	}

	return NULL;
}

/* A NOTIFY outside a dialog (no To tag): an unsolicited message summary
 * for `ua`, or not ours (false: libre's event socket answers it). */
bool mdt_mwi_unsolicited(struct mdt_ua *ua, const struct sip_msg *msg)
{
	const struct sip_hdr *hdr;
	struct sipevent_event se;
	char *body = NULL;

	if (pl_isset(&msg->to.tag))
		return false;

	hdr = sip_msg_hdr(msg, SIP_HDR_EVENT);
	if (!hdr || sipevent_event_decode(&se, &hdr->val) ||
	    pl_strcasecmp(&se.event, "message-summary"))
		return false;

	if (mbuf_get_left(msg->mb) &&
	    0 == re_sdprintf(&body, "%b", mbuf_buf(msg->mb),
			     mbuf_get_left(msg->mb)))
		bsdk_ev_mwi(bsdk_account_find(ua), body);
	mem_deref(body);

	(void)sip_treply(NULL, g_sip.sip, msg, 200, "OK");
	return true;
}

void mdt_mwi_reg_ok(struct mdt_ua *ua)
{
	if (ua && g_sip.evsock && !mwi_find(ua))
		(void)mwi_subscribe(ua);
}

void mdt_mwi_ua_close(struct mdt_ua *ua)
{
	struct mwi *mwi = mwi_find(ua);

	if (mwi) {
		MDT_INFO("mwi: %s: unsubscribing\n", ua->aor);
		mem_deref(mwi);
	}
}

void mdt_mwi_close(void)
{
	list_flush(&mwil);
}
