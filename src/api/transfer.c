/**
 * @file transfer.c  Blind + attended transfer (REFER / REFER w/ Replaces)
 *
 * Blind transfer   — medut_call_transfer() (in call.c)
 *                    sends REFER with Refer-To: <uri>
 *
 * Attended transfer — medut_call_attended_transfer(call_a, call_b)
 *                    sends REFER with Refer-To: <call_b_uri>;Replaces=...
 *                    mdt_call_replace_transfer(call_a->ec, call_b->ec)
 *
 * Incoming REFER   — the engine reports the Refer-To URI
 *                    (bsdk_ev_transfer_request).  If it contains "?Replaces="
 *                    it is an attended transfer.  Surfaced as
 *                    MEDUT_EV_TRANSFER_REQUEST.
 */

#include <string.h>
#include "bsdk.h"

/* ── Attended transfer ───────────────────────────────────────────────────── */

typedef struct {
	struct medut_call *call_a;
	struct medut_call *call_b;
	int                  result;
} atxfer_ctx_t;

static void attended_transfer_fn(void *arg)
{
	atxfer_ctx_t *ctx = arg;
	if (!ctx->call_a->ec || !ctx->call_b->ec) {
		ctx->result = ENOENT;
		return;
	}
	ctx->result = mdt_call_replace_transfer(ctx->call_a->ec,
	                                        ctx->call_b->ec);
}

int medut_call_attended_transfer(medut_call_handle_t call_a,
                                    medut_call_handle_t call_b)
{
	if (!call_a || !call_b) return MEDUT_ERR_INVAL;
	atxfer_ctx_t ctx = {.call_a = call_a, .call_b = call_b, .result = 0};
	int err = bsdk_dispatch_sync(attended_transfer_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── Incoming REFER (core/mdt_events.h) ────────────────────────────────────── */

void bsdk_ev_transfer_request(struct medut_call *lc,
                              struct medut_account *acct,
                              const char *text)   /* Refer-To URI */
{
	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type = MEDUT_EV_TRANSFER_REQUEST;
	medut_ev_transfer_req_t *tr = &qev->ev.u.transfer_req;
	tr->call    = lc;
	tr->account = acct;

	/* Pack Refer-To URI into buf */
	if (text) {
		str_ncpy(qev->buf, text, sizeof(qev->buf));
		tr->refer_to_uri = qev->buf;
		/* Attended transfer if Replaces parameter is present */
		tr->has_replaces = (strstr(text, "?Replaces=") != NULL ||
		                    strstr(text, "&Replaces=") != NULL);

		/* Keep our own copy: the app answers this asynchronously, long
		 * after qev->buf and the engine event behind it are gone. */
		if (lc) {
			str_ncpy(lc->xfer_refer_to, text,
			         sizeof(lc->xfer_refer_to));
			lc->xfer_pending = true;
		}
	}

	/* Nothing follows this REFER unless the app says so.  The engine has
	 * already sent 202 Accepted and NOTIFY 100 Trying and is now waiting on
	 * us for the terminating NOTIFY — see medut_call_transfer_accept(). */
	tr->auto_followed = false;

	bsdk_event_post_qev(qev);   /* warns and frees qev when the queue is full */
}

/* ── Incoming REFER: accept / reject ─────────────────────────────────────────
 *
 * RFC 3515 makes the REFER an implicit subscription: the transferor is owed a
 * final `message/sipfrag` NOTIFY saying whether the reference succeeded, and
 * until it arrives it has no way to know whether to hang up or recover.
 * The engine sends the 202 and the `100 Trying` for us and then stops,
 * because deciding whether to follow a transfer is policy, not transport.
 * The two functions below are that decision.
 *
 * Accepting is not "hang up and dial".  The notifier lives on the
 * transferee's call, and the only thing that moves it to the new call is
 * allocating that call from the old one (mdt_call_xfer_alloc()) — the engine
 * then emits `NOTIFY 200 OK` on its own when the new call reaches
 * ESTABLISHED, and reports a failure on the new call the same way.  So the
 * whole progression is automatic provided the call is allocated that way,
 * and is lost entirely if the app dials by hand.
 */

typedef struct {
	struct medut_call  *lc;
	struct medut_call **out;
	int                   result;
} xfer_accept_ctx_t;

static void transfer_accept_fn(void *arg)
{
	xfer_accept_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;
	struct medut_call *new_lc = NULL;
	struct mdt_call *call2 = NULL;
	int err;

	if (!lc->ec || !lc->acct || !lc->acct->ea) {
		ctx->result = ENOENT;
		return;
	}

	if (!lc->xfer_pending || !lc->xfer_refer_to[0]) {
		/* No REFER outstanding.  Distinct from a transfer that failed:
		 * there is nothing to notify and nothing to dial. */
		ctx->result = MEDUT_ERR_STATE;
		return;
	}

	/* xcall = lc->ec is the whole point — it hands the new call the
	 * notifier this transfer is owed a NOTIFY on. */
	err = mdt_call_xfer_alloc(&call2, lc->acct->ea, lc->ec);
	if (err) {
		MDT_WARN("Medut/transfer: call alloc failed (%m)\n", err);
		(void)mdt_call_notify_sipfrag(lc->ec, 500, "Call Error");
		lc->xfer_pending = false;
		ctx->result = err;
		return;
	}

	/* Wrap before dialling: dialling can emit call events synchronously,
	 * and event.c silently reports a NULL handle for a call it has no
	 * wrapper for. */
	new_lc = bsdk_call_wrap_new(call2, lc->acct, MEDUT_CALL_CALLING, true);
	if (!new_lc) {
		(void)mdt_call_notify_sipfrag(lc->ec, 500, "Out of Memory");
		mdt_call_free(call2);
		lc->xfer_pending = false;
		ctx->result = ENOMEM;
		return;
	}

	err = mdt_call_dial(call2, lc->xfer_refer_to);
	if (err) {
		MDT_WARN("Medut/transfer: connect to '%s' failed (%m)\n",
		        lc->xfer_refer_to, err);
		/* Tell the transferor now.  Without this it waits out the
		 * 60 s subscription believing the transfer may still land. */
		(void)mdt_call_notify_sipfrag(lc->ec, 500, "Call Error");

		/* Drop the wrapper only if it is still ours to drop.  Dialling
		 * reports the call as outgoing before it can fail, and any event
		 * that ends the call hands the wrapper's last reference to the
		 * queued event (event.c, deref_after_deliver) after unregistering
		 * it.  Dereffing again here would be a double free. */
		if (bsdk_call_find(call2) == new_lc) {
			bsdk_call_unregister(new_lc);
			new_lc->ec = NULL;
			mem_deref(new_lc);
		}
		mdt_call_free(call2);
		lc->xfer_pending = false;
		ctx->result = err;
		return;
	}

	bsdk_call_setup_watch_start(new_lc);

	lc->xfer_pending = false;
	if (ctx->out)
		*ctx->out = new_lc;
	ctx->result = 0;
}

int medut_call_transfer_accept(medut_call_handle_t call,
                                 medut_call_handle_t *out)
{
	if (!call)
		return MEDUT_ERR_INVAL;

	if (out)
		*out = NULL;

	xfer_accept_ctx_t ctx = { .lc = call, .out = out, .result = 0 };
	int err = bsdk_dispatch_sync(transfer_accept_fn, &ctx);
	return err ? err : ctx.result;
}

typedef struct {
	struct medut_call *lc;
	uint16_t             scode;
	char                 reason[128];
	int                  result;
} xfer_reject_ctx_t;

static void transfer_reject_fn(void *arg)
{
	xfer_reject_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;

	if (!lc->ec) {
		ctx->result = ENOENT;
		return;
	}

	if (!lc->xfer_pending) {
		ctx->result = MEDUT_ERR_STATE;
		return;
	}

	/* scode >= 200 makes this the terminating NOTIFY: libre sends it with
	 * SIPEVENT_TERMINATED and the engine drops the notifier, so the
	 * subscription is closed and no second answer can be sent. */
	ctx->result = mdt_call_notify_sipfrag(lc->ec, ctx->scode,
	                                      ctx->reason[0] ? ctx->reason
	                                                     : "Declined");
	lc->xfer_pending = false;
}

int medut_call_transfer_reject(medut_call_handle_t call,
                                 uint16_t scode, const char *reason)
{
	if (!call)
		return MEDUT_ERR_INVAL;

	/* Below 200 the NOTIFY is a progress report and leaves the
	 * subscription open, so the transferor would keep waiting — which is
	 * the opposite of a refusal. */
	if (scode < 400 || scode > 699)
		return MEDUT_ERR_INVAL;

	xfer_reject_ctx_t ctx = { .lc = call, .scode = scode, .result = 0 };
	if (reason)
		str_ncpy(ctx.reason, reason, sizeof(ctx.reason));

	int err = bsdk_dispatch_sync(transfer_reject_fn, &ctx);
	return err ? err : ctx.result;
}
