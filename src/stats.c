/**
 * @file stats.c  RTCP stats polling timer + MOS calculation
 *
 * A tmr fires on re_main every cfg->stats_interval_ms. For each active call
 * it reads RTCP stats from the audio stream and posts ECHOSDK_EV_MEDIA_STATS.
 *
 * ── Loss rates are windowed, not cumulative ──────────────────────────────
 * RTCP carries lifetime cumulative counters (RFC 3550 A.3), the same
 * convention as WebRTC getStats() and PJSIP, both of which expect the
 * application to difference consecutive samples to obtain a rate.  We do
 * that differencing here: packets_lost/packets_lost_rx stay cumulative (they
 * are counters and documented as such), while loss_pct/loss_pct_rx are rates
 * over the last poll window.  Feeding a lifetime average into the E-model
 * would let one early burst depress MOS for the rest of the call with no
 * way to recover.
 *
 * ── E-model (ITU-T G.107 narrowband, G.107.1 wideband) ───────────────────
 *   Ie-eff = Ie + (95-Ie)·Ppl/(Ppl+Bpl)      per-codec, ITU-T G.113
 *   R-LQ   = Ro - Ie-eff                     listening quality: no delay term
 *   Id     = 0.024·Ta + 0.11·(Ta-177.3)·[Ta>177.3]
 *   R-CQ   = R-LQ - Id                       conversational: delay included
 *   MOS    = 1 + 0.035·R' + 7e-6·R'·(R'-60)·(100-R'),  R' = R·100/Rmax
 *
 * MOS-LQ excludes delay by definition — it scores the received signal alone.
 * MOS-CQ is the one that carries Id.  Ta is a real one-way delay estimate:
 * network one-way (RTT/2) plus the jitter buffer depth actually in use.
 *
 * Ro/Rmax are 93.2/100 narrowband and 129/129 wideband.  The wideband
 * normalisation is checked against ITU's own reference calculator, which
 * reports R=128.8 -> MOS-CQE-wb=4.50.
 *
 * Ppl is *effective* loss — network loss plus packets the jitter buffer
 * threw away (late/overflow/flush).  A packet discarded by the buffer is as
 * gone as one lost in the network, and omitting it is the single largest
 * source of MOS over-estimation in a jittery call.  This only applies to the
 * RX direction: the peer's jitter buffer is not observable from here.
 *
 * Simplified MOS (Telchemy/Cisco standard form):
 *   MOS-LQ = 4.5 - 0.09·loss_pct - 0.0009·jitter_ms
 *   MOS-CQ = MOS-LQ - 0.0005·RTT     (= 0.001 per ms of one-way delay)
 */

#include <math.h>
#include "echosdk_internal.h"

/* ── MOS calculations ───────────────────────────────────────────────────── */

/** Audio bandwidth class — selects the narrowband or wideband R scale. */
typedef enum {
	BSDK_BW_NARROW,   /**< ITU-T G.107:   Ro = 93.2, R in [0,100] */
	BSDK_BW_WIDE      /**< ITU-T G.107.1: Ro = 129,  R in [0,129] */
} bsdk_bw_class_t;

/* ITU-T G.113 Appendix I codec parameters: Ie (baseline impairment at 0 loss)
 * and Bpl (packet-loss robustness factor). */
typedef struct {
	float           ie;
	float           bpl;
	bsdk_bw_class_t bw;
} codec_params_t;

static codec_params_t codec_emodel_params(const char *name, uint32_t srate,
                                          uint32_t bitrate_kbps)
{
	/* G.711 is the narrowband reference codec: Ie = 0 by definition. */
	if (!name)
		return (codec_params_t){0.f, 4.3f, BSDK_BW_NARROW};
	if (!str_casecmp(name, "PCMU") || !str_casecmp(name, "PCMA"))
		return (codec_params_t){0.f, 4.3f, BSDK_BW_NARROW};
	if (!str_casecmp(name, "G729"))
		return (codec_params_t){11.f, 19.0f, BSDK_BW_NARROW};
	if (!str_casecmp(name, "G723"))
		return (codec_params_t){15.f, 16.0f, BSDK_BW_NARROW};

	/* G.722 is the *wideband* reference codec, exactly as G.711 is the
	 * narrowband one: Ie,wb = 0 on the G.107.1 scale.  Scoring it as a
	 * narrowband codec with an invented Ie penalty (as this used to) both
	 * penalises it for nothing and caps it at the narrowband ceiling. */
	if (!str_casecmp(name, "G722"))
		return (codec_params_t){0.f, 4.3f, BSDK_BW_WIDE};

	if (!str_casecmp(name, "opus")) {
		/* Opus Ie/Bpl are approximations from the published instrumental
		 * derivations (Voznak et al.); ITU-T G.113 does not tabulate
		 * Opus.  Treat as indicative, not normative. */
		bsdk_bw_class_t bw = (srate >= 16000) ? BSDK_BW_WIDE
		                                      : BSDK_BW_NARROW;
		if (bitrate_kbps <= 8)  return (codec_params_t){14.f, 12.0f, bw};
		if (bitrate_kbps <= 12) return (codec_params_t){11.f, 14.0f, bw};
		return (codec_params_t){7.f, 14.0f, bw};
	}

	return (codec_params_t){0.f, 4.3f, (srate >= 16000) ? BSDK_BW_WIDE
	                                                    : BSDK_BW_NARROW};
}

/** Listening- and conversational-quality scores from one sample. */
typedef struct {
	float lq;   /**< MOS-LQ (1.0 .. 4.5) */
	float cq;   /**< MOS-CQ (1.0 .. 4.5) */
	float q;    /**< quantity to average over the session (see stats_q_sum) */
} mos_pair_t;

/* Convert an R factor to MOS.  The G.107 polynomial is defined on a 0..100
 * scale, so the wideband R (0..129) is normalised onto it first. */
static float r_to_mos(float R, bsdk_bw_class_t bw)
{
	float rmax = (bw == BSDK_BW_WIDE) ? 129.f : 100.f;
	float r    = R * 100.f / rmax;

	if (r <= 0.f)   return 1.0f;
	if (r >= 100.f) return 4.5f;

	return 1.0f + 0.035f * r + 7e-6f * r * (r - 60.f) * (100.f - r);
}

/**
 * E-model score.
 *
 * @param loss_pct  effective loss % over the poll window (network +, for the
 *                  RX direction, jitter buffer discards)
 * @param jb_ms     jitter buffer depth in ms on the path being scored
 * @param rtt_ms    round-trip time in ms
 */
static mos_pair_t mos_emodel(float loss_pct, float jb_ms, float rtt_ms,
                             codec_params_t cp)
{
	float Ro = (cp.bw == BSDK_BW_WIDE) ? 129.f : 93.2f;
	mos_pair_t out;

	/* ITU-T G.107: Ie-eff = Ie + (95-Ie) * Ppl / (Ppl + Bpl) */
	float Ie_eff = cp.ie
	             + (95.f - cp.ie) * loss_pct / (loss_pct + cp.bpl);

	/* Listening quality carries no delay impairment. */
	float R_lq = Ro - Ie_eff;

	/* Conversational quality adds Id from the one-way delay Ta. */
	float Ta = rtt_ms * 0.5f + jb_ms;
	float Id = 0.024f * Ta
	         + 0.11f * (Ta - 177.3f) * (Ta > 177.3f ? 1.f : 0.f);
	if (Id < 0.f)
		Id = 0.f;

	out.lq = r_to_mos(R_lq, cp.bw);
	out.cq = r_to_mos(R_lq - Id, cp.bw);
	out.q  = R_lq;   /* average in the R domain — MOS is non-linear in R */
	return out;
}

static mos_pair_t mos_simplified(float loss_pct, float jitter_ms, float rtt_ms)
{
	/* Telchemy/Cisco simplified VoIP MOS:
	 *   -0.09   per 1% packet loss
	 *   -0.0009 per ms jitter
	 *   -0.0005 per ms RTT  (= -0.001 per ms one-way delay, RTT/2)
	 * The RTT term is a delay impairment, so it belongs to CQ only. */
	mos_pair_t out;
	float lq = 4.5f - 0.09f * loss_pct - 0.0009f * jitter_ms;
	float cq = lq - 0.0005f * rtt_ms;

	if (lq < 1.0f) lq = 1.0f;
	if (lq > 4.5f) lq = 4.5f;
	if (cq < 1.0f) cq = 1.0f;
	if (cq > 4.5f) cq = 4.5f;

	out.lq = lq;
	out.cq = cq;
	out.q  = lq;   /* no R factor — average in the MOS domain */
	return out;
}

static mos_pair_t calc_mos(float loss_pct, float jitter_ms, float jb_ms,
                           float rtt_ms, echosdk_mos_method_t method,
                           codec_params_t cp)
{
	if (method == ECHOSDK_MOS_SIMPLIFIED)
		return mos_simplified(loss_pct, jitter_ms, rtt_ms);
	return mos_emodel(loss_pct, jb_ms, rtt_ms, cp);
}

/* ── Counter differencing ────────────────────────────────────────────────── */

/* Delta between two cumulative counters, clamped at zero.  A negative delta
 * means the counters were reset under us (SSRC change, stream restart after
 * an ICE restart or re-INVITE); report nothing rather than a wild value and
 * let the next window re-establish the baseline. */
static uint32_t counter_delta(uint32_t now, uint32_t prev)
{
	return (now >= prev) ? (now - prev) : 0u;
}

static uint32_t counter_delta_i(int32_t now, int32_t prev)
{
	/* RTCP lost counts are signed and may legitimately decrease when
	 * duplicates arrive after a reordering burst (RFC 3550 A.3). */
	return (now > prev) ? (uint32_t)(now - prev) : 0u;
}

/* ── Shared stats population ──────────────────────────────────── */

/* Fill all numeric/address fields of *s from the live audio stream.
 * Does NOT set s->codec_name (pointer lifetime differs per caller).
 *
 * @param advance  true on the polling tick, which owns the loss window and
 *                 rolls it forward; false for the sync getter, which reads
 *                 the same window without consuming it.
 *
 * Returns the aucodec so the caller can set codec_name appropriately. */
static const struct aucodec *fill_audio_stats(echosdk_ev_media_stats_t *s,
                                               struct echosdk_call *lc,
                                               struct audio *au,
                                               struct stream *strm,
                                               bool advance)
{
	s->call = lc;

	/* ── Packet counters ─────────────────────────────────────── */
	s->packets_sent      = stream_metric_get_tx_n_packets(strm);
	s->packets_received  = stream_metric_get_rx_n_packets(strm);
	s->bytes_sent        = stream_metric_get_tx_n_bytes(strm);
	s->bytes_received    = stream_metric_get_rx_n_bytes(strm);
	s->tx_errors         = stream_metric_get_tx_n_err(strm);
	s->rx_errors         = stream_metric_get_rx_n_err(strm);

	/* ── Bandwidth ─────────────────────────────────────────── */
	s->bandwidth_kbps_tx     = stream_metric_get_tx_bitrate(strm) / 1000;
	s->bandwidth_kbps_rx     = stream_metric_get_rx_bitrate(strm) / 1000;
	s->avg_bandwidth_kbps_tx = (uint32_t)(stream_metric_get_tx_avg_bitrate(strm) / 1000.0);
	s->avg_bandwidth_kbps_rx = (uint32_t)(stream_metric_get_rx_avg_bitrate(strm) / 1000.0);

	/* ── Codec ───────────────────────────────────────────── */
	const struct aucodec *ac = audio_codec(au, true);
	if (ac) {
		s->codec_clock_rate  = ac->crate;
		s->codec_sample_rate = ac->srate;
		s->codec_channels    = ac->ch;
	}

	/* ── Jitter buffer (read before RTCP: its discard count feeds Ppl) ── */
	uint32_t jb_discard_total = 0;
	uint32_t jb_get_total     = 0;
	struct jbuf_stat jb;
	if (stream_jbuf_stats(strm, &jb) == 0) {
		s->jitter_buffer_ms        = jb.c_delay;
		s->jitter_buffer_load      = jb.c_packets;
		s->late_packets            = jb.n_late;
		s->discarded_packets       = jb.n_overflow + jb.n_flush;
		s->jitter_buffer_target_ms = jb.c_jitter;
		s->jitter_buffer_adaptive  =
		    (conf_config()->avt.audio.jbtype == JBUF_ADAPTIVE);
		s->plc_frames = jb.n_lost;
		s->plc_ratio  = jb.n_get > 0
		              ? (float)jb.n_lost / (float)jb.n_get : 0.f;

		/* Frames the buffer threw away rather than played out.  These
		 * never reach the ear, so to the E-model they are loss. */
		jb_discard_total = jb.n_late + jb.n_overflow + jb.n_flush;
		jb_get_total     = jb.n_get;
	}

	/* ── RTCP (may be absent early in a call; leave zeros if so) ─── */
	s->mos_method = g_bsdk.cfg.mos_method;

	const struct rtcp_stats *rs = stream_rtcp_stats(strm);
	if (rs) {
		/* rtt and jit are both in microseconds.  libre has already done
		 * the RTP-timestamp-to-time conversion using the codec clock
		 * rate baresip handed it (re/src/rtp/sess.c: rtcp_stats() and
		 * handle_rr_block(), both `1000000 * jitter / srate`), so the
		 * only thing left to do is us -> ms.  Dividing by the clock
		 * rate a second time inflated jitter by 1e6/crate — 125x at
		 * 8 kHz — which pinned MOS at 1.0 and made the jitter alert
		 * fire permanently. */
		s->rtt_ms       = (float)rs->rtt / 1000.f;
		s->jitter_ms    = (float)rs->rx.jit / 1000.f;
		s->tx_jitter_ms = (float)rs->tx.jit / 1000.f;

		/* Cumulative counters stay cumulative — same convention as
		 * WebRTC getStats() and PJSIP. */
		int32_t cum_tx_lost = (int32_t)rs->tx.lost;
		int32_t cum_rx_lost = (int32_t)rs->rx.lost;
		s->packets_lost    = cum_tx_lost > 0 ? (uint32_t)cum_tx_lost : 0u;
		s->packets_lost_rx = cum_rx_lost > 0 ? (uint32_t)cum_rx_lost : 0u;

		/* ── TX loss rate over the poll window ──────────────── */
		/* Denominator is what we sent in the window; the peer's RR is
		 * the only source for the numerator and can lag it by up to one
		 * RTCP interval, so a burst shows up slightly smeared. */
		uint32_t d_tx_sent = counter_delta(rs->tx.sent,
		                                   lc->stats_prev_tx_sent);
		uint32_t d_tx_lost = counter_delta_i(cum_tx_lost,
		                                     lc->stats_prev_tx_lost);
		if (d_tx_lost > d_tx_sent)
			d_tx_lost = d_tx_sent;
		s->loss_pct = d_tx_sent > 0
		            ? (float)d_tx_lost * 100.f / (float)d_tx_sent : 0.f;

		/* ── RX loss rate over the poll window ──────────────── */
		/* rs->rx.sent is the RTCP source's own received count — the
		 * very counter rtp_source_calc_lost() differences against, so
		 * received + lost is exactly `expected`.  Using the stream
		 * metric here instead mixed two counters that do not agree. */
		uint32_t d_rx_recv = counter_delta(rs->rx.sent,
		                                   lc->stats_prev_rx_recv);
		uint32_t d_rx_lost = counter_delta_i(cum_rx_lost,
		                                     lc->stats_prev_rx_lost);
		uint32_t d_rx_expected = d_rx_recv + d_rx_lost;
		s->loss_pct_rx = d_rx_expected > 0
		               ? (float)d_rx_lost * 100.f / (float)d_rx_expected
		               : 0.f;

		/* ── Jitter buffer discard rate over the window ───────── */
		uint32_t d_jb_disc = counter_delta(jb_discard_total,
		                                   lc->stats_prev_jb_discard);
		uint32_t d_jb_get  = counter_delta(jb_get_total,
		                                   lc->stats_prev_jb_get);
		uint32_t d_jb_total = d_jb_get + d_jb_disc;
		float discard_pct = d_jb_total > 0
		                  ? (float)d_jb_disc * 100.f / (float)d_jb_total
		                  : 0.f;

		/* Effective loss drives Ppl on the RX path only — our jitter
		 * buffer is ours; the peer's is not observable from here. */
		float eff_loss_rx = s->loss_pct_rx + discard_pct;
		if (eff_loss_rx > 100.f)
			eff_loss_rx = 100.f;

		codec_params_t cp = codec_emodel_params(ac ? ac->name : NULL,
		                                        s->codec_sample_rate,
		                                        s->avg_bandwidth_kbps_tx);

		/* Each direction is scored from its own loss and its own jitter.
		 * Pairing TX loss with RX jitter (as this used to) described a
		 * path that does not exist. */

		/* Far end: what the peer hears.  Their buffer depth is not
		 * reported, so estimate it from the jitter they report — an
		 * adaptive buffer tracks roughly twice the jitter estimate. */
		float peer_jb_ms = 2.0f * s->tx_jitter_ms;
		mos_pair_t tx = calc_mos(s->loss_pct, s->tx_jitter_ms,
		                         peer_jb_ms, s->rtt_ms,
		                         g_bsdk.cfg.mos_method, cp);
		s->mos_lq = tx.lq;
		s->mos_cq = tx.cq;

		/* Near end: what we hear.  Real measured buffer depth. */
		mos_pair_t rx = calc_mos(eff_loss_rx, s->jitter_ms,
		                         (float)s->jitter_buffer_ms, s->rtt_ms,
		                         g_bsdk.cfg.mos_method, cp);
		s->mos_lq_rx = rx.lq;
		s->mos_cq_rx = rx.cq;

		if (advance) {
			lc->stats_prev_tx_sent    = rs->tx.sent;
			lc->stats_prev_tx_lost    = cum_tx_lost;
			lc->stats_prev_rx_recv    = rs->rx.sent;
			lc->stats_prev_rx_lost    = cum_rx_lost;
			lc->stats_prev_jb_discard = jb_discard_total;
			lc->stats_prev_jb_get     = jb_get_total;

			/* Session history folds in the R factor, not the MOS:
			 * MOS is a non-linear function of R, so averaging MOS
			 * across ticks is not statistically meaningful. */
			lc->stats_q_sum += tx.q;
			lc->stats_mos_n++;
			if (lc->stats_mos_min == 0.f || tx.lq < lc->stats_mos_min)
				lc->stats_mos_min = tx.lq;
		}
	}

	/* ── Audio levels (dBov) — computed from PCM in media_tap.c ──── */
	uint32_t rx_bits = re_atomic_rlx(&lc->rx_level_bits);
	memcpy(&s->audio_level_dbov, &rx_bits, 4);
	uint32_t tx_bits = re_atomic_rlx(&lc->tx_level_bits);
	memcpy(&s->mic_level_dbov, &tx_bits, 4);

	/* ── Stream identity ───────────────────────────────────── */
	struct rtp_sock *rsock = stream_rtp_sock(strm);
	if (rsock)
		s->ssrc_tx = rtp_sess_ssrc(rsock);
	stream_ssrc_rx(strm, &s->ssrc_rx);

	s->payload_type = stream_pt_enc(strm);

	/* ── ICE ───────────────────────────────────────────────────────
	 *
	 * Ask the agent, not the stream.  Everything above ICE talks to one
	 * constant virtual peer address so that a pair switch or an ICE
	 * restart cannot re-key the media encryption (src/ice/ice_mod.c), so
	 * stream_raddr() on an ICE call is that placeholder and never a
	 * routable address.  `remote_addr` has always meant "where is the
	 * audio going", and for an ICE call that is the selected pair. */
	{
		struct sa ice_l, ice_r;
		int ltype = 0, rtype = 0;
		uint32_t rtt = 0, gen = 0;

		s->ice_state       = (echosdk_ice_state_t)
		                     bsdk_ice_call_state(lc->bc);
		s->ice_local_type  = ECHOSDK_ICE_CAND_NONE;
		s->ice_remote_type = ECHOSDK_ICE_CAND_NONE;

		if (bsdk_ice_call_selected(lc->bc, &ice_l, &ice_r, &ltype,
		                           &rtype, &rtt, &gen)) {
			s->ice_local_type  = (echosdk_ice_cand_type_t)ltype;
			s->ice_remote_type = (echosdk_ice_cand_type_t)rtype;
			s->ice_rtt_ms      = (float)rtt;
			s->ice_generation  = gen;
			re_snprintf(s->ice_local_addr,
			            sizeof(s->ice_local_addr), "%J", &ice_l);
			re_snprintf(s->ice_remote_addr,
			            sizeof(s->ice_remote_addr), "%J", &ice_r);
			re_snprintf(s->remote_addr, sizeof(s->remote_addr),
			            "%J", &ice_r);
			return ac;
		}
	}

	{
		const struct sa *raddr = stream_raddr(strm);
		if (raddr)
			re_snprintf(s->remote_addr, sizeof(s->remote_addr),
			            "%J", raddr);
	}

	return ac;
}

/* Session-average MOS.  The running sum is in the R domain for the E-model,
 * so the conversion to MOS happens once, on the mean — not per tick. */
static float session_mos_avg(const struct echosdk_call *lc,
                             const echosdk_ev_media_stats_t *s)
{
	if (!lc->stats_mos_n)
		return 0.f;

	float mean = lc->stats_q_sum / (float)lc->stats_mos_n;

	if (g_bsdk.cfg.mos_method == ECHOSDK_MOS_SIMPLIFIED)
		return mean;   /* already a MOS */

	codec_params_t cp = codec_emodel_params(s->codec_name,
	                                        s->codec_sample_rate,
	                                        s->avg_bandwidth_kbps_tx);
	return r_to_mos(mean, cp.bw);
}

/* ── Quality alert helper ────────────────────────────────────────────────── */

static const char *quality_issue_str(echosdk_quality_issue_t issue)
{
	switch (issue) {
	case ECHOSDK_QUALITY_MOS:         return "mos";
	case ECHOSDK_QUALITY_LOSS:        return "loss";
	case ECHOSDK_QUALITY_JITTER:      return "jitter";
	case ECHOSDK_QUALITY_RTT:         return "rtt";
	case ECHOSDK_QUALITY_MEDIA_STALL: return "media-stall";
	default:                          return "?";
	}
}

void bsdk_post_quality_alert(struct echosdk_call *lc,
                             echosdk_quality_issue_t issue,
                             float value, float threshold, bool recovering)
{
	struct echosdk_queued_event *qev;

	/* Logged as well as posted.  An alert only reaches the app as an event,
	 * and an app is free to route it somewhere that is not the log — this
	 * example app puts it in a snackbar and a Diagnostics list, neither of
	 * which survives into a device log.  MEDIA_STALL on a call with no audio
	 * is then the one thing a capture of a broken call most needs and least
	 * often has: chasing a silent 36 s call on 2026-08-31 meant proving from
	 * SDP and ICE state that media never came up, while the SDK had known it
	 * for 32 of those seconds. */
	info("EchoSDK: quality %s %s: %.1f (threshold %.1f)\n",
	     quality_issue_str(issue), recovering ? "recovered" : "alert",
	     value, threshold);

	qev = bsdk_qev_alloc();
	if (!qev) return;
	qev->ev.type = ECHOSDK_EV_QUALITY_ALERT;
	echosdk_ev_quality_alert_t *a = &qev->ev.u.quality_alert;
	a->call       = lc;
	a->issue      = issue;
	a->value      = value;
	a->threshold  = threshold;
	a->recovering = recovering;
	bsdk_event_post_qev(qev);
}

/* ── Per-call stats collection (timer path) ──────────────────────────────── */

static void collect_call_stats(struct echosdk_call *lc)
{
	if (!lc->bc)
		return;

	struct audio *au = call_audio(lc->bc);
	if (!au)
		return;

	struct stream *strm = audio_strm(au);
	if (!strm)
		return;

	struct echosdk_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type = ECHOSDK_EV_MEDIA_STATS;
	echosdk_ev_media_stats_t *s = &qev->ev.u.stats;

	/* advance=true: this is the tick that owns the loss window. */
	const struct aucodec *ac = fill_audio_stats(s, lc, au, strm, true);
	if (ac) {
		str_ncpy(qev->buf, ac->name, sizeof(qev->buf));
		s->codec_name = qev->buf;
	}

	/* Session history — min/sum are folded in by fill_audio_stats(), which
	 * is where the R factor behind the score is still in scope. */
	lc->stats_tick++;
	s->mos_lq_min      = lc->stats_mos_min;
	s->mos_lq_avg      = session_mos_avg(lc, s);
	s->stats_tick      = lc->stats_tick;
	s->call_duration_ms = lc->stats_call_start
	                    ? (tmr_jiffies() - lc->stats_call_start) : 0u;
	s->is_final        = false;

	/* Quality alert threshold crossing detection */
	const echosdk_config_t *cfg = &g_bsdk.cfg;
	if (cfg->mos_alert_threshold > 0.f && s->mos_lq > 0.f) {
		bool was_bad = lc->last_mos_lq > 0.f
		            && lc->last_mos_lq < cfg->mos_alert_threshold;
		bool is_bad  = s->mos_lq < cfg->mos_alert_threshold;
		if (is_bad != was_bad)
			bsdk_post_quality_alert(lc, ECHOSDK_QUALITY_MOS, s->mos_lq,
			                   cfg->mos_alert_threshold, was_bad);
	}
	if (cfg->loss_alert_threshold > 0.f) {
		bool was_bad = lc->last_loss_pct > cfg->loss_alert_threshold;
		bool is_bad  = s->loss_pct      > cfg->loss_alert_threshold;
		if (is_bad != was_bad)
			bsdk_post_quality_alert(lc, ECHOSDK_QUALITY_LOSS, s->loss_pct,
			                   cfg->loss_alert_threshold, was_bad);
	}
	if (cfg->jitter_alert_threshold > 0.f) {
		bool was_bad = lc->last_jitter_ms > cfg->jitter_alert_threshold;
		bool is_bad  = s->jitter_ms       > cfg->jitter_alert_threshold;
		if (is_bad != was_bad)
			bsdk_post_quality_alert(lc, ECHOSDK_QUALITY_JITTER, s->jitter_ms,
			                   cfg->jitter_alert_threshold, was_bad);
	}
	lc->last_mos_lq    = s->mos_lq;
	lc->last_loss_pct  = s->loss_pct;
	lc->last_jitter_ms = s->jitter_ms;

	/* Stall detection and bitrate adaptation read the same sample.  Run
	 * them before the event is posted so the app cannot observe a stats
	 * tick whose consequences have not been applied yet. */
	bsdk_adapt_tick(lc, s);

	bsdk_event_post_qev(qev);
}

/* ── Final stats snapshot on call teardown ───────────────────────────────── */

void bsdk_stats_collect_final(struct echosdk_call *lc)
{
	if (!lc || !lc->bc)
		return;

	struct audio *au = call_audio(lc->bc);
	if (!au)
		return;

	struct stream *strm = audio_strm(au);
	if (!strm)
		return;

	struct echosdk_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type = ECHOSDK_EV_MEDIA_STATS;
	echosdk_ev_media_stats_t *s = &qev->ev.u.stats;

	/* advance=false: the teardown snapshot reports the window since the
	 * last tick but must not fold itself into the session average, which
	 * belongs to the polling cadence. */
	const struct aucodec *ac = fill_audio_stats(s, lc, au, strm, false);
	if (ac) {
		str_ncpy(qev->buf, ac->name, sizeof(qev->buf));
		s->codec_name = qev->buf;
	}

	s->is_final        = true;
	s->mos_lq_min      = lc->stats_mos_min;
	s->mos_lq_avg      = session_mos_avg(lc, s);
	s->stats_tick      = lc->stats_tick;
	s->call_duration_ms = lc->stats_call_start
	                    ? (tmr_jiffies() - lc->stats_call_start) : 0u;

	bsdk_event_post_qev(qev);
}

/* ── Timer handler (re_main thread) ─────────────────────────────────────── */

static void stats_visit(struct echosdk_call *lc, void *arg)
{
	(void)arg;
	if (lc->state == ECHOSDK_CALL_ESTABLISHED)
		collect_call_stats(lc);
}

static void stats_timer_handler(void *arg)
{
	(void)arg;
	bsdk_call_foreach(stats_visit, NULL);
	tmr_start(&g_bsdk.stats_tmr, g_bsdk.cfg.stats_interval_ms,
	          stats_timer_handler, NULL);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bsdk_stats_init(void)
{
	if (!g_bsdk.cfg.stats_interval_ms)
		return 0;

	tmr_init(&g_bsdk.stats_tmr);
	tmr_start(&g_bsdk.stats_tmr, g_bsdk.cfg.stats_interval_ms,
	          stats_timer_handler, NULL);
	return 0;
}

void bsdk_stats_close(void)
{
	tmr_cancel(&g_bsdk.stats_tmr);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

typedef struct {
	struct echosdk_call      *lc;
	echosdk_ev_media_stats_t *out;
	int                       result;
} getstats_ctx_t;

static void getstats_fn(void *arg)
{
	getstats_ctx_t *ctx = arg;
	struct echosdk_call *lc = ctx->lc;
	if (!lc->bc) { ctx->result = ENOENT; return; }

	struct audio *au = call_audio(lc->bc);
	if (!au) { ctx->result = ENOENT; return; }

	struct stream *strm = audio_strm(au);
	if (!strm) { ctx->result = ENOENT; return; }

	echosdk_ev_media_stats_t *s = ctx->out;

	/* advance=false: a getter must not consume the polling tick's loss
	 * window, or an app that polls between ticks would zero the rates the
	 * next MEDIA_STATS event is meant to report. */
	const struct aucodec *ac = fill_audio_stats(s, lc, au, strm, false);
	/* codec_name points directly into baresip's stable registered-codec list */
	if (ac)
		s->codec_name = ac->name;

	/* Session history.  Read from the call, never advanced here: these
	 * counters belong to the polling tick, and a getter that bumped
	 * stats_tick or folded its own sample into the average would skew the
	 * very numbers the app polled for.  Filling them matters because they
	 * are the only per-call history the SDK keeps — without this the sync
	 * getter reported 0 for all of them at every point in a call, however
	 * many ticks had already run. */
	s->mos_lq_min       = lc->stats_mos_min;
	s->mos_lq_avg       = session_mos_avg(lc, s);
	s->stats_tick       = lc->stats_tick;
	s->call_duration_ms = lc->stats_call_start
	                    ? (tmr_jiffies() - lc->stats_call_start) : 0u;
	s->is_final         = false;

	ctx->result = 0;
}

int echosdk_call_get_stats(echosdk_call_handle_t call,
                            echosdk_ev_media_stats_t *out)
{
	if (!call || !out) return ECHOSDK_ERR_INVAL;

	/* Zero before anything can fail.  Every early return below — no call
	 * object, no audio, no stream, or a dispatch that never runs — used to
	 * leave *out exactly as the caller passed it, so a caller with a stack
	 * struct that skips the return code read uninitialised memory: floats
	 * come back as garbage or NaN, not as the documented zeros. */
	memset(out, 0, sizeof(*out));

	getstats_ctx_t ctx = {.lc = call, .out = out, .result = 0};
	int err = bsdk_dispatch_sync(getstats_fn, &ctx);
	return err ? err : ctx.result;
}
