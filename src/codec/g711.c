/**
 * @file g711.c G.711 Audio Codec
 *
 * Copyright (C) 2010 - 2015 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/g711/g711.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */

#include <re.h>
#include <rem.h>
#include "../media/mdt_media.h"


/**
 * @defgroup g711 g711
 *
 * The G.711 audio codec
 */


static int pcmu_encode(struct mdt_auenc_state *aes, bool *marker, uint8_t *buf,
		       size_t *len, int fmt, const void *sampv, size_t sampc)
{
	const int16_t *p = sampv;

	(void)aes;
	(void)marker;

	if (!buf || !len || !sampv)
		return EINVAL;

	if (*len < sampc)
		return ENOMEM;

	if (fmt != AUFMT_S16LE)
		return ENOTSUP;

	*len = sampc;

	while (sampc--)
		*buf++ = g711_pcm2ulaw(*p++);

	return 0;
}


static int pcmu_decode(struct mdt_audec_state *ads, int fmt, void *sampv,
		       size_t *sampc, bool marker,
		       const uint8_t *buf, size_t len)
{
	int16_t *p = sampv;

	(void)ads;
	(void)marker;

	if (!sampv || !sampc || !buf)
		return EINVAL;

	if (*sampc < len)
		return ENOMEM;

	if (fmt != AUFMT_S16LE)
		return ENOTSUP;

	*sampc = len;

	while (len--)
		*p++ = g711_ulaw2pcm(*buf++);

	return 0;
}


static int pcma_encode(struct mdt_auenc_state *aes, bool *marker, uint8_t *buf,
		       size_t *len, int fmt, const void *sampv, size_t sampc)
{
	const int16_t *p = sampv;

	(void)aes;
	(void)marker;

	if (!buf || !len || !sampv)
		return EINVAL;

	if (*len < sampc)
		return ENOMEM;

	if (fmt != AUFMT_S16LE)
		return ENOTSUP;

	*len = sampc;

	while (sampc--)
		*buf++ = g711_pcm2alaw(*p++);

	return 0;
}


static int pcma_decode(struct mdt_audec_state *ads, int fmt, void *sampv,
		       size_t *sampc, bool marker,
		       const uint8_t *buf, size_t len)
{
	int16_t *p = sampv;

	(void)ads;
	(void)marker;

	if (!sampv || !sampc || !buf)
		return EINVAL;

	if (*sampc < len)
		return ENOMEM;

	if (fmt != AUFMT_S16LE)
		return ENOTSUP;

	*sampc = len;

	while (len--)
		*p++ = g711_alaw2pcm(*buf++);

	return 0;
}


static struct mdt_aucodec pcmu = {
	.pt    = "0",
	.name  = "PCMU",
	.srate = 8000,
	.crate = 8000,
	.ch    = 1,
	.pch   = 1,
	.ench  = pcmu_encode,
	.dech  = pcmu_decode,
};

static struct mdt_aucodec pcma = {
	.pt    = "8",
	.name  = "PCMA",
	.srate = 8000,
	.crate = 8000,
	.ch    = 1,
	.pch   = 1,
	.ench  = pcma_encode,
	.dech  = pcma_decode,
};


int mdt_g711_init(void)
{
	mdt_aucodec_register(mdt_aucodecl(), &pcmu);
	mdt_aucodec_register(mdt_aucodecl(), &pcma);

	return 0;
}


void mdt_g711_close(void)
{
	mdt_aucodec_unregister(&pcma);
	mdt_aucodec_unregister(&pcmu);
}
