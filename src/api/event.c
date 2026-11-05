/**
 * @file event.c  engine events → medut_event_t bridge
 *
 * The engine reports what happens through the hooks of core/mdt_events.h, on
 * re_main.  We CANNOT call consumer
 * callbacks from re_main because consumers may call back into Medut
 * (e.g. medut_call_answer() from inside MEDUT_EV_INCOMING_CALL), which
 * would deadlock if we tried to dispatch to re_main from re_main.
 *
 * Solution: re_main-side handler enqueues to a mutex+condvar queue.
 * A dedicated event thread drains the queue and calls the consumer callback.
 * The consumer callback runs on the event thread — never on re_main.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "bsdk.h"
#include "sip_status.h"

/* ── Queued-event allocation ─────────────────────────────────────────────── */

/* A queued event may own a reference to the call wrapper it describes
 * (deref_after_deliver — see the field comment in medut_internal.h).  That
 * reference has to be released on *every* path the event can die on: normal
 * delivery, medut_event_release() in owned mode, and the queue-full drop
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
	struct medut_queued_event *qev = arg;

	mem_deref(qev->deref_after_deliver);
}

/* Allocate a zeroed queued event.  Every producer goes through here: log.c,
 * trace.c, sdp.c, stats.c, message.c, presence.c, transfer.c, netmon.c and
 * this file. */
struct medut_queued_event *bsdk_qev_alloc(void)
{
	return mem_zalloc(sizeof(struct medut_queued_event), qev_destructor);
}

/* ── Owned-event cloning (cfg.deliver_owned_events) ──────────────────────── */

/* Rebase a string pointer from the old event's inline buf into the clone's.
 * Pointers outside buf (static strings, call-wrapper buffers, NULL) pass
 * through unchanged. */
static const char *rb(const char *p,
                      const struct medut_queued_event *o,
                      const struct medut_queued_event *n)
{
	if (p >= o->buf && p < o->buf + sizeof(o->buf))
		return n->buf + (p - o->buf);
	return p;
}

/* Deep-clone a queued event for handoff to an async consumer.
 *
 * The whole block (struct + inline buf) is memcpy'd, then every string
 * pointer of the active union member is rebased into the clone's buf.
 * Keep the switch in sync with the medut_event_t union in medut.h
 * and with the event construction sites (log.c, trace.c, sdp.c, stats.c,
 * message.c, presence.c, transfer.c, netmon.c, this file).
 *
 * Ownership: `old`'s deref_after_deliver (the call wrapper kept alive for
 * CALL_ENDED delivery) is transferred to the clone.  SDP events point
 * local_sdp/remote_sdp into the call wrapper's stable buffers, so the
 * clone takes an extra reference on the wrapper to keep them valid until
 * medut_event_release(). */
static struct medut_queued_event *
qev_clone(struct medut_queued_event *old)
{
	struct medut_queued_event *n = bsdk_qev_alloc();
	if (!n)
		return NULL;

	memcpy(n, old, sizeof(*n));
	memset(&n->le, 0, sizeof(n->le));
	n->deref_after_deliver   = old->deref_after_deliver;
	old->deref_after_deliver = NULL;

	medut_event_t *ev = &n->ev;
	switch (ev->type) {
	case MEDUT_EV_LOG:
		ev->u.log.message = rb(ev->u.log.message, old, n);
		break;
	case MEDUT_EV_REG_STATE:
		ev->u.reg.error_str = rb(ev->u.reg.error_str, old, n);
		break;
	case MEDUT_EV_INCOMING_CALL:
		ev->u.incoming.from_uri     = rb(ev->u.incoming.from_uri, old, n);
		ev->u.incoming.display_name = rb(ev->u.incoming.display_name, old, n);
		break;
	case MEDUT_EV_CALL_STATE:
		ev->u.call_state.reason = rb(ev->u.call_state.reason, old, n);
		break;
	case MEDUT_EV_SDP_NEGOTIATION:
		ev->u.sdp.negotiated_codec  = rb(ev->u.sdp.negotiated_codec, old, n);
		ev->u.sdp.negotiated_crypto = rb(ev->u.sdp.negotiated_crypto, old, n);
		/* local_sdp/remote_sdp live in the call wrapper (sdp.c) —
		 * hold a reference so they outlive call teardown.
		 * rejected_codecs/warnings are never populated today; if that
		 * changes they must be packed into buf and rebased here. */
		if (ev->u.sdp.call && !n->deref_after_deliver)
			n->deref_after_deliver = mem_ref(ev->u.sdp.call);
		break;
	case MEDUT_EV_SIP_TRACE:
		ev->u.sip_trace.transport   = rb(ev->u.sip_trace.transport, old, n);
		ev->u.sip_trace.remote_addr = rb(ev->u.sip_trace.remote_addr, old, n);
		ev->u.sip_trace.raw_message = rb(ev->u.sip_trace.raw_message, old, n);
		break;
	case MEDUT_EV_MEDIA_STATS:
		ev->u.stats.codec_name = rb(ev->u.stats.codec_name, old, n);
		break;
	case MEDUT_EV_REGISTRAR_WARNING:
		ev->u.reg_warn.message = rb(ev->u.reg_warn.message, old, n);
		break;
	case MEDUT_EV_TRANSFER_REQUEST:
		ev->u.transfer_req.refer_to_uri =
			rb(ev->u.transfer_req.refer_to_uri, old, n);
		break;
	case MEDUT_EV_TRANSFER_FAILED:
		ev->u.transfer_failed.reason =
			rb(ev->u.transfer_failed.reason, old, n);
		break;
	case MEDUT_EV_MWI:
		ev->u.mwi.raw_body = rb(ev->u.mwi.raw_body, old, n);
		break;
	case MEDUT_EV_MESSAGE:
		ev->u.msg.from_uri     = rb(ev->u.msg.from_uri, old, n);
		ev->u.msg.body         = rb(ev->u.msg.body, old, n);
		ev->u.msg.content_type = rb(ev->u.msg.content_type, old, n);
		break;
	case MEDUT_EV_PRESENCE_STATE:
		ev->u.presence.target_uri = rb(ev->u.presence.target_uri, old, n);
		break;
	case MEDUT_EV_QUALITY_ALERT:
	case MEDUT_EV_CALL_DTMF:
		/* no string fields */
		break;
	case MEDUT_EV_NETWORK:
		ev->u.network.local_addr = rb(ev->u.network.local_addr, old, n);
		break;
	case MEDUT_EV_ICE_STATE:
		ev->u.ice.local_addr  = rb(ev->u.ice.local_addr, old, n);
		ev->u.ice.remote_addr = rb(ev->u.ice.remote_addr, old, n);
		break;
	}

	return n;
}

void medut_event_release(const medut_event_t *ev)
{
	if (!ev)
		return;

	struct medut_queued_event *qev = (struct medut_queued_event *)
		((char *)(uintptr_t)ev -
		 offsetof(struct medut_queued_event, ev));

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
			struct medut_queued_event *qev = le->data;
			list_unlink(&qev->le);
			g_bsdk.ev_queue_len--;
			/* Snapshot the handler while still holding ev_lock:
			 * medut_set_event_handler() may swap all three fields
			 * concurrently and this delivery must use one coherent
			 * set. Once taken, the old callback can still be running
			 * here after the swap returns, which is why the swap only
			 * promises that no *new* delivery uses it. */
			medut_event_cb_t  cb    = g_bsdk.cfg.event_cb;
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
					 * frees it via medut_event_release().
					 * OOM → event dropped. */
					struct medut_queued_event *own =
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
bool bsdk_event_post_qev(struct medut_queued_event *qev)
{
	mtx_lock(&g_bsdk.ev_lock);
	if (g_bsdk.ev_queue_len >= g_bsdk.ev_queue_max) {
		int type = qev->ev.type;   /* read before deref */
		mtx_unlock(&g_bsdk.ev_lock);
		mem_deref(qev);
		MDT_WARN("Medut/event: dropped event type %d (queue full)\n", type);
		return false;
	}
	list_append(&g_bsdk.ev_queue, &qev->le, qev);
	g_bsdk.ev_queue_len++;
	cnd_signal(&g_bsdk.ev_cond);
	mtx_unlock(&g_bsdk.ev_lock);
	return true;
}

/* Post a copy of ev to the event queue. Safe to call from re_main. */
void bsdk_event_post(const medut_event_t *ev)
{
	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev) {
		MDT_WARN("Medut/event: dropped event type %d (OOM)\n", ev->type);
		return;
	}
	memcpy(&qev->ev, ev, sizeof(*ev));
	bsdk_event_post_qev(qev);  /* warns and frees qev on failure */
}

/* ── Engine event hooks (core/mdt_events.h; run on re_main) ────────────────── */

/**
 * True when the engine's "no common codecs" is really a media-encryption
 * mismatch.
 *
 * Only claimed when this account offers no encryption at all: that is the case
 * where the codec lists are irrelevant and the profile is certain to be the
 * problem.  The opposite direction (we require encryption, peer offers plain
 * RTP) is left with the engine's own wording — the peer's capabilities are not
 * ours to describe, and a genuine codec mismatch is still possible there.
 */
static bool codec_reason_is_really_menc(const char *reason,
                                        const struct medut_account *acct)
{
	medut_media_enc_t enc;

	if (!reason || !acct)
		return false;

	if (!strstr(reason, "common audio") && !strstr(reason, "common media"))
		return false;

	enc = acct->cfg.media_enc ? acct->cfg.media_enc : g_bsdk.cfg.media_enc;
	return enc == MEDUT_MEDIA_ENC_NONE;
}

void bsdk_ev_reg_sending(struct medut_account *acct)
{
	medut_event_t ev = {0};

	ev.type          = MEDUT_EV_REG_STATE;
	ev.u.reg.account = acct;

	if (acct && acct->reconnecting) {
		/* A REGISTER going out inside a recovery — a retry, or a
		 * handover's re-REGISTER — is still the reconnect the app was
		 * told about, not a new registration.  Reporting REGISTERING
		 * here would flip the status line between "Reconnecting…" and
		 * "Registering…" once per attempt for the whole outage. */
		ev.u.reg.state  = MEDUT_REG_RECONNECTING;
		ev.u.reg.error  = acct->reg_error;
		acct->reg_state = MEDUT_REG_RECONNECTING;
		/* The countdown event already said exactly this, so
		 * the attempt leaving the socket is not news. */
		if (!bsdk_account_reg_ev_prepare(acct, &ev))
			return;

		/* Carry the reason the recovery started with, so an app that
		 * renders only the latest event still has one.  Queued with its
		 * own copy: acct->reg_error_str is re-written by the next
		 * failure, and the consumer reads this on another thread. */
		if (acct->reg_error_str[0]) {
			struct medut_queued_event *qev = bsdk_qev_alloc();
			if (qev) {
				str_ncpy(qev->buf, acct->reg_error_str,
				         sizeof(qev->buf));
				memcpy(&qev->ev, &ev, sizeof(ev));
				qev->ev.u.reg.error_str = qev->buf;
				bsdk_event_post_qev(qev);
				return;
			}
		}
	}
	else {
		ev.u.reg.state = MEDUT_REG_REGISTERING;
		if (acct)
			acct->reg_state = MEDUT_REG_REGISTERING;
	}

	bsdk_event_post(&ev);
}

void bsdk_ev_reg_ok(struct medut_account *acct)
{
	/* Resolve the account's STUN/TURN servers now, so its calls do not
	 * wait on DNS (src/ice: the server address cache). */
	if (acct && (acct->cfg.ice_enabled || g_bsdk.cfg.ice_enabled))
		bsdk_ice_prewarm(acct);

	medut_event_t ev = {0};

	ev.type        = MEDUT_EV_REG_STATE;
	ev.u.reg.state = MEDUT_REG_REGISTERED;
	ev.u.reg.error = MEDUT_OK;
	ev.u.reg.account = acct;
	if (acct) {
		bool moved = acct->conn_lost;

		acct->reg_state    = MEDUT_REG_REGISTERED;
		acct->retry_attempt = 0;
		/* Recovered — the next failure starts a new reconnect. */
		acct->reconnecting = false;
		acct->conn_lost    = false;
		/* Back on a new connection: the calls' dialogs follow. */
		if (moved) {
			acct->reconnected_at = tmr_jiffies();
			bsdk_netmon_account_reconnected(acct);
		}
		acct->ka_failed    = false;
		acct->reg_error    = MEDUT_OK;
		acct->reg_error_str[0] = '\0';
		/* Registered and reachable — start probing so we notice if
		 * that stops being true before the next refresh. */
		bsdk_account_keepalive_arm(acct);
		/* A push token that changed mid-REGISTER goes out now. */
		bsdk_account_push_settled(acct);
		(void)bsdk_account_reg_ev_prepare(acct, &ev);
	}

	bsdk_event_post(&ev);
}

/* A TLS or WSS connection the server's certificate failed verification on.
 *
 * libre reported every handshake failure as EPROTO, so this used to arrive as
 * "Protocol error [71]" — a transport error, retried forever on a backoff no
 * retry could win.  The patched tls_connect() (cmake/patch-re-sources.cmake,
 * hunk AA) returns EKEYREJECTED for it instead and keeps OpenSSL's reason,
 * which goes into `buf` as the reason the app sees. */
static bool tls_cert_rejected(const char *reason, char *buf, size_t sz)
{
	const char *why;

	if (bsdk_reason_errno(reason) != EKEYREJECTED)
		return false;

	why = bsdk_tls_verify_reason();
	re_snprintf(buf, sz, "TLS certificate rejected: %s",
	            why ? why : "verification failed");
	return true;
}

void bsdk_ev_reg_fail(struct medut_account *acct, uint16_t code,
                      const char *reason)
{
	char cert_reason[96];
	medut_event_t ev = {0};

	ev.type            = MEDUT_EV_REG_STATE;
	ev.u.reg.account   = acct;
	/* Classify error by 3-digit SIP status code — after a rejected server
	 * certificate, which has none and which no retry can fix. */
	if (!code && tls_cert_rejected(reason, cert_reason, sizeof(cert_reason))) {
		ev.u.reg.error = MEDUT_ERR_TLS_CERT;
		reason = cert_reason;
	}
	else if (code == 401 || code == 403 || code == 407)
		ev.u.reg.error = MEDUT_ERR_AUTH;
	else if (code >= 500 && code < 600)
		ev.u.reg.error = MEDUT_ERR_SERVER_5XX;
	else
		ev.u.reg.error = MEDUT_ERR_TRANSPORT;

	/* RECONNECTING while the SDK still has a retry to spend on this —
	 * a timeout, a 5xx, a transport that died with the network.  FAILED
	 * only for the ones no retry can fix (auth) or when the budget is
	 * gone, or for a server certificate that was rejected, which will be
	 * the same certificate next time.  Decided before the event is posted
	 * so both agree.
	 *
	 * An account the app unregistered only fails its un-REGISTER (not
	 * sent, or not answered): it is unregistered either way, and the
	 * binding lapses at the registrar on its own. */
	if (acct && !acct->reg_wanted)
		ev.u.reg.state = MEDUT_REG_UNREGISTERED;
	else
		ev.u.reg.state = acct
		        ? bsdk_account_reg_fail_state(acct, ev.u.reg.error)
		        : MEDUT_REG_FAILED;

	if (acct) {
		acct->reg_state  = ev.u.reg.state;
		acct->reg_error  = ev.u.reg.error;
		/* No SIP status: the connection itself failed (reset,
		 * closed, refused), so the retry registers on a new one. */
		if (ev.u.reg.error == MEDUT_ERR_TRANSPORT && !code)
			acct->conn_lost = true;
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
		if (ev.u.reg.error != MEDUT_ERR_AUTH &&
		    ev.u.reg.error != MEDUT_ERR_TLS_CERT)
			bsdk_account_schedule_retry(acct);

		/* schedule_retry() has just reported this same reconnect,
		 * carrying the same numbers and the same reason. */
		if (!bsdk_account_reg_ev_prepare(acct, &ev))
			return;
	}

	/* Enqueue via a queued_event so the string lives long enough */
	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (qev) {
		if (reason)
			str_ncpy(qev->buf, reason, sizeof(qev->buf));
		memcpy(&qev->ev, &ev, sizeof(ev));
		qev->ev.u.reg.error_str = reason ? qev->buf : NULL;
		bsdk_event_post_qev(qev);
	}
}

void bsdk_ev_reg_unregistering(struct medut_account *acct)
{
	medut_event_t ev = {0};

	ev.type        = MEDUT_EV_REG_STATE;
	ev.u.reg.state = MEDUT_REG_UNREGISTERING;
	ev.u.reg.account = acct;
	if (acct) {
		acct->reg_state    = MEDUT_REG_UNREGISTERING;
		acct->reconnecting = false;
		(void)bsdk_account_reg_ev_prepare(acct, &ev);
	}

	bsdk_event_post(&ev);
}

void bsdk_ev_reg_unregistered(struct medut_account *acct)
{
	medut_event_t ev = {0};

	ev.type          = MEDUT_EV_REG_STATE;
	ev.u.reg.state   = MEDUT_REG_UNREGISTERED;
	ev.u.reg.error   = MEDUT_OK;
	ev.u.reg.account = acct;
	if (acct) {
		acct->reg_state    = MEDUT_REG_UNREGISTERED;
		acct->reconnecting = false;
		(void)bsdk_account_reg_ev_prepare(acct, &ev);
	}

	bsdk_event_post(&ev);
}

void bsdk_ev_call_incoming(struct medut_account *acct, struct mdt_call *ec,
                           const char *peer, const char *pname)
{
	/* An incoming call inherits no account headers: those exist to be
	 * put on requests we originate, and this dialog's requests were
	 * originated by the peer. */
	struct medut_call *new_lc =
		bsdk_call_wrap_new(ec, acct, MEDUT_CALL_RINGING, false);
	if (!new_lc)
		return;

	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev) {
		bsdk_call_unregister(new_lc);
		mem_deref(new_lc);
		return;
	}
	qev->ev.type = MEDUT_EV_INCOMING_CALL;
	qev->ev.u.incoming.account = acct;
	qev->ev.u.incoming.call    = new_lc;
	/* Pack strings into buf: from_uri\0display_name\0 */
	size_t off = 0;

	if (peer) {
		str_ncpy(qev->buf, peer, sizeof(qev->buf));
		qev->ev.u.incoming.from_uri = qev->buf;
		off += strlen(qev->buf) + 1;
	}

	/* From-header display name; NULL when the peer sent none. */
	if (pname && off < sizeof(qev->buf)) {
		str_ncpy(qev->buf + off, pname, sizeof(qev->buf) - off);
		qev->ev.u.incoming.display_name = qev->buf + off;
	}

	if (!bsdk_event_post_qev(qev)) {
		bsdk_call_unregister(new_lc);
		mem_deref(new_lc);
	}
}

/* 180, or 183 with SDP — alerting; early media may already be flowing, so
 * the consumer needs the same signal for both. */
void bsdk_ev_call_ringing(struct medut_call *lc,
                          struct medut_account *acct)
{
	medut_event_t ev = {0};

	ev.type = MEDUT_EV_CALL_STATE;
	ev.u.call_state.call    = lc;
	ev.u.call_state.account = acct;
	ev.u.call_state.state   = MEDUT_CALL_RINGING;
	if (lc) lc->state = MEDUT_CALL_RINGING;

	bsdk_event_post(&ev);
}

void bsdk_ev_call_established(struct medut_call *lc,
                              struct medut_account *acct)
{
	medut_event_t ev = {0};

	ev.type = MEDUT_EV_CALL_STATE;
	ev.u.call_state.call    = lc;
	ev.u.call_state.account = acct;
	ev.u.call_state.state   = MEDUT_CALL_ESTABLISHED;
	if (lc) {
		lc->state            = MEDUT_CALL_ESTABLISHED;
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

	bsdk_event_post(&ev);
}

void bsdk_ev_call_closed(struct medut_call *lc,
                         struct medut_account *acct,
                         const struct bsdk_close_cause *cause)
{
	medut_event_t ev = {0};
	const char *reason = cause->reason;

	ev.type = MEDUT_EV_CALL_STATE;
	ev.u.call_state.call    = lc;
	ev.u.call_state.account = acct;

	/* Classify by SIP status: 487 → CANCELLED, 4xx/5xx/6xx → FAILED, else → ENDED.
	 * OS-level transport errors arrive without a SIP code (e.g.
	 * "Connection reset by peer [104]"); the engine flags them as
	 * cause->transport_err. */
	int scode = cause->scode;
	bool is_transport_err = cause->transport_err;

	/* ...except that ECONNRESET is also how libre reports a peer-initiated
	 * termination, not only a dead socket.  Its BYE handler answers 200 OK
	 * and then calls sipsess_terminate(sess, ECONNRESET, NULL)
	 * (re/src/sipsess/listen.c), so a remote BYE arrives here as
	 * "Connection reset by peer [104]" with nothing to tell it apart from a
	 * genuine reset.  (A CANCEL comes with its request, so the call layer
	 * reports it as peer_cancel.)
	 *
	 * The call's own state tells the two apart well enough to matter: a
	 * call that was ESTABLISHED and then saw this ended normally — the far
	 * end hung up.  Reporting that as FAILED with a transport error was the
	 * visible bug: apps that branch on ENDED left the call on screen, and
	 * every normal remote hangup looked like a network fault.
	 *
	 * A real socket death on an established call is then also reported as
	 * ENDED.  That is the right trade: the call is over either way, and the
	 * transport itself is covered by REG_STATE and MEDUT_EV_NETWORK, which
	 * a plain hangup never touches.
	 *
	 * An engine that sees the BYE / CANCEL itself says so (peer_bye /
	 * peer_cancel) and needs none of this guessing. */
	bool peer_hangup = cause->peer_bye ||
	                   (is_transport_err && lc &&
	                    lc->state == MEDUT_CALL_ESTABLISHED);

	if (scode == 487 || cause->peer_cancel)
		ev.u.call_state.state = MEDUT_CALL_CANCELLED;
	else if (peer_hangup)
		ev.u.call_state.state = MEDUT_CALL_ENDED;
	else if (scode >= 400 || is_transport_err)
		ev.u.call_state.state = MEDUT_CALL_FAILED;
	else
		ev.u.call_state.state = MEDUT_CALL_ENDED;

	/* A call that never connected because the server's certificate was
	 * refused carries the same code and reason as the registration would. */
	char cert_reason[96];
	bool cert_err = !peer_hangup && is_transport_err &&
	        tls_cert_rejected(reason, cert_reason, sizeof(cert_reason));

	ev.u.call_state.error = bsdk_sip_call_error(scode);
	if (cert_err)
		ev.u.call_state.error = MEDUT_ERR_TLS_CERT;
	else if (ev.u.call_state.error == MEDUT_OK &&
	    is_transport_err && !peer_hangup && !cause->peer_cancel)
		ev.u.call_state.error = MEDUT_ERR_TRANSPORT;

	if (lc) {
		bsdk_stats_collect_final(lc);  /* must be before ec = NULL */
		lc->state = ev.u.call_state.state;
		lc->ec    = NULL; /* the engine will free the call */
		bsdk_call_unregister(lc);
	}

	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (qev) {
		memcpy(&qev->ev, &ev, sizeof(ev));
		if (peer_hangup && !(cause->peer_bye && reason)) {
			/* Don't pass libre's ECONNRESET text on as the
			 * reason for a normal hangup — it reads as a
			 * network fault to anyone logging it. */
			str_ncpy(qev->buf, "Remote hangup",
			         sizeof(qev->buf));
			qev->ev.u.call_state.reason = qev->buf;
		}
		else if (cert_err) {
			str_ncpy(qev->buf, cert_reason, sizeof(qev->buf));
			qev->ev.u.call_state.reason = qev->buf;
		}
		else if (reason && codec_reason_is_really_menc(reason, acct)) {
			/* The engine disables a stream whose media profile it
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
}

/* HELD while either side holds the call (our hold once the peer accepted
 * it), ESTABLISHED otherwise; an event only when that changes. */
void bsdk_ev_call_hold(struct medut_call *lc,
                       struct medut_account *acct, bool held, bool local)
{
	medut_event_t ev = {0};
	const medut_call_state_t st = held ? MEDUT_CALL_HELD
	                                     : MEDUT_CALL_ESTABLISHED;

	if (!lc)
		return;

	/* A refused hold is not wanted any more (medut_call_is_held()). */
	lc->local_hold = local;

	if (lc->state != MEDUT_CALL_ESTABLISHED &&
	    lc->state != MEDUT_CALL_HELD)
		return;
	if (lc->state == st)
		return;

	lc->state = st;
	if (!held)
		bsdk_netmon_call_refreshable(lc);

	ev.type = MEDUT_EV_CALL_STATE;
	ev.u.call_state.call    = lc;
	ev.u.call_state.account = acct;
	ev.u.call_state.state   = st;
	bsdk_event_post(&ev);
}

void bsdk_ev_call_dtmf(struct medut_call *lc, char digit, bool end)
{
	medut_event_t ev = {0};

	(void)end;   /* start and end are both reported, as before */
	if (!digit)
		return;

	ev.type = MEDUT_EV_CALL_DTMF;
	ev.u.dtmf.call  = lc;
	ev.u.dtmf.digit = digit;

	bsdk_event_post(&ev);
}

void bsdk_ev_call_sdp(struct medut_call *lc, bool remote, bool answer)
{
	if (!lc)
		return;

	bsdk_sdp_note(lc, remote);
	if (remote)
		bsdk_adapt_media_renegotiated(lc);
	/* The answer to a migration re-INVITE — report that the peer
	 * accepted the new address, before audio is confirmed. */
	if (remote && answer)
		bsdk_netmon_call_sdp_answer(lc);
}

/* Not posted, but netmon wants to know: a migration held back because the
 * encryption was mid-handshake can go out as soon as it finishes, instead of
 * waiting for the next verify tick.  The bounce only acts on a call that is
 * actually deferred, and send_migration() re-tests the handshake itself, so a
 * progress report that is not "secure" costs a no-op. */
void bsdk_ev_call_menc(struct medut_call *lc)
{
	if (lc)
		bsdk_netmon_call_refreshable(lc);
}

/* A refused REFER leaves the call ESTABLISHED — the engine does not close
 * it, and reporting MEDUT_CALL_FAILED here did: consumers treat that state
 * as terminal and drop their call handle, so a blind transfer to a busy or
 * unknown extension silently destroyed the live call the user was still
 * talking on.  Report the transfer outcome instead and leave lc->state
 * alone. */
void bsdk_ev_transfer_failed(struct medut_call *lc,
                             struct medut_account *acct,
                             const char *reason)
{
	medut_event_t ev = {0};

	ev.type = MEDUT_EV_TRANSFER_FAILED;
	ev.u.transfer_failed.call    = lc;
	ev.u.transfer_failed.account = acct;
	ev.u.transfer_failed.reason  = NULL;

	if (reason) {
		struct medut_queued_event *qev = bsdk_qev_alloc();
		if (qev) {
			memcpy(&qev->ev, &ev, sizeof(ev));
			str_ncpy(qev->buf, reason, sizeof(qev->buf));
			qev->ev.u.transfer_failed.reason = qev->buf;
			bsdk_event_post_qev(qev);
			return;
		}
	}

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

	return mdt_events_start();
}

void bsdk_event_close(void)
{
	/* Stop the engine's events first so no new ones arrive */
	mdt_events_stop();

	/* Signal the event thread to drain and exit.
	 *
	 * Guarded because this also runs from medut_init()'s failure path,
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
		struct medut_queued_event *qev = le->data;
		list_unlink(&qev->le);
		mem_deref(qev);
	}

	/* Keep the counter with the list.  medut_init() re-zeroes it, so this
	 * is belt-and-braces today, but a stale non-zero length here would make
	 * a re-initialised SDK think its empty queue was already full and drop
	 * every event forever. */
	g_bsdk.ev_queue_len = 0;
}
