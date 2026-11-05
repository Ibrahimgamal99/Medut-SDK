/**
 * @file sdp.c  SDP offer/answer capture → MEDUT_EV_SDP_NEGOTIATION
 *
 * Called from event.c (bsdk_ev_call_sdp) for each local and remote SDP.
 * Tracks when both local and remote SDP have been exchanged, then fires a
 * single negotiation event with the active codec and crypto info.
 *
 * The negotiation event resets the flags so re-INVITE renegotiation produces
 * another event.
 */

#include "bsdk.h"

void bsdk_sdp_note(struct medut_call *lc, bool remote)
{
	if (!lc || !lc->ec)
		return;

	if (!remote)
		lc->local_sdp_set = true;
	else
		lc->remote_sdp_set = true;

	if (!lc->local_sdp_set || !lc->remote_sdp_set)
		return;

	/* Reset so re-INVITE renegotiation fires again */
	lc->local_sdp_set  = false;
	lc->remote_sdp_set = false;

	/* Collect negotiated codec from active TX encoder */
	const char *codec_name = mdt_call_codec_name(lc->ec);

	/* Determine crypto from per-account config (accounts may differ) */
	const char *crypto;
	switch (lc->acct ? lc->acct->cfg.media_enc : g_bsdk.cfg.media_enc) {
	case MEDUT_MEDIA_ENC_SDES:       crypto = "SDES";      break;
	case MEDUT_MEDIA_ENC_DTLS_SRTP:  crypto = "DTLS-SRTP"; break;
	default:                           crypto = "NONE";       break;
	}

	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type       = MEDUT_EV_SDP_NEGOTIATION;
	qev->ev.u.sdp.call = lc;

	/* Pack small strings into the inline buffer */
	size_t off = 0;

	if (codec_name) {
		str_ncpy(qev->buf + off, codec_name, sizeof(qev->buf) - off);
		qev->ev.u.sdp.negotiated_codec = qev->buf + off;
		off += strlen(codec_name) + 1;
	}

	str_ncpy(qev->buf + off, crypto, sizeof(qev->buf) - off);
	qev->ev.u.sdp.negotiated_crypto = qev->buf + off;

	/* SDP text lives in the call wrapper's stable buffers (valid for the
	 * lifetime of the call handle, which outlives this event delivery). */
	if (lc->local_sdp[0])
		qev->ev.u.sdp.local_sdp = lc->local_sdp;
	if (lc->remote_sdp[0])
		qev->ev.u.sdp.remote_sdp = lc->remote_sdp;

	bsdk_event_post_qev(qev);
}
