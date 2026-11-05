/**
 * @file aucodec.c Audio Codec
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's src/aucodec.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */

#include <re.h>
#include "mdt_media.h"


static struct list s_aucodecl;

struct list *mdt_aucodecl(void)
{
	return &s_aucodecl;
}


/**
 * Register an Audio Codec
 *
 * @param aucodecl List of audio-codecs
 * @param ac       Audio Codec object
 */
void mdt_aucodec_register(struct list *aucodecl, struct mdt_aucodec *ac)
{
	if (!aucodecl || !ac)
		return;

	list_append(aucodecl, &ac->le, ac);

	MDT_INFO("aucodec: %s/%u/%u\n", ac->name, ac->srate, ac->ch);
}


/**
 * Unregister an Audio Codec
 *
 * @param ac Audio Codec object
 */
void mdt_aucodec_unregister(struct mdt_aucodec *ac)
{
	if (!ac)
		return;

	list_unlink(&ac->le);
}


/**
 * Find an Audio Codec
 *
 * @param aucodecl List of audio-codecs
 * @param name     Audio codec name
 * @param srate    Audio codec sampling rate
 * @param ch       Audio codec number of channels
 *
 * @return Matching audio codec if found, NULL if not found
 */
const struct mdt_aucodec *mdt_aucodec_find(const struct list *aucodecl,
				   const char *name, uint32_t srate,
				   uint8_t ch)
{
	struct le *le;

	for (le=list_head(aucodecl); le; le=le->next) {

		struct mdt_aucodec *ac = le->data;

		if (name && 0 != str_casecmp(name, ac->name))
			continue;

		if (srate && srate != ac->srate)
			continue;

		if (ch && ch != ac->ch)
			continue;

		return ac;
	}

	return NULL;
}


int mdt_aucodec_print(struct re_printf *pf, const struct mdt_aucodec *ac)
{
	if (!ac)
		return 0;

	return re_hprintf(pf, "%s %uHz/%dch",
			  ac->name, ac->srate, ac->ch);
}
