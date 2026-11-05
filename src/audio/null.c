/**
 * @file null.c  Medut stack: the silent audio device ("null")
 *
 * For a host with no audio hardware the SDK drives (desktop builds, tests,
 * servers): the source produces silence and the player discards what it is
 * given, both paced in real time by their own thread, exactly like a sound
 * card.  The pacing is the point — with no frames coming in, a call would
 * send no RTP, and a PBX that latches media on the first packet (Asterisk's
 * rtp_symmetric) would never send any back.
 *
 * Contract (core/mdt_mod.h): alloc and destructor on re_main; the
 * destructor joins the device thread before it returns; rh / wh run on the
 * device thread with prm-sized S16LE frames.
 */

#include <string.h>
#include "../core/mdt_stack.h"
#include <rem.h>

struct dev {
	thrd_t       thr;
	RE_ATOMIC bool run;
	bool         started;
	int16_t     *buf;
	size_t       sampc;
	uint32_t     srate;
	uint8_t      ch;
	uint32_t     ptime;
};

struct mdt_ausrc_st {
	struct dev          d;
	mdt_ausrc_read_h  *rh;
	void               *arg;
};

struct mdt_auplay_st {
	struct dev           d;
	mdt_auplay_write_h *wh;
	void                *arg;
};

static struct mdt_ausrc  *s_ausrc;
static struct mdt_auplay *s_auplay;

/* Sleep until `deadline` (tmr_jiffies_usec() time): an absolute deadline,
 * so the pacing does not drift by the time each frame took.  libre's clock
 * and sleep, not clock_nanosleep(TIMER_ABSTIME), which Apple platforms and
 * Windows do not have. */
static void sleep_until(uint64_t deadline)
{
	const uint64_t now = tmr_jiffies_usec();

	if (deadline > now)
		(void)sys_usleep((unsigned int)(deadline - now));
}

static int src_thread(void *arg)
{
	struct mdt_ausrc_st *st = arg;
	uint64_t t = tmr_jiffies_usec();

	while (re_atomic_rlx(&st->d.run)) {
		struct auframe af;

		memset(st->d.buf, 0, st->d.sampc * sizeof(int16_t));
		auframe_init(&af, AUFMT_S16LE, st->d.buf, st->d.sampc,
			     st->d.srate, st->d.ch);
		st->rh(&af, st->arg);

		t += (uint64_t)st->d.ptime * 1000;
		sleep_until(t);
	}
	return 0;
}

static int play_thread(void *arg)
{
	struct mdt_auplay_st *st = arg;
	uint64_t t = tmr_jiffies_usec();

	while (re_atomic_rlx(&st->d.run)) {
		struct auframe af;

		auframe_init(&af, AUFMT_S16LE, st->d.buf, st->d.sampc,
			     st->d.srate, st->d.ch);
		st->wh(&af, st->arg);

		t += (uint64_t)st->d.ptime * 1000;
		sleep_until(t);
	}
	return 0;
}

static void dev_stop(struct dev *d)
{
	if (!d->started)
		return;
	re_atomic_rlx_set(&d->run, false);
	thrd_join(d->thr, NULL);
	d->started = false;
}

static int dev_init(struct dev *d, uint32_t srate, uint8_t ch,
		    uint32_t ptime, int fmt)
{
	if (fmt != AUFMT_S16LE || !srate || !ch)
		return ENOTSUP;

	d->srate = srate;
	d->ch    = ch;
	d->ptime = ptime ? ptime : 20;
	d->sampc = (size_t)srate * ch * d->ptime / 1000;
	d->buf   = mem_zalloc(d->sampc * sizeof(int16_t), NULL);

	return d->buf ? 0 : ENOMEM;
}

static void src_destructor(void *arg)
{
	struct mdt_ausrc_st *st = arg;

	dev_stop(&st->d);
	mem_deref(st->d.buf);
}

static void play_destructor(void *arg)
{
	struct mdt_auplay_st *st = arg;

	dev_stop(&st->d);
	mem_deref(st->d.buf);
}

static int src_alloc(struct mdt_ausrc_st **stp, const struct mdt_ausrc *as,
		     struct mdt_ausrc_prm *prm, const char *device,
		     mdt_ausrc_read_h *rh, mdt_ausrc_error_h *errh,
		     void *arg)
{
	struct mdt_ausrc_st *st;
	int err;
	(void)as; (void)device; (void)errh;

	if (!stp || !prm || !rh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), src_destructor);
	if (!st)
		return ENOMEM;

	st->rh  = rh;
	st->arg = arg;
	err = dev_init(&st->d, prm->srate, prm->ch, prm->ptime, prm->fmt);
	if (!err) {
		re_atomic_rlx_set(&st->d.run, true);
		err = thrd_create(&st->d.thr, src_thread, st) == thrd_success
		    ? 0 : ENOMEM;
		st->d.started = !err;
	}
	if (err) {
		mem_deref(st);
		return err;
	}

	*stp = st;
	return 0;
}

static int play_alloc(struct mdt_auplay_st **stp,
		      const struct mdt_auplay *ap,
		      struct mdt_auplay_prm *prm, const char *device,
		      mdt_auplay_write_h *wh, void *arg)
{
	struct mdt_auplay_st *st;
	int err;
	(void)ap; (void)device;

	if (!stp || !prm || !wh)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), play_destructor);
	if (!st)
		return ENOMEM;

	st->wh  = wh;
	st->arg = arg;
	err = dev_init(&st->d, prm->srate, prm->ch, prm->ptime, prm->fmt);
	if (!err) {
		re_atomic_rlx_set(&st->d.run, true);
		err = thrd_create(&st->d.thr, play_thread, st) == thrd_success
		    ? 0 : ENOMEM;
		st->d.started = !err;
	}
	if (err) {
		mem_deref(st);
		return err;
	}

	*stp = st;
	return 0;
}

int mdt_null_init(void)
{
	int err;

	err = mdt_ausrc_register(&s_ausrc, mdt_ausrcl(), "null", src_alloc);
	if (!err)
		err = mdt_auplay_register(&s_auplay, mdt_auplayl(), "null",
					   play_alloc);
	return err;
}

void mdt_null_close(void)
{
	s_ausrc  = mem_deref(s_ausrc);
	s_auplay = mem_deref(s_auplay);
}
