/**
 * @file message.c  SIP MESSAGE — send and receive
 *
 * Send: medut_message_send() → bsdk_dispatch_sync → message_send()
 *       baresip's message_send() supports text/plain only; content_type
 *       parameter is accepted for API consistency but currently ignored.
 *
 * Receive: message_listen() registers a handler on the baresip message
 *          subsystem. Each incoming SIP MESSAGE fires MEDUT_EV_MESSAGE
 *          via the event queue.
 *
 * Lifecycle: bsdk_message_init() is called from core.c after ua_init().
 *            bsdk_message_close() is called from medut_shutdown().
 */

#include <string.h>
#include "medut_internal.h"

/* ── Incoming MESSAGE handler ────────────────────────────────────────────── */

static void message_recv_handler(struct ua *ua, const struct pl *peer,
                                  const struct pl *ctype,
                                  struct mbuf *body, void *arg)
{
	(void)arg;

	struct medut_account *acct = bsdk_account_find_by_ua(ua);

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
	if (!ctx->acct->ua) { ctx->result = ENOENT; return; }
	/* baresip message_send uses text/plain; custom content_type via
	 * medut_message_send() is accepted for future use */
	ctx->result = message_send(ctx->acct->ua, ctx->to_uri,
	                           ctx->body, NULL, NULL);
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

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bsdk_message_init(void)
{
	return message_listen(baresip_message(), message_recv_handler, NULL);
}

void bsdk_message_close(void)
{
	message_unlisten(baresip_message(), message_recv_handler);
}
