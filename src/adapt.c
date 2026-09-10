/**
 * @file adapt.c  Degraded-link adaptation
 *
 * Handover (netmon.c) answers the question "my address changed, where did my
 * media go?".  This file answers the other one: the address is fine, the
 * registration is fine, the call is up — and the link underneath it has gone
 * bad.  A phone at one bar, a saturated uplink, a cell that stops forwarding
 * packets without ever tearing down the PDP context.  SIP notices none of
 * that: the dialog is healthy, the transport is "connected", and the user
 * hears nothing.
 *
 * Two independent mechanisms, both driven from the stats tick in stats.c so
 * they cost nothing beyond the RTCP polling the app already asked for:
 *
 *   1. Media-stall detection.  Watch the inbound RTP packet counter.  When it
 *      stops advancing for cfg.media_stall_ms, raise a non-fatal
 *      ECHOSDK_QUALITY_MEDIA_STALL alert; raise it again with `recovering`
 *      when packets resume.  This is deliberately *not* a call teardown —
 *      cfg.rtp_timeout_s (baresip's avt.rtp_timeout) is there for apps that
 *      want the fatal version, and it defaults to off because ending a call
 *      is the app's decision.
 *
 *   2. Adaptive bitrate.  Read the loss the *peer* reports in its RTCP
 *      receiver report and walk the encoder's bitrate down when it climbs,
 *      back up when it clears.  Halve on the way down, +25% on the way up,
 *      with a dead band between the two thresholds so a link hovering near
 *      one of them does not oscillate, and a run of clean ticks required
 *      before any increase, because recovering too eagerly is what turns a
 *      marginal link into an oscillating one.
 *
 * How the bitrate is applied matters.  baresip exposes audio_set_bitrate(),
 * which looks like the obvious call and is a trap: it invokes the codec's
 * encoder-update handler with a NULL fmtp, so Opus re-derives every other
 * encoder setting from defaults and silently drops the negotiated
 * useinbandfec, usedtx, cbr and stereo flags — turning FEC *off* at the exact
 * moment the link needs it.  Instead we do what baresip's own audio_update()
 * does — audio_encoder_set() with the negotiated remote fmtp — having first
 * rewritten `maxaveragebitrate` inside that fmtp.  Every other negotiated
 * parameter survives untouched, and because the codec is unchanged
 * audio_encoder_set() skips its teardown path: no re-INVITE, no
 * renegotiation, no gap in the audio.
 *
 * Fixed-rate codecs (G.711) have no encoder-update handler and are left
 * alone; there is no bitrate to vary.  For those the concealment path is the
 * `plc` module, loaded in modules_init.c.
 *
 * All functions here run on the re_main thread.
 */

#include "echosdk_internal.h"

/* Opus refuses a maxaveragebitrate outside this range (modules/opus/sdp.c),
 * so a target outside it would be silently ignored rather than clamped. */
#define BSDK_ADAPT_BR_MIN   6000u
#define BSDK_ADAPT_BR_MAX 510000u

/* ── Config accessors (0 in the config means "use the documented default") ── */

static uint32_t adapt_min_br(void)
{
	uint32_t v = g_bsdk.cfg.adapt_min_bitrate ? g_bsdk.cfg.adapt_min_bitrate
	                                          : 12000u;
	return v < BSDK_ADAPT_BR_MIN ? BSDK_ADAPT_BR_MIN : v;
}

static uint32_t adapt_max_br(void)
{
	uint32_t v = g_bsdk.cfg.adapt_max_bitrate ? g_bsdk.cfg.adapt_max_bitrate
	                                          : 32000u;
	if (v > BSDK_ADAPT_BR_MAX)
		v = BSDK_ADAPT_BR_MAX;
	/* A max below min would make every step a no-op in one direction. */
	return v < adapt_min_br() ? adapt_min_br() : v;
}

static float adapt_down_pct(void)
{
	return g_bsdk.cfg.adapt_loss_down_pct > 0.f
	     ? g_bsdk.cfg.adapt_loss_down_pct : 5.0f;
}

static float adapt_up_pct(void)
{
	float up = g_bsdk.cfg.adapt_loss_up_pct > 0.f
	         ? g_bsdk.cfg.adapt_loss_up_pct : 1.0f;
	/* Keep the dead band non-empty; equal thresholds would let a single
	 * loss reading both trigger a step down and count as a clean tick. */
	return up >= adapt_down_pct() ? adapt_down_pct() * 0.5f : up;
}

static uint32_t adapt_recover_ticks(void)
{
	return g_bsdk.cfg.adapt_recover_ticks ? g_bsdk.cfg.adapt_recover_ticks
	                                      : 5u;
}

/* ── fmtp rewriting ──────────────────────────────────────────────────────── */

/**
 * Copy `src` (an SDP fmtp parameter string) into `buf`, dropping any
 * maxaveragebitrate parameter, then append `bitrate` as a new one.
 *
 * Exposed (not static) only so test/unit/test_fmtp_bitrate.c can exercise it
 * directly — every caller is in this file.
 *
 * Parameters are `;`-separated key=value pairs.  A leading or doubled `;` is
 * legal in what libre hands us, so empty tokens are skipped rather than
 * copied — fmt_param_get() tolerates them, but emitting them back would make
 * the string grow every time we adapt.
 *
 * @param bitrate  bps to advertise, or 0 to only strip.
 * @return 0 on success, EINVAL on overflow (caller then leaves the rate be).
 */
int bsdk_adapt_fmtp_set_bitrate(char *buf, size_t sz, const char *src,
                                uint32_t bitrate)
{
	static const char key[] = "maxaveragebitrate";
	size_t pos = 0;
	const char *p = src;

	if (!buf || sz == 0)
		return EINVAL;
	buf[0] = '\0';

	while (p && *p) {
		const char *end = strchr(p, ';');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		/* Skip empty tokens and the parameter we are replacing.  The
		 * comparison is on the key only, so "maxaveragebitrate=24000"
		 * and a bare "maxaveragebitrate" are both dropped. */
		if (len > 0 &&
		    !(len >= sizeof(key) - 1 &&
		      !strncasecmp(p, key, sizeof(key) - 1) &&
		      (len == sizeof(key) - 1 || p[sizeof(key) - 1] == '='))) {

			if (pos + len + 2 > sz)
				return EINVAL;
			if (pos)
				buf[pos++] = ';';
			memcpy(buf + pos, p, len);
			pos += len;
			buf[pos] = '\0';
		}

		p = end ? end + 1 : NULL;
	}

	if (bitrate) {
		int n = re_snprintf(buf + pos, sz - pos, "%s%s=%u",
		                    pos ? ";" : "", key, bitrate);
		if (n < 0)
			return EINVAL;
	}

	return 0;
}

/* ── Bitrate application ─────────────────────────────────────────────────── */

/**
 * Re-run the encoder update for this call with `bitrate` substituted into the
 * negotiated fmtp.  `bitrate` of 0 restores the negotiated rate.
 *
 * Mirrors baresip's audio_update(): same aucodec, same payload type, same
 * remote parameters — only maxaveragebitrate differs.  Returns ENOTSUP when
 * the negotiated codec has no encoder-update handler, which is how a G.711
 * call reports "nothing to adapt".
 */
int bsdk_adapt_apply_bitrate(struct echosdk_call *lc, uint32_t bitrate)
{
	struct audio            *au;
	struct stream           *strm;
	const struct sdp_format *sc;
	const struct aucodec    *ac;
	char                     fmtp[512];
	int                      err;

	if (!lc || !lc->bc)
		return EINVAL;

	au = call_audio(lc->bc);
	strm = au ? audio_strm(au) : NULL;
	if (!strm)
		return ENOENT;

	/* sc->data is the negotiated aucodec; sc->params the remote fmtp.  Both
	 * are NULL until SDP has been answered, which is why adaptation only
	 * runs on established calls. */
	sc = sdp_media_rformat(stream_sdpmedia(strm), NULL);
	if (!sc || !sc->data)
		return ENOENT;

	ac = sc->data;
	if (!ac->encupdh)
		return ENOTSUP;   /* fixed-rate codec — nothing to vary */

	err = bsdk_adapt_fmtp_set_bitrate(fmtp, sizeof(fmtp), sc->params,
	                                  bitrate);
	if (err)
		return err;

	err = audio_encoder_set(au, ac, sc->pt, fmtp);
	if (err)
		return err;

	lc->adapt_bitrate = bitrate;
	return 0;
}

/* ── Media-stall detection ───────────────────────────────────────────────── */

/* How long media setup gets before its silence counts as a stall.
 *
 * dtls_srtp's own restart ladder is 1200 ms plus four 2500 ms attempts
 * (cmake/patches/dtls_srtp-state.new), so a handshake that is going to
 * complete at all can still be trying 11.2 s in.  This spans it, and matches
 * the deadline ice_shim.c holds its candidate re-offer for. */
#define BSDK_MEDIA_SETUP_GRACE_MS 12000

/**
 * Is this call's media encryption still being set up?
 *
 * There is no inbound RTP during a DTLS handshake and there is not meant to
 * be: dtls_srtp sets `wait_secure`, so stream_is_ready() is false and baresip
 * has not opened the audio device yet.  Calling that a stall starts a repair
 * (netmon.c) whose ICE restart re-INVITEs the call in the middle of the very
 * handshake it is waiting for — which ice_shim.c measured across 22 calls as
 * the single strongest predictor of a permanently silent call, and which on
 * 2026-09-02 turned a 4 s handshake on cellular into five re-INVITEs and a
 * dead call (outbound to *43 over WSS).
 *
 * The account's media_enc answers "is there a handshake at all" the same way
 * sdp.c does, and stream_is_secure() answers "has it finished".  A call
 * configured for DTLS whose peer answered plain RTP never reads as secure and
 * so holds for the full grace once — bounded, once per call, and the alert
 * still follows.
 */
static bool media_setup_pending(const struct echosdk_call *lc)
{
	struct list *streaml;
	struct le *le;
	echosdk_media_enc_t enc;

	if (!lc->bc)
		return false;

	enc = lc->acct ? lc->acct->cfg.media_enc : g_bsdk.cfg.media_enc;
	if (enc != ECHOSDK_MEDIA_ENC_DTLS_SRTP)
		return false;

	streaml = call_streaml(lc->bc);
	if (!streaml)
		return false;

	LIST_FOREACH(streaml, le) {
		const struct stream *strm = le->data;

		if (!stream_is_secure(strm))
			return true;
	}

	return false;
}

static void stall_clear(struct echosdk_call *lc, uint32_t elapsed_ms)
{
	if (!lc->stall_active)
		return;

	lc->stall_active = false;
	bsdk_post_quality_alert(lc, ECHOSDK_QUALITY_MEDIA_STALL,
	                        (float)elapsed_ms,
	                        (float)g_bsdk.cfg.media_stall_ms, true);
}

static void stall_tick(struct echosdk_call *lc,
                        const echosdk_ev_media_stats_t *s)
{
	uint64_t now = tmr_jiffies();
	uint32_t elapsed;
	bool pending;

	if (!g_bsdk.cfg.media_stall_ms)
		return;

	/* A held call carries no RTP by design, and a call mid-handover has
	 * netmon.c reporting the same outage in richer terms (CALL_MIGRATING /
	 * MIGRATED / MIGRATION_FAILED).  Reporting a stall in either case would
	 * be a second, less informative voice on the same event — so treat both
	 * as a fresh start rather than a stall. */
	if (lc->state == ECHOSDK_CALL_HELD || lc->local_hold ||
	    call_is_onhold(lc->bc) ||
	    lc->net_mig_state == BSDK_MIG_STALLED ||
	    lc->net_mig_state == BSDK_MIG_WAIT_ADDR ||
	    lc->net_mig_state == BSDK_MIG_DEFERRED ||
	    lc->net_mig_state == BSDK_MIG_SENT) {

		stall_clear(lc, 0);
		lc->stall_rx_packets = s->packets_received;
		lc->stall_since      = now;
		return;
	}

	if (s->packets_received != lc->stall_rx_packets) {
		lc->stall_rx_packets = s->packets_received;
		lc->stall_since      = now;
		stall_clear(lc, 0);
		return;
	}

	/* Media that has only just become able to carry RTP has not been silent
	 * for any of the time before that, so restart the clock on the edge.
	 *
	 * stall_since is stamped at CALL_ESTABLISHED, which on an ICE +
	 * DTLS-SRTP call is well before the first packet *could* arrive: the
	 * connectivity checks and then the handshake have to finish first.  The
	 * grace below covers that window while it lasts, but it is conditioned
	 * on the handshake still being unfinished — so the moment DTLS
	 * completed, the guard fell away and left the whole setup time sitting
	 * on the clock.  A call whose handshake took longer than
	 * cfg.media_stall_ms was therefore reported as stalled on the very tick
	 * its media came up, and netmon repaired a call that had never had a
	 * chance to receive anything.
	 *
	 * Measured on-device (2026-09-10, inbound over WSS): established
	 * 15:21:53.203, DTLS complete 15:21:54.288, alert at 15:21:58.055
	 * reporting 4854 ms — of which only 3765 ms were after media was ready,
	 * i.e. below the threshold.  The ICE restart that followed took the
	 * call from "slow to start" to permanently silent.
	 *
	 * A renegotiation that puts the encryption back into handshake (an ICE
	 * restart, a re-INVITE that re-keys) raises the flag again, so the same
	 * edge covers the second and later handshakes on one call. */
	pending = media_setup_pending(lc);

	if (lc->stall_setup_pending && !pending) {
		lc->stall_setup_pending = false;
		lc->stall_rx_packets    = s->packets_received;
		lc->stall_since         = now;
		stall_clear(lc, 0);
		return;
	}

	lc->stall_setup_pending = pending;

	/* stall_since is stamped when the media path becomes able to carry RTP,
	 * so a call that never receives a single packet — one-way audio from the
	 * start, the classic symptom of a NAT that never opened — is reported
	 * too. */
	elapsed = (uint32_t)(now - lc->stall_since);

	if (!lc->stall_active && elapsed >= g_bsdk.cfg.media_stall_ms) {

		/* Media that has not started yet has not stalled.  Give the
		 * handshake its ladder before calling this a fault — the alert
		 * and the repair both still follow if it never lands, they just
		 * stop arriving while it is still plausibly in flight. */
		if (elapsed < BSDK_MEDIA_SETUP_GRACE_MS && pending)
			return;

		lc->stall_active = true;
		bsdk_post_quality_alert(lc, ECHOSDK_QUALITY_MEDIA_STALL,
		                        (float)elapsed,
		                        (float)g_bsdk.cfg.media_stall_ms, false);

		/* Reporting it is not the same as doing something about it.  The
		 * repair is netmon's — the route may have moved under the call
		 * (a NAT rebinding, a multi-WAN router changing which egress
		 * address our flow uses), none of which raises a local address
		 * change for netmon to notice on its own.  It is bounded and
		 * rate-limited there, and it declines calls it cannot help. */
		bsdk_netmon_call_stalled(lc);
	}
}

/* ── Bitrate adaptation ──────────────────────────────────────────────────── */

static void bitrate_tick(struct echosdk_call *lc,
                          const echosdk_ev_media_stats_t *s)
{
	uint32_t min_br, max_br, cur, next;

	if (!g_bsdk.cfg.adaptive_bitrate)
		return;

	/* loss_pct is derived from the peer's receiver report, which baresip
	 * only has once RTCP has flowed; before then it reads 0 and would look
	 * like a perfectly clean link.  Wait for the report rather than
	 * ramping up on no evidence. */
	if (!s->packets_sent || s->rtt_ms <= 0.f)
		return;

	min_br = adapt_min_br();
	max_br = adapt_max_br();

	/* An unadapted call is at its negotiated rate, which we treat as the
	 * ceiling: the first step down must actually lower it. */
	cur = lc->adapt_bitrate ? lc->adapt_bitrate : max_br;

	if (s->loss_pct > adapt_down_pct()) {
		lc->adapt_clean_ticks = 0;
		next = cur / 2;
		if (next < min_br)
			next = min_br;
	}
	else if (s->loss_pct <= adapt_up_pct()) {
		if (++lc->adapt_clean_ticks < adapt_recover_ticks())
			return;
		lc->adapt_clean_ticks = 0;
		next = cur + cur / 4;
		if (next > max_br)
			next = max_br;
	}
	else {
		/* Dead band: neither bad enough to back off nor clean enough to
		 * count towards a recovery.  Hold, and reset the run so a link
		 * flickering through the band never accumulates enough clean
		 * ticks to step up. */
		lc->adapt_clean_ticks = 0;
		return;
	}

	if (next == cur)
		return;

	if (bsdk_adapt_apply_bitrate(lc, next))
		return;   /* fixed-rate codec or transient error — try again later */

	info("EchoSDK: adaptive bitrate %u -> %u bps (loss %.1f%%)\n",
	     cur, next, s->loss_pct);
}

/* ── Entry points ────────────────────────────────────────────────────────── */

void bsdk_adapt_call_start(struct echosdk_call *lc)
{
	if (!lc)
		return;

	lc->adapt_bitrate     = 0;
	lc->adapt_clean_ticks = 0;
	lc->stall_active      = false;
	lc->stall_rx_packets  = 0;
	lc->stall_since       = tmr_jiffies();
	/* Seeded, not assumed: a plain-RTP call never reads as pending, so it
	 * sees no edge and keeps the old behaviour exactly. */
	lc->stall_setup_pending = media_setup_pending(lc);
}

void bsdk_adapt_tick(struct echosdk_call *lc,
                      const echosdk_ev_media_stats_t *s)
{
	if (!lc || !lc->bc || !s)
		return;

	stall_tick(lc, s);
	bitrate_tick(lc, s);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

typedef struct {
	struct echosdk_call *lc;
	uint32_t             bitrate;
	int                  result;
} setbr_ctx_t;

static void set_bitrate_fn(void *arg)
{
	setbr_ctx_t *ctx = arg;
	int err = bsdk_adapt_apply_bitrate(ctx->lc, ctx->bitrate);

	switch (err) {
	case 0:       ctx->result = ECHOSDK_OK;            break;
	case EINVAL:  ctx->result = ECHOSDK_ERR_INVAL;     break;
	case ENOENT:
	case ENOTSUP: ctx->result = ECHOSDK_ERR_STATE;     break;
	default:      ctx->result = ECHOSDK_ERR_TRANSPORT; break;
	}
}

int echosdk_call_set_bitrate(echosdk_call_handle_t call, uint32_t bitrate_bps)
{
	setbr_ctx_t ctx = { .lc = call, .bitrate = bitrate_bps, .result = 0 };
	int err;

	if (!call)
		return ECHOSDK_ERR_INVAL;
	if (bitrate_bps && (bitrate_bps < BSDK_ADAPT_BR_MIN ||
	                    bitrate_bps > BSDK_ADAPT_BR_MAX))
		return ECHOSDK_ERR_INVAL;

	err = bsdk_dispatch_sync(set_bitrate_fn, &ctx);
	return err ? err : ctx.result;
}

typedef struct {
	bool     enabled;
	uint32_t min_bps;
	uint32_t max_bps;
} adaptcfg_ctx_t;

static void set_adaptive_fn(void *arg)
{
	adaptcfg_ctx_t *ctx = arg;

	g_bsdk.cfg.adaptive_bitrate = ctx->enabled;
	if (ctx->min_bps)
		g_bsdk.cfg.adapt_min_bitrate = ctx->min_bps;
	if (ctx->max_bps)
		g_bsdk.cfg.adapt_max_bitrate = ctx->max_bps;
}

void echosdk_set_adaptive_bitrate(bool enabled, uint32_t min_bps,
                                   uint32_t max_bps)
{
	adaptcfg_ctx_t ctx = { .enabled = enabled, .min_bps = min_bps,
	                       .max_bps = max_bps };
	(void)bsdk_dispatch_sync(set_adaptive_fn, &ctx);
}

/* ── Per-call RTP timeout ────────────────────────────────────────────────── */

typedef struct {
	struct echosdk_call *lc;
	uint32_t             seconds;
	int                  result;
} rtptmo_ctx_t;

static void set_rtp_timeout_fn(void *arg)
{
	rtptmo_ctx_t *ctx = arg;
	struct echosdk_call *lc = ctx->lc;

	if (!lc->bc) {
		ctx->result = ECHOSDK_ERR_STATE;
		return;
	}

	/* Two calls, because baresip splits the job: call_enable_rtp_timeout()
	 * only records the value — the streams are armed from it when the call
	 * transitions to established (src/call.c).  For a call that is already
	 * established that transition has been and gone, so arm the audio
	 * stream directly as well.  Recording it too keeps the setting correct
	 * across a re-establish, e.g. after a handover re-INVITE. */
	call_enable_rtp_timeout(lc->bc, ctx->seconds * 1000u);
	{
		struct audio  *au   = call_audio(lc->bc);
		struct stream *strm = au ? audio_strm(au) : NULL;
		if (strm)
			stream_enable_rtp_timeout(strm, ctx->seconds * 1000u);
	}
	ctx->result = ECHOSDK_OK;
}

int echosdk_call_set_rtp_timeout(echosdk_call_handle_t call, uint32_t seconds)
{
	rtptmo_ctx_t ctx = { .lc = call, .seconds = seconds, .result = 0 };
	int err;

	if (!call)
		return ECHOSDK_ERR_INVAL;

	err = bsdk_dispatch_sync(set_rtp_timeout_fn, &ctx);
	return err ? err : ctx.result;
}
