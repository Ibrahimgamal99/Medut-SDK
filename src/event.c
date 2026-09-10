/**
 * @file event.c  baresip bevent → echosdk_event_t bridge
 *
 * baresip's global event handler fires on re_main. We CANNOT call consumer
 * callbacks from re_main because consumers may call back into EchoSDK
 * (e.g. echosdk_call_answer() from inside ECHOSDK_EV_INCOMING_CALL), which
 * would deadlock if we tried to dispatch to re_main from re_main.
 *
 * Solution: re_main-side handler enqueues to a mutex+condvar queue.
 * A dedicated event thread drains the queue and calls the consumer callback.
 * The consumer callback runs on the event thread — never on re_main.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "echosdk_internal.h"

/* ── Queued-event allocation ─────────────────────────────────────────────── */

/* A queued event may own a reference to the call wrapper it describes
 * (deref_after_deliver — see the field comment in echosdk_internal.h).  That
 * reference has to be released on *every* path the event can die on: normal
 * delivery, echosdk_event_release() in owned mode, and the queue-full drop
 * inside bsdk_event_post_qev().
 *
 * It used to be released by hand at each of those, which meant the drop path —
 * the one nobody exercises — silently leaked the wrapper, and with it the
 * ~16 KB of SDP/mixing buffers it carries and any WAV recording still open (the
 * destructor below is what closes rec_file).  Hanging it off the allocation
 * instead makes the release automatic and exactly-once, so a future call site
 * cannot forget it. */
static void qev_destructor(void *arg)
{
	struct echosdk_queued_event *qev = arg;

	mem_deref(qev->deref_after_deliver);
}

/* Allocate a zeroed queued event.  Every producer goes through here: log.c,
 * trace.c, sdp.c, stats.c, message.c, presence.c, transfer.c, netmon.c and
 * this file. */
struct echosdk_queued_event *bsdk_qev_alloc(void)
{
	return mem_zalloc(sizeof(struct echosdk_queued_event), qev_destructor);
}

/* ── Owned-event cloning (cfg.deliver_owned_events) ──────────────────────── */

/* Rebase a string pointer from the old event's inline buf into the clone's.
 * Pointers outside buf (static strings, call-wrapper buffers, NULL) pass
 * through unchanged. */
static const char *rb(const char *p,
                      const struct echosdk_queued_event *o,
                      const struct echosdk_queued_event *n)
{
	if (p >= o->buf && p < o->buf + sizeof(o->buf))
		return n->buf + (p - o->buf);
	return p;
}

/* Deep-clone a queued event for handoff to an async consumer.
 *
 * The whole block (struct + inline buf) is memcpy'd, then every string
 * pointer of the active union member is rebased into the clone's buf.
 * Keep the switch in sync with the echosdk_event_t union in echosdk.h
 * and with the event construction sites (log.c, trace.c, sdp.c, stats.c,
 * message.c, presence.c, transfer.c, netmon.c, this file).
 *
 * Ownership: `old`'s deref_after_deliver (the call wrapper kept alive for
 * CALL_ENDED delivery) is transferred to the clone.  SDP events point
 * local_sdp/remote_sdp into the call wrapper's stable buffers, so the
 * clone takes an extra reference on the wrapper to keep them valid until
 * echosdk_event_release(). */
static struct echosdk_queued_event *
qev_clone(struct echosdk_queued_event *old)
{
	struct echosdk_queued_event *n = bsdk_qev_alloc();
	if (!n)
		return NULL;

	memcpy(n, old, sizeof(*n));
	memset(&n->le, 0, sizeof(n->le));
	n->deref_after_deliver   = old->deref_after_deliver;
	old->deref_after_deliver = NULL;

	echosdk_event_t *ev = &n->ev;
	switch (ev->type) {
	case ECHOSDK_EV_LOG:
		ev->u.log.message = rb(ev->u.log.message, old, n);
		break;
	case ECHOSDK_EV_REG_STATE:
		ev->u.reg.error_str = rb(ev->u.reg.error_str, old, n);
		break;
	case ECHOSDK_EV_INCOMING_CALL:
		ev->u.incoming.from_uri     = rb(ev->u.incoming.from_uri, old, n);
		ev->u.incoming.display_name = rb(ev->u.incoming.display_name, old, n);
		break;
	case ECHOSDK_EV_CALL_STATE:
		ev->u.call_state.reason = rb(ev->u.call_state.reason, old, n);
		break;
	case ECHOSDK_EV_SDP_NEGOTIATION:
		ev->u.sdp.negotiated_codec  = rb(ev->u.sdp.negotiated_codec, old, n);
		ev->u.sdp.negotiated_crypto = rb(ev->u.sdp.negotiated_crypto, old, n);
		/* local_sdp/remote_sdp live in the call wrapper (sdp.c) —
		 * hold a reference so they outlive call teardown.
		 * rejected_codecs/warnings are never populated today; if that
		 * changes they must be packed into buf and rebased here. */
		if (ev->u.sdp.call && !n->deref_after_deliver)
			n->deref_after_deliver = mem_ref(ev->u.sdp.call);
		break;
	case ECHOSDK_EV_SIP_TRACE:
		ev->u.sip_trace.transport   = rb(ev->u.sip_trace.transport, old, n);
		ev->u.sip_trace.remote_addr = rb(ev->u.sip_trace.remote_addr, old, n);
		ev->u.sip_trace.raw_message = rb(ev->u.sip_trace.raw_message, old, n);
		break;
	case ECHOSDK_EV_MEDIA_STATS:
		ev->u.stats.codec_name = rb(ev->u.stats.codec_name, old, n);
		break;
	case ECHOSDK_EV_REGISTRAR_WARNING:
		ev->u.reg_warn.message = rb(ev->u.reg_warn.message, old, n);
		break;
	case ECHOSDK_EV_TRANSFER_REQUEST:
		ev->u.transfer_req.refer_to_uri =
			rb(ev->u.transfer_req.refer_to_uri, old, n);
		break;
	case ECHOSDK_EV_TRANSFER_FAILED:
		ev->u.transfer_failed.reason =
			rb(ev->u.transfer_failed.reason, old, n);
		break;
	case ECHOSDK_EV_MWI:
		ev->u.mwi.raw_body = rb(ev->u.mwi.raw_body, old, n);
		break;
	case ECHOSDK_EV_MESSAGE:
		ev->u.msg.from_uri     = rb(ev->u.msg.from_uri, old, n);
		ev->u.msg.body         = rb(ev->u.msg.body, old, n);
		ev->u.msg.content_type = rb(ev->u.msg.content_type, old, n);
		break;
	case ECHOSDK_EV_PRESENCE_STATE:
		ev->u.presence.target_uri = rb(ev->u.presence.target_uri, old, n);
		break;
	case ECHOSDK_EV_QUALITY_ALERT:
	case ECHOSDK_EV_CALL_DTMF:
		/* no string fields */
		break;
	case ECHOSDK_EV_NETWORK:
		ev->u.network.local_addr = rb(ev->u.network.local_addr, old, n);
		break;
	}

	return n;
}

void echosdk_event_release(const echosdk_event_t *ev)
{
	if (!ev)
		return;

	struct echosdk_queued_event *qev = (struct echosdk_queued_event *)
		((char *)(uintptr_t)ev -
		 offsetof(struct echosdk_queued_event, ev));

	/* qev_destructor releases deref_after_deliver. */
	mem_deref(qev);
}

/* ── Event thread ────────────────────────────────────────────────────────── */

static int event_thread_fn(void *arg)
{
	(void)arg;

	mtx_lock(&g_bsdk.ev_lock);

	for (;;) {
		/* Wait for work or shutdown signal */
		while (list_isempty(&g_bsdk.ev_queue) && !g_bsdk.ev_shutdown)
			cnd_wait(&g_bsdk.ev_cond, &g_bsdk.ev_lock);

		/* Drain the whole queue before checking shutdown again */
		struct le *le = list_head(&g_bsdk.ev_queue);
		if (le) {
			struct echosdk_queued_event *qev = le->data;
			list_unlink(&qev->le);
			g_bsdk.ev_queue_len--;
			/* Snapshot the handler while still holding ev_lock:
			 * echosdk_set_event_handler() may swap all three fields
			 * concurrently and this delivery must use one coherent
			 * set. Once taken, the old callback can still be running
			 * here after the swap returns, which is why the swap only
			 * promises that no *new* delivery uses it. */
			echosdk_event_cb_t  cb    = g_bsdk.cfg.event_cb;
			void               *cb_ud = g_bsdk.cfg.event_userdata;
			bool                owned = g_bsdk.cfg.deliver_owned_events;
			/* Held across the callback so a handler swap can wait the
			 * old callback out instead of returning while it still
			 * runs — consumers free the callback right after swapping
			 * (a Dart NativeCallable is closed, a plugin unloads). */
			g_bsdk.ev_delivering = true;
			mtx_unlock(&g_bsdk.ev_lock);

			if (cb) {
				if (owned) {
					/* Hand the consumer an owned clone; it
					 * frees it via echosdk_event_release().
					 * OOM → event dropped. */
					struct echosdk_queued_event *own =
						qev_clone(qev);
					if (own)
						cb(&own->ev, cb_ud);
				}
				else {
					cb(&qev->ev, cb_ud);
				}
			}
			/* Frees the call wrapper too (qev_destructor), only now
			 * that the app callback has returned, so the handle
			 * stays valid for the whole delivery.  In owned mode
			 * qev_clone already took that reference over and this
			 * qev's deref_after_deliver is NULL. */
			mem_deref(qev);

			mtx_lock(&g_bsdk.ev_lock);
			g_bsdk.ev_delivering = false;
			cnd_broadcast(&g_bsdk.ev_idle_cond);
		} else if (g_bsdk.ev_shutdown) {
			break;
		}
	}

	mtx_unlock(&g_bsdk.ev_lock);
	return 0;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Takes ownership of qev. Returns true on success, false if queue was full
 * (qev is freed on failure). Safe to call from re_main or any thread. */
bool bsdk_event_post_qev(struct echosdk_queued_event *qev)
{
	mtx_lock(&g_bsdk.ev_lock);
	if (g_bsdk.ev_queue_len >= g_bsdk.ev_queue_max) {
		int type = qev->ev.type;   /* read before deref */
		mtx_unlock(&g_bsdk.ev_lock);
		mem_deref(qev);
		warning("EchoSDK/event: dropped event type %d (queue full)\n", type);
		return false;
	}
	list_append(&g_bsdk.ev_queue, &qev->le, qev);
	g_bsdk.ev_queue_len++;
	cnd_signal(&g_bsdk.ev_cond);
	mtx_unlock(&g_bsdk.ev_lock);
	return true;
}

/* Post a copy of ev to the event queue. Safe to call from re_main. */
void bsdk_event_post(const echosdk_event_t *ev)
{
	struct echosdk_queued_event *qev = bsdk_qev_alloc();
	if (!qev) {
		warning("EchoSDK/event: dropped event type %d (OOM)\n", ev->type);
		return;
	}
	memcpy(&qev->ev, ev, sizeof(*ev));
	bsdk_event_post_qev(qev);  /* warns and frees qev on failure */
}

/* Parse the leading 3-digit SIP status from a reason string like "403 Forbidden".
 * Returns 0 if the string doesn't start with a valid 3-digit code. */
static int sip_reason_code(const char *reason)
{
	if (!reason) return 0;
	char *end;
	long code = strtol(reason, &end, 10);
	if (end == reason || end - reason != 3 || code < 100 || code > 699)
		return 0;
	return (int)code;
}

/* ── bevent handler (runs on re_main) ────────────────────────────────────── */

/**
 * True when baresip's "no common codecs" is really a media-encryption mismatch.
 *
 * Only claimed when this account offers no encryption at all: that is the case
 * where the codec lists are irrelevant and the profile is certain to be the
 * problem.  The opposite direction (we require encryption, peer offers plain
 * RTP) is left with baresip's own wording — the peer's capabilities are not
 * ours to describe, and a genuine codec mismatch is still possible there.
 */
static bool codec_reason_is_really_menc(const char *reason,
                                        const struct echosdk_account *acct)
{
	echosdk_media_enc_t enc;

	if (!reason || !acct)
		return false;

	if (!strstr(reason, "common audio") && !strstr(reason, "common media"))
		return false;

	enc = acct->cfg.media_enc ? acct->cfg.media_enc : g_bsdk.cfg.media_enc;
	return enc == ECHOSDK_MEDIA_ENC_NONE;
}

static void bevent_handler(enum bevent_ev bev,
                            struct bevent *event,
                            void *arg)
{
	(void)arg;

	echosdk_event_t ev = {0};
	bool post = true;
	struct ua   *ua   = bevent_get_ua(event);
	struct call *bc   = bevent_get_call(event);

	struct echosdk_account *acct =
		ua ? bsdk_account_find_by_ua(ua) : NULL;
	struct echosdk_call    *lc   =
		bc ? bsdk_call_find(bc) : NULL;

	switch (bev) {

	case BEVENT_SIPSESS_CONN: {
		const struct sip_msg *msg = bevent_get_msg(event);
		if (!msg) break;
		struct ua *found_ua = uag_find_msg(msg);
		if (found_ua)
			ua_accept(found_ua, msg);
		post = false;
		break;
	}

	case BEVENT_REGISTERING:
		ev.type          = ECHOSDK_EV_REG_STATE;
		ev.u.reg.account = acct;

		if (acct && acct->reconnecting) {
			/* A REGISTER going out inside a recovery — a retry, or a
			 * handover's re-REGISTER — is still the reconnect the app was
			 * told about, not a new registration.  Reporting REGISTERING
			 * here would flip the status line between "Reconnecting…" and
			 * "Registering…" once per attempt for the whole outage. */
			ev.u.reg.state  = ECHOSDK_REG_RECONNECTING;
			ev.u.reg.error  = acct->reg_error;
			acct->reg_state = ECHOSDK_REG_RECONNECTING;
			/* The countdown event already said exactly this, so
			 * the attempt leaving the socket is not news. */
			if (!bsdk_account_reg_ev_prepare(acct, &ev)) {
				post = false;
				break;
			}

			/* Carry the reason the recovery started with, so an app that
			 * renders only the latest event still has one.  Queued with its
			 * own copy: acct->reg_error_str is re-written by the next
			 * failure, and the consumer reads this on another thread. */
			if (acct->reg_error_str[0]) {
				struct echosdk_queued_event *qev =
				        bsdk_qev_alloc();
				if (qev) {
					str_ncpy(qev->buf, acct->reg_error_str,
					         sizeof(qev->buf));
					memcpy(&qev->ev, &ev, sizeof(ev));
					qev->ev.u.reg.error_str = qev->buf;
					bsdk_event_post_qev(qev);
					post = false;
				}
			}
		}
		else {
			ev.u.reg.state = ECHOSDK_REG_REGISTERING;
			if (acct)
				acct->reg_state = ECHOSDK_REG_REGISTERING;
		}
		break;

	/* FALLBACK_OK is the same success, reported under a different name:
	 * baresip picks the event by reg->regint (src/reg.c), so a registration
	 * whose interval reaches reg_register() as 0 — a fallback registration,
	 * or an account whose regint never made it through — answers a 200 OK
	 * with FALLBACK_OK. Left unhandled it hit the silent `default` below,
	 * so the stack registered on the wire while every consumer sat in
	 * REGISTERING forever with no event and no error. */
	case BEVENT_REGISTER_OK:
	case BEVENT_FALLBACK_OK:
		ev.type        = ECHOSDK_EV_REG_STATE;
		ev.u.reg.state = ECHOSDK_REG_REGISTERED;
		ev.u.reg.error = ECHOSDK_OK;
		ev.u.reg.account = acct;
		if (acct) {
			acct->reg_state    = ECHOSDK_REG_REGISTERED;
			acct->retry_attempt = 0;
			/* Recovered — the next failure starts a new reconnect. */
			acct->reconnecting = false;
			acct->ka_failed    = false;
			acct->reg_error    = ECHOSDK_OK;
			acct->reg_error_str[0] = '\0';
			/* Registered and reachable — start probing so we notice if
			 * that stops being true before the next refresh. */
			bsdk_account_keepalive_arm(acct);
			(void)bsdk_account_reg_ev_prepare(acct, &ev);
		}
		break;

	case BEVENT_REGISTER_FAIL:
	case BEVENT_FALLBACK_FAIL: {   /* see FALLBACK_OK above */
		const char *reason = bevent_get_text(event);
		ev.type            = ECHOSDK_EV_REG_STATE;
		ev.u.reg.account   = acct;
		/* Classify error by 3-digit SIP status code */
		int code = sip_reason_code(reason);
		if (code == 401 || code == 403 || code == 407)
			ev.u.reg.error = ECHOSDK_ERR_AUTH;
		else if (code >= 500 && code < 600)
			ev.u.reg.error = ECHOSDK_ERR_SERVER_5XX;
		else
			ev.u.reg.error = ECHOSDK_ERR_TRANSPORT;

		/* RECONNECTING while the SDK still has a retry to spend on this —
		 * a timeout, a 5xx, a transport that died with the network.  FAILED
		 * only for the ones no retry can fix (auth) or when the budget is
		 * gone.  Decided before the event is posted so both agree. */
		ev.u.reg.state = acct
		        ? bsdk_account_reg_fail_state(acct, ev.u.reg.error)
		        : ECHOSDK_REG_FAILED;

		if (acct) {
			acct->reg_state  = ev.u.reg.state;
			acct->reg_error  = ev.u.reg.error;
			/* No point probing a registration that is down; the
			 * retry path re-arms this on success. */
			bsdk_account_keepalive_cancel(acct);
			if (reason)
				str_ncpy(acct->reg_error_str, reason,
				         sizeof(acct->reg_error_str));

			/* Arm the retry before reporting the failure, not after:
			 * scheduling settles the attempt number and the countdown
			 * this failure is reported with.  Done the other way round
			 * the app got the failure with attempt 0 / delay 0, then
			 * the countdown, then the retry's own REGISTER with 0 and
			 * 0 again — three events per cycle, two of them empty.
			 *
			 * Outside the queued-event allocation below as well: a
			 * failed alloc costs one event, it must not cost the
			 * recovery. */
			if (ev.u.reg.error != ECHOSDK_ERR_AUTH)
				bsdk_account_schedule_retry(acct);

			/* schedule_retry() has just reported this same reconnect,
			 * carrying the same numbers and the same reason. */
			if (!bsdk_account_reg_ev_prepare(acct, &ev)) {
				post = false;
				break;
			}
		}

		/* Enqueue via a queued_event so the string lives long enough */
		struct echosdk_queued_event *qev = bsdk_qev_alloc();
		if (qev) {
			if (reason)
				str_ncpy(qev->buf, reason, sizeof(qev->buf));
			memcpy(&qev->ev, &ev, sizeof(ev));
			qev->ev.u.reg.error_str = reason ? qev->buf : NULL;
			bsdk_event_post_qev(qev);
		}
		post = false;
		break;
	}

	case BEVENT_UNREGISTERING:
		ev.type        = ECHOSDK_EV_REG_STATE;
		ev.u.reg.state = ECHOSDK_REG_UNREGISTERING;
		ev.u.reg.account = acct;
		if (acct) {
			acct->reg_state    = ECHOSDK_REG_UNREGISTERING;
			acct->reconnecting = false;
			(void)bsdk_account_reg_ev_prepare(acct, &ev);
		}
		break;

	case BEVENT_CALL_INCOMING: {
		/* An incoming call inherits no account headers: those exist to be
		 * put on requests we originate, and this dialog's requests were
		 * originated by the peer. */
		struct echosdk_call *new_lc =
			bsdk_call_wrap_new(bc, acct, ECHOSDK_CALL_RINGING, false);
		if (!new_lc) { post = false; break; }

		struct echosdk_queued_event *qev = bsdk_qev_alloc();
		if (!qev) {
			bsdk_call_unregister(new_lc);
			mem_deref(new_lc);
			post = false;
			break;
		}
		qev->ev.type = ECHOSDK_EV_INCOMING_CALL;
		qev->ev.u.incoming.account = acct;
		qev->ev.u.incoming.call    = new_lc;
		const char *peer = call_peeruri(bc);
		if (peer) {
			str_ncpy(qev->buf, peer, sizeof(qev->buf));
			qev->ev.u.incoming.from_uri = qev->buf;
		}
		if (!bsdk_event_post_qev(qev)) {
			bsdk_call_unregister(new_lc);
			mem_deref(new_lc);
		}
		post = false;
		break;
	}

	case BEVENT_CALL_RINGING:
	case BEVENT_CALL_PROGRESS:   /* 183 w/ SDP — alerting, early media may
	                              * already be flowing, so the consumer needs
	                              * the same signal as a plain 180. */
		ev.type = ECHOSDK_EV_CALL_STATE;
		ev.u.call_state.call    = lc;
		ev.u.call_state.account = acct;
		ev.u.call_state.state   = ECHOSDK_CALL_RINGING;
		if (lc) lc->state = ECHOSDK_CALL_RINGING;
		break;

	case BEVENT_CALL_ANSWERED:
	case BEVENT_CALL_ESTABLISHED:
		ev.type = ECHOSDK_EV_CALL_STATE;
		ev.u.call_state.call    = lc;
		ev.u.call_state.account = acct;
		ev.u.call_state.state   = ECHOSDK_CALL_ESTABLISHED;
		if (lc) {
			lc->state            = ECHOSDK_CALL_ESTABLISHED;
			lc->stats_call_start = tmr_jiffies();
			/* The call is up — the setup watchdog has nothing left to
			 * guard against. */
			bsdk_call_setup_watch_cancel(lc);
			/* Arm stall detection from here, so a call that never
			 * receives a packet is reported rather than waiting for a
			 * counter that will never move. */
			bsdk_adapt_call_start(lc);
			/* A migration deferred while the dialog was early can now
			 * proceed — the SDP negotiation is complete. */
			bsdk_netmon_call_refreshable(lc);
		}
		break;

	case BEVENT_CALL_CLOSED: {
		const char *reason = bevent_get_text(event);
		ev.type = ECHOSDK_EV_CALL_STATE;
		ev.u.call_state.call    = lc;
		ev.u.call_state.account = acct;

		/* Classify by SIP status: 487 → CANCELLED, 4xx/5xx/6xx → FAILED, else → ENDED.
		 * OS-level transport errors arrive as reason strings without a SIP code (e.g.
		 * "Connection reset by peer [104]") — detect them by a trailing "[<errno>]". */
		int scode = sip_reason_code(reason);
		bool is_transport_err = (scode == 0 && reason &&
		                         strrchr(reason, '[') && strrchr(reason, ']') > strrchr(reason, '['));

		/* ...except that ECONNRESET is also how libre reports a peer-initiated
		 * termination, not only a dead socket.  Its BYE handler answers 200 OK
		 * and then calls sipsess_terminate(sess, ECONNRESET, NULL)
		 * (re/src/sipsess/listen.c), and the CANCEL path does the same with the
		 * CANCEL message (accept.c).  baresip's close handler tests `err`
		 * before `msg`, so both arrive here as "Connection reset by peer [104]"
		 * with nothing to tell them apart from a genuine reset.
		 *
		 * The call's own state does tell them apart well enough to matter: a
		 * call that was ESTABLISHED and then saw this ended normally — the far
		 * end hung up.  Reporting that as FAILED with a transport error was the
		 * visible bug: apps that branch on ENDED left the call on screen, and
		 * every normal remote hangup looked like a network fault.
		 *
		 * A real socket death on an established call is then also reported as
		 * ENDED.  That is the right trade: the call is over either way, and the
		 * transport itself is covered by REG_STATE and ECHOSDK_EV_NETWORK, which
		 * a plain hangup never touches. */
		bool peer_hangup = is_transport_err && lc &&
		                   lc->state == ECHOSDK_CALL_ESTABLISHED;

		if (scode == 487)
			ev.u.call_state.state = ECHOSDK_CALL_CANCELLED;
		else if (peer_hangup)
			ev.u.call_state.state = ECHOSDK_CALL_ENDED;
		else if (scode >= 400 || is_transport_err)
			ev.u.call_state.state = ECHOSDK_CALL_FAILED;
		else
			ev.u.call_state.state = ECHOSDK_CALL_ENDED;

		if (scode == 401 || scode == 403 || scode == 407)
			ev.u.call_state.error = ECHOSDK_ERR_AUTH;
		/* 408 is what the setup watchdog cancels with, and what a proxy
		 * sends when its own transaction timed out — both are timeouts,
		 * not the malformed-request sense of ECHOSDK_ERR_INVAL. */
		else if (scode == 408)
			ev.u.call_state.error = ECHOSDK_ERR_TIMEOUT;
		else if (scode >= 500 && scode < 600)
			ev.u.call_state.error = ECHOSDK_ERR_SERVER_5XX;
		else if (scode >= 400)
			ev.u.call_state.error = ECHOSDK_ERR_INVAL;
		else if (is_transport_err && !peer_hangup)
			ev.u.call_state.error = ECHOSDK_ERR_TRANSPORT;

		if (lc) {
			bsdk_stats_collect_final(lc);  /* must be before bc = NULL */
			lc->state = ev.u.call_state.state;
			lc->bc    = NULL; /* baresip will free the call */
			bsdk_call_unregister(lc);
		}

		struct echosdk_queued_event *qev = bsdk_qev_alloc();
		if (qev) {
			memcpy(&qev->ev, &ev, sizeof(ev));
			if (peer_hangup) {
				/* Don't pass libre's ECONNRESET text on as the
				 * reason for a normal hangup — it reads as a
				 * network fault to anyone logging it. */
				str_ncpy(qev->buf, "Remote hangup",
				         sizeof(qev->buf));
				qev->ev.u.call_state.reason = qev->buf;
			}
			else if (reason && codec_reason_is_really_menc(reason, acct)) {
				/* baresip disables a stream whose media profile it
				 * cannot match and then reports the call as having
				 * no common codecs.  When the profiles differ that
				 * message is actively misleading: an account
				 * offering RTP/AVP against a peer offering
				 * UDP/TLS/RTP/SAVPF fails with a full codec list on
				 * both sides and nothing wrong with any of them.
				 * Name the real mismatch. */
				str_ncpy(qev->buf,
				         "No common media: this account offers "
				         "unencrypted RTP and the peer requires "
				         "encrypted media (set media_enc, e.g. "
				         "DTLS-SRTP)",
				         sizeof(qev->buf));
				qev->ev.u.call_state.reason = qev->buf;
			}
			else if (reason) {
				str_ncpy(qev->buf, reason, sizeof(qev->buf));
				qev->ev.u.call_state.reason = qev->buf;
			}
			/* Transfer ownership of lc to the event; freed after callback. */
			qev->deref_after_deliver = lc;
			bsdk_event_post_qev(qev);
		}
		post = false;
		break;
	}

	case BEVENT_CALL_HOLD:
		ev.type = ECHOSDK_EV_CALL_STATE;
		ev.u.call_state.call    = lc;
		ev.u.call_state.account = acct;
		ev.u.call_state.state   = ECHOSDK_CALL_HELD;
		if (lc) lc->state = ECHOSDK_CALL_HELD;
		break;

	case BEVENT_CALL_RESUME:
		ev.type = ECHOSDK_EV_CALL_STATE;
		ev.u.call_state.call    = lc;
		ev.u.call_state.account = acct;
		ev.u.call_state.state   = ECHOSDK_CALL_ESTABLISHED;
		if (lc) {
			lc->state = ECHOSDK_CALL_ESTABLISHED;
			bsdk_netmon_call_refreshable(lc);
		}
		break;

	case BEVENT_CALL_DTMF_START:
	case BEVENT_CALL_DTMF_END: {
		const char *txt = bevent_get_text(event);
		if (!txt || !txt[0]) { post = false; break; }
		ev.type = ECHOSDK_EV_CALL_DTMF;
		ev.u.dtmf.call  = lc;
		ev.u.dtmf.digit = txt[0];
		break;
	}

	case BEVENT_CALL_LOCAL_SDP:
	case BEVENT_CALL_REMOTE_SDP: {
		const char *param = bevent_get_text(event);
		bsdk_sdp_handle_event(bev, event);
		/* The answer to a migration re-INVITE — report that the peer
		 * accepted the new address, before audio is confirmed. */
		if (lc && bev == BEVENT_CALL_REMOTE_SDP &&
		    param && !str_casecmp(param, "answer"))
			bsdk_netmon_call_sdp_answer(lc);
		post = false;
		break;
	}

	/* ── Transfer / MWI ──────────────────────────────────────────────────── */

	case BEVENT_CALL_TRANSFER:
	case BEVENT_REFER:
		/* Refer-To URI is in bevent_get_text(); attended = has "Replaces=" */
		bsdk_transfer_handle_event(event);
		post = false;
		break;

	case BEVENT_CALL_TRANSFER_FAILED: {
		/* A refused REFER leaves the call ESTABLISHED — baresip does not
		 * close it, and reporting ECHOSDK_CALL_FAILED here did: consumers
		 * treat that state as terminal and drop their call handle, so a
		 * blind transfer to a busy or unknown extension silently destroyed
		 * the live call the user was still talking on.  Report the transfer
		 * outcome instead and leave lc->state alone. */
		const char *reason = bevent_get_text(event);
		ev.type = ECHOSDK_EV_TRANSFER_FAILED;
		ev.u.transfer_failed.call    = lc;
		ev.u.transfer_failed.account = acct;
		ev.u.transfer_failed.reason  = NULL;

		if (reason) {
			struct echosdk_queued_event *qev = bsdk_qev_alloc();
			if (qev) {
				memcpy(&qev->ev, &ev, sizeof(ev));
				str_ncpy(qev->buf, reason, sizeof(qev->buf));
				qev->ev.u.transfer_failed.reason = qev->buf;
				bsdk_event_post_qev(qev);
				post = false;
			}
		}
		break;
	}

	case BEVENT_MWI_NOTIFY:
		bsdk_presence_handle_mwi(event);
		post = false;
		break;

	/* Known and deliberately not forwarded.  Listed so they stay out of the
	 * "unhandled" log below — an unexplained drop reads as a gap in the
	 * mapping, and CALL_RTCP alone fires every few seconds of every call.
	 *
	 *   OUTGOING  fires inside ua_connect(), before echosdk_call_invite()
	 *             has built its wrapper, so there is no call handle to
	 *             report yet.  The invite returns the handle in state
	 *             CALLING instead.
	 *   RTPESTAB  first RTP packet; MEDIA_STATS and the audio-level events
	 *             already cover live media.
	 *   RTCP      periodic report; stats.c samples the same counters off
	 *             its own timer for ECHOSDK_EV_MEDIA_STATS.
	 *   MENC      media-encryption progress; the SDK reports the outcome
	 *             through the call state and SDP negotiation events.
	 *   CREATE    a UA was allocated, which the caller already knows:
	 *             echosdk_account_create() returned it the handle.
	 */
	case BEVENT_CALL_MENC:
		/* Not posted, but netmon wants to know: a migration held back
		 * because the encryption was mid-handshake can go out as soon
		 * as it finishes, instead of waiting for the next verify tick.
		 * The bounce only acts on a call that is actually deferred, and
		 * send_migration() re-tests the handshake itself, so a MENC
		 * event that is not "secure" costs a no-op. */
		if (lc)
			bsdk_netmon_call_refreshable(lc);
		post = false;
		break;

	case BEVENT_CREATE:
	case BEVENT_CALL_OUTGOING:
	case BEVENT_CALL_RTPESTAB:
	case BEVENT_CALL_RTCP:
		post = false;
		break;

	default:
		/* Name what we drop. A baresip event this switch does not know
		 * is indistinguishable from one that never fired, which is how
		 * FALLBACK_OK above cost a day: the stack was registered and the
		 * app could not tell. */
		debug("EchoSDK: unhandled baresip event %s (%d)\n",
		      bevent_str(bev), (int)bev);
		post = false;
		break;
	}

	if (post)
		bsdk_event_post(&ev);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bsdk_event_init(void)
{
	g_bsdk.ev_shutdown = false;

	int rc = thrd_create(&g_bsdk.ev_thread, event_thread_fn, NULL);
	if (rc != thrd_success)
		return ENOMEM;

	g_bsdk.ev_thread_started = true;

	return bevent_register(bevent_handler, NULL);
}

void bsdk_event_close(void)
{
	/* Unregister the baresip hook first so no new events arrive */
	bevent_unregister(bevent_handler);

	/* Signal the event thread to drain and exit.
	 *
	 * Guarded because this also runs from echosdk_init()'s failure path,
	 * which unwinds every subsystem whether or not it got as far as being
	 * started.  Joining a thrd_t that was never assigned is undefined and in
	 * practice segfaults, so an SDK that failed to initialise crashed the
	 * host application instead of returning the error. */
	if (g_bsdk.ev_thread_started) {
		mtx_lock(&g_bsdk.ev_lock);
		g_bsdk.ev_shutdown = true;
		cnd_signal(&g_bsdk.ev_cond);
		mtx_unlock(&g_bsdk.ev_lock);

		thrd_join(g_bsdk.ev_thread, NULL);
		g_bsdk.ev_thread_started = false;
	}

	/* Free any remaining queued events */
	struct le *le, *le_tmp;
	LIST_FOREACH_SAFE(&g_bsdk.ev_queue, le, le_tmp) {
		struct echosdk_queued_event *qev = le->data;
		list_unlink(&qev->le);
		mem_deref(qev);
	}

	/* Keep the counter with the list.  echosdk_init() re-zeroes it, so this
	 * is belt-and-braces today, but a stale non-zero length here would make
	 * a re-initialised SDK think its empty queue was already full and drop
	 * every event forever. */
	g_bsdk.ev_queue_len = 0;
}
