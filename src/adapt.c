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
 *   3. Opus FEC switched on and off with the link.  The loss in the peer's
 *      receiver reports turns our in-band FEC on (4%, or 2% twice running)
 *      and off (10 s under 1%), and sizes it while on, so a clean link carries no redundancy
 *      and a lossy one lets the peer rebuild a dropped frame from the one
 *      after it.  Only while cfg.opus.fec is on and the app has not pinned
 *      cfg.opus_expected_loss_pct.
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
 * alone; there is no bitrate to vary, and no concealment either — the `plc`
 * module is not built (see modules_init.c).
 *
 * All functions here run on the re_main thread.
 */

#include "echosdk_internal.h"

/* Opus refuses a maxaveragebitrate outside this range (modules/opus/sdp.c),
 * so a target outside it would be silently ignored rather than clamped. */
#define BSDK_ADAPT_BR_MIN   6000u
#define BSDK_ADAPT_BR_MAX 510000u

/* Past this the redundant copy costs the primary encoding more than it
 * saves; a link that bad is adaptive bitrate's job. */
#define BSDK_FEC_MAX_PCT 25u

/* The lowest bitrate libopus will still code in-band FEC at, for any loss
 * from 5% to 25%: its decide_fec() wants the loss-discounted equivalent rate
 * (compute_equiv_rate()) above ~15 kbps even at narrowband.  At the 12 kbps
 * adaptation floor it never can, and at 16 kbps only if FEC was already on —
 * so bitrate adaptation stops here while FEC is in use, or backing off for
 * loss would switch off the one thing that repairs it. */
#define BSDK_FEC_MIN_BR 20000u

/* FEC on/off, from the loss in the peer's receiver reports.  On at once for
 * a report of BSDK_FEC_ON_NOW_PCT or more; for BSDK_FEC_ON_PCT up to that,
 * only once two reports in a row say so.  Off once no report has shown
 * BSDK_FEC_OFF_PCT for BSDK_FEC_OFF_MS.
 *
 * Below ~2% random loss Opus's own concealment is close enough that
 * redundancy is not worth the bits it takes from the primary encoding.  A
 * report covers only a few hundred packets, though, so on a link at 1–2% one
 * report in four crosses 2% by chance (measured: a 1.4% link turned FEC on
 * within 10 s, and the 1–2% band then kept it on); the second report is what
 * tells that from a link that really degraded.  The gap between the on and
 * off thresholds keeps a link sitting near one of them from switching every
 * report, and 10 s is two clean reports at the usual 5 s RTCP interval. */
/* A report showing no progress past packets the peer already had, while we
 * sent at least this many since the last one, means none of them arrived. */
#define BSDK_DEAD_PKTS 50u

/* How long bitrate adaptation sits at the FEC floor before judging whether
 * the loss it still sees is congestion: one more receiver report. */
#define BSDK_CONGESTED_MS 6000u

#define BSDK_FEC_ON_NOW_PCT 4.0f
#define BSDK_FEC_ON_PCT     2.0f
#define BSDK_FEC_OFF_PCT    1.0f
#define BSDK_FEC_OFF_MS     10000u

/* baresip's opus module keeps its expected packet loss in this global and
 * reads it on every encoder update (modules/opus/encode.c); it is what the
 * FEC redundancy is sized from.  So it is set to a call's level right before
 * that call's encoder is updated.  Always linked: opus is in COMMON_MODULES. */
extern int32_t opus_packet_loss;

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

/* FEC redundancy follows the link unless the app pinned it, or turned FEC
 * off.  A pinned value is written once at init (core.c) and left alone. */
static bool fec_auto(void)
{
	return g_bsdk.cfg.opus.fec && !g_bsdk.cfg.opus_expected_loss_pct;
}

/* The expected loss this call's encoder is sized for, followed or pinned. */
static uint32_t fec_level_now(const struct echosdk_call *lc)
{
	if (!fec_auto())
		return g_bsdk.cfg.opus_expected_loss_pct;

	/* On a link whose loss follows the bitrate, redundancy only adds to
	 * the congestion (bitrate_tick()). */
	return lc->fec.active && !lc->adapt_congested ? lc->fec.level : 0;
}

/* ── fmtp rewriting ──────────────────────────────────────────────────────── */

/**
 * Copy `src` (an SDP fmtp parameter string) into `buf`, dropping any `key`
 * parameter, then append key=`value` as a new one.
 *
 * Exposed (not static) only so test/unit/test_fmtp_bitrate.c can exercise it
 * directly — every caller is in this file.
 *
 * Parameters are `;`-separated key=value pairs.  A leading or doubled `;` is
 * legal in what libre hands us, so empty tokens are skipped rather than
 * copied — fmt_param_get() tolerates them, but emitting them back would make
 * the string grow every time we adapt.
 *
 * @param value  the new value, or 0 to only strip.
 * @return 0 on success, EINVAL on overflow (caller then leaves the rate be).
 */
int bsdk_adapt_fmtp_set(char *buf, size_t sz, const char *src,
                        const char *key, uint32_t value)
{
	size_t klen = key ? strlen(key) : 0;
	size_t pos = 0;
	const char *p = src;

	if (!buf || sz == 0 || !klen)
		return EINVAL;
	buf[0] = '\0';

	while (p && *p) {
		const char *end = strchr(p, ';');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		/* Skip empty tokens and the parameter we are replacing.  The
		 * comparison is on the key only, so "maxaveragebitrate=24000"
		 * and a bare "maxaveragebitrate" are both dropped. */
		if (len > 0 &&
		    !(len >= klen && !strncasecmp(p, key, klen) &&
		      (len == klen || p[klen] == '='))) {

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

	if (value) {
		int n = re_snprintf(buf + pos, sz - pos, "%s%s=%u",
		                    pos ? ";" : "", key, value);
		if (n < 0)
			return EINVAL;
	}

	return 0;
}

int bsdk_adapt_fmtp_set_bitrate(char *buf, size_t sz, const char *src,
                                uint32_t bitrate)
{
	return bsdk_adapt_fmtp_set(buf, sz, src, "maxaveragebitrate", bitrate);
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
	char                     fmtp[512], nofec[512];
	const char              *params;
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

	params = sc->params;

	/* 0 is the negotiated rate, i.e. the remote fmtp as it came.  Stripping
	 * maxaveragebitrate from it instead would drop a cap the peer asked for. */
	if (bitrate) {
		err = bsdk_adapt_fmtp_set_bitrate(fmtp, sizeof(fmtp), params,
		                                  bitrate);
		if (err)
			return err;
		params = fmtp;
	}

	/* The encoder takes its FEC switch from the peer's useinbandfec and its
	 * redundancy from the expected-loss global, and every update re-reads
	 * both — so hand it this call's state each time, or a bitrate step
	 * would reset it.  Off means the flag itself is off, not just a 0%
	 * budget. */
	if (fec_auto()) {
		if (!fec_level_now(lc)) {
			err = bsdk_adapt_fmtp_set(nofec, sizeof(nofec), params,
			                          "useinbandfec", 0);
			if (err)
				return err;
			params = nofec;
		}
		opus_packet_loss = (int32_t)fec_level_now(lc);
	}

	err = audio_encoder_set(au, ac, sc->pt, params);
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
 * the deadline src/ice/ice_mod.c holds its candidate re-offer for. */
#define BSDK_MEDIA_SETUP_GRACE_MS 12000

/**
 * Is this call's media encryption still being set up?
 *
 * There is no inbound RTP during a DTLS handshake and there is not meant to
 * be: dtls_srtp sets `wait_secure`, so stream_is_ready() is false and baresip
 * has not opened the audio device yet.  Calling that a stall starts a repair
 * (netmon.c) whose ICE restart re-INVITEs the call in the middle of the very
 * handshake it is waiting for — which the ICE module measured across 22 calls as
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
bool bsdk_media_setup_pending(const struct echosdk_call *lc)
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
	 * edge covers the second and later handshakes on one call.
	 *
	 * ICE counts as setup for the same reason, and not only on DTLS calls:
	 * connectivity checks have to nominate a pair before a packet can
	 * arrive on it.  A call with SDES or plain RTP over ICE has no
	 * handshake to wait for, so the DTLS edge alone left its whole check
	 * phase on the clock — the same bug, on the accounts where it was
	 * invisible.  An ICE restart drops the state back below CONNECTED and
	 * therefore re-arms this edge too. */
	pending = bsdk_media_setup_pending(lc) ||
	          (lc->bc && bsdk_ice_setup_pending(lc->bc));

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
	float    loss;

	if (!g_bsdk.cfg.adaptive_bitrate)
		return;

	/* loss_pct is derived from the peer's receiver report, which baresip
	 * only has once RTCP has flowed; before then it reads 0 and would look
	 * like a perfectly clean link.  Wait for the report rather than
	 * ramping up on no evidence. */
	if (!s->packets_sent || s->rtt_ms <= 0.f)
		return;

	/* An uplink that delivers nothing reads 0% in the window: the peer
	 * counts no gaps in what it never received (bsdk_adapt_fec_update()).
	 * And the report after it comes back is that outage catching up. */
	loss = lc->fec.dead ? 100.f : lc->fec.recovered ? 0.f : s->loss_pct;

	min_br = adapt_min_br();
	max_br = adapt_max_br();

	/* An unadapted call is at its negotiated rate, which we treat as the
	 * ceiling: the first step down must actually lower it. */
	cur = lc->adapt_bitrate ? lc->adapt_bitrate : max_br;

	/* The FEC floor, while FEC is actually coded: a peer without
	 * useinbandfec=1 gets none, and holding its bitrate up buys nothing.
	 *
	 * Unless the loss turns out to follow the bitrate.  Random loss stays
	 * put when the rate drops, and FEC is what repairs it; congestion eases
	 * when it drops.  So at the floor, one report later, loss still above
	 * the step-down threshold but a third lower than when we stepped here
	 * is a link too narrow for the floor: release it, and FEC's bits with
	 * it.  Two reports running, not the smoothed figure alone: a link that
	 * has simply recovered also shows loss easing, and its next report
	 * reads clean (seen live after an uplink outage).  Measured live on a 32 kbps uplink, the floor otherwise held the
	 * call at 16-26% loss for as long as the cap lasted. */
	if (bsdk_adapt_fec_status(lc, NULL) && min_br < BSDK_FEC_MIN_BR) {
		uint64_t now = tmr_jiffies();

		if (cur > BSDK_FEC_MIN_BR)
			lc->adapt_floor_since = 0;
		else if (!lc->adapt_floor_since)
			lc->adapt_floor_since = now;
		else if (now - lc->adapt_floor_since >= BSDK_CONGESTED_MS &&
		         lc->fec.last_loss > adapt_down_pct() &&
		         lc->fec.prev_loss > adapt_down_pct() &&
		         lc->fec.smooth <= lc->adapt_step_loss * (2.f / 3.f)) {
			lc->adapt_congested = true;
			lc->fec_applied     = false;   /* re-apply without FEC */
			/* A pinned level stays set (fec_level_now()); the
			 * encoder just codes no FEC at the rates below the
			 * floor. */
			info("EchoSDK: congested at %u bps (loss %.1f%%, "
			     "%.1f%% before): %s\n",
			     cur, lc->fec.smooth, lc->adapt_step_loss,
			     fec_auto() ? "FEC off, floor released"
			                : "floor released (FEC pinned, not coded "
			                  "at this rate)");
		}

		if (!lc->adapt_congested)
			min_br = BSDK_FEC_MIN_BR < max_br ? BSDK_FEC_MIN_BR
			                                  : max_br;
	}

	if (cur < min_br) {
		/* FEC came on while the rate was below where it can be coded. */
		next = min_br;
	}
	else if (loss > adapt_down_pct()) {
		lc->adapt_clean_ticks = 0;
		/* What the floor test above compares against. */
		if (cur > BSDK_FEC_MIN_BR)
			lc->adapt_step_loss = lc->fec.smooth > loss
			                    ? lc->fec.smooth : loss;
		next = cur / 2;
		if (next < min_br)
			next = min_br;
	}
	else if (loss <= adapt_up_pct()) {
		if (++lc->adapt_clean_ticks < adapt_recover_ticks())
			return;
		lc->adapt_clean_ticks = 0;
		next = cur + cur / 4;
		if (next > max_br)
			next = max_br;
		/* Back above the floor: FEC may come back if the loss does. */
		if (lc->adapt_congested && next >= BSDK_FEC_MIN_BR) {
			lc->adapt_congested = false;
			lc->fec_applied     = false;
		}
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
	     cur, next, loss);
}

/* ── FEC adaptation ──────────────────────────────────────────────────────── */

/**
 * Redundancy level (%) while FEC is on, for a smoothed peer loss `loss_pct`.
 *
 * Bands: under 4% → 5, 4–10% → 10, 10–15% → 15, 15–20% → 20, above → 25
 * (BSDK_FEC_MAX_PCT).  Up at once; down only once the loss is a point below
 * the band `cur` belongs to, so a link on a band edge does not re-run the
 * encoder update every report.
 */
uint32_t bsdk_adapt_fec_level(float loss_pct, uint32_t cur)
{
	static const struct { float lo; uint32_t level; } band[] = {
		{ 20.f, 25u }, { 15.f, 20u }, { 10.f, 15u }, { 4.f, 10u },
		{  0.f,  5u },
	};
	uint32_t want = 5u;
	float    cur_lo = 0.f;

	for (size_t i = 0; i < RE_ARRAY_SIZE(band); i++) {
		if (loss_pct >= band[i].lo) {
			want = band[i].level;
			break;
		}
	}
	for (size_t i = 0; i < RE_ARRAY_SIZE(band); i++) {
		if (cur >= band[i].level) {
			cur_lo = band[i].lo;
			break;
		}
	}

	if (want < cur && loss_pct >= cur_lo - 1.f)
		return cur;

	return want;
}

/**
 * Advance the FEC state by one stats tick.
 *
 * Only a new receiver report carries news.  The per-tick loss the stats
 * window computes reads 0% on every tick between two reports, whatever the
 * link is doing — so this keeps its own mark of the last report (a changed
 * RTT or cumulative loss means a new one) and takes the loss over the span
 * between two reports: what the peer lost of what we sent in that time.
 */
bool bsdk_adapt_fec_update(struct bsdk_fec *f, uint32_t sent, uint32_t lost,
                           uint32_t ehsn, float rtt_ms, uint64_t now_ms)
{
	const bool     was_active = f->active;
	const uint32_t was_level  = f->level;
	uint32_t d_sent, d_ehsn;
	int64_t  d_lost;
	float    loss;

	f->recovered = false;

	/* No receiver report yet: nothing to go on. */
	if (rtt_ms <= 0.f)
		return false;

	if (!f->rep_seen) {
		f->rep_seen      = true;
		f->rep_sent      = sent;
		f->rep_lost      = lost;
		f->rep_ehsn      = ehsn;
		f->rep_rtt       = rtt_ms;
		f->last_lossy_ms = now_ms;
		return false;
	}

	if (rtt_ms == f->rep_rtt && lost == f->rep_lost && ehsn == f->rep_ehsn)
		return false;             /* the same report as last tick */

	d_sent = sent - f->rep_sent;
	d_lost = (int64_t)lost - (int64_t)f->rep_lost;
	d_ehsn = ehsn - f->rep_ehsn;
	f->rep_sent = sent;
	f->rep_lost = lost;
	f->rep_ehsn = ehsn;
	f->rep_rtt  = rtt_ms;

	if (!d_sent)
		return false;

	if (!d_ehsn && d_sent >= BSDK_DEAD_PKTS) {
		/* The peer has heard nothing past what it had last time, though
		 * we kept sending: none of it arrived.  Its own loss count cannot
		 * show that — a receiver counts only gaps below the highest
		 * sequence it has seen — so this is the only way a dead uplink
		 * does not read as a clean one. */
		f->dead = true;
		loss = 100.f;
	}
	else if (f->dead) {
		/* Back.  This report's loss is the outage catching up, already
		 * acted on while it lasted. */
		f->dead      = false;
		f->recovered = true;
		return false;
	}
	else {
		if (d_lost < 0)           /* duplicates can walk it back */
			d_lost = 0;
		loss = (float)d_lost * 100.f / (float)d_sent;
		if (loss > 100.f)
			loss = 100.f;
	}

	f->prev_loss = f->last_loss;
	f->last_loss = loss;

	if (loss >= BSDK_FEC_OFF_PCT)
		f->last_lossy_ms = now_ms;

	if (!f->active) {
		if (loss >= BSDK_FEC_ON_NOW_PCT ||
		    (loss >= BSDK_FEC_ON_PCT && f->pending >= BSDK_FEC_ON_PCT)) {
			f->active  = true;
			f->smooth  = loss > f->pending ? loss : f->pending;
			f->level   = bsdk_adapt_fec_level(f->smooth, 0);
			f->pending = 0.f;
		}
		else {
			/* 2–4%: wait for the next report to confirm it. */
			f->pending = loss >= BSDK_FEC_ON_PCT ? loss : 0.f;
		}
	}
	else if (loss < BSDK_FEC_OFF_PCT &&
	         now_ms - f->last_lossy_ms >= BSDK_FEC_OFF_MS) {
		f->active  = false;
		f->smooth  = 0.f;
		f->level   = 0;
		f->pending = 0.f;
	}
	else {
		/* Up with the first worse report; ease down over several. */
		if (loss >= f->smooth)
			f->smooth = loss;
		else
			f->smooth += (loss - f->smooth) * 0.5f;
		f->level = bsdk_adapt_fec_level(f->smooth, f->level);
	}

	return f->active != was_active || f->level != was_level;
}

bool bsdk_adapt_fec_status(const struct echosdk_call *lc, uint32_t *level)
{
	struct audio            *au;
	struct stream           *strm;
	const struct sdp_format *sc;
	const struct aucodec    *ac;
	uint32_t                 lvl;
	struct pl                params, v;

	if (level)
		*level = 0;
	if (!lc || !lc->bc)
		return false;

	au   = call_audio(lc->bc);
	strm = au ? audio_strm(au) : NULL;
	sc   = strm ? sdp_media_rformat(stream_sdpmedia(strm), NULL) : NULL;
	ac   = sc ? sc->data : NULL;
	if (!ac || !ac->name || str_casecmp(ac->name, "opus"))
		return false;

	/* Our encoder codes FEC only for a peer that said it can use it. */
	if (!sc->params)
		return false;
	pl_set_str(&params, sc->params);
	if (!fmt_param_get(&params, "useinbandfec", &v) || pl_u32(&v) != 1)
		return false;

	lvl = fec_level_now(lc);
	if (level)
		*level = lvl;
	return lvl > 0;
}

static void fec_tick(struct echosdk_call *lc, bool changed)
{
	if (!fec_auto())
		return;

	if (changed)
		lc->fec_applied = false;

	/* Applied once per call even with nothing measured yet: the global is
	 * shared, and an encoder opened after the last call left it raised
	 * would otherwise keep that call's redundancy. */
	if (lc->fec_applied)
		return;

	/* Not Opus, or media not up yet: the state stands, and the next tick
	 * tries again. */
	if (bsdk_adapt_apply_bitrate(lc, lc->adapt_bitrate))
		return;
	lc->fec_applied = true;

	if (lc->fec.active != lc->fec_applied_active)
		info("EchoSDK: Opus FEC %s (%u%%, peer loss %.1f%%)\n",
		     lc->fec.active ? "on" : "off", lc->fec.level,
		     lc->fec.smooth);
	else if (lc->fec.level != lc->fec_applied_level)
		info("EchoSDK: Opus FEC %u%% -> %u%% (peer loss %.1f%%)\n",
		     lc->fec_applied_level, lc->fec.level, lc->fec.smooth);
	lc->fec_applied_active = lc->fec.active;
	lc->fec_applied_level  = lc->fec.level;
}

/* ── Entry points ────────────────────────────────────────────────────────── */

void bsdk_adapt_call_start(struct echosdk_call *lc)
{
	if (!lc)
		return;

	lc->adapt_bitrate     = 0;
	lc->adapt_clean_ticks = 0;
	memset(&lc->fec, 0, sizeof(lc->fec));
	lc->adapt_floor_since  = 0;
	lc->adapt_step_loss    = 0.f;
	lc->adapt_congested    = false;
	lc->fec_applied        = false;
	lc->fec_applied_active = false;
	lc->fec_applied_level  = 0;
	/* A new call starts from a clean link, not the last call's. */
	if (fec_auto())
		opus_packet_loss = 0;
	lc->stall_active      = false;
	lc->stall_rx_packets  = 0;
	lc->stall_since       = tmr_jiffies();
	/* Seeded, not assumed: a plain-RTP call never reads as pending, so it
	 * sees no edge and keeps the old behaviour exactly. */
	lc->stall_setup_pending = bsdk_media_setup_pending(lc);
}

void bsdk_adapt_tick(struct echosdk_call *lc,
                      const echosdk_ev_media_stats_t *s)
{
	if (!lc || !lc->bc || !s)
		return;

	stall_tick(lc, s);
	/* The report tracker runs whatever the FEC mode: bitrate adaptation
	 * reads a dead uplink and the smoothed loss from it too. */
	fec_tick(lc, bsdk_adapt_fec_update(&lc->fec, s->packets_sent,
	                                   s->packets_lost, lc->rtcp_ehsn,
	                                   s->rtt_ms, tmr_jiffies()));
	bitrate_tick(lc, s);
}

void bsdk_adapt_media_renegotiated(struct echosdk_call *lc)
{
	/* baresip's audio_update() has just rebuilt the encoder from the remote
	 * fmtp and the shared expected-loss global; re-apply this call's level
	 * (and bitrate) on the next tick. */
	if (lc)
		lc->fec_applied = false;
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
