/**
 * @file call.c  INVITE FSM, hold/resume, DTMF, blind transfer, per-dialog headers
 *
 * medut_call_t wraps a baresip struct call. Call objects are created either
 * by medut_call_invite() (outgoing) or by event.c when BEVENT_CALL_INCOMING
 * fires. The call list is guarded by s_calls_lock.
 */

#include "medut_internal.h"
#include <math.h>

static struct list s_calls;
static mtx_t       s_calls_lock;
/* Call-setup watchdog sweep timer — see the Timer B section below. */
static struct tmr  s_setup_tmr;

/* Called once from medut_init before any call API is reachable. */
void bsdk_call_global_init(void)
{
	mtx_init(&s_calls_lock, mtx_plain);
	list_init(&s_calls);
	tmr_init(&s_setup_tmr);
}

void bsdk_call_global_reset(void)
{
	mtx_lock(&s_calls_lock);
	struct le *le, *le_tmp;
	LIST_FOREACH_SAFE(&s_calls, le, le_tmp) {
		struct medut_call *lc = le->data;
		list_unlink(&lc->le);
		mem_deref(lc);
	}
	mtx_unlock(&s_calls_lock);
	mtx_destroy(&s_calls_lock);
}

/* ── Call lookup ─────────────────────────────────────────────────────────── */

struct medut_call *bsdk_call_find(const struct call *bc)
{
	struct le *le;
	mtx_lock(&s_calls_lock);
	LIST_FOREACH(&s_calls, le) {
		struct medut_call *lc = le->data;
		if (lc->bc == bc) {
			mtx_unlock(&s_calls_lock);
			return lc;
		}
	}
	mtx_unlock(&s_calls_lock);
	return NULL;
}

void bsdk_call_register(struct medut_call *lc)
{
	mtx_lock(&s_calls_lock);
	list_append(&s_calls, &lc->le, lc);
	mtx_unlock(&s_calls_lock);
}

void bsdk_call_unregister(struct medut_call *lc)
{
	mtx_lock(&s_calls_lock);
	list_unlink(&lc->le);
	mtx_unlock(&s_calls_lock);
}

void bsdk_call_foreach(void (*fn)(struct medut_call *, void *), void *arg)
{
	mtx_lock(&s_calls_lock);
	struct le *le;
	LIST_FOREACH(&s_calls, le) {
		struct medut_call *lc = le->data;
		fn(lc, arg);
	}
	mtx_unlock(&s_calls_lock);
}

/* ── Call-setup watchdog (RFC 3261 Timer B, SDK-side) ────────────────────────
 *
 * An INVITE sent onto a link that is up but not carrying traffic gets no
 * response of any kind.  libre's transaction layer bounds that at Timer B =
 * 64·T1 = 32 s, and T1 is a compile-time constant (SIP_T1) with no runtime
 * knob — so a mobile app that would rather fail in eight seconds and offer to
 * retry has nowhere to say so.  cfg.sip_timer_b_ms is that place, enforced
 * here.
 *
 * One global sweep timer rather than a struct tmr per call: a per-call timer
 * would have to be cancelled from bsdk_call_destructor(), which runs on
 * whichever thread drops the last reference, and cancelling a timer off
 * re_main is not safe.  Each call carries only a start timestamp.
 *
 * Only MEDUT_CALL_CALLING is watched.  A call that reached RINGING has had a
 * provisional response, which proves the path works; how long to let it ring
 * is the app's decision, not a transport timeout.
 */

#define BSDK_SETUP_TICK_MS  500
#define BSDK_SETUP_MAX_SNAP 64

struct setup_snap {
	struct medut_call **v;
	size_t                max;
	size_t                n;
	bool                  more;   /* a call still inside the deadline */
	uint64_t              now;
};

static void setup_visit(struct medut_call *lc, void *arg)
{
	struct setup_snap *sn = arg;
	uint32_t limit = g_bsdk.cfg.sip_timer_b_ms;

	if (!lc->setup_start)
		return;

	if (lc->state != MEDUT_CALL_CALLING) {
		lc->setup_start = 0;   /* answered, ringing or gone — done */
		return;
	}

	if (!limit) {
		lc->setup_start = 0;   /* watchdog disabled mid-flight */
		return;
	}

	if ((uint64_t)(sn->now - lc->setup_start) < (uint64_t)limit) {
		sn->more = true;
		return;
	}

	if (sn->n < sn->max)
		sn->v[sn->n++] = lc;
}

static void setup_watch_handler(void *arg)
{
	struct medut_call *snap[BSDK_SETUP_MAX_SNAP];
	struct setup_snap sn = { .v = snap, .max = RE_ARRAY_SIZE(snap),
	                         .n = 0, .more = false, .now = tmr_jiffies() };
	(void)arg;

	/* Collect under the list lock, act outside it: ua_hangup() re-enters
	 * baresip's event handler, which calls back into bsdk_call_unregister()
	 * and takes the same lock. */
	bsdk_call_foreach(setup_visit, &sn);

	for (size_t i = 0; i < sn.n; i++) {
		struct medut_call *lc = snap[i];

		if (!lc->bc || !lc->acct || !lc->acct->ua) {
			lc->setup_start = 0;
			continue;
		}

		info("Medut: call setup timed out after %u ms; cancelling\n",
		     g_bsdk.cfg.sip_timer_b_ms);

		lc->setup_start = 0;
		/* 408 rather than a local teardown: it reaches the app as
		 * MEDUT_CALL_FAILED / MEDUT_ERR_TIMEOUT through the same
		 * classification every other failure uses, and sends CANCEL so a
		 * proxy that did receive the INVITE stops forking it. */
		ua_hangup(lc->acct->ua, lc->bc, 408, "Request Timeout");
	}

	if (sn.more)
		tmr_start(&s_setup_tmr, BSDK_SETUP_TICK_MS,
		          setup_watch_handler, NULL);
}

void bsdk_call_setup_watch_start(struct medut_call *lc)
{
	if (!lc || !g_bsdk.cfg.sip_timer_b_ms)
		return;

	lc->setup_start = tmr_jiffies();

	/* tmr_start on an already-running timer re-arms it, which would push
	 * the deadline of a call that is already being watched out by a tick
	 * on every new dial. */
	if (!tmr_isrunning(&s_setup_tmr))
		tmr_start(&s_setup_tmr, BSDK_SETUP_TICK_MS,
		          setup_watch_handler, NULL);
}

void bsdk_call_setup_watch_cancel(struct medut_call *lc)
{
	if (lc)
		lc->setup_start = 0;
}

void bsdk_call_setup_watch_close(void)
{
	tmr_cancel(&s_setup_tmr);
}

/* ── call_destructor ─────────────────────────────────────────────────────── */

static void custom_hdr_destructor(void *data)
{
	struct bsdk_custom_hdr *hdr = data;
	mem_deref(hdr->name);
	mem_deref(hdr->value);
}

void bsdk_call_destructor(void *data)
{
	struct medut_call *lc = data;
	struct le *le, *le_tmp;
	LIST_FOREACH_SAFE(&lc->custom_hdrs, le, le_tmp) {
		struct bsdk_custom_hdr *hdr = le->data;
		list_unlink(&hdr->le);
		mem_deref(hdr);
	}
	mtx_destroy(&lc->tap_lock);
	/* Close any open recording files (if record_stop was never called) */
	mtx_lock(&lc->rec_lock);
	lc->rec_active = false;
	if (lc->rec_file) { fclose(lc->rec_file); lc->rec_file = NULL; }
	mtx_unlock(&lc->rec_lock);
	mtx_destroy(&lc->rec_lock);
}

/**
 * Wrap a baresip call in a medut_call and register it.
 *
 * Three paths produce a call the app must be able to hold a handle to:
 * medut_call_invite() (outgoing), the BEVENT_CALL_INCOMING branch of
 * event.c, and medut_call_transfer_accept() (following a REFER).  They all
 * need the same construction — both mutexes, the custom-header list, the NaN
 * audio-level seeding — and getting any of it wrong is quiet: event.c tolerates
 * a NULL wrapper everywhere, so a call with none simply emits state events
 * carrying a NULL handle and the app never sees it.
 *
 * @param bc     baresip call to wrap (borrowed; the wrapper holds a weak ref).
 * @param acct   Owning account.
 * @param state  Initial state — CALLING for outgoing, RINGING for incoming.
 * @param inherit_hdrs  Copy the account's custom headers onto the call, which
 *                      an outgoing INVITE wants and an incoming call does not.
 *
 * @return The registered wrapper, or NULL on allocation failure.
 */
struct medut_call *bsdk_call_wrap_new(struct call *bc,
					struct medut_account *acct,
					medut_call_state_t state,
					bool inherit_hdrs)
{
	struct medut_call *lc = mem_zalloc(sizeof(*lc), bsdk_call_destructor);
	if (!lc)
		return NULL;

	mtx_init(&lc->tap_lock, mtx_plain);
	mtx_init(&lc->rec_lock, mtx_plain);
	list_init(&lc->custom_hdrs);
	lc->bc    = bc;
	lc->acct  = acct;
	lc->state = state;

	/* NaN, not -127: the header documents NaN for "unavailable" and -127 for
	 * "silent", and tap_compute_dbov() genuinely returns -127 for all-zero
	 * PCM.  Seeding -127 collapsed the two, so a microphone delivering
	 * digital silence and a level that was never measured at all read back
	 * identically — which is precisely the distinction you need when audio
	 * is missing and you are trying to work out whether the capture path is
	 * dead or merely quiet. */
	uint32_t _sil_bits; float _sil = NAN; memcpy(&_sil_bits, &_sil, 4);
	re_atomic_rlx_set(&lc->rx_level_bits, _sil_bits);
	re_atomic_rlx_set(&lc->tx_level_bits, _sil_bits);

	if (inherit_hdrs && acct) {
		struct le *le;
		LIST_FOREACH(&acct->custom_hdrs, le) {
			struct bsdk_custom_hdr *acct_hdr = le->data;
			struct bsdk_custom_hdr *ch =
				mem_zalloc(sizeof(*ch), custom_hdr_destructor);
			if (!ch) continue;
			ch->name  = bsdk_strdup(acct_hdr->name);
			ch->value = bsdk_strdup(acct_hdr->value);
			if (!ch->name || !ch->value) {
				mem_deref(ch);
				continue;
			}
			list_append(&lc->custom_hdrs, &ch->le, ch);
		}
	}

	bsdk_call_register(lc);
	return lc;
}

/* ── medut_call_invite ─────────────────────────────────────────────────── */

typedef struct {
	struct medut_account   *acct;
	char                      uri[512];
	medut_call_handle_t    *out;
	int                       result;
} invite_ctx_t;

static void invite_fn(void *arg)
{
	invite_ctx_t *ctx = arg;
	struct call *bc = NULL;
	char *ruri = NULL;
	const char *dial = ctx->uri;
	struct pl pl;

	/* Refuse to dial when the transport never connected — ua_connect would
	 * crash inside baresip trying to use a dead socket.
	 *
	 * That state used to be reported as FAILED and is now RECONNECTING while
	 * a retry is armed, so the guard has to cover both — but only when
	 * baresip has no successful registration behind it either.  Otherwise
	 * this would newly refuse to dial through the seconds of a network
	 * handover, whose transports have just been re-bound and are usable. */
	if (ctx->acct->reg_state == MEDUT_REG_FAILED ||
	    (ctx->acct->reg_state == MEDUT_REG_RECONNECTING &&
	     !ua_isregistered(ctx->acct->ua))) {
		ctx->result = ENOTCONN;
		return;
	}

	/* Complete the dial string into a full SIP URI.
	 *
	 * ua_connect() sends the request URI verbatim — it only appends the
	 * account's URI parameters — so a bare extension has to be turned into
	 * user@domain here. Sending "INVITE sip:*43 SIP/2.0" (no host) is not a
	 * valid request line: a strict registrar does not answer 4xx, it drops
	 * the whole packet at the parser ("PJSIP syntax error ... 'Request
	 * Line' ... col 12" on Asterisk), so the call just hangs with no
	 * response and no media.
	 *
	 * account_uri_complete_strdup() adds the scheme, leaves IP literals and
	 * anything already carrying user@host alone, and appends the account
	 * domain (and port, when not 5060) otherwise. It skips completion
	 * entirely for a string that already has a scheme, so drop a leading
	 * "sip:" when there is no host yet — "sips:" is left alone rather than
	 * silently downgraded to "sip:". */
	if (!strchr(dial, '@') && strncasecmp(dial, "sip:", 4) == 0)
		dial += 4;

	pl_set_str(&pl, dial);
	ctx->result = account_uri_complete_strdup(ua_account(ctx->acct->ua),
	                                          &ruri, &pl);
	if (ctx->result)
		return;

	ctx->result = ua_connect(ctx->acct->ua, &bc, NULL,
	                         ruri, VIDMODE_OFF);
	if (ctx->result == EAFNOSUPPORT) {
		/* ICE STUN/TURN lookup failed because this host has no IPv6.
		 * Disable ICE and retry with direct RTP.
		 *
		 * Put it back afterwards.  The account outlives the call, and
		 * leaving the media-NAT cleared meant one dial on an IPv4-only
		 * network silently downgraded every later call on that account
		 * to no-ICE — including calls placed after the device moved to a
		 * network where ICE was both available and needed. */
		struct account *ba = ua_account(ctx->acct->ua);
		const char *mnatid = account_medianat(ba);
		char keep[32] = "";

		if (mnatid)
			str_ncpy(keep, mnatid, sizeof(keep));

		account_set_medianat(ba, NULL);
		ctx->result = ua_connect(ctx->acct->ua, &bc, NULL,
		                         ruri, VIDMODE_OFF);
		if (keep[0])
			account_set_medianat(ba, keep);
	}
	mem_deref(ruri);
	if (ctx->result)
		return;
	if (!bc) {
		/* ua_connect reported success without producing a call. Nothing
		 * downstream can work with that, and returning 0 would hand the
		 * caller an untouched *out — a stale or uninitialised handle for
		 * every binding that does not pre-zero it. */
		ctx->result = EINVAL;
		return;
	}

	struct medut_call *lc = bsdk_call_wrap_new(bc, ctx->acct,
						     MEDUT_CALL_CALLING, true);
	if (!lc) {
		ua_hangup(ctx->acct->ua, bc, 500, "Out of Memory");
		ctx->result = ENOMEM;
		return;
	}
	/* The SDP offer was created inside ua_connect(), before this wrapper
	 * existed — the BEVENT_CALL_LOCAL_SDP that fired then could not be
	 * matched by bsdk_sdp_handle_event().  Record it here so the
	 * SDP_NEGOTIATION event fires when the remote answer arrives. */
	lc->local_sdp_set = true;

	bsdk_call_setup_watch_start(lc);
	*ctx->out = lc;
}

int medut_call_invite(medut_account_handle_t acct,
                         const char *uri,
                         medut_call_handle_t *out)
{
	if (!acct || !uri || !out)
		return MEDUT_ERR_INVAL;

	/* Store the dial string as given. Completing it needs the account's
	 * domain, which may only be read on re_main — invite_fn does it. */
	invite_ctx_t ctx = {.acct = acct, .out = out, .result = 0};
	str_ncpy(ctx.uri, uri, sizeof(ctx.uri));

	int err = bsdk_dispatch_sync(invite_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── medut_call_answer ─────────────────────────────────────────────────── */

typedef struct { struct medut_call *lc; int result; } simple_call_ctx_t;

static void answer_fn(void *arg)
{
	simple_call_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;
	if (!lc->bc) { ctx->result = ENOENT; return; }
	ctx->result = ua_answer(lc->acct->ua, lc->bc, VIDMODE_OFF);
}

int medut_call_answer(medut_call_handle_t call)
{
	if (!call) return MEDUT_ERR_INVAL;
	simple_call_ctx_t ctx = {.lc = call, .result = 0};
	int err = bsdk_dispatch_sync(answer_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── medut_call_hangup ─────────────────────────────────────────────────── */

static void hangup_fn(void *arg)
{
	struct medut_call *lc = arg;
	if (lc->bc) {
		ua_hangup(lc->acct->ua, lc->bc, 0, NULL);
		lc->state = MEDUT_CALL_ENDED;
	}
	mem_deref(lc);
}

int medut_call_hangup(medut_call_handle_t call)
{
	if (!call) return MEDUT_ERR_INVAL;
	/* Fire-and-forget, so the queued work holds its own reference.  A
	 * remote BYE can end the call on re_main while hangup_fn is still
	 * queued; the terminal CALL_STATE event then owns the wrapper, and a
	 * consumer that releases that event frees it before hangup_fn runs. */
	mem_ref(call);
	int err = bsdk_dispatch(hangup_fn, call);
	if (err)
		mem_deref(call);
	return err;
}

/* ── medut_call_reject ─────────────────────────────────────────────────── */

typedef struct {
	struct medut_call *lc;
	uint16_t             scode;
	char                 reason[128];
} reject_ctx_t;

static void reject_ctx_destructor(void *arg)
{
	reject_ctx_t *ctx = arg;
	mem_deref(ctx->lc);
}

static void reject_fn(void *arg)
{
	reject_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;
	if (lc->bc)
		ua_hangup(lc->acct->ua, lc->bc, ctx->scode,
		          ctx->reason[0] ? ctx->reason : NULL);
	lc->state = MEDUT_CALL_ENDED;
	mem_deref(ctx);
}

int medut_call_reject(medut_call_handle_t call,
                         uint16_t scode, const char *reason)
{
	if (!call) return MEDUT_ERR_INVAL;

	reject_ctx_t *ctx = mem_alloc(sizeof(*ctx), reject_ctx_destructor);
	if (!ctx) return MEDUT_ERR_NOMEM;
	memset(ctx, 0, sizeof(*ctx));
	/* Same reason as medut_call_hangup(): the queued work keeps the
	 * wrapper alive until it has run.  Released by the ctx destructor. */
	ctx->lc    = mem_ref(call);
	ctx->scode = scode;
	if (reason)
		str_ncpy(ctx->reason, reason, sizeof(ctx->reason));

	int err = bsdk_dispatch(reject_fn, ctx);
	if (err)
		mem_deref(ctx);
	return err;
}


/* ── medut_call_get_info ───────────────────────────────────────────────── */

/* baresip's transport enum → ours.  Defaults to UDP for SIP_TRANSP_NONE, which
 * is what an un-negotiated dialog reports. */
static medut_transport_t transp_from_baresip(enum sip_transp tp)
{
	switch (tp) {
	case SIP_TRANSP_TCP: return MEDUT_TRANSPORT_TCP;
	case SIP_TRANSP_TLS: return MEDUT_TRANSPORT_TLS;
	case SIP_TRANSP_WS:  return MEDUT_TRANSPORT_WS;
	case SIP_TRANSP_WSS: return MEDUT_TRANSPORT_WSS;
	default:             return MEDUT_TRANSPORT_UDP;
	}
}

typedef struct {
	struct medut_call *lc;
	medut_call_info_t *out;
} call_info_ctx_t;

static void call_info_fn(void *arg)
{
	call_info_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;
	medut_call_info_t *o  = ctx->out;
	const char *sv;

	/* Read from the wrapper first: these survive the baresip call being
	 * torn down, so an app that asks after CALL_ENDED still gets an answer
	 * rather than a struct of zeroes. */
	o->state = lc->state;

	if (lc->stats_call_start)
		o->duration_ms = tmr_jiffies() - lc->stats_call_start;

	if (!lc->bc)
		return;

	sv = call_peeruri(lc->bc);
	if (sv) str_ncpy(o->peer_uri, sv, sizeof(o->peer_uri));

	sv = call_peername(lc->bc);
	if (sv) str_ncpy(o->peer_display_name, sv, sizeof(o->peer_display_name));

	sv = call_localuri(lc->bc);
	if (sv) str_ncpy(o->local_uri, sv, sizeof(o->local_uri));

	sv = call_contacturi(lc->bc);
	if (sv) str_ncpy(o->contact_uri, sv, sizeof(o->contact_uri));

	sv = call_id(lc->bc);
	if (sv) str_ncpy(o->call_id, sv, sizeof(o->call_id));

	sv = call_diverteruri(lc->bc);
	if (sv) str_ncpy(o->diverter_uri, sv, sizeof(o->diverter_uri));

	o->is_outgoing       = call_is_outgoing(lc->bc);
	o->is_remote_hold    = call_is_onhold(lc->bc);
	o->sip_status        = call_scode(lc->bc);
	o->setup_duration_ms = call_setup_duration(lc->bc) * 1000u;
	o->line_number       = call_linenum(lc->bc);
	o->transport         = transp_from_baresip(call_transp(lc->bc));
}

int medut_call_get_info(medut_call_handle_t call, medut_call_info_t *out)
{
	if (!call || !out)
		return MEDUT_ERR_INVAL;

	memset(out, 0, sizeof(*out));

	call_info_ctx_t ctx = { .lc = call, .out = out };
	return bsdk_dispatch_sync(call_info_fn, &ctx);
}

/* ── medut_call_hold ───────────────────────────────────────────────────── */

typedef struct { struct medut_call *lc; bool hold; int result; } hold_ctx_t;

static void hold_fn(void *arg)
{
	hold_ctx_t *ctx = arg;
	if (!ctx->lc->bc) { ctx->result = ENOENT; return; }
	ctx->result = call_hold(ctx->lc->bc, ctx->hold);
	if (!ctx->result)
		ctx->lc->local_hold = ctx->hold;
}

int medut_call_hold(medut_call_handle_t call)
{
	if (!call) return MEDUT_ERR_INVAL;
	hold_ctx_t ctx = {.lc = call, .hold = true, .result = 0};
	int err = bsdk_dispatch_sync(hold_fn, &ctx);
	return err ? err : ctx.result;
}

int medut_call_resume(medut_call_handle_t call)
{
	if (!call) return MEDUT_ERR_INVAL;
	hold_ctx_t ctx = {.lc = call, .hold = false, .result = 0};
	int err = bsdk_dispatch_sync(hold_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── medut_call_is_held ────────────────────────────────────────────────── */

bool medut_call_is_held(medut_call_handle_t call)
{
	if (!call) return false;
	struct medut_call *lc = call;
	/* local_hold: our own call_hold(); state HELD: peer put us on hold. */
	return lc->local_hold || lc->state == MEDUT_CALL_HELD;
}

/* ── medut_call_send_dtmf ──────────────────────────────────────────────── */

typedef struct { struct medut_call *lc; char digit; int result; } dtmf_ctx_t;

static void dtmf_fn(void *arg)
{
	dtmf_ctx_t *ctx = arg;
	if (!ctx->lc->bc) { ctx->result = ENOENT; return; }
	ctx->result = call_send_digit(ctx->lc->bc, ctx->digit);
}

int medut_call_send_dtmf(medut_call_handle_t call, char digit)
{
	if (!call) return MEDUT_ERR_INVAL;
	dtmf_ctx_t ctx = {.lc = call, .digit = digit, .result = 0};
	int err = bsdk_dispatch_sync(dtmf_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── medut_call_transfer ───────────────────────────────────────────────── */

typedef struct { struct medut_call *lc; char uri[512]; int result; } xfer_ctx_t;

static void transfer_fn(void *arg)
{
	xfer_ctx_t *ctx = arg;
	if (!ctx->lc->bc) { ctx->result = ENOENT; return; }
	ctx->result = call_transfer(ctx->lc->bc, ctx->uri);
}

int medut_call_transfer(medut_call_handle_t call, const char *uri)
{
	if (!call || !uri) return MEDUT_ERR_INVAL;
	xfer_ctx_t ctx = {.lc = call, .result = 0};
	str_ncpy(ctx.uri, uri, sizeof(ctx.uri));
	int err = bsdk_dispatch_sync(transfer_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── medut_call_foreach ────────────────────────────────────────────────── */

typedef struct {
	medut_call_iter_fn fn;
	void                *arg;
} foreach_ctx_t;

static void public_foreach_cb(struct medut_call *lc, void *arg)
{
	foreach_ctx_t *ctx = arg;
	ctx->fn((medut_call_handle_t)lc, ctx->arg);
}

void medut_call_foreach(medut_call_iter_fn fn, void *arg)
{
	if (!fn) return;
	foreach_ctx_t ctx = { .fn = fn, .arg = arg };
	bsdk_call_foreach(public_foreach_cb, &ctx);
}

medut_account_handle_t medut_call_get_account(medut_call_handle_t call)
{
	if (!call) return NULL;
	return (medut_account_handle_t)call->acct;
}

medut_call_state_t medut_call_get_state(medut_call_handle_t call)
{
	if (!call) return MEDUT_CALL_ENDED;
	/* Local hold is tracked separately (medut_call_is_held); a call we
	 * put on hold ourselves still reads as ESTABLISHED here. */
	return call->state;
}

medut_ice_state_t medut_call_ice_state(medut_call_handle_t call)
{
	if (!call) return MEDUT_ICE_NEW;
	/* Written on re_main by the ICE module on every transition. */
	return (medut_ice_state_t)atomic_load(&call->ice_state);
}

/* ── Per-dialog custom headers ─────────────────────────────────────────── */

typedef struct {
	struct medut_call *lc;
	const char          *name;
	const char          *value;
	int                  result;
} call_hdr_ctx_t;

static void add_call_hdr_fn(void *arg)
{
	call_hdr_ctx_t *ctx = arg;
	if (!ctx->lc->bc) { ctx->result = ENOENT; return; }

	struct bsdk_custom_hdr *ch = mem_zalloc(sizeof(*ch),
	                                        custom_hdr_destructor);
	if (!ch) { ctx->result = ENOMEM; return; }
	ch->name  = bsdk_strdup(ctx->name);
	ch->value = bsdk_strdup(ctx->value);
	if (!ch->name || !ch->value) {
		mem_deref(ch);
		ctx->result = ENOMEM;
		return;
	}

	list_append(&ctx->lc->custom_hdrs, &ch->le, ch);
}

int medut_call_add_header(medut_call_handle_t call,
                             const char *name, const char *value)
{
	if (!call || !name || !value) return MEDUT_ERR_INVAL;
	call_hdr_ctx_t ctx = {.lc = call, .name = name,
	                       .value = value, .result = 0};
	int err = bsdk_dispatch_sync(add_call_hdr_fn, &ctx);
	return err ? err : ctx.result;
}
