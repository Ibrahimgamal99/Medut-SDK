/**
 * @file opus_dec.c Opus Decode
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/opus/decode.c (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */

#include <re.h>
#include <rem.h>
#include <opus/opus.h>
#include "../media/mdt_media.h"
#include "opus.h"


struct mdt_audec_state {
	OpusDecoder *dec;
	unsigned ch;
	/* how lost frames were filled in, for stats.c */
	uint32_t n_fec;   /**< Rebuilt from the next packet's LBRR copy */
	uint32_t n_plc;   /**< Concealed by the decoder alone           */
};


/* The split, read by stats.c through mdt_call_media_snap(). */
int mdt_opus_decode_fec_stats(const struct mdt_audec_state *ads,
			  uint32_t *fec, uint32_t *plc)
{
	if (!ads)
		return EINVAL;
	if (fec)
		*fec = ads->n_fec;
	if (plc)
		*plc = ads->n_plc;
	return 0;
}


static void destructor(void *arg)
{
	struct mdt_audec_state *ads = arg;

	if (ads->dec)
		opus_decoder_destroy(ads->dec);
}


int mdt_opus_decode_update(struct mdt_audec_state **adsp, const struct mdt_aucodec *ac,
		       const char *fmtp)
{
	struct mdt_audec_state *ads;
	int opuserr, err = 0;
	(void)fmtp;

	if (!adsp || !ac || !ac->ch)
		return EINVAL;

	ads = *adsp;

	if (ads)
		return 0;

	ads = mem_zalloc(sizeof(*ads), destructor);
	if (!ads)
		return ENOMEM;

	ads->ch = ac->ch;

	ads->dec = opus_decoder_create(ac->srate, ac->ch, &opuserr);
	if (!ads->dec) {
		MDT_WARN("opus: decoder create: %s\n", opus_strerror(opuserr));
		err = ENOMEM;
		goto out;
	}

 out:
	if (err)
		mem_deref(ads);
	else
		*adsp = ads;

	return err;
}


int mdt_opus_decode_frm(struct mdt_audec_state *ads,
		    int fmt, void *sampv, size_t *sampc,
		    bool marker, const uint8_t *buf, size_t len)
{
	int n;
	(void)marker;

	if (!ads || !sampv || !sampc || !buf)
		return EINVAL;

	switch (fmt) {

	case AUFMT_S16LE:
		n = opus_decode(ads->dec, buf, (opus_int32)len,
				sampv, (int)(*sampc/ads->ch), 0);
		if (n < 0) {
			MDT_WARN("opus: decode error: %s\n", opus_strerror(n));
			return EPROTO;
		}
		break;

	case AUFMT_FLOAT:
		n = opus_decode_float(ads->dec, buf, (opus_int32)len,
				      sampv, (int)(*sampc/ads->ch), 0);
		if (n < 0) {
			MDT_WARN("opus: float decode error: %s\n",
				opus_strerror(n));
			return EPROTO;
		}
		break;

	default:
		return ENOTSUP;
	}

	*sampc = (size_t)n * ads->ch;

	return 0;
}



int mdt_opus_decode_pkloss(struct mdt_audec_state *ads,
		       int fmt, void *sampv, size_t *sampc,
		       const uint8_t *buf, size_t len)
{
	int n;
	opus_int32 frame_size;
	size_t nsamp = 0;
	bool fec;

	if (!ads || !sampv || !sampc)
		return EINVAL;

	/*
	 * FEC=0 -> use PLC (packet loss concealment, no coded data
	 *          available for this lost frame)
	 * FEC=1 -> use inband FEC (recover this lost frame from the
	 *          FEC data carried in the next, already received,
	 *          packet)
	 *
	 * Inband FEC can only be attempted when the caller actually
	 * supplies the coded payload of the following packet; otherwise
	 * fall back to plain concealment.
	 */
	/* whether the peer sent FEC is not a question our
	 * own expected loss (g_mdt_opus_loss) can answer — the packet is.
	 * Without an LBRR copy libopus would conceal anyway; asking first
	 * tells the two apart for the counters. */
	fec = buf && len && opus_packet_has_lbrr(buf, (opus_int32)len) == 1;

	opus_decoder_ctl(ads->dec, OPUS_GET_LAST_PACKET_DURATION(&frame_size));

	switch (fmt) {

	case AUFMT_S16LE:
		n = opus_decode(ads->dec,
				fec ? buf : NULL,
				fec ? (opus_int32)len : 0,
				sampv, (int)(frame_size), fec);
		if (n < 0) {
			MDT_WARN("opus: decode error: %s\n", opus_strerror(n));
			return EPROTO;
		}
		break;

	case AUFMT_FLOAT:
		n = opus_decode_float(ads->dec,
				      fec ? buf : NULL,
				      fec ? (opus_int32)len : 0,
				      sampv, (int)(frame_size), fec);
		if (n < 0) {
			MDT_WARN("opus: decode error: %s\n", opus_strerror(n));
			return EPROTO;
		}

		break;

	default:
		return ENOTSUP;
	}

	if (fec)
		ads->n_fec++;
	else
		ads->n_plc++;

	nsamp = (n * ads->ch);

	if (nsamp > *sampc) {
		MDT_WARN("opus: pkloss: buffer too small.\n");
		return ENOMEM;
	}

	*sampc = nsamp;

	return 0;
}
