/**
 * @file sipreq.c  An authenticated SIP request outside a dialog
 *
 * One request (OPTIONS, MESSAGE, PUBLISH, ...) from a user agent through its
 * outbound route, answering a 401/407 challenge with the UA's credentials.
 * The response handler is called once, with the final response; the request
 * frees itself after.
 *
 * Adapted from baresip's src/sipreq.c (BSD-3-Clause, Copyright (C) 2011
 * Alfred E. Heggestad); see THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include "mdt_sip.h"

struct mdt_sip_req {
	struct sip_loopstate ls;
	struct sip_dialog   *dlg;
	struct sip_auth     *auth;
	struct sip_request  *req;
	char                *method;
	char                *fmt;
	sip_resp_h          *resph;
	void                *arg;
};

static int request(struct mdt_sip_req *sr);

static void destructor(void *arg)
{
	struct mdt_sip_req *sr = arg;

	mem_deref(sr->req);
	mem_deref(sr->auth);
	mem_deref(sr->dlg);
	mem_deref(sr->method);
	mem_deref(sr->fmt);
}

static void resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct mdt_sip_req *sr = arg;

	if (err || sip_request_loops(&sr->ls, msg->scode))
		goto out;

	if (msg->scode < 200)
		return;

	switch (msg->scode) {

	case 401:
	case 407:
		err = sip_auth_authenticate(sr->auth, msg);
		if (err) {
			/* EAUTH: the credentials were refused already;
			 * report the challenge itself. */
			err = (err == EAUTH) ? 0 : err;
			break;
		}

		err = request(sr);
		if (err)
			break;

		return;

	case 403:
		sip_auth_reset(sr->auth);
		break;

	default:
		break;
	}

 out:
	if (sr->resph)
		sr->resph(err, err ? NULL : msg, sr->arg);

	mem_deref(sr);
}

static int request(struct mdt_sip_req *sr)
{
	return sip_drequestf(&sr->req, g_sip.sip, true, sr->method, sr->dlg,
			     0, sr->auth, NULL, resp_handler, sr,
			     sr->fmt ? "%s" : NULL, sr->fmt);
}

int mdt_sip_req_send(struct mdt_ua *ua, const char *method,
		      const char *uri, sip_resp_h *resph, void *arg,
		      const char *fmt, ...)
{
	const char *routev[1];
	struct mdt_sip_req *sr;
	int err;

	if (!ua || !method || !uri || !fmt)
		return EINVAL;

	routev[0] = ua->outbound;

	sr = mem_zalloc(sizeof(*sr), destructor);
	if (!sr)
		return ENOMEM;

	sr->resph = resph;
	sr->arg   = arg;

	err = str_dup(&sr->method, method);
	if (!err) {
		va_list ap;

		va_start(ap, fmt);
		err = re_vsdprintf(&sr->fmt, fmt, ap);
		va_end(ap);
	}
	if (err)
		goto out;

	err = sip_dialog_alloc(&sr->dlg, uri, uri, NULL, ua->aor,
			       routev[0] ? routev : NULL,
			       routev[0] ? 1 : 0);
	if (err)
		goto out;

	err = sip_auth_alloc(&sr->auth, mdt_ua_auth, ua, true);
	if (err)
		goto out;

	err = request(sr);

 out:
	if (err)
		mem_deref(sr);

	return err;
}
