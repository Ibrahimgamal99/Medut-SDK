/**
 * @file compat_contract_test.c  The plug-in contract the stack must honour
 *
 * Medut's audio drivers (platform/android/sles_vc.c, platform/<os>/ for
 * pulse, coreaudio, wasapi and audiounit, src/audio/external.c and null.c),
 * its audio filters (src/audio/processing.c, tap.c) and the ICE media-NAT
 * plug into the stack through src/core/mdt_mod.h: mdt_ausrc / mdt_auplay
 * / mdt_aufilt / mdt_mnat.  Matching the names is the easy part.  What the
 * plug-ins actually depend on is behaviour:
 *
 *   L  layout   — the prm / frame / filter-state structs the plug-ins read
 *                 and write (compile-time pins below);
 *   F  frames   — what a driver is handed in prm, what frames it must
 *                 produce, what frames it is handed back;
 *   T  threads  — which thread the stack calls each handler on;
 *   O  lifetime — alloc and mem_deref are paired, both on re_main, and a
 *                 device is released when its stream ends;
 *   E  errors   — a driver error (errh, err != 0) ends the call, with the
 *                 driver's text as the reason.
 *
 * This file pins all five with a fake device and a fake filter, through a
 * real call: two accounts in one SDK instance call each other over UDP on
 * this host's own address (PCMU, no encryption, no ICE, no registrar), so it
 * runs offline and in CI.  It is written against the plug-in API only.  The
 * contract was first observed on baresip, the engine the drivers were
 * written for, and the stack keeps it:
 *   - errh must be called on re_main: the stack closes the call synchronously
 *     inside it.  None of Medut's drivers call errh today.
 *   - A device's alloc and its destructor (mem_deref) both run on re_main;
 *     the destructor must stop the device thread before it returns.
 *   - The device thread calls rh / wh; frames are S16LE at prm's rate and
 *     channels, prm.srate * prm.ch * prm.ptime / 1000 samples each.
 *   - Filter encupd / decupd run on re_main; ench on the TX (pacing)
 *     thread, never re_main; dech on re_main.  Frames match the
 *     mdt_aufilt_prm their update announced.
 *
 * Build (from repo root, after scripts/build-linux.sh):
 *   gcc -g -O1 -std=gnu11 -Iinclude -Isrc -Idist/linux/x86_64/include \
 *       -Idist/linux/x86_64/include/re test/compat_contract_test.c \
 *       -Wl,--whole-archive dist/linux/x86_64/medut.a -Wl,--no-whole-archive \
 *       -lssl -lcrypto -lz -lpthread -lm -lresolv -ldl -lstdc++ -lpulse \
 *       -o test/compat_contract_test && ./test/compat_contract_test
 *
 * Exit 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include "../src/core/medut_internal.h"
#include <rem.h>   /* struct auframe — the plug-ins include it the same way */

/* ── L: layout pins ──────────────────────────────────────────────────────── */

#define PIN(T, f, sz) \
	_Static_assert(sizeof(((T *)0)->f) == (sz), #T "." #f " size")

PIN(struct mdt_ausrc_prm, srate,    4);
PIN(struct mdt_ausrc_prm, ch,       1);
PIN(struct mdt_ausrc_prm, ptime,    4);
PIN(struct mdt_ausrc_prm, fmt,      sizeof(int));
PIN(struct mdt_ausrc_prm, duration, sizeof(size_t));
PIN(struct mdt_auplay_prm, srate,   4);
PIN(struct mdt_auplay_prm, ch,      1);
PIN(struct mdt_auplay_prm, ptime,   4);
PIN(struct mdt_auplay_prm, fmt,     sizeof(int));
PIN(struct mdt_aufilt_prm, srate,   4);
PIN(struct mdt_aufilt_prm, ch,      1);
PIN(struct mdt_aufilt_prm, fmt,     sizeof(int));
PIN(struct auframe, srate,      4);
PIN(struct auframe, sampv,      sizeof(void *));
PIN(struct auframe, sampc,      sizeof(size_t));
PIN(struct auframe, timestamp,  8);
PIN(struct auframe, level,      sizeof(double));
PIN(struct auframe, ch,         1);
/* Filters embed the engine's state header first and cast back to it. */
_Static_assert(offsetof(struct mdt_aufilt_enc_st, af) == 0, "enc_st.af first");
_Static_assert(offsetof(struct mdt_aufilt_dec_st, af) == 0, "dec_st.af first");

/* ── Test plumbing ───────────────────────────────────────────────────────── */

static int g_pass, g_fail;

/* stdout, not stderr: medut_init() takes stderr over. */
#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("FAIL(line %d): ", __LINE__); \
	       printf(__VA_ARGS__); printf("\n"); fflush(stdout); } \
} while (0)

static thrd_t g_re_tid;

static void get_tid_fn(void *arg)
{
	*(thrd_t *)arg = thrd_current();
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ── Fake device ─────────────────────────────────────────────────────────── */

#define TONE_HZ   440.0
#define TONE_AMP  8000.0
#define MAX_INST  16

/* One record per device instance, kept after the instance is freed so the
 * results can be read once the call is over. */
struct inst_rec {
	bool       src;
	bool       used;
	thrd_t     alloc_tid, dtor_tid;
	bool       dtor_ran;
	uint32_t   srate, ptime;
	uint8_t    ch;
	int        fmt;
	atomic_uint frames;            /* rh / wh calls                  */
	atomic_uint bad_frames;        /* wh frames that broke the rules */
	atomic_uint late_cb;           /* callbacks after stop           */
	/* player analysis over the frames after the first second */
	atomic_ullong crossings, samples, sq_sum;
	mdt_ausrc_error_h *errh;
	void          *arg;
};

static struct inst_rec g_inst[MAX_INST];
static atomic_int g_ninst;
static atomic_int g_src_alloc, g_src_free, g_play_alloc, g_play_free;

struct dev {
	struct inst_rec *rec;
	thrd_t           thr;
	atomic_bool      stop;
	bool             started;
	int16_t         *buf;
	size_t           sampc;
	uint32_t         ptime;
};

struct mdt_ausrc_st {
	struct dev      d;          /* first: shared by the device thread */
	mdt_ausrc_read_h   *rh;
	void           *arg;
	double          phase;
};

struct mdt_auplay_st {
	struct dev      d;
	mdt_auplay_write_h *wh;
	void           *arg;
	int16_t         last;
};

static struct inst_rec *inst_new(bool src)
{
	int i = atomic_fetch_add(&g_ninst, 1);
	if (i >= MAX_INST)
		return NULL;
	memset(&g_inst[i], 0, sizeof(g_inst[i]));
	g_inst[i].src  = src;
	g_inst[i].used = true;
	return &g_inst[i];
}

/* Paced device loop: one callback every ptime, scheduled on an absolute
 * clock so the rate does not drift. */
static int src_thread(void *arg)
{
	struct mdt_ausrc_st *st = arg;
	struct timespec next;
	const double step = 2.0 * M_PI * TONE_HZ / st->d.rec->srate;

	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!atomic_load(&st->d.stop)) {
		struct auframe af;

		for (size_t i = 0; i < st->d.sampc; i += st->d.rec->ch) {
			int16_t v = (int16_t)(TONE_AMP * sin(st->phase));
			for (uint8_t c = 0; c < st->d.rec->ch; c++)
				st->d.buf[i + c] = v;
			st->phase += step;
		}
		auframe_init(&af, AUFMT_S16LE, st->d.buf, st->d.sampc,
		             st->d.rec->srate, st->d.rec->ch);
		st->rh(&af, st->arg);
		atomic_fetch_add(&st->d.rec->frames, 1);

		next.tv_nsec += (long)st->d.ptime * 1000000L;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return 0;
}

static int play_thread(void *arg)
{
	struct mdt_auplay_st *st = arg;
	struct inst_rec *r = st->d.rec;
	struct timespec next;
	unsigned n = 0;
	const unsigned skip = 1000 / st->d.ptime;   /* first second */

	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!atomic_load(&st->d.stop)) {
		struct auframe af;

		memset(st->d.buf, 0, st->d.sampc * sizeof(int16_t));
		auframe_init(&af, AUFMT_S16LE, st->d.buf, st->d.sampc,
		             r->srate, r->ch);
		st->wh(&af, st->arg);
		atomic_fetch_add(&r->frames, 1);

		/* F: the engine fills exactly the frame it was handed. */
		if (af.fmt != AUFMT_S16LE || af.sampc != st->d.sampc ||
		    af.sampv != st->d.buf)
			atomic_fetch_add(&r->bad_frames, 1);

		if (++n > skip) {
			uint64_t cross = 0, sq = 0;
			for (size_t i = 0; i < st->d.sampc; i += r->ch) {
				int16_t v = st->d.buf[i];
				if ((v >= 0) != (st->last >= 0))
					cross++;
				st->last = v;
				sq += (uint64_t)((int32_t)v * v);
			}
			atomic_fetch_add(&r->crossings, cross);
			atomic_fetch_add(&r->samples, st->d.sampc / r->ch);
			atomic_fetch_add(&r->sq_sum, sq);
		}

		next.tv_nsec += (long)st->d.ptime * 1000000L;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return 0;
}

static void dev_stop(struct dev *d)
{
	if (!d->started)
		return;
	atomic_store(&d->stop, true);
	thrd_join(d->thr, NULL);
	d->started = false;
}

static void src_destructor(void *arg)
{
	struct mdt_ausrc_st *st = arg;

	/* O: the destructor stops the device thread before it returns. */
	dev_stop(&st->d);
	st->d.rec->dtor_tid = thrd_current();
	st->d.rec->dtor_ran = true;
	st->d.rec->errh     = NULL;
	mem_deref(st->d.buf);
	atomic_fetch_add(&g_src_free, 1);
}

static void play_destructor(void *arg)
{
	struct mdt_auplay_st *st = arg;

	dev_stop(&st->d);
	st->d.rec->dtor_tid = thrd_current();
	st->d.rec->dtor_ran = true;
	mem_deref(st->d.buf);
	atomic_fetch_add(&g_play_free, 1);
}

static int src_alloc(struct mdt_ausrc_st **stp, const struct mdt_ausrc *as,
                     struct mdt_ausrc_prm *prm, const char *device,
                     mdt_ausrc_read_h *rh, mdt_ausrc_error_h *errh, void *arg)
{
	struct mdt_ausrc_st *st;
	struct inst_rec *r;
	(void)as; (void)device;

	if (!stp || !prm || !rh)
		return EINVAL;
	if (prm->fmt != AUFMT_S16LE)
		return ENOTSUP;
	r = inst_new(true);
	if (!r)
		return ENOMEM;

	r->alloc_tid = thrd_current();
	r->srate = prm->srate; r->ch = prm->ch; r->ptime = prm->ptime;
	r->fmt   = prm->fmt;
	r->errh  = errh;
	r->arg   = arg;

	st = mem_zalloc(sizeof(*st), src_destructor);
	if (!st)
		return ENOMEM;
	st->d.rec   = r;
	st->d.ptime = prm->ptime ? prm->ptime : 20;
	st->d.sampc = prm->srate * prm->ch * st->d.ptime / 1000;
	st->d.buf   = mem_zalloc(st->d.sampc * sizeof(int16_t), NULL);
	st->rh  = rh;
	st->arg = arg;
	if (!st->d.buf || thrd_create(&st->d.thr, src_thread, st) != thrd_success) {
		mem_deref(st);
		return ENOMEM;
	}
	st->d.started = true;
	atomic_fetch_add(&g_src_alloc, 1);
	*stp = st;
	return 0;
}

static int play_alloc(struct mdt_auplay_st **stp, const struct mdt_auplay *ap,
                      struct mdt_auplay_prm *prm, const char *device,
                      mdt_auplay_write_h *wh, void *arg)
{
	struct mdt_auplay_st *st;
	struct inst_rec *r;
	(void)ap; (void)device;

	if (!stp || !prm || !wh)
		return EINVAL;
	if (prm->fmt != AUFMT_S16LE)
		return ENOTSUP;
	r = inst_new(false);
	if (!r)
		return ENOMEM;

	r->alloc_tid = thrd_current();
	r->srate = prm->srate; r->ch = prm->ch; r->ptime = prm->ptime;
	r->fmt   = prm->fmt;

	st = mem_zalloc(sizeof(*st), play_destructor);
	if (!st)
		return ENOMEM;
	st->d.rec   = r;
	st->d.ptime = prm->ptime ? prm->ptime : 20;
	st->d.sampc = prm->srate * prm->ch * st->d.ptime / 1000;
	st->d.buf   = mem_zalloc(st->d.sampc * sizeof(int16_t), NULL);
	st->wh  = wh;
	st->arg = arg;
	if (!st->d.buf || thrd_create(&st->d.thr, play_thread, st) != thrd_success) {
		mem_deref(st);
		return ENOMEM;
	}
	st->d.started = true;
	atomic_fetch_add(&g_play_alloc, 1);
	*stp = st;
	return 0;
}

/* ── Fake filter ─────────────────────────────────────────────────────────── */

static atomic_int g_encupd_n, g_decupd_n, g_enc_off_re, g_dec_on_re;
static atomic_uint g_ench_n, g_dech_n, g_ench_bad, g_dech_bad;
static atomic_int g_encupd_off_re, g_decupd_off_re;

struct probe_enc { struct mdt_aufilt_enc_st af; struct mdt_aufilt_prm prm; };
struct probe_dec { struct mdt_aufilt_dec_st af; struct mdt_aufilt_prm prm; };

static int probe_encupd(struct mdt_aufilt_enc_st **stp, void **ctx,
                        const struct mdt_aufilt *af, struct mdt_aufilt_prm *prm,
                        const struct mdt_audio *au)
{
	struct probe_enc *st;
	(void)ctx; (void)af; (void)au;

	if (!stp || !prm)
		return EINVAL;
	atomic_fetch_add(&g_encupd_n, 1);
	if (!thrd_equal(thrd_current(), g_re_tid))
		atomic_fetch_add(&g_encupd_off_re, 1);
	if (*stp)
		return 0;
	st = mem_zalloc(sizeof(*st), NULL);
	if (!st)
		return ENOMEM;
	st->prm = *prm;
	*stp = (struct mdt_aufilt_enc_st *)st;
	return 0;
}

static int probe_ench(struct mdt_aufilt_enc_st *st, struct auframe *af)
{
	struct probe_enc *p = (struct probe_enc *)st;

	atomic_fetch_add(&g_ench_n, 1);
	if (thrd_equal(thrd_current(), g_re_tid))
		atomic_fetch_add(&g_enc_off_re, 1);   /* counted when ON re */
	if (af->fmt != AUFMT_S16LE || af->srate != p->prm.srate ||
	    af->ch != p->prm.ch)
		atomic_fetch_add(&g_ench_bad, 1);
	return 0;
}

static int probe_decupd(struct mdt_aufilt_dec_st **stp, void **ctx,
                        const struct mdt_aufilt *af, struct mdt_aufilt_prm *prm,
                        const struct mdt_audio *au)
{
	struct probe_dec *st;
	(void)ctx; (void)af; (void)au;

	if (!stp || !prm)
		return EINVAL;
	atomic_fetch_add(&g_decupd_n, 1);
	if (!thrd_equal(thrd_current(), g_re_tid))
		atomic_fetch_add(&g_decupd_off_re, 1);
	if (*stp)
		return 0;
	st = mem_zalloc(sizeof(*st), NULL);
	if (!st)
		return ENOMEM;
	st->prm = *prm;
	*stp = (struct mdt_aufilt_dec_st *)st;
	return 0;
}

static int probe_dech(struct mdt_aufilt_dec_st *st, struct auframe *af)
{
	struct probe_dec *p = (struct probe_dec *)st;

	atomic_fetch_add(&g_dech_n, 1);
	if (thrd_equal(thrd_current(), g_re_tid))
		atomic_fetch_add(&g_dec_on_re, 1);
	if (af->fmt != AUFMT_S16LE || af->srate != p->prm.srate ||
	    af->ch != p->prm.ch)
		atomic_fetch_add(&g_dech_bad, 1);
	return 0;
}

static struct mdt_aufilt g_probe = {
	.name    = "compat_probe",
	.encupdh = probe_encupd,
	.ench    = probe_ench,
	.decupdh = probe_decupd,
	.dech    = probe_dech,
};

/* ── Registration on re_main ─────────────────────────────────────────────── */

static struct mdt_ausrc  *g_ausrc;
static struct mdt_auplay *g_auplay;

static void plugins_register_fn(void *arg)
{
	int *err = arg;

	*err  = mdt_ausrc_register(&g_ausrc, mdt_ausrcl(), "compat_fake",
	                       src_alloc);
	*err |= mdt_auplay_register(&g_auplay, mdt_auplayl(), "compat_fake",
	                        play_alloc);
	mdt_aufilt_register(mdt_aufiltl(), &g_probe);
	if (!*err)
		*err = mdt_audio_use_driver("compat_fake");
	bsdk_audio_external_sync();
}

static void plugins_unregister_fn(void *arg)
{
	(void)arg;
	mdt_aufilt_unregister(&g_probe);
	g_ausrc  = mem_deref(g_ausrc);
	g_auplay = mem_deref(g_auplay);
}

static struct inst_rec *g_fail_inst;

static void fire_errh_fn(void *arg)
{
	int *fired = arg;

	*fired = 0;
	for (int i = 0; i < atomic_load(&g_ninst) && i < MAX_INST; i++) {
		struct inst_rec *r = &g_inst[i];
		if (r->src && !r->dtor_ran && r->errh) {
			g_fail_inst = r;
			r->errh(EIO, "compat: device failed", r->arg);
			*fired = 1;
			return;
		}
	}
}

/* ── Calls ───────────────────────────────────────────────────────────────── */

static mtx_t g_ev_lock;
static medut_call_handle_t g_call_a, g_call_b;
static int  g_state_a = -1, g_state_b = -1;
static char g_reason_a[160], g_reason_b[160];

static void ev_handler(const medut_event_t *ev, void *ud)
{
	(void)ud;

	switch (ev->type) {
	case MEDUT_EV_LOG:
		if (getenv("V"))
			printf("[log] %s", ev->u.log.message);
		break;

	case MEDUT_EV_INCOMING_CALL:
		mtx_lock(&g_ev_lock);
		g_call_b  = ev->u.incoming.call;
		g_state_b = MEDUT_CALL_RINGING;
		mtx_unlock(&g_ev_lock);
		(void)medut_call_answer(ev->u.incoming.call);
		break;

	case MEDUT_EV_CALL_STATE:
		mtx_lock(&g_ev_lock);
		if (ev->u.call_state.call == g_call_a && g_call_a) {
			g_state_a = ev->u.call_state.state;
			if (ev->u.call_state.reason)
				snprintf(g_reason_a, sizeof g_reason_a, "%s",
				         ev->u.call_state.reason);
		}
		else if (ev->u.call_state.call == g_call_b && g_call_b) {
			g_state_b = ev->u.call_state.state;
			if (ev->u.call_state.reason)
				snprintf(g_reason_b, sizeof g_reason_b, "%s",
				         ev->u.call_state.reason);
		}
		mtx_unlock(&g_ev_lock);
		break;

	default:
		break;
	}
}

static int state_of(int *s)
{
	int v;
	mtx_lock(&g_ev_lock);
	v = *s;
	mtx_unlock(&g_ev_lock);
	return v;
}

static bool wait_states(int want_a, int want_b, unsigned ms)
{
	for (unsigned t = 0; t < ms; t += 20) {
		int a = state_of(&g_state_a), b = state_of(&g_state_b);
		bool ok_a = want_a < 0 ? a >= MEDUT_CALL_ENDED : a == want_a;
		bool ok_b = want_b < 0 ? b >= MEDUT_CALL_ENDED : b == want_b;
		if (ok_a && ok_b)
			return true;
		sleep_ms(20);
	}
	return false;
}

static void reset_calls(void)
{
	mtx_lock(&g_ev_lock);
	g_call_a = g_call_b = NULL;
	g_state_a = g_state_b = -1;
	g_reason_a[0] = g_reason_b[0] = '\0';
	mtx_unlock(&g_ev_lock);
}

/* ── Scenarios ───────────────────────────────────────────────────────────── */

static uint16_t g_port;
static char     g_ip[64];   /* this host's primary IPv4: engines keep
                             * loopback out of their address lists */

static int place_call(medut_account_handle_t a)
{
	char dial[128];
	medut_call_handle_t h = NULL;
	int err;

	snprintf(dial, sizeof dial, "sip:b@%s:%u", g_ip, g_port);
	mtx_lock(&g_ev_lock);
	err = medut_call_invite(a, dial, &h);
	g_call_a  = h;
	g_state_a = MEDUT_CALL_CALLING;
	mtx_unlock(&g_ev_lock);
	return err;
}

static void check_threads_and_frames(int first, int last)
{
	for (int i = first; i < last && i < MAX_INST; i++) {
		struct inst_rec *r = &g_inst[i];
		const char *k = r->src ? "mdt_ausrc" : "mdt_auplay";

		/* T/O: alloc and destructor on re_main. */
		CHECK(thrd_equal(r->alloc_tid, g_re_tid),
		      "%s[%d] alloc not on re_main", k, i);
		CHECK(r->dtor_ran, "%s[%d] never freed", k, i);
		if (r->dtor_ran)
			CHECK(thrd_equal(r->dtor_tid, g_re_tid),
			      "%s[%d] destructor not on re_main", k, i);

		/* F: what the driver is asked for. */
		CHECK(r->fmt == AUFMT_S16LE, "%s[%d] fmt %d", k, i, r->fmt);
		CHECK(r->srate >= 8000 && r->srate <= 48000,
		      "%s[%d] srate %u", k, i, r->srate);
		CHECK(r->ch >= 1 && r->ch <= 2, "%s[%d] ch %u", k, i, r->ch);
		CHECK(r->ptime >= 10 && r->ptime <= 60,
		      "%s[%d] ptime %u", k, i, r->ptime);
		CHECK(atomic_load(&r->frames) > 50,
		      "%s[%d] only %u frames", k, i, atomic_load(&r->frames));

		if (!r->src) {
			unsigned long long n = atomic_load(&r->samples);
			double secs = n ? (double)n / r->srate : 0;
			double hz   = secs ? atomic_load(&r->crossings) / 2.0 / secs
			                   : 0;
			double rms  = n ? sqrt((double)atomic_load(&r->sq_sum) / n)
			                : 0;

			CHECK(atomic_load(&r->bad_frames) == 0,
			      "mdt_auplay[%d] %u frames broke the rules", i,
			      atomic_load(&r->bad_frames));
			/* F: the far end's tone arrives — the whole pipeline
			 * (filters, encoder, RTP, jitter buffer, decoder). */
			CHECK(fabs(hz - TONE_HZ) < 40.0,
			      "mdt_auplay[%d] heard %.0f Hz, want %.0f", i, hz, TONE_HZ);
			CHECK(rms > TONE_AMP / 8.0,
			      "mdt_auplay[%d] rms %.0f, tone too quiet", i, rms);
			printf("     mdt_auplay[%d] %u Hz x%u ptime %u: tone %.0f Hz "
			       "rms %.0f over %.1f s\n", i, r->srate, r->ch,
			       r->ptime, hz, rms, secs);
		}
		else {
			printf("     mdt_ausrc[%d]  %u Hz x%u ptime %u: %u frames\n",
			       i, r->srate, r->ch, r->ptime,
			       atomic_load(&r->frames));
		}
	}
}

static void scenario_call(medut_account_handle_t a)
{
	int first = atomic_load(&g_ninst);
	int err;

	printf("S1. loopback call: frames, threads, lifetime\n");
	reset_calls();
	err = place_call(a);
	CHECK(err == 0, "invite failed: %d", err);
	CHECK(wait_states(MEDUT_CALL_ESTABLISHED, MEDUT_CALL_ESTABLISHED,
	                  10000), "call did not establish (a=%d b=%d)",
	      state_of(&g_state_a), state_of(&g_state_b));

	sleep_ms(3500);
	CHECK(atomic_load(&g_src_alloc) - atomic_load(&g_src_free) == 2,
	      "expected 2 live sources, have %d",
	      atomic_load(&g_src_alloc) - atomic_load(&g_src_free));
	CHECK(atomic_load(&g_play_alloc) - atomic_load(&g_play_free) == 2,
	      "expected 2 live players, have %d",
	      atomic_load(&g_play_alloc) - atomic_load(&g_play_free));

	CHECK(medut_call_hangup(g_call_a) == 0, "hangup failed");
	CHECK(wait_states(-1, -1, 10000), "call did not end (a=%d b=%d)",
	      state_of(&g_state_a), state_of(&g_state_b));
	sleep_ms(300);

	/* O: every device the call opened was released with it. */
	CHECK(atomic_load(&g_src_alloc) == atomic_load(&g_src_free),
	      "sources: %d alloc, %d free", atomic_load(&g_src_alloc),
	      atomic_load(&g_src_free));
	CHECK(atomic_load(&g_play_alloc) == atomic_load(&g_play_free),
	      "players: %d alloc, %d free", atomic_load(&g_play_alloc),
	      atomic_load(&g_play_free));

	check_threads_and_frames(first, atomic_load(&g_ninst));

	/* T: filter update handlers on re_main; the encoder off it. */
	CHECK(atomic_load(&g_encupd_n) > 0, "filter encupd never called");
	CHECK(atomic_load(&g_decupd_n) > 0, "filter decupd never called");
	CHECK(atomic_load(&g_encupd_off_re) == 0,
	      "encupd ran off re_main %d times", atomic_load(&g_encupd_off_re));
	CHECK(atomic_load(&g_decupd_off_re) == 0,
	      "decupd ran off re_main %d times", atomic_load(&g_decupd_off_re));
	CHECK(atomic_load(&g_ench_n) > 100, "ench %u calls",
	      atomic_load(&g_ench_n));
	CHECK(atomic_load(&g_dech_n) > 100, "dech %u calls",
	      atomic_load(&g_dech_n));
	CHECK(atomic_load(&g_enc_off_re) == 0,
	      "ench ran on re_main %d times (must be the TX thread)",
	      atomic_load(&g_enc_off_re));
	/* T: decoding (and so the decode-side filters) runs on re_main, paced
	 * by the jitter buffer — the model the playout tuning assumes. */
	CHECK((unsigned)atomic_load(&g_dec_on_re) == atomic_load(&g_dech_n),
	      "dech ran off re_main %u of %u times",
	      atomic_load(&g_dech_n) - (unsigned)atomic_load(&g_dec_on_re),
	      atomic_load(&g_dech_n));
	/* F: filters see frames in the format their update announced. */
	CHECK(atomic_load(&g_ench_bad) == 0, "ench: %u off-format frames",
	      atomic_load(&g_ench_bad));
	CHECK(atomic_load(&g_dech_bad) == 0, "dech: %u off-format frames",
	      atomic_load(&g_dech_bad));
	printf("     filter: encupd %d decupd %d ench %u dech %u "
	       "(dech on re_main: %d)\n",
	       atomic_load(&g_encupd_n), atomic_load(&g_decupd_n),
	       atomic_load(&g_ench_n), atomic_load(&g_dech_n),
	       atomic_load(&g_dec_on_re));
}

static void scenario_errh(medut_account_handle_t a)
{
	int fired = 0;

	printf("S2. driver error ends the call\n");
	reset_calls();
	CHECK(place_call(a) == 0, "invite failed");
	CHECK(wait_states(MEDUT_CALL_ESTABLISHED, MEDUT_CALL_ESTABLISHED,
	                  10000), "call did not establish (a=%d b=%d)",
	      state_of(&g_state_a), state_of(&g_state_b));
	sleep_ms(500);

	/* E: errh on re_main — the only thread it is defined on. */
	CHECK(bsdk_dispatch_sync(fire_errh_fn, &fired) == 0 && fired,
	      "no live source to fail");
	CHECK(wait_states(-1, -1, 10000), "call did not end after errh "
	      "(a=%d b=%d)", state_of(&g_state_a), state_of(&g_state_b));
	sleep_ms(300);

	mtx_lock(&g_ev_lock);
	CHECK(strstr(g_reason_a, "compat: device failed") ||
	      strstr(g_reason_b, "compat: device failed"),
	      "driver text not in the reason (a='%s' b='%s')",
	      g_reason_a, g_reason_b);
	printf("     a: state %d '%s'   b: state %d '%s'\n", g_state_a,
	       g_reason_a, g_state_b, g_reason_b);
	mtx_unlock(&g_ev_lock);

	CHECK(g_fail_inst && g_fail_inst->dtor_ran,
	      "failed device not released");
	CHECK(atomic_load(&g_src_alloc) == atomic_load(&g_src_free) &&
	      atomic_load(&g_play_alloc) == atomic_load(&g_play_free),
	      "devices leaked after the error (src %d/%d play %d/%d)",
	      atomic_load(&g_src_alloc), atomic_load(&g_src_free),
	      atomic_load(&g_play_alloc), atomic_load(&g_play_free));
}

/* ── main ────────────────────────────────────────────────────────────────── */

static int make_account(const char *user, medut_account_handle_t *out)
{
	char uri[128], ob[128];
	medut_account_config_t a;

	memset(&a, 0, sizeof a);
	snprintf(uri, sizeof uri, "sip:%s@%s", user, g_ip);
	snprintf(ob, sizeof ob, "sip:%s:%u;transport=udp", g_ip, g_port);
	a.uri      = uri;
	a.password = "x";
	a.outbound = ob;
	a.audio_codec_name_count = 1;
	snprintf(a.audio_codec_names[0], sizeof a.audio_codec_names[0], "PCMU");
	return medut_account_create(&a, out);
}

int main(void)
{
	medut_config_t cfg;
	medut_account_handle_t acct_a = NULL, acct_b = NULL;
	int err = 0;

	mtx_init(&g_ev_lock, mtx_plain);
	g_port = (uint16_t)(20000 + getpid() % 20000);
	{
		struct sa sa;
		if (net_default_source_addr_get(AF_INET, &sa)) {
			printf("SKIP: no IPv4 address to call over\n");
			return 0;
		}
		re_snprintf(g_ip, sizeof g_ip, "%j", &sa);
	}

	medut_config_init(&cfg);
	cfg.event_cb   = ev_handler;
	cfg.log_level  = getenv("V") ? 3 : 0;
	cfg.local_ip   = g_ip;
	cfg.local_port = g_port;
	cfg.stats_interval_ms = 0;
	/* Nothing between the fake source and the wire that changes a tone. */
	cfg.aec_mode = MEDUT_AEC_OFF;
	cfg.ns  = false;
	cfg.agc = false;

	err = medut_init(&cfg);
	CHECK(err == 0, "medut_init: %d", err);
	if (err)
		return 1;

	CHECK(bsdk_dispatch_sync(get_tid_fn, &g_re_tid) == 0, "re tid");
	CHECK(bsdk_dispatch_sync(plugins_register_fn, &err) == 0 && err == 0,
	      "plug-in registration / driver selection: %d", err);

	CHECK(make_account("a", &acct_a) == 0, "account a");
	CHECK(make_account("b", &acct_b) == 0, "account b");

	if (!g_fail) {
		scenario_call(acct_a);
		scenario_errh(acct_a);
	}

	bsdk_dispatch_sync(plugins_unregister_fn, NULL);
	medut_shutdown();

	printf("compat_contract_test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
