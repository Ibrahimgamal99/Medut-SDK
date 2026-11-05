/**
 * @file mod.c  Medut stack: plug-in registries (core/mdt_mod.h)
 *
 * Plain lists, with the lookup and lifetime rules the plug-ins rely on: a
 * device driver is registered until its handle is mem_deref'd; a filter or a
 * media-NAT is linked in by the caller's own static struct; a lookup by an
 * empty name answers the first registered entry.  Registration and lookup
 * run on re_main.
 *
 * Adapted from baresip's src/ausrc.c, src/auplay.c, src/aufilt.c and
 * src/mnat.c (BSD-3-Clause, Copyright (C) 2010 Alfred E. Heggestad); see
 * THIRD_PARTY_NOTICES.
 */

#include "../api/bsdk.h"
#include "mdt_mod.h"

static struct list s_ausrcl;
static struct list s_auplayl;
static struct list s_aufiltl;
static struct list s_mnatl;

struct list *mdt_ausrcl(void)  { return &s_ausrcl; }
struct list *mdt_auplayl(void) { return &s_auplayl; }
struct list *mdt_aufiltl(void) { return &s_aufiltl; }
struct list *mdt_mnatl(void)   { return &s_mnatl; }

int mdt_mod_init(void)
{
	list_init(&s_ausrcl);
	list_init(&s_auplayl);
	list_init(&s_aufiltl);
	list_init(&s_mnatl);
	return 0;
}

void mdt_mod_close(void)
{
	/* Device entries are owned by the drivers that registered them and
	 * unlink themselves when released; filters and media-NATs are static
	 * structs of their plug-ins.  Whatever is still linked is let go of
	 * here so a later init starts empty. */
	list_clear(&s_ausrcl);
	list_clear(&s_auplayl);
	list_clear(&s_aufiltl);
	list_clear(&s_mnatl);
}

/* ── Audio source ────────────────────────────────────────────────────────── */

static void ausrc_destructor(void *arg)
{
	struct mdt_ausrc *as = arg;

	list_flush(&as->dev_list);
	list_unlink(&as->le);
}

int mdt_ausrc_register(struct mdt_ausrc **asp, struct list *ausrcl,
			const char *name, mdt_ausrc_alloc_h *alloch)
{
	struct mdt_ausrc *as;

	if (!asp || !ausrcl)
		return EINVAL;

	as = mem_zalloc(sizeof(*as), ausrc_destructor);
	if (!as)
		return ENOMEM;

	list_append(ausrcl, &as->le, as);
	as->name   = name;
	as->alloch = alloch;

	MDT_INFO("mdt_ausrc: %s\n", name);

	*asp = as;
	return 0;
}

const struct mdt_ausrc *mdt_ausrc_find(const struct list *ausrcl,
					 const char *name)
{
	struct le *le;

	for (le = list_head(ausrcl); le; le = le->next) {
		const struct mdt_ausrc *as = le->data;

		if (str_isset(name) && str_casecmp(name, as->name))
			continue;
		return as;
	}

	return NULL;
}

int mdt_ausrc_alloc(struct mdt_ausrc_st **stp, struct list *ausrcl,
		     const char *name, struct mdt_ausrc_prm *prm,
		     const char *device, mdt_ausrc_read_h *rh,
		     mdt_ausrc_error_h *errh, void *arg)
{
	const struct mdt_ausrc *as = mdt_ausrc_find(ausrcl, name);

	if (!as)
		return ENOENT;

	return as->alloch(stp, as, prm, device, rh, errh, arg);
}

/* ── Audio player ────────────────────────────────────────────────────────── */

static void auplay_destructor(void *arg)
{
	struct mdt_auplay *ap = arg;

	list_flush(&ap->dev_list);
	list_unlink(&ap->le);
}

int mdt_auplay_register(struct mdt_auplay **app, struct list *auplayl,
			 const char *name, mdt_auplay_alloc_h *alloch)
{
	struct mdt_auplay *ap;

	if (!app || !auplayl)
		return EINVAL;

	ap = mem_zalloc(sizeof(*ap), auplay_destructor);
	if (!ap)
		return ENOMEM;

	list_append(auplayl, &ap->le, ap);
	ap->name   = name;
	ap->alloch = alloch;

	MDT_INFO("mdt_auplay: %s\n", name);

	*app = ap;
	return 0;
}

const struct mdt_auplay *mdt_auplay_find(const struct list *auplayl,
					   const char *name)
{
	struct le *le;

	for (le = list_head(auplayl); le; le = le->next) {
		const struct mdt_auplay *ap = le->data;

		if (str_isset(name) && str_casecmp(name, ap->name))
			continue;
		return ap;
	}

	return NULL;
}

int mdt_auplay_alloc(struct mdt_auplay_st **stp, struct list *auplayl,
		      const char *name, struct mdt_auplay_prm *prm,
		      const char *device, mdt_auplay_write_h *wh, void *arg)
{
	const struct mdt_auplay *ap = mdt_auplay_find(auplayl, name);

	if (!ap)
		return ENOENT;

	return ap->alloch(stp, ap, prm, device, wh, arg);
}

/* ── Audio filter ────────────────────────────────────────────────────────── */

void mdt_aufilt_register(struct list *aufiltl, struct mdt_aufilt *af)
{
	if (!aufiltl || !af)
		return;

	af->enabled = true;
	list_append(aufiltl, &af->le, af);

	MDT_INFO("mdt_aufilt: %s\n", af->name);
}

void mdt_aufilt_unregister(struct mdt_aufilt *af)
{
	if (af)
		list_unlink(&af->le);
}

void mdt_aufilt_enable(struct list *aufiltl, const char *name, bool enable)
{
	struct le *le;

	if (!aufiltl || !name)
		return;

	LIST_FOREACH(aufiltl, le) {
		struct mdt_aufilt *af = le->data;

		if (str_casecmp(af->name, name))
			continue;
		af->enabled = enable;
		break;
	}
}

/* ── Media NAT ───────────────────────────────────────────────────────────── */

void mdt_mnat_register(struct list *mnatl, struct mdt_mnat *mdt_mnat)
{
	if (!mnatl || !mdt_mnat)
		return;

	list_append(mnatl, &mdt_mnat->le, mdt_mnat);

	MDT_INFO("medianat: %s\n", mdt_mnat->id);
}

void mdt_mnat_unregister(struct mdt_mnat *mdt_mnat)
{
	if (mdt_mnat)
		list_unlink(&mdt_mnat->le);
}

const struct mdt_mnat *mdt_mnat_find(const struct list *mnatl,
				       const char *id)
{
	struct le *le;

	for (le = list_head(mnatl); le; le = le->next) {
		const struct mdt_mnat *mdt_mnat = le->data;

		if (!str_casecmp(mdt_mnat->id, id))
			return mdt_mnat;
	}

	return NULL;
}


/* ── Devices a driver enumerates ─────────────────────────────────────────── */

static void mediadev_destructor(void *arg)
{
	struct mdt_mediadev *dev = arg;

	list_unlink(&dev->le);
	mem_deref(dev->name);
}

struct mdt_mediadev *mdt_mediadev_find(const struct list *dev_list,
					 const char *name)
{
	struct le *le;

	LIST_FOREACH(dev_list, le) {
		struct mdt_mediadev *dev = le->data;
		if (!str_casecmp(dev->name, name))
			return dev;
	}

	return NULL;
}

int mdt_mediadev_add(struct list *dev_list, const char *name)
{
	struct mdt_mediadev *dev;
	int err;

	if (!dev_list || !str_isset(name))
		return EINVAL;

	if (mdt_mediadev_find(dev_list, name))
		return 0;

	dev = mem_zalloc(sizeof(*dev), mediadev_destructor);
	if (!dev)
		return ENOMEM;

	err = str_dup(&dev->name, name);
	if (err) {
		mem_deref(dev);
		return err;
	}

	list_append(dev_list, &dev->le, dev);
	return 0;
}
