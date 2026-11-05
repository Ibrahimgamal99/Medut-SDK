/**
 * @file media_api.c  A call's media, as core/mdt.h offers it to the SDK
 *
 * Mute, DSCP, RTP timeout, the negotiated codec and its live re-tuning
 * (adapt.c), one consistent snapshot of the stream's counters (stats.c),
 * and moving live calls when the app picks another audio device.
 */

#include <string.h>
#include "../sip/mdt_call.h"

/* Opus expected loss [%] the encoder sizes its in-band FEC from; read by
 * the Opus codec on every encoder update. */
int32_t g_mdt_opus_loss;

static struct mdt_stream *audio_strm(const struct mdt_call *ec)
{
	return mdt_audio_strm(mdt_call_audio(ec));
}

int mdt_call_mute(struct mdt_call *ec, bool mute)
{
	struct mdt_audio *au = mdt_call_audio(ec);

	if (!au)
		return ENOENT;

	mdt_audio_mute(au, mute);
	return 0;
}

bool mdt_call_is_muted(const struct mdt_call *ec)
{
	struct mdt_audio *au = mdt_call_audio(ec);

	return au ? mdt_audio_ismuted(au) : false;
}

int mdt_call_mute_rx(struct mdt_call *ec, bool mute)
{
	struct mdt_stream *strm = audio_strm(ec);

	if (!strm)
		return ENOENT;

	return mdt_stream_enable_rx(strm, !mute);
}

/* dscp is a DSCP value; the socket takes the TOS byte it sits in. */
int mdt_call_set_dscp_rtp(struct mdt_call *ec, uint8_t dscp)
{
	struct mdt_stream *strm = audio_strm(ec);
	struct rtp_sock *rs;
	struct udp_sock *us;

	if (!strm)
		return ENOENT;

	rs = mdt_stream_rtp_sock(strm);
	us = rs ? rtp_sock(rs) : NULL;
	if (!us)
		return ENOENT;

	return udp_settos(us, (uint8_t)(dscp << 2));
}

int mdt_call_set_rtp_timeout(struct mdt_call *ec, uint32_t ms)
{
	struct mdt_stream *strm;

	/* Recorded for when the call (re-)establishes, and armed on the
	 * stream now for a call that already has. */
	mdt_call_enable_rtp_timeout(ec, ms);

	strm = audio_strm(ec);
	if (strm)
		mdt_stream_enable_rtp_timeout(strm, ms);

	return 0;
}

bool mdt_call_menc_pending(const struct mdt_call *ec)
{
	struct list *streaml = mdt_call_streaml(ec);
	struct le *le;

	if (!streaml)
		return false;

	LIST_FOREACH(streaml, le) {
		if (!mdt_stream_is_secure(le->data))
			return true;
	}

	return false;
}

uint32_t mdt_call_rx_packets(const struct mdt_call *ec)
{
	struct mdt_stream *strm = audio_strm(ec);

	return strm ? mdt_stream_metric_get_rx_n_packets(strm) : 0;
}

/* The negotiated remote format: sc->data is our codec for it, sc->params
 * the peer's fmtp.  NULL until an answer was applied. */
static const struct sdp_format *rformat(const struct mdt_call *ec)
{
	struct mdt_stream *strm = audio_strm(ec);

	return strm ? sdp_media_rformat(mdt_stream_sdpmedia(strm), NULL)
		    : NULL;
}

/* The encoder's codec — or, before the encoder starts (an incoming call
 * being answered), the one the negotiation chose. */
const char *mdt_call_codec_name(const struct mdt_call *ec)
{
	struct mdt_audio *au = mdt_call_audio(ec);
	const struct mdt_aucodec *ac = au ? mdt_audio_codec(au, true) : NULL;
	const struct sdp_format *sc;

	if (ac)
		return ac->name;

	sc = rformat(ec);
	ac = sc ? sc->data : NULL;

	return ac ? ac->name : NULL;
}

int mdt_call_rfmt(const struct mdt_call *ec, struct mdt_rfmt *rf)
{
	const struct sdp_format *sc;
	const struct mdt_aucodec *ac;

	memset(rf, 0, sizeof(*rf));

	if (!audio_strm(ec))
		return ENOENT;

	sc = rformat(ec);
	if (!sc || !sc->data)
		return ENOENT;

	ac = sc->data;
	rf->codec  = ac->name;
	rf->params = sc->params;
	rf->encupd = ac->encupdh != NULL;

	return 0;
}

/* The same codec and payload type, new remote parameters: what
 * mdt_audio_update() does after an answer, on demand.  The expected loss
 * is set right before, as the encoder reads it on the update. */
int mdt_call_encoder_set(struct mdt_call *ec, const char *params,
			  int32_t loss_pct)
{
	struct mdt_audio *au = mdt_call_audio(ec);
	const struct sdp_format *sc = rformat(ec);

	if (!au || !sc || !sc->data)
		return ENOENT;

	if (loss_pct >= 0)
		g_mdt_opus_loss = loss_pct;

	return mdt_audio_encoder_set(au, sc->data, sc->pt, params);
}

void mdt_media_expected_loss(int32_t loss_pct)
{
	g_mdt_opus_loss = loss_pct;
}

int mdt_call_media_snap(const struct mdt_call *ec, struct mdt_media_snap *s)
{
	struct mdt_audio *au = mdt_call_audio(ec);
	struct mdt_stream *strm = au ? mdt_audio_strm(au) : NULL;
	const struct mdt_aucodec *ac, *rac;
	struct mdt_jbuf_stat jb;
	const struct rtcp_stats *rs;
	struct rtp_sock *rsock;
	const struct sa *raddr;

	memset(s, 0, sizeof(*s));

	if (!strm)
		return ENOENT;

	s->tx_packets     = mdt_stream_metric_get_tx_n_packets(strm);
	s->rx_packets     = mdt_stream_metric_get_rx_n_packets(strm);
	s->tx_bytes       = mdt_stream_metric_get_tx_n_bytes(strm);
	s->rx_bytes       = mdt_stream_metric_get_rx_n_bytes(strm);
	s->tx_err         = mdt_stream_metric_get_tx_n_err(strm);
	s->rx_err         = mdt_stream_metric_get_rx_n_err(strm);
	s->tx_bitrate     = mdt_stream_metric_get_tx_bitrate(strm);
	s->rx_bitrate     = mdt_stream_metric_get_rx_bitrate(strm);
	s->tx_avg_bitrate = mdt_stream_metric_get_tx_avg_bitrate(strm);
	s->rx_avg_bitrate = mdt_stream_metric_get_rx_avg_bitrate(strm);

	ac = mdt_audio_codec(au, true);
	if (ac) {
		s->codec       = ac->name;
		s->codec_srate = ac->srate;
		s->codec_crate = ac->crate;
		s->codec_ch    = ac->ch;
	}

	if (mdt_stream_jbuf_stats(strm, &jb) == 0) {
		s->jb_valid     = true;
		s->jb_adaptive  = mdt_config()->avt.audio.jbtype ==
				  MDT_JBUF_ADAPTIVE;
		s->jb_delay_ms  = jb.c_delay;
		s->jb_packets   = jb.c_packets;
		s->jb_late      = jb.n_late;
		s->jb_overflow  = jb.n_overflow;
		s->jb_flush     = jb.n_flush;
		s->jb_jitter_ms = jb.c_jitter;
		s->jb_lost      = jb.n_lost;
		s->jb_get       = jb.n_get;
	}

	/* How the lost frames were filled in (Opus only). */
	rac = mdt_audio_codec(au, false);
	if (rac) {
		s->rx_codec = rac->name;
		if (rac->name && !str_casecmp(rac->name, "opus")) {
			uint32_t fec = 0, plc = 0;

			if (!mdt_opus_decode_fec_stats(
				    mdt_audio_decoder_state(au), &fec, &plc)) {
				s->fec_valid  = true;
				s->fec_frames = fec;
				s->plc_frames = plc;
			}
		}
	}

	rs = mdt_stream_rtcp_stats(strm);
	if (rs) {
		s->rtcp_valid = true;
		s->rtt_us     = rs->rtt;
		s->rx_jit_us  = rs->rx.jit;
		s->tx_jit_us  = rs->tx.jit;
		s->tx_ehsn    = rs->tx.ehsn;
		s->tx_sent    = rs->tx.sent;
		s->rx_sent    = rs->rx.sent;
		s->tx_lost    = (int32_t)rs->tx.lost;
		s->rx_lost    = (int32_t)rs->rx.lost;
	}

	rsock = mdt_stream_rtp_sock(strm);
	if (rsock)
		s->ssrc_tx = rtp_sess_ssrc(rsock);
	(void)mdt_stream_ssrc_rx(strm, &s->ssrc_rx);
	s->pt = mdt_stream_pt_enc(strm);

	raddr = mdt_stream_raddr(strm);
	if (raddr) {
		s->raddr_valid = true;
		sa_cpy(&s->raddr, raddr);
	}

	return 0;
}

/* The app picked another driver or device: move every established call
 * onto it now, not only the next one (src/audio/device.c). */
void mdt_media_device_changed(bool input, bool output)
{
	struct le *ule, *cle;

	LIST_FOREACH(&g_sip.ual, ule) {
		struct mdt_ua *ua = ule->data;

		LIST_FOREACH(&ua->calls, cle) {
			struct mdt_call *call = cle->data;
			struct mdt_audio *au = mdt_call_audio(call);

			if (!au ||
			    mdt_call_state(call) != MDT_CALL_ESTABLISHED)
				continue;

			if (input)
				(void)mdt_audio_set_source(au,
					mdt_audio_driver(),
					mdt_audio_dev(true));
			if (output)
				(void)mdt_audio_set_player(au,
					mdt_audio_driver(),
					mdt_audio_dev(false));
		}
	}
}
