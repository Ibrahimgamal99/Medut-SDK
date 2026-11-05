/**
 * @file menc.c  Media encryption
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's src/menc.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */
#include <re.h>
#include "mdt_media.h"


static struct list s_mencl;

struct list *mdt_mencl(void)
{
	return &s_mencl;
}


/**
 * Register a new Media encryption module
 *
 * @param mencl List of Media-encryption modules
 * @param menc  Media encryption module
 */
void mdt_menc_register(struct list *mencl, struct mdt_menc *menc)
{
	if (!mencl || !menc)
		return;

	list_append(mencl, &menc->le, menc);

	MDT_INFO("mediaenc: %s\n", menc->id);
}


/**
 * Unregister a Media encryption module
 *
 * @param menc Media encryption module
 */
void mdt_menc_unregister(struct mdt_menc *menc)
{
	if (!menc)
		return;

	list_unlink(&menc->le);
}


/**
 * Find a Media Encryption module by name
 *
 * @param mencl List of Media-encryption modules
 * @param id    Name of the Media Encryption module to find
 *
 * @return Matching Media Encryption module if found, otherwise NULL
 */
const struct mdt_menc *mdt_menc_find(const struct list *mencl, const char *id)
{
	struct le *le;

	if (!mencl)
		return NULL;

	for (le = mencl->head; le; le = le->next) {
		struct mdt_menc *me = le->data;

		if (0 == str_casecmp(id, me->id))
			return me;
	}

	return NULL;
}


/**
 * Get the name of a media encryption event
 *
 * @param event Media encryption event
 *
 * @return String with media encryption event name
 */
const char *mdt_menc_event_name(enum mdt_menc_event event)
{
	switch (event) {

	case MDT_MENC_EVENT_SECURE:         return "Secure";
	case MDT_MENC_EVENT_VERIFY_REQUEST: return "Verify Request";
	case MDT_MENC_EVENT_PEER_VERIFIED:  return "Peer Verified";
	default: return "?";
	}
}
