/**
 * @file presence.c  Presence (RFC 3856 / 3863 / 3903): watch, be watched,
 *                   publish
 *
 * Subscriber (BLF): mdt_presence_subscribe() SUBSCRIBEs to a contact's
 *   presence from the account that asked (the target a dial string, a URI
 *   or a name-addr: "bob", "bob@host", "Bob <sip:bob@host>"); each change of the
 *   contact's state, as its NOTIFYs report it, goes to the SDK
 *   (bsdk_ev_presence → MEDUT_EV_PRESENCE_STATE, under the target as the
 *   app gave it).  A subscription that closes reports UNKNOWN and is made
 *   again after a back-off.
 *
 * Notifier: a SUBSCRIBE to an account's own presence is accepted, and the
 *   watcher is sent the account's state, then every change of it.
 *
 * Publisher: mdt_presence_publish() sets the account's state, sends it to
 *   the server in a PUBLISH (refreshed before it expires) and NOTIFYs the
 *   account's watchers.  UNKNOWN withdraws the published state.
 *
 * The state, as PIDF: OPEN and BUSY are <basic>open</basic> (BUSY with an
 * rpid "busy" activity), CLOSED is <basic>closed</basic>.  An account that
 * never published counts as OPEN while registered, CLOSED otherwise.
 *
 * Adapted from baresip's modules/presence/ (subscriber.c, notifier.c,
 * publisher.c; BSD-3-Clause, Copyright (C) 2010 Alfred E. Heggestad,
 * Copyright (C) 2014 Juha Heinanen); see THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include "mdt_sip.h"

enum {
	SUB_EXPIRES = 600,     /* SUBSCRIBE, ours and watchers' [s]       */
	PUB_EXPIRES = 600,     /* PUBLISH [s]                             */
};

static struct list subl;   /* struct presence  (we watch)            */
static struct list notl;   /* struct notifier  (watches us)          */
static struct list publ;   /* struct publisher (one per account)     */

/* ── The account's state, as PIDF ────────────────────────────────────────── */

static medut_presence_status_t ua_state(const struct mdt_ua *ua)
{
	if (ua->pstat != MEDUT_PRESENCE_UNKNOWN)
		return ua->pstat;

	return mdt_ua_isregistered(ua) ? MEDUT_PRESENCE_OPEN
					: MEDUT_PRESENCE_CLOSED;
}

/* The account's state as a PIDF document (a %H printer: one argument). */
static int pidf_print(struct re_printf *pf, const struct mdt_ua *ua)
{
	const medut_presence_status_t st = ua_state(ua);
	const bool open = (st == MEDUT_PRESENCE_OPEN ||
			   st == MEDUT_PRESENCE_BUSY);

	return re_hprintf(pf,
	"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\r\n"
	"<presence xmlns=\"urn:ietf:params:xml:ns:pidf\"\r\n"
	"    xmlns:dm=\"urn:ietf:params:xml:ns:pidf:data-model\"\r\n"
	"    xmlns:rpid=\"urn:ietf:params:xml:ns:pidf:rpid\"\r\n"
	"    entity=\"%s\">\r\n"
	"  <dm:person id=\"p4159\">%s<dm:note>%s</dm:note></dm:person>\r\n"
	"  <tuple id=\"t4109\">\r\n"
	"    <status>\r\n"
	"      <basic>%s</basic>\r\n"
	"    </status>\r\n"
	"    <contact>%s</contact>\r\n"
	"  </tuple>\r\n"
	"</presence>\r\n",
	ua->aor,
	st == MEDUT_PRESENCE_BUSY
		? "<rpid:activities><rpid:busy/></rpid:activities>"
		: "<rpid:activities/>",
	st == MEDUT_PRESENCE_BUSY ? "Busy" : open ? "Available" : "Offline",
	open ? "open" : "closed", ua->aor);
}

/* A NOTIFY's PIDF body, read the way PBXs write it: <basic> open or closed,
 * then an rpid activity — away is CLOSED, busy / on-the-phone is BUSY —
 * under whatever prefix the document gave the rpid namespace. */
static medut_presence_status_t pidf_parse(const struct mbuf *mb)
{
	const char *p = (const char *)mbuf_buf(mb);
	const size_t n = mbuf_get_left(mb);
	medut_presence_status_t st = MEDUT_PRESENCE_CLOSED;
	struct pl basic;

	if (!re_regex(p, n, "<basic[ \t]*>[ \t\r\n]*[^< \t\r\n]+",
		      NULL, NULL, &basic) &&
	    !pl_strcasecmp(&basic, "open"))
		st = MEDUT_PRESENCE_OPEN;

	/* re_regex() takes one argument per [...] group. */
	if (!re_regex(p, n, "<[^:<>/ \t]+:away[ \t]*/>", NULL, NULL))
		st = MEDUT_PRESENCE_CLOSED;
	else if (!re_regex(p, n, "<[^:<>/ \t]+:busy[ \t]*/>", NULL, NULL) ||
		 !re_regex(p, n, "<[^:<>/ \t]+:on-the-phone[ \t]*/>",
			   NULL, NULL))
		st = MEDUT_PRESENCE_BUSY;

	return st;
}

/* ── Subscriber: the contacts an account watches ─────────────────────────── */

struct presence {
	struct le       le;      /* in subl                                */
	struct sipsub  *sub;
	struct tmr      tmr;     /* re-subscribe                           */
	unsigned        failc;
	int             status;  /* last reported; -1 = none yet           */
	char           *target;  /* as the app gave it: find, report        */
	char           *uri;     /* completed: the SUBSCRIBE goes there     */
	struct mdt_ua *ua;      /* not a reference: mdt_presence_ua_close */
};

static int  subscribe(struct presence *pres);
static void sub_tmr_handler(void *arg);

static uint32_t wait_term(const struct sipevent_substate *substate)
{
	switch (substate->reason) {

	case SIPEVENT_DEACTIVATED:
	case SIPEVENT_TIMEOUT:
		return 5;

	case SIPEVENT_REJECTED:
	case SIPEVENT_NORESOURCE:
		return 3600;

	case SIPEVENT_PROBATION:
	case SIPEVENT_GIVEUP:
	default:
		if (pl_isset(&substate->retry_after))
			return max(300u, pl_u32(&substate->retry_after));
		return 300;
	}
}

static uint32_t wait_fail(unsigned failc)
{
	switch (failc) {

	case 1:  return 30;
	case 2:  return 300;
	case 3:  return 3600;
	default: return 86400;
	}
}

static void report(struct presence *pres, medut_presence_status_t st)
{
	if (pres->status == (int)st)
		return;

	pres->status = (int)st;
	bsdk_ev_presence(bsdk_account_find(pres->ua), pres->target, st);
}

static void sub_destructor(void *arg)
{
	struct presence *pres = arg;

	list_unlink(&pres->le);
	tmr_cancel(&pres->tmr);
	mem_deref(pres->sub);  /* libre un-SUBSCRIBEs a live subscription */
	mem_deref(pres->target);
	mem_deref(pres->uri);
}

static void sub_notify_handler(struct sip *sip, const struct sip_msg *msg,
			       void *arg)
{
	struct presence *pres = arg;
	medut_presence_status_t st;

	pres->failc = 0;

	if (!mbuf_get_left(msg->mb)) {
		/* No state yet (a pending subscription's first NOTIFY). */
		st = MEDUT_PRESENCE_UNKNOWN;
	}
	else if (msg_ctype_cmp(&msg->ctyp, "application", "pidf+xml")) {
		st = pidf_parse(msg->mb);
	}
	else {
		MDT_WARN("presence: %s: unsupported content-type '%r'\n",
			  pres->uri, &msg->ctyp.type);
		(void)sip_treplyf(NULL, NULL, sip, msg, false,
				  415, "Unsupported Media Type",
				  "Accept: application/pidf+xml\r\n"
				  "Content-Length: 0\r\n"
				  "\r\n");
		return;
	}

	(void)sip_treply(NULL, sip, msg, 200, "OK");
	report(pres, st);
}

static void sub_close_handler(int err, const struct sip_msg *msg,
			      const struct sipevent_substate *substate,
			      void *arg)
{
	struct presence *pres = arg;
	uint32_t wait;

	pres->sub = mem_deref(pres->sub);

	if (substate) {
		wait = wait_term(substate);
		MDT_INFO("presence: %s: subscription closed: %s;"
			  " again in %u s\n", pres->uri,
			  sipevent_reason_name(substate->reason), wait);
	}
	else {
		wait = wait_fail(++pres->failc);
		if (msg)
			MDT_INFO("presence: %s: subscription closed: %u %r;"
				  " again in %u s\n", pres->uri, msg->scode,
				  &msg->reason, wait);
		else
			MDT_INFO("presence: %s: subscription closed: %m;"
				  " again in %u s\n", pres->uri, err, wait);
	}

	tmr_start(&pres->tmr, wait * 1000, sub_tmr_handler, pres);
	report(pres, MEDUT_PRESENCE_UNKNOWN);
}

static int subscribe(struct presence *pres)
{
	struct mdt_ua *ua = pres->ua;
	const char *routev[1];
	int err;

	routev[0] = ua->outbound;

	err = sipevent_subscribe(&pres->sub, g_sip.evsock, pres->uri, NULL,
				 ua->aor, "presence", NULL, SUB_EXPIRES,
				 mdt_ua_cuser(ua),
				 routev, routev[0] ? 1 : 0,
				 mdt_ua_auth, ua, true, NULL,
				 sub_notify_handler, sub_close_handler, pres,
				 "Accept: application/pidf+xml\r\n"
				 "%H",
				 mdt_ua_print_supported, ua);
	if (err)
		MDT_WARN("presence: %s: subscribe: %m\n", pres->uri, err);

	return err;
}

static void sub_tmr_handler(void *arg)
{
	struct presence *pres = arg;

	if (subscribe(pres))
		tmr_start(&pres->tmr, wait_fail(++pres->failc) * 1000,
			  sub_tmr_handler, pres);
}

static struct presence *sub_find(const struct mdt_ua *ua, const char *uri)
{
	struct le *le;

	LIST_FOREACH(&subl, le) {
		struct presence *pres = le->data;

		if (pres->ua == ua && !str_casecmp(pres->target, uri))
			return pres;
	}

	return NULL;
}

int mdt_presence_subscribe(struct mdt_ua *ua, const char *uri)
{
	struct presence *pres;
	int err;

	if (!ua || !uri)
		return EINVAL;

	if (!g_sip.evsock)
		return MEDUT_ERR_STATE;

	if (sub_find(ua, uri))
		return 0;

	pres = mem_zalloc(sizeof(*pres), sub_destructor);
	if (!pres)
		return ENOMEM;

	pres->ua     = ua;
	pres->status = -1;
	tmr_init(&pres->tmr);

	err = str_dup(&pres->target, uri);
	if (!err)
		err = mdt_ua_target(ua, &pres->uri, uri);
	if (err) {
		mem_deref(pres);
		return err;
	}

	list_append(&subl, &pres->le, pres);

	MDT_INFO("presence: %s: watching %s\n", ua->aor, pres->uri);

	/* A request that could not even be sent is tried again later, as one
	 * that failed is: the subscription itself stands. */
	if (subscribe(pres))
		tmr_start(&pres->tmr, wait_fail(++pres->failc) * 1000,
			  sub_tmr_handler, pres);

	return 0;
}

int mdt_presence_unsubscribe(struct mdt_ua *ua, const char *uri)
{
	struct presence *pres;

	if (!ua || !uri)
		return EINVAL;

	pres = sub_find(ua, uri);
	if (pres) {
		MDT_INFO("presence: %s: no longer watching %s\n",
			  ua->aor, uri);
		mem_deref(pres);
	}

	return 0;
}

/* ── Notifier: the watchers of an account ────────────────────────────────── */

struct notifier {
	struct le       le;      /* in notl                                */
	struct sipnot  *not;
	struct mdt_ua *ua;      /* not a reference: mdt_presence_ua_close */
};

static void not_destructor(void *arg)
{
	struct notifier *not = arg;

	list_unlink(&not->le);
	mem_deref(not->not);  /* libre sends the terminating NOTIFY */
}

static int notify(struct notifier *not)
{
	struct mbuf *mb;
	int err;

	mb = mbuf_alloc(1024);
	if (!mb)
		return ENOMEM;

	err = mbuf_printf(mb, "%H", pidf_print, not->ua);
	if (!err) {
		mb->pos = 0;
		err = sipevent_notify(not->not, mb, SIPEVENT_ACTIVE, 0, 0);
	}
	if (err)
		MDT_WARN("presence: %s: notify: %m\n", not->ua->aor, err);

	mem_deref(mb);
	return err;
}

static void not_close_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct notifier *not = arg;

	if (err)
		MDT_INFO("presence: %s: watcher gone: %m\n",
			  not->ua->aor, err);
	else if (msg)
		MDT_INFO("presence: %s: watcher gone: %u %r\n",
			  not->ua->aor, msg->scode, &msg->reason);

	mem_deref(not);
}

/* g_sip.subh: a SUBSCRIBE for a user agent (arg) outside a dialog. */
bool mdt_presence_sub_handler(const struct sip_msg *msg, void *arg)
{
	struct mdt_ua *ua = arg;
	const struct sip_hdr *hdr;
	struct sipevent_event se;
	struct notifier *not;
	int err;

	hdr = sip_msg_hdr(msg, SIP_HDR_EVENT);
	if (!hdr || sipevent_event_decode(&se, &hdr->val) ||
	    pl_strcasecmp(&se.event, "presence")) {
		(void)sip_treplyf(NULL, NULL, g_sip.sip, msg, false,
				  489, "Bad Event",
				  "Allow-Events: presence\r\n"
				  "Content-Length: 0\r\n"
				  "\r\n");
		return true;
	}

	not = mem_zalloc(sizeof(*not), not_destructor);
	if (!not)
		goto error;

	not->ua = ua;

	err = sipevent_accept(&not->not, g_sip.evsock, msg, NULL, &se,
			      200, "OK", SUB_EXPIRES, SUB_EXPIRES, SUB_EXPIRES,
			      mdt_ua_cuser(ua), "application/pidf+xml",
			      mdt_ua_auth, ua, true,
			      not_close_handler, not, NULL);
	if (err) {
		MDT_WARN("presence: %s: accept watcher: %m\n", ua->aor, err);
		mem_deref(not);
		goto error;
	}

	list_append(&notl, &not->le, not);
	MDT_INFO("presence: %s: watched by %r\n", ua->aor, &msg->from.auri);

	(void)notify(not);
	return true;

 error:
	(void)sip_treply(NULL, g_sip.sip, msg, 500, "Server Internal Error");
	return true;
}

static void notify_all(const struct mdt_ua *ua)
{
	struct le *le;

	LIST_FOREACH(&notl, le) {
		struct notifier *not = le->data;

		if (not->ua == ua)
			(void)notify(not);
	}
}

/* ── Publisher: the account's state, to the server ───────────────────────── */

struct publisher {
	struct le       le;      /* in publ                                */
	struct tmr      tmr;     /* refresh                                */
	char           *etag;    /* the server's entity tag, NULL = none   */
	uint32_t        expires; /* the server's, of the last PUBLISH [s]  */
	bool            closed;  /* the account went: answers are dropped  */
	struct mdt_ua *ua;      /* a reference: a PUBLISH may outlive it  */
};

static int publish(struct publisher *pub, bool refresh, uint32_t expires);

static void pub_destructor(void *arg)
{
	struct publisher *pub = arg;

	list_unlink(&pub->le);
	tmr_cancel(&pub->tmr);
	mem_deref(pub->etag);
	mem_deref(pub->ua);
}

static void pub_tmr_handler(void *arg)
{
	struct publisher *pub = arg;

	(void)publish(pub, true, PUB_EXPIRES);
}

static void pub_resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct publisher *pub = arg;
	const struct sip_hdr *hdr;

	if (pub->closed)
		goto out;

	if (err) {
		MDT_WARN("presence: %s: publish: %m\n", pub->ua->aor, err);
		goto out;
	}

	if (msg->scode < 300) {
		hdr = sip_msg_xhdr(msg, "SIP-ETag");
		if (!hdr) {
			MDT_WARN("presence: %s: publish: %u without"
				  " SIP-ETag\n", pub->ua->aor, msg->scode);
			goto out;
		}

		pub->etag = mem_deref(pub->etag);
		(void)pl_strdup(&pub->etag, &hdr->val);

		/* The server may shorten the time; refresh at 90 % of it. */
		pub->expires = pl_isset(&msg->expires) ? pl_u32(&msg->expires)
						       : PUB_EXPIRES;
		if (pub->expires)
			tmr_start(&pub->tmr, pub->expires * 900,
				  pub_tmr_handler, pub);
	}
	else if (msg->scode == 412) {
		/* The server lost our entity: publish it afresh. */
		pub->etag = mem_deref(pub->etag);
		(void)publish(pub, false, PUB_EXPIRES);
	}
	else {
		MDT_WARN("presence: %s: publish: %u %r\n", pub->ua->aor,
			  msg->scode, &msg->reason);
	}

 out:
	mem_deref(pub);
}

static int print_etag(struct re_printf *pf, const char *etag)
{
	return etag ? re_hprintf(pf, "SIP-If-Match: %s\r\n", etag) : 0;
}

/* A PUBLISH: the state (initial or modify), a refresh (no body, the entity
 * tag), or with expires 0 the removal of what we published. */
static int publish(struct publisher *pub, bool refresh, uint32_t expires)
{
	struct mdt_ua *ua = pub->ua;
	struct mbuf *mb;
	int err;

	mb = mbuf_alloc(1024);
	if (!mb)
		return ENOMEM;

	if (expires && !refresh)
		err = mbuf_printf(mb, "%H", pidf_print, ua);
	else
		err = 0;
	if (err)
		goto out;

	mb->pos = 0;

	err = mdt_sip_req_send(ua, "PUBLISH", ua->aor,
				expires ? pub_resp_handler : NULL,
				expires ? mem_ref(pub) : NULL,
				"Event: presence\r\n"
				"Expires: %u\r\n"
				"%H"
				"%s"
				"Content-Length: %zu\r\n"
				"\r\n"
				"%b",
				expires, print_etag, pub->etag,
				mbuf_get_left(mb)
				  ? "Content-Type: application/pidf+xml\r\n"
				  : "",
				mbuf_get_left(mb),
				mbuf_buf(mb), mbuf_get_left(mb));
	if (err) {
		MDT_WARN("presence: %s: publish: %m\n", ua->aor, err);
		if (expires)
			mem_deref(pub);  /* the reference the request held */
	}

 out:
	mem_deref(mb);
	return err;
}

/* Stop publishing: remove what we published, and let answers to PUBLISHes
 * still under way find the publisher closed. */
static void pub_close(struct publisher *pub)
{
	pub->closed = true;
	tmr_cancel(&pub->tmr);
	if (pub->etag)
		(void)publish(pub, false, 0);
	list_unlink(&pub->le);
	mem_deref(pub);
}

static struct publisher *pub_find(const struct mdt_ua *ua)
{
	struct le *le;

	LIST_FOREACH(&publ, le) {
		struct publisher *pub = le->data;

		if (pub->ua == ua)
			return pub;
	}

	return NULL;
}

int mdt_presence_publish(struct mdt_ua *ua, medut_presence_status_t st)
{
	struct publisher *pub;
	int err;

	if (!ua)
		return EINVAL;

	if (st != MEDUT_PRESENCE_UNKNOWN && st != MEDUT_PRESENCE_OPEN &&
	    st != MEDUT_PRESENCE_CLOSED && st != MEDUT_PRESENCE_BUSY)
		return EINVAL;

	ua->pstat = st;
	notify_all(ua);

	pub = pub_find(ua);

	if (st == MEDUT_PRESENCE_UNKNOWN) {
		/* Withdraw what we published, if anything. */
		if (pub)
			pub_close(pub);
		return 0;
	}

	if (!pub) {
		pub = mem_zalloc(sizeof(*pub), pub_destructor);
		if (!pub)
			return ENOMEM;

		pub->ua = mem_ref(ua);
		tmr_init(&pub->tmr);
		list_append(&publ, &pub->le, pub);
	}

	tmr_cancel(&pub->tmr);
	err = publish(pub, false, PUB_EXPIRES);

	return err;
}

/* ── A user agent going away ─────────────────────────────────────────────── */

void mdt_presence_ua_close(struct mdt_ua *ua)
{
	struct publisher *pub;
	struct le *le;

	le = list_head(&subl);
	while (le) {
		struct presence *pres = le->data;

		le = le->next;
		if (pres->ua == ua)
			mem_deref(pres);
	}

	le = list_head(&notl);
	while (le) {
		struct notifier *not = le->data;

		le = le->next;
		if (not->ua == ua)
			mem_deref(not);
	}

	pub = pub_find(ua);
	if (pub)
		pub_close(pub);
}

void mdt_presence_close(void)
{
	list_flush(&subl);
	list_flush(&notl);
	list_flush(&publ);
}
