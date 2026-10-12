/**
 * @file test_audio_external.c  Unit tests for the app-owned audio device
 *
 * Links the real audio_external.c and drives it through baresip's ausrc/auplay
 * API, exactly the way the audio stream does.  The device registry itself is
 * reimplemented here rather than linked: baresip_ausrcl() lives in the same
 * object as the player and recorder, so linking it drags in every audio
 * backend the sysroot was built with (pulse, opus, webrtc_aec, ...) and makes
 * the test's dependencies a function of the build profile.  The registry is a
 * name lookup and an indirect call; the behaviour under test is all in
 * audio_external.c.  So this links libre alone.
 *
 * The three cases that matter most are regressions, and each was a silent
 * failure in the field rather than a crash:
 *   - push() spun forever when the negotiated ptime was under 20 ms, wedging
 *     the app's realtime capture thread while it held the device lock,
 *   - ending the second of two concurrent calls took the microphone away for
 *     the rest of the session,
 *   - the driver did not survive a shutdown/init cycle, and use_external()
 *     kept returning success while the call had no audio at all.
 *
 * Build: make test_audio_external  (needs the linux-x86_64 sysroot)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "../../src/medut_internal.h"

static int g_pass, g_fail;

#define CHECK(cond, ...)                                                     \
	do {                                                                 \
		if (cond) { g_pass++; }                                      \
		else {                                                       \
			g_fail++;                                            \
			printf("  FAIL %s:%d: ", __func__, __LINE__);        \
			printf(__VA_ARGS__);                                 \
			printf("\n");                                        \
		}                                                            \
	} while (0)

/* ── baresip logging, standing in for libbaresip ───────────────────────── */

static void log_va(const char *fmt, va_list ap)
{
	if (getenv("VERBOSE"))
		re_vfprintf(stderr, fmt, ap);
}

void _info(bool safe, const char *fmt, ...)
{
	va_list ap;
	(void)safe;
	va_start(ap, fmt);
	log_va(fmt, ap);
	va_end(ap);
}

void _warning(bool safe, const char *fmt, ...)
{
	va_list ap;
	(void)safe;
	va_start(ap, fmt);
	log_va(fmt, ap);
	va_end(ap);
}

/* ── baresip device registry, standing in for libbaresip ───────────────── */

static struct list g_ausrcl  = LIST_INIT;
static struct list g_auplayl = LIST_INIT;

struct list *baresip_ausrcl(void)  { return &g_ausrcl; }
struct list *baresip_auplayl(void) { return &g_auplayl; }

static void ausrc_dtor(void *arg)
{
	struct ausrc *as = arg;
	list_unlink(&as->le);
}

static void auplay_dtor(void *arg)
{
	struct auplay *ap = arg;
	list_unlink(&ap->le);
}

int ausrc_register(struct ausrc **asp, struct list *ausrcl, const char *name,
                   ausrc_alloc_h *alloch)
{
	struct ausrc *as = mem_zalloc(sizeof(*as), ausrc_dtor);
	if (!as)
		return ENOMEM;

	list_append(ausrcl, &as->le, as);
	as->name   = name;
	as->alloch = alloch;

	*asp = as;
	return 0;
}

int auplay_register(struct auplay **pp, struct list *auplayl, const char *name,
                    auplay_alloc_h *alloch)
{
	struct auplay *ap = mem_zalloc(sizeof(*ap), auplay_dtor);
	if (!ap)
		return ENOMEM;

	list_append(auplayl, &ap->le, ap);
	ap->name   = name;
	ap->alloch = alloch;

	*pp = ap;
	return 0;
}

const struct ausrc *ausrc_find(const struct list *ausrcl, const char *name)
{
	for (struct le *le = list_head(ausrcl); le; le = le->next) {
		struct ausrc *as = le->data;
		if (!name || !str_cmp(as->name, name))
			return as;
	}
	return NULL;
}

const struct auplay *auplay_find(const struct list *auplayl, const char *name)
{
	for (struct le *le = list_head(auplayl); le; le = le->next) {
		struct auplay *ap = le->data;
		if (!name || !str_cmp(ap->name, name))
			return ap;
	}
	return NULL;
}

int ausrc_alloc(struct ausrc_st **stp, struct list *ausrcl, const char *name,
                struct ausrc_prm *prm, const char *device,
                ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	const struct ausrc *as = ausrc_find(ausrcl, name);
	if (!as)
		return ENOENT;
	return as->alloch(stp, as, prm, device, rh, errh, arg);
}

int auplay_alloc(struct auplay_st **stp, struct list *auplayl, const char *name,
                 struct auplay_prm *prm, const char *device,
                 auplay_write_h *wh, void *arg)
{
	const struct auplay *ap = auplay_find(auplayl, name);
	if (!ap)
		return ENOENT;
	return ap->alloch(stp, ap, prm, device, wh, arg);
}

/* ── Capture side: record what the encoder was handed ──────────────────── */

#define MAX_FRAMES 64

struct rec {
	int     nframes;
	size_t  sampc[MAX_FRAMES];
	int16_t first[MAX_FRAMES];   /* first sample of each frame */
};

static void read_h(struct auframe *af, void *arg)
{
	struct rec *r = arg;
	const int16_t *v = af->sampv;

	if (r->nframes < MAX_FRAMES) {
		r->sampc[r->nframes] = af->sampc;
		r->first[r->nframes] = af->sampc ? v[0] : 0;
	}
	++r->nframes;
}

/* ── Playback side: hand back a known ramp ─────────────────────────────── */

static void write_h(struct auframe *af, void *arg)
{
	int16_t *v = af->sampv;
	int base = *(int *)arg;

	for (size_t i = 0; i < af->sampc; i++)
		v[i] = (int16_t)(base + (int)i);
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int open_src(struct ausrc_st **stp, uint32_t srate, uint8_t ch,
                    uint32_t ptime, struct rec *r)
{
	struct ausrc_prm prm = {
		.srate = srate, .ch = ch, .ptime = ptime, .fmt = AUFMT_S16LE,
	};
	return ausrc_alloc(stp, baresip_ausrcl(), "external", &prm, NULL,
	                   read_h, NULL, r);
}

static int open_play(struct auplay_st **stp, uint32_t srate, uint8_t ch,
                     uint32_t ptime, int *base)
{
	struct auplay_prm prm = {
		.srate = srate, .ch = ch, .ptime = ptime, .fmt = AUFMT_S16LE,
	};
	return auplay_alloc(stp, baresip_auplayl(), "external", &prm, NULL,
	                    write_h, base);
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

/* With no device open the app still has to get a usable buffer back, because
 * it hands pull() straight to the speaker without checking. */
static void test_idle(void)
{
	int16_t buf[160];
	int err;

	memset(buf, 0xAA, sizeof(buf));

	err = medut_audio_external_pull(buf, RE_ARRAY_SIZE(buf));
	CHECK(err == ENODEV, "pull with no device: expected ENODEV, got %d", err);

	bool zeroed = true;
	for (size_t i = 0; i < RE_ARRAY_SIZE(buf); i++)
		if (buf[i]) zeroed = false;
	CHECK(zeroed, "pull must zero the buffer even with no device");

	int16_t one = 0;
	err = medut_audio_external_push(&one, 1);
	CHECK(err == ENODEV, "push with no device: expected ENODEV, got %d", err);

	CHECK(!medut_audio_external_is_active(), "is_active with no device");

	err = medut_audio_external_format(NULL, NULL, NULL);
	CHECK(err == ENODEV, "format with no device: expected ENODEV, got %d", err);
}

/* The app's buffer size is its own business; the encoder only ever sees whole
 * ptime frames, in order, with nothing dropped or reordered. */
static void test_reframing(void)
{
	struct ausrc_st *st = NULL;
	struct rec r = {0};
	int16_t chunk[512];
	int err;

	err = open_src(&st, 8000, 1, 20, &r);   /* sampc = 160 */
	CHECK(err == 0, "open_src: %d", err);
	if (err) return;

	/* A running ramp, pushed in deliberately mismatched chunk sizes. */
	int16_t next = 0;
	const size_t sizes[] = {40, 160, 500, 7};
	size_t total = 0;

	for (size_t s = 0; s < RE_ARRAY_SIZE(sizes); s++) {
		for (size_t i = 0; i < sizes[s]; i++)
			chunk[i] = next++;
		err = medut_audio_external_push(chunk, sizes[s]);
		CHECK(err == 0, "push %zu: %d", sizes[s], err);
		total += sizes[s];
	}

	CHECK(r.nframes == (int)(total / 160),
	      "expected %d frames from %zu samples, got %d",
	      (int)(total / 160), total, r.nframes);

	bool sized = true, ordered = true;
	for (int i = 0; i < r.nframes && i < MAX_FRAMES; i++) {
		if (r.sampc[i] != 160) sized = false;
		if (r.first[i] != (int16_t)(i * 160)) ordered = false;
	}
	CHECK(sized, "every frame handed to the encoder must be one ptime");
	CHECK(ordered, "frames must arrive in order with no samples dropped");

	mem_deref(st);
}

/* Regression: aubuf's pre-fill threshold was 20 ms while the drain loop gated
 * on one ptime, so at ptime < 20 ms the loop read silence without draining and
 * never terminated.  The alarm() in main() is what turns that into a failure
 * rather than a hung CI job. */
static void test_short_ptime_no_hang(void)
{
	struct ausrc_st *st = NULL;
	struct rec r = {0};
	int16_t chunk[80];
	int err;

	err = open_src(&st, 8000, 1, 10, &r);   /* sampc = 80 */
	CHECK(err == 0, "open_src ptime=10: %d", err);
	if (err) return;

	memset(chunk, 0, sizeof(chunk));

	for (int i = 0; i < 10; i++) {
		err = medut_audio_external_push(chunk, RE_ARRAY_SIZE(chunk));
		CHECK(err == 0, "push at ptime=10: %d", err);
	}

	CHECK(r.nframes > 0, "ptime=10 must still reach the encoder");

	mem_deref(st);
}

/* Regression: the selected device was a bare pointer cleared only when the
 * closing device was the selected one, so ending the newer of two calls left
 * the older one alive but unreachable — push() returned ENODEV forever. */
static void test_second_call_returns_the_mic(void)
{
	struct ausrc_st *a = NULL, *b = NULL;
	struct rec ra = {0}, rb = {0};
	int16_t chunk[160] = {0};
	int err;

	CHECK(open_src(&a, 8000, 1, 20, &ra) == 0, "open A");
	CHECK(open_src(&b, 8000, 1, 20, &rb) == 0, "open B");

	/* Newest wins while both are up. */
	err = medut_audio_external_push(chunk, RE_ARRAY_SIZE(chunk));
	CHECK(err == 0, "push with both open: %d", err);
	CHECK(rb.nframes == 1 && ra.nframes == 0,
	      "newest device should get the audio (A=%d B=%d)",
	      ra.nframes, rb.nframes);

	/* B hangs up; the mic must fall back to A, not vanish. */
	mem_deref(b);

	err = medut_audio_external_push(chunk, RE_ARRAY_SIZE(chunk));
	CHECK(err == 0, "push after B closed: %d", err);
	CHECK(ra.nframes == 1, "A must get the mic back after B closes (A=%d)",
	      ra.nframes);

	mem_deref(a);
}

static void test_format(void)
{
	struct ausrc_st *st = NULL;
	struct auplay_st *pst = NULL;
	struct rec r = {0};
	int base = 0;
	uint32_t srate = 0, ptime = 0;
	uint8_t ch = 0;
	int err;

	CHECK(open_src(&st, 16000, 1, 20, &r) == 0, "open_src 16k");

	err = medut_audio_external_format(&srate, &ch, &ptime);
	CHECK(err == 0, "format while open: %d", err);
	CHECK(srate == 16000 && ch == 1 && ptime == 20,
	      "format: got %u Hz %u ch %u ms", srate, ch, ptime);

	mem_deref(st);

	err = medut_audio_external_format(&srate, &ch, &ptime);
	CHECK(err == ENODEV, "format after close: expected ENODEV, got %d", err);

	/* Playback alone still answers — a receive-only call has no capture. */
	CHECK(open_play(&pst, 8000, 1, 0, &base) == 0, "open_play");
	err = medut_audio_external_format(&srate, &ch, &ptime);
	CHECK(err == 0, "format from playback alone: %d", err);
	CHECK(ptime == 20, "ptime 0 must report as the 20 ms default, got %u",
	      ptime);
	mem_deref(pst);
}

static void test_playback(void)
{
	struct auplay_st *st = NULL;
	int base = 1000;
	int16_t buf[160];
	int err;

	CHECK(open_play(&st, 8000, 1, 20, &base) == 0, "open_play");

	memset(buf, 0xAA, sizeof(buf));
	err = medut_audio_external_pull(buf, RE_ARRAY_SIZE(buf));
	CHECK(err == 0, "pull: %d", err);

	bool ramp = true;
	for (size_t i = 0; i < RE_ARRAY_SIZE(buf); i++)
		if (buf[i] != (int16_t)(base + (int)i)) ramp = false;
	CHECK(ramp, "pull must return what the decoder wrote");

	CHECK(medut_audio_external_is_active(), "is_active with playback open");

	mem_deref(st);

	memset(buf, 0xAA, sizeof(buf));
	err = medut_audio_external_pull(buf, RE_ARRAY_SIZE(buf));
	CHECK(err == ENODEV, "pull after close: expected ENODEV, got %d", err);
	CHECK(buf[0] == 0 && buf[159] == 0, "buffer must still be zeroed");
}

/* Regression: close() never ran at shutdown, so s_ready stayed true, init()
 * returned early on the next start, and the registration baresip_init() had
 * already discarded was never replaced — leaving use_external() succeeding
 * against a device that no longer existed. */
static void test_close_reinit(void)
{
	int err;

	bsdk_audio_external_close();

	CHECK(ausrc_find(baresip_ausrcl(), "external") == NULL,
	      "close must unregister the capture device");
	CHECK(auplay_find(baresip_auplayl(), "external") == NULL,
	      "close must unregister the playback device");

	err = bsdk_audio_external_init();
	CHECK(err == 0, "re-init after close: %d", err);

	CHECK(ausrc_find(baresip_ausrcl(), "external") != NULL,
	      "re-init must register the capture device again");
	CHECK(auplay_find(baresip_auplayl(), "external") != NULL,
	      "re-init must register the playback device again");

	/* And it has to actually work, not merely be findable. */
	struct ausrc_st *st = NULL;
	struct rec r = {0};
	CHECK(open_src(&st, 8000, 1, 20, &r) == 0, "open after re-init");
	mem_deref(st);
}

/* A channel-count mismatch used to slide every later sample into the wrong
 * channel, which sounds like noise rather than like a bug. */
static void test_partial_frame_rejected(void)
{
	struct ausrc_st *st = NULL;
	struct rec r = {0};
	int16_t chunk[161] = {0};
	int err;

	CHECK(open_src(&st, 8000, 2, 20, &r) == 0, "open_src stereo");

	err = medut_audio_external_push(chunk, 161);
	CHECK(err == EINVAL, "odd sample count on stereo: expected EINVAL, got %d",
	      err);

	mem_deref(st);
}

int main(void)
{
	/* Nothing here should take a second; the ptime regression hangs
	 * outright, so bound the run rather than let CI stall on it. */
	alarm(10);

	if (bsdk_audio_external_init()) {
		printf("audio_external: init failed\n");
		return 1;
	}

	test_idle();
	test_reframing();
	test_short_ptime_no_hang();
	test_second_call_returns_the_mic();
	test_format();
	test_playback();
	test_partial_frame_rejected();
	test_close_reinit();

	bsdk_audio_external_close();

	printf("test_audio_external: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
