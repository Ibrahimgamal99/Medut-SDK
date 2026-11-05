/**
 * @file message.c  SIP MESSAGE (RFC 3428), outside a dialog
 *
 * Send: text/plain to a target given as a dial string ("bob", "bob@host"),
 * a URI or a name-addr, through the account's outbound route, answering an
 * auth challenge with its credentials (sipreq.c).
 *
 * Receive: sip_stack.c routes a MESSAGE to the user agent it is for.
 * text/plain and application/json go to the SDK (bsdk_ev_message, which
 * raises MEDUT_EV_MESSAGE) and are answered 200; any other body is 415.
 *
 * Adapted from baresip's src/message.c (BSD-3-Clause, Copyright (C) 2010
 * Alfred E. Heggestad); see THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include "mdt_sip.h"

void mdt_message_recv(struct mdt_ua *ua, const struct sip_msg *msg)
{
	static const struct pl text_plain = PL("text/plain");
	static const struct pl app_json   = PL("application/json");
	const struct pl *ctype = NULL;

	if (msg_ctype_cmp(&msg->ctyp, "text", "plain"))
		ctype = &text_plain;
	else if (msg_ctype_cmp(&msg->ctyp, "application", "json"))
		ctype = &app_json;

	if (!ctype) {
		(void)sip_treplyf(NULL, NULL, g_sip.sip, msg, false, 415,
				  "Unsupported Media Type",
				  "Accept: text/plain\r\n"
				  "Content-Length: 0\r\n"
				  "\r\n");
		return;
	}

	bsdk_ev_message(bsdk_account_find(ua), &msg->from.auri, ctype,
			msg->mb);

	/* A server transaction, so a retransmission over UDP is answered
	 * again rather than delivered twice. */
	(void)sip_treply(NULL, g_sip.sip, msg, 200, "OK");
}

static void resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	(void)arg;

	if (err)
		MDT_WARN("message: send: %m\n", err);
	else if (msg->scode >= 300)
		MDT_WARN("message: send: %u %r\n", msg->scode, &msg->reason);
}

int mdt_message_send(struct mdt_ua *ua, const char *to_uri,
		      const char *body)
{
	char *uri = NULL;
	int err;

	if (!ua || !to_uri || !body)
		return EINVAL;

	err = mdt_ua_target(ua, &uri, to_uri);
	if (err)
		return err;

	err = mdt_sip_req_send(ua, "MESSAGE", uri, resp_handler, NULL,
				"Accept: text/plain\r\n"
				"Content-Type: text/plain\r\n"
				"Content-Length: %zu\r\n"
				"\r\n%s",
				str_len(body), body);

	mem_deref(uri);
	return err;
}
