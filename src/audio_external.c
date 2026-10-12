/**
 * @file audio_external.c  App-owned audio device ("external")
 *
 * Hands the microphone and the speaker back to the host app.  With this driver
 * selected the SDK opens no capture or playback device of its own — no
 * OpenSL ES on Android, no AudioUnit on iOS — and the app moves PCM across the
 * boundary itself, from whatever the platform gives it (AudioRecord/AudioTrack,
 * AVAudioEngine, a WebRTC AudioDeviceModule, a test file):
 *
 *   app capture thread   --> medut_audio_external_push()  --> encoder --> RTP
 *   app playback thread  <-- medut_audio_external_pull()  <-- decoder <-- RTP
 *
 * That is the whole contract.  push() is what the far end hears; pull() is what
 * the local user hears.  Both are S16LE interleaved at the call's negotiated
 * rate and channel count, which medut_audio_external_format() reports once a
 * call has media.
 *
 * Threading
 * ─────────
 * push()/pull() are meant to be called from the app's own realtime audio
 * threads and are safe to call concurrently with each other.  They must not be
 * called from inside a Medut event callback: both take the lock that the
 * device teardown path also takes, and the callback thread may be the one
 * running that teardown.  Neither call blocks on the network or allocates on
 * the steady-state path.
 *
 * One device, one call
 * ────────────────────
 * baresip allocates a device per call, but the app has one microphone and one
 * speaker.  The most recently opened device of each kind wins — that is the
 * call the user is actually on.  A second concurrent call gets silence rather
 * than a share of the mic, which is the honest outcome: the app cannot capture
 * twice.
 *
 * The open devices are kept in a list rather than a single pointer so that
 * closing the newest one falls back to whichever is still open.  With a bare
 * pointer, ending the second of two calls left the first one alive but no
 * longer selected, and the mic never came back for the rest of the session.
 */

#include <string.h>
#include <re.h>
#include <rem.h>
#include "medut_internal.h"

/* Capture backlog before the oldest audio is dropped.  The app pushes on its
 * own clock and the encoder drains on the call's; a little slack absorbs the
 * jitter between them, while a hard ceiling keeps a stalled encoder from
 * turning into unbounded latency and unbounded memory. */
#define EXT_BUF_MAX_MS 500

struct ausrc_st {
	struct le        le;      /* member of s_srcl, newest last */
	struct ausrc_prm prm;
	ausrc_read_h    *rh;
	void            *arg;
	struct aubuf    *ab;
	int16_t         *frame;   /* one ptime of samples, handed to rh */
	size_t           sampc;
};

struct auplay_st {
	struct le         le;     /* member of s_playl, newest last */
	struct auplay_prm prm;
	auplay_write_h   *wh;
	void             *arg;
};

static struct ausrc  *s_ausrc;
static struct auplay *s_auplay;

/* The lock is created once per process and never destroyed — see
 * bsdk_audio_external_close() for why. */
static mtx_t     s_lock;
static once_flag s_lock_once = ONCE_FLAG_INIT;

static RE_ATOMIC bool s_ready;
static struct list    s_srcl;    /* struct ausrc_st  */
static struct list    s_playl;   /* struct auplay_st */

static void lock_init(void)
{
	mtx_init(&s_lock, mtx_plain);
}

/* The device the app is actually talking to: the most recently opened one.
 * Both are called with s_lock held. */
static struct ausrc_st *cur_src(void)
{
	return list_ledata(list_tail(&s_srcl));
}

static struct auplay_st *cur_play(void)
{
	return list_ledata(list_tail(&s_playl));
}

static size_t nsamp_for(uint32_t srate, uint8_t ch, uint32_t ptime)
{
	return (size_t)srate * ch * (ptime ? ptime : 20) / 1000;
}

/* ── Capture (app -> SDK) ───────────────────────────────────────────────── */

static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	mtx_lock(&s_lock);
	list_unlink(&st->le);
	mtx_unlock(&s_lock);

	mem_deref(st->ab);
	mem_deref(st->frame);
}

static int ext_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
                         struct ausrc_prm *prm, const char *device,
                         ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err = 0;
	(void)as; (void)device; (void)errh;

	if (!stp || !prm || !rh)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("audio_external: capture: unsupported format (%s)\n",
		        aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->rh    = rh;
	st->arg   = arg;
	st->prm   = *prm;
	st->sampc = nsamp_for(prm->srate, prm->ch, prm->ptime);

	st->frame = mem_zalloc(st->sampc * 2, NULL);
	if (!st->frame) {
		err = ENOMEM;
		goto out;
	}

	/* min_sz must be exactly one frame, never more.  aubuf treats it as a
	 * pre-fill threshold: while it is unmet, aubuf_read_auframe() returns
	 * silence *without* draining, so a min_sz above one frame would let the
	 * drain loop in push() see a readable buffer that never shrinks. */
	err = aubuf_alloc(&st->ab, st->sampc * 2,
	                  nsamp_for(prm->srate, prm->ch, EXT_BUF_MAX_MS) * 2);
	if (err)
		goto out;

	/* aubuf defaults to live mode, where the first read discards the whole
	 * backlog down to one frame to cut latency.  That is right for a
	 * playback jitter buffer and wrong here: this is a plain capture FIFO,
	 * and dropping the backlog throws away microphone audio the app has
	 * already handed us. */
	aubuf_set_live(st->ab, false);

	mtx_lock(&s_lock);
	list_append(&s_srcl, &st->le, st);
	mtx_unlock(&s_lock);

	info("audio_external: capture open — app pushes %u Hz %u ch,"
	     " %zu samples per frame\n", prm->srate, prm->ch, st->sampc);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;
	return err;
}

int medut_audio_external_push(const int16_t *pcm, size_t nsamp)
{
	struct auframe af;
	int err;

	if (!pcm || !nsamp)
		return EINVAL;
	if (!re_atomic_rlx(&s_ready))
		return ENODEV;

	mtx_lock(&s_lock);

	struct ausrc_st *st = cur_src();
	if (!st) {
		/* No call is capturing. Not an error the app can act on — it
		 * simply pushed between calls — so say so quietly. */
		mtx_unlock(&s_lock);
		return ENODEV;
	}

	if (nsamp % st->prm.ch) {
		/* A partial frame would silently shift every later sample into
		 * the wrong channel, which sounds like garbage rather than like
		 * a bug.  Say so instead. */
		mtx_unlock(&s_lock);
		return EINVAL;
	}

	auframe_init(&af, AUFMT_S16LE, (void *)pcm, nsamp,
	             st->prm.srate, st->prm.ch);
	af.timestamp = tmr_jiffies_usec();

	err = aubuf_write_auframe(st->ab, &af);
	if (err) {
		mtx_unlock(&s_lock);
		return err;
	}

	/* Hand the encoder whole frames only; it is written for a device that
	 * delivers a fixed ptime, and the app's buffer size is its own business. */
	while (aubuf_cur_size(st->ab) >= st->sampc * 2) {

		size_t before = aubuf_cur_size(st->ab);

		auframe_init(&af, AUFMT_S16LE, st->frame, st->sampc,
		             st->prm.srate, st->prm.ch);
		aubuf_read_auframe(st->ab, &af);

		/* aubuf hands back silence without draining while it is still
		 * pre-filling.  Bail rather than spin: this runs on the app's
		 * realtime capture thread holding s_lock, so a spin here wedges
		 * the device teardown path with it. */
		if (aubuf_cur_size(st->ab) >= before)
			break;

		af.timestamp = tmr_jiffies_usec();

		st->rh(&af, st->arg);
	}

	mtx_unlock(&s_lock);
	return 0;
}

/* ── Playback (SDK -> app) ──────────────────────────────────────────────── */

static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	mtx_lock(&s_lock);
	list_unlink(&st->le);
	mtx_unlock(&s_lock);
}

static int ext_play_alloc(struct auplay_st **stp, const struct auplay *ap,
                          struct auplay_prm *prm, const char *device,
                          auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	(void)ap; (void)device;

	if (!stp || !prm || !wh)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("audio_external: playback: unsupported format (%s)\n",
		        aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->wh  = wh;
	st->arg = arg;
	st->prm = *prm;

	mtx_lock(&s_lock);
	list_append(&s_playl, &st->le, st);
	mtx_unlock(&s_lock);

	info("audio_external: playback open — app pulls %u Hz %u ch\n",
	     prm->srate, prm->ch);

	*stp = st;
	return 0;
}

int medut_audio_external_pull(int16_t *pcm, size_t nsamp)
{
	struct auframe af;

	if (!pcm || !nsamp)
		return EINVAL;

	/* Silence, not stale audio, whenever there is nothing to play: the app
	 * hands this straight to the speaker and must always get a full buffer
	 * back, call or no call. */
	memset(pcm, 0, nsamp * 2);

	if (!re_atomic_rlx(&s_ready))
		return ENODEV;

	mtx_lock(&s_lock);

	struct auplay_st *st = cur_play();
	if (!st) {
		mtx_unlock(&s_lock);
		return ENODEV;
	}

	if (nsamp % st->prm.ch) {
		mtx_unlock(&s_lock);
		return EINVAL;
	}

	auframe_init(&af, AUFMT_S16LE, pcm, nsamp, st->prm.srate, st->prm.ch);
	st->wh(&af, st->arg);

	mtx_unlock(&s_lock);
	return 0;
}

/* ── Negotiated format ──────────────────────────────────────────────────── */

int medut_audio_external_format(uint32_t *srate, uint8_t *ch, uint32_t *ptime)
{
	struct ausrc_st  *src;
	struct auplay_st *play;
	int err = ENODEV;

	if (!re_atomic_rlx(&s_ready))
		return ENODEV;

	mtx_lock(&s_lock);

	src  = cur_src();
	play = cur_play();

	/* Prefer the capture side; both are opened from the same negotiated
	 * codec, and playback alone still answers for a receive-only call. */
	if (src) {
		if (srate) *srate = src->prm.srate;
		if (ch)    *ch    = src->prm.ch;
		if (ptime) *ptime = src->prm.ptime ? src->prm.ptime : 20;
		err = 0;
	}
	else if (play) {
		if (srate) *srate = play->prm.srate;
		if (ch)    *ch    = play->prm.ch;
		if (ptime) *ptime = play->prm.ptime ? play->prm.ptime : 20;
		err = 0;
	}

	mtx_unlock(&s_lock);
	return err;
}

bool medut_audio_external_is_active(void)
{
	bool active;

	if (!re_atomic_rlx(&s_ready))
		return false;

	mtx_lock(&s_lock);
	active = (cur_src() != NULL || cur_play() != NULL);
	mtx_unlock(&s_lock);

	return active;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

int bsdk_audio_external_init(void)
{
	int err;

	if (re_atomic_rlx(&s_ready))
		return 0;

	call_once(&s_lock_once, lock_init);

	/* baresip_init() re-inits the device lists on every restart, so these
	 * registrations do not survive a shutdown/init cycle — which is exactly
	 * why close() has to clear s_ready and let us get back here. */
	err  = ausrc_register(&s_ausrc, baresip_ausrcl(), "external",
	                      ext_src_alloc);
	err |= auplay_register(&s_auplay, baresip_auplayl(), "external",
	                       ext_play_alloc);
	if (err) {
		s_ausrc  = mem_deref(s_ausrc);
		s_auplay = mem_deref(s_auplay);
		return err;
	}

	re_atomic_rlx_set(&s_ready, true);
	return 0;
}

void bsdk_audio_external_close(void)
{
	if (!re_atomic_rlx(&s_ready))
		return;

	/* Stop admitting callers first: everything below either frees state or
	 * takes the lock, and the app's realtime threads are outside our
	 * lifecycle — they can be mid-push() right now. */
	re_atomic_rlx_set(&s_ready, false);

	s_ausrc  = mem_deref(s_ausrc);
	s_auplay = mem_deref(s_auplay);

	mtx_lock(&s_lock);
	list_clear(&s_srcl);
	list_clear(&s_playl);
	mtx_unlock(&s_lock);

	/* s_lock is deliberately not destroyed.  A push() that cleared the
	 * s_ready check just before we ran is about to lock it, and it has no
	 * way to know we are tearing down; a destroyed mutex there is undefined
	 * behaviour on the app's audio thread.  One mutex per process is a
	 * cheaper price than that race. */
}
