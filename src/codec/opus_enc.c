/**
 * @file opus_enc.c Opus Encode
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/opus/encode.c (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */

#include <re.h>
#include <rem.h>
#include <opus/opus.h>
#include "../media/mdt_media.h"
#include "opus.h"




struct mdt_auenc_state {
	OpusEncoder *enc;
	unsigned ch;
};


static void destructor(void *arg)
{
	struct mdt_auenc_state *aes = arg;

	if (aes->enc)
		opus_encoder_destroy(aes->enc);
}


static opus_int32 srate2bw(opus_int32 srate)
{
	if (srate >= 48000)
		return OPUS_BANDWIDTH_FULLBAND;
	else if (srate >= 24000)
		return OPUS_BANDWIDTH_SUPERWIDEBAND;
	else if (srate >= 16000)
		return OPUS_BANDWIDTH_WIDEBAND;
	else if (srate >= 12000)
		return OPUS_BANDWIDTH_MEDIUMBAND;
	else
		return OPUS_BANDWIDTH_NARROWBAND;
}


#if 0
static const char *bwname(opus_int32 bw)
{
	switch (bw) {
	case OPUS_BANDWIDTH_FULLBAND:      return "full";
	case OPUS_BANDWIDTH_SUPERWIDEBAND: return "superwide";
	case OPUS_BANDWIDTH_WIDEBAND:      return "wide";
	case OPUS_BANDWIDTH_MEDIUMBAND:    return "medium";
	case OPUS_BANDWIDTH_NARROWBAND:    return "narrow";
	default:                           return "???";
	}
}


static const char *chname(opus_int32 ch)
{
	switch (ch) {
	case OPUS_AUTO: return "auto";
	case 1:         return "mono";
	case 2:         return "stereo";
	default:        return "???";
	}
}
#endif


int mdt_opus_encode_update(struct mdt_auenc_state **aesp, const struct mdt_aucodec *ac,
		       struct mdt_auenc_param *param, const char *fmtp)
{
	struct mdt_auenc_state *aes;
	struct mdt_opus_param prm, conf_prm;
	opus_int32 fch, vbr;
	const struct mdt_aucodec *auc = ac;

	(void)param;

	if (!aesp || !ac || !ac->ch)
		return EINVAL;

	MDT_DBG("opus: encoder fmtp (%s)\n", fmtp);

	aes = *aesp;

	if (!aes) {
		const opus_int32 complex = mdt_opus_complexity;
		int opuserr;

		aes = mem_zalloc(sizeof(*aes), destructor);
		if (!aes)
			return ENOMEM;

		aes->ch = ac->ch;

		aes->enc = opus_encoder_create(ac->srate, ac->ch,
					       mdt_opus_application,
					       &opuserr);
		if (!aes->enc) {
			MDT_WARN("opus: encoder create: %s\n",
				opus_strerror(opuserr));
			mem_deref(aes);
			return ENOMEM;
		}

		(void)opus_encoder_ctl(aes->enc, OPUS_SET_COMPLEXITY(complex));

		*aesp = aes;
	}

	prm.srate      = 48000;
	prm.bitrate    = OPUS_AUTO;
	prm.stereo     = 1;
	prm.cbr        = 0;
	prm.inband_fec = 0;
	prm.dtx        = 0;

	mdt_opus_decode_fmtp(&prm, fmtp);

	conf_prm.bitrate = OPUS_AUTO;
	mdt_opus_decode_fmtp(&conf_prm, auc->fmtp);

	if ((prm.bitrate == OPUS_AUTO) ||
	    ((conf_prm.bitrate != OPUS_AUTO) &&
	     (conf_prm.bitrate < prm.bitrate)))
		prm.bitrate = conf_prm.bitrate;

	fch = prm.stereo ? OPUS_AUTO : 1;
	vbr = prm.cbr ? 0 : 1;

	/* override local bitrate */
	if (param && param->bitrate)
		prm.bitrate = param->bitrate;

	(void)opus_encoder_ctl(aes->enc,
			       OPUS_SET_MAX_BANDWIDTH(srate2bw(prm.srate)));
	(void)opus_encoder_ctl(aes->enc, OPUS_SET_BITRATE(prm.bitrate));
	(void)opus_encoder_ctl(aes->enc, OPUS_SET_FORCE_CHANNELS(fch));
	(void)opus_encoder_ctl(aes->enc, OPUS_SET_VBR(vbr));
	(void)opus_encoder_ctl(aes->enc, OPUS_SET_INBAND_FEC(prm.inband_fec));
	/* usedtx in the peer's fmtp is only its preference
	 * (RFC 7587 6.1); DTX is ours to switch on, and a DTX stream decodes
	 * anywhere. */
	(void)opus_encoder_ctl(aes->enc, OPUS_SET_DTX(prm.dtx || mdt_opus_dtx));

	/* unconditional, so the level can return to 0. */
	opus_encoder_ctl(aes->enc,
			 OPUS_SET_PACKET_LOSS_PERC(g_mdt_opus_loss));

#if 0
	{
	opus_int32 bw, complex;

	(void)opus_encoder_ctl(aes->enc, OPUS_GET_MAX_BANDWIDTH(&bw));
	(void)opus_encoder_ctl(aes->enc, OPUS_GET_BITRATE(&prm.bitrate));
	(void)opus_encoder_ctl(aes->enc, OPUS_GET_FORCE_CHANNELS(&fch));
	(void)opus_encoder_ctl(aes->enc, OPUS_GET_VBR(&vbr));
	(void)opus_encoder_ctl(aes->enc, OPUS_GET_INBAND_FEC(&prm.inband_fec));
	(void)opus_encoder_ctl(aes->enc, OPUS_GET_DTX(&prm.dtx));
	(void)opus_encoder_ctl(aes->enc, OPUS_GET_COMPLEXITY(&complex));

	MDT_DBG("opus: encode bw=%s bitrate=%i fch=%s "
	      "vbr=%i fec=%i dtx=%i complex=%i\n",
	      bwname(bw), prm.bitrate, chname(fch),
	      vbr, prm.inband_fec, prm.dtx, complex);
	}
#endif

	return 0;
}


int mdt_opus_encode_frm(struct mdt_auenc_state *aes,
		    bool *marker, uint8_t *buf, size_t *len,
		    int fmt, const void *sampv, size_t sampc)
{
	opus_int32 n;
	(void)marker;

	if (!aes || !buf || !len || !sampv)
		return EINVAL;

	switch (fmt) {

	case AUFMT_S16LE:
		n = opus_encode(aes->enc, sampv, (int)(sampc/aes->ch),
				buf, (opus_int32)(*len));
		if (n < 0) {
			MDT_WARN("opus: encode error: %s\n",
				opus_strerror((int)n));
			return EPROTO;
		}
		break;

	case AUFMT_FLOAT:
		n = opus_encode_float(aes->enc, sampv, (int)(sampc/aes->ch),
				      buf, (opus_int32)(*len));
		if (n < 0) {
			MDT_WARN("opus: float encode error: %s\n",
				opus_strerror((int)n));
			return EPROTO;
		}
		break;

	default:
		return ENOTSUP;
	}

	/* a packet of 2 bytes or less is one the encoder says
	 * not to transmit (DTX).  Zero length makes src/media/audio.c skip the send
	 * while its RTP timestamp still advances, so the next packet lands a
	 * whole number of frames later (RFC 7587 3.1.3). */
	*len = (n <= 2) ? 0 : (size_t)n;

	return 0;
}
