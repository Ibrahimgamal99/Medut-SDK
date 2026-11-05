/**
 * @file message.c  SIP MESSAGE — send and receive
 *
 * Send: medut_message_send() → bsdk_dispatch_sync → mdt_message_send()
 *       The stack sends text/plain; the content_type parameter is accepted
 *       for API consistency but currently ignored.
 *
 * Receive: the stack reports each incoming SIP MESSAGE (bsdk_ev_message),
 *          which fires MEDUT_EV_MESSAGE via the event queue.
 */

#include <string.h>
#include "bsdk.h"

/* ── Incoming MESSAGE handler ────────────────────────────────────────────── */

void bsdk_ev_message(struct medut_account *acct, const struct pl *peer,
                     const struct pl *ctype, struct mbuf *body)
{
	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type = MEDUT_EV_MESSAGE;
	medut_ev_message_t *m = &qev->ev.u.msg;
	m->account = acct;

	/* Pack strings into buf: peer\0ctype\0body\0 */
	size_t off = 0;
	size_t avail = sizeof(qev->buf);

	if (peer && peer->p) {
		size_t n = peer->l < avail - 1 ? peer->l : avail - 1;
		memcpy(qev->buf + off, peer->p, n);
		qev->buf[off + n] = '\0';
		m->from_uri = qev->buf + off;
		off += n + 1;
	}

	if (ctype && ctype->p && off < avail) {
		size_t n = ctype->l < avail - off - 1 ? ctype->l : avail - off - 1;
		memcpy(qev->buf + off, ctype->p, n);
		qev->buf[off + n] = '\0';
		m->content_type = qev->buf + off;
		off += n + 1;
	}

	if (body && off < avail) {
		size_t body_len = mbuf_get_left(body);
		size_t n = body_len < avail - off - 1 ? body_len : avail - off - 1;
		memcpy(qev->buf + off, mbuf_buf(body), n);
		qev->buf[off + n] = '\0';
		m->body = qev->buf + off;
	}

	bsdk_event_post_qev(qev);   /* warns and frees qev when the queue is full */
}

/* ── Send ────────────────────────────────────────────────────────────────── */

typedef struct {
	struct medut_account *acct;
	const char             *to_uri;
	const char             *body;
	int                     result;
} send_msg_ctx_t;

static void send_msg_fn(void *arg)
{
	send_msg_ctx_t *ctx = arg;
	if (!ctx->acct->ea) { ctx->result = ENOENT; return; }
	/* text/plain; custom content_type via medut_message_send() is
	 * accepted for future use */
	ctx->result = mdt_message_send(ctx->acct->ea, ctx->to_uri, ctx->body);
}

int medut_message_send(medut_account_handle_t account,
                          const char *to_uri,
                          const char *body,
                          const char *content_type)
{
	if (!account || !to_uri || !body) return MEDUT_ERR_INVAL;
	(void)content_type; /* currently always text/plain */
	send_msg_ctx_t ctx = {.acct = account, .to_uri = to_uri,
	                       .body = body, .result = 0};
	int err = bsdk_dispatch_sync(send_msg_fn, &ctx);
	return err ? err : ctx.result;
}
