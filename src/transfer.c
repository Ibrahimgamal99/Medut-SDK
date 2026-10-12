/**
 * @file transfer.c  Blind + attended transfer (REFER / REFER w/ Replaces)
 *
 * Blind transfer   — medut_call_transfer() (in call.c)
 *                    sends REFER with Refer-To: <uri>
 *
 * Attended transfer — medut_call_attended_transfer(call_a, call_b)
 *                    sends REFER with Refer-To: <call_b_uri>;Replaces=...
 *                    baresip: call_replace_transfer(call_a->bc, call_b->bc)
 *
 * Incoming REFER   — BEVENT_CALL_TRANSFER fires with Refer-To URI in text.
 *                    If URI contains "?Replaces=" it is an attended transfer.
 *                    Surfaced as MEDUT_EV_TRANSFER_REQUEST.
 */

#include <string.h>
#include "medut_internal.h"

/* ── Attended transfer ───────────────────────────────────────────────────── */

typedef struct {
	struct medut_call *call_a;
	struct medut_call *call_b;
	int                  result;
} atxfer_ctx_t;

static void attended_transfer_fn(void *arg)
{
	atxfer_ctx_t *ctx = arg;
	if (!ctx->call_a->bc || !ctx->call_b->bc) {
		ctx->result = ENOENT;
		return;
	}
	ctx->result = call_replace_transfer(ctx->call_a->bc, ctx->call_b->bc);
}

int medut_call_attended_transfer(medut_call_handle_t call_a,
                                    medut_call_handle_t call_b)
{
	if (!call_a || !call_b) return MEDUT_ERR_INVAL;
	atxfer_ctx_t ctx = {.call_a = call_a, .call_b = call_b, .result = 0};
	int err = bsdk_dispatch_sync(attended_transfer_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── Incoming REFER handler — called from event.c ────────────────────────── */

void bsdk_transfer_handle_event(struct bevent *event)
{
	struct call *bc   = bevent_get_call(event);
	struct ua   *ua   = bevent_get_ua(event);
	const char  *text = bevent_get_text(event);   /* Refer-To URI */

	struct medut_call    *lc   = bc ? bsdk_call_find(bc)          : NULL;
	struct medut_account *acct = ua ? bsdk_account_find_by_ua(ua) : NULL;

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
		 * after qev->buf and the bevent behind it are gone. */
		if (lc) {
			str_ncpy(lc->xfer_refer_to, text,
			         sizeof(lc->xfer_refer_to));
			lc->xfer_pending = true;
		}
	}

	/* Nothing follows this REFER unless the app says so.  baresip has
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
 * baresip sends the 202 and the `100 Trying` for us (sipsess_refer_handler)
 * and then stops, because deciding whether to follow a transfer is policy, not
 * transport.  The two functions below are that decision.
 *
 * Accepting is not "hang up and dial".  The notifier lives on the transferee's
 * call as `call->not`, and the only thing that moves it to the new call is
 * passing the old one as `xcall` to ua_call_alloc() — baresip then does
 * `call->not = mem_ref(xcall->not)` and, when the new call reaches
 * ESTABLISHED, emits `NOTIFY 200 OK` on its own.  A failure on the new call
 * reports itself the same way from sipsess_close_handler().  So the whole
 * progression is automatic provided the call is allocated that way, and is
 * lost entirely if the app dials by hand.  modules/menu/menu.c does exactly
 * this; we are matching it.
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
	struct call *call2 = NULL;
	struct pl pl;
	int err;

	if (!lc->bc || !lc->acct || !lc->acct->ua) {
		ctx->result = ENOENT;
		return;
	}

	if (!lc->xfer_pending || !lc->xfer_refer_to[0]) {
		/* No REFER outstanding.  Distinct from a transfer that failed:
		 * there is nothing to notify and nothing to dial. */
		ctx->result = MEDUT_ERR_STATE;
		return;
	}

	/* xcall = lc->bc is the whole point — it hands the new call the
	 * notifier this transfer is owed a NOTIFY on. */
	err = ua_call_alloc(&call2, lc->acct->ua, VIDMODE_OFF, NULL, lc->bc,
	                    call_localuri(lc->bc), true);
	if (err) {
		warning("Medut/transfer: ua_call_alloc failed (%m)\n", err);
		(void)call_notify_sipfrag(lc->bc, 500, "Call Error");
		lc->xfer_pending = false;
		ctx->result = err;
		return;
	}

	/* Carry the app's per-call data across, as menu.c does. */
	{
		const struct pl *ud = call_user_data(lc->bc);
		if (ud)
			(void)call_set_user_data(call2, ud);
	}

	/* Wrap before connecting: call_connect() can emit call events
	 * synchronously, and event.c silently reports a NULL handle for a call
	 * it has no wrapper for. */
	new_lc = bsdk_call_wrap_new(call2, lc->acct, MEDUT_CALL_CALLING, true);
	if (!new_lc) {
		(void)call_notify_sipfrag(lc->bc, 500, "Out of Memory");
		mem_deref(call2);
		lc->xfer_pending = false;
		ctx->result = ENOMEM;
		return;
	}

	pl_set_str(&pl, lc->xfer_refer_to);
	err = call_connect(call2, &pl);
	if (err) {
		warning("Medut/transfer: connect to '%s' failed (%m)\n",
		        lc->xfer_refer_to, err);
		/* Tell the transferor now.  Without this it waits out the
		 * 60 s subscription believing the transfer may still land. */
		(void)call_notify_sipfrag(lc->bc, 500, "Call Error");

		/* Drop the wrapper only if it is still ours to drop.  call_connect()
		 * emits CALL_EVENT_OUTGOING before it can fail, and any event that
		 * ends the call hands the wrapper's last reference to the queued
		 * event (event.c, deref_after_deliver) after unregistering it.
		 * Dereffing again here would be a double free. */
		if (bsdk_call_find(call2) == new_lc) {
			bsdk_call_unregister(new_lc);
			new_lc->bc = NULL;
			mem_deref(new_lc);
		}
		mem_deref(call2);
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

	if (!lc->bc) {
		ctx->result = ENOENT;
		return;
	}

	if (!lc->xfer_pending) {
		ctx->result = MEDUT_ERR_STATE;
		return;
	}

	/* scode >= 200 makes this the terminating NOTIFY: libre sends it with
	 * SIPEVENT_TERMINATED and baresip drops call->not, so the subscription
	 * is closed and no second answer can be sent. */
	ctx->result = call_notify_sipfrag(lc->bc, ctx->scode, "%s",
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
