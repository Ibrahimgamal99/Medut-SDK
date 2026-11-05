/**
 * @file opus.c Opus Audio Codec
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/opus/opus.c (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */

#include <re.h>
#include <opus/opus.h>
#include "../media/mdt_media.h"
#include "opus.h"


/*
 * The Opus codec (libopus 1.0.0 or later), configured from
 * medut_config_t.opus by mdt_opus_init().
 *
 * References:
 *    RFC 6716  Definition of the Opus Audio Codec
 *    RFC 7587  RTP Payload Format for the Opus Speech and Audio Codec
 */


static char fmtp[256];

uint32_t mdt_opus_complexity = 10;
opus_int32 mdt_opus_application = OPUS_APPLICATION_AUDIO;
opus_int32 mdt_opus_dtx = 0;   /* our own DTX switch (opus_enc.c) */



static struct mdt_aucodec opus = {
	.name      = "opus",
	.srate     = 48000,
	.crate     = 48000,
	.ch        = 2,
	.pch       = 2,
	.fmtp      = fmtp,
	.encupdh   = mdt_opus_encode_update,
	.ench      = mdt_opus_encode_frm,
	.decupdh   = mdt_opus_decode_update,
	.dech      = mdt_opus_decode_frm,
	.plch      = mdt_opus_decode_pkloss,
};



/* Register Opus with the parameters the app configured.  Every value is set
 * from scratch, so a second medut_init() in the process does not inherit
 * the first one's.
 *
 * stereo: send and ask for stereo, or mono both ways — a mono microphone
 * coded as stereo is the same voice twice at twice the bitrate.
 * expected_loss: the encoder's FEC sizing; 0 leaves it to adapt.c. */
int mdt_opus_init(const medut_opus_config_t *cfg, uint32_t expected_loss)
{
	char *p = fmtp;
	size_t left = sizeof(fmtp);
	int n;

	fmtp[0] = '\0';

	opus.ch                = cfg->stereo ? 2 : 1;
	opus.srate             = 48000;
	opus.fmtp              = fmtp;
	opus.fmtp_ench         = NULL;
	mdt_opus_complexity   = 10;
	mdt_opus_application  = OPUS_APPLICATION_AUDIO;
	mdt_opus_dtx          = cfg->dtx;
	g_mdt_opus_loss       = expected_loss > 100 ? 100
				 : (int32_t)expected_loss;

	/* the stereo parameters always come first */
	n = re_snprintf(p, left, "stereo=%d;sprop-stereo=%d",
			cfg->stereo, cfg->stereo);
	if (n <= 0)
		return ENOMEM;
	p += n; left -= (size_t)n;

	if (cfg->bitrate > 0) {
		n = re_snprintf(p, left, ";maxaveragebitrate=%d", cfg->bitrate);
		if (n <= 0)
			return ENOMEM;
		p += n; left -= (size_t)n;
	}

	if (cfg->cbr) {
		n = re_snprintf(p, left, ";cbr=1");
		if (n <= 0)
			return ENOMEM;
		p += n; left -= (size_t)n;
	}

	if (cfg->fec) {
		n = re_snprintf(p, left, ";useinbandfec=1");
		if (n <= 0)
			return ENOMEM;
		p += n; left -= (size_t)n;
	}

	/* DTX is ours to switch on, not the peer's to ask for. */
	if (cfg->dtx) {
		n = re_snprintf(p, left, ";usedtx=1");
		if (n <= 0)
			return ENOMEM;
	}

	if (cfg->complexity >= 0)
		mdt_opus_complexity = cfg->complexity > 10
				       ? 10 : (uint32_t)cfg->complexity;

	MDT_DBG("opus: fmtp=\"%s\"\n", fmtp);

	mdt_aucodec_register(mdt_aucodecl(), &opus);

	return 0;
}


void mdt_opus_close(void)
{
	mdt_aucodec_unregister(&opus);
}
