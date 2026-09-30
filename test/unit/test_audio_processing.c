/**
 * @file test_audio_processing.c  Unit tests for the TX/RX audio filters
 *
 * What these filters do is decided by numbers — time constants, thresholds,
 * a noise-floor estimate — and every one of them is audible when wrong: a
 * gate that opens slowly clips the start of every word, a noise floor that
 * starts at zero leaves the gate open for the first half-minute of a call,
 * a suppressor that recovers slowly swallows the near end's replies.  None
 * of that shows up as an error anywhere, so it is pinned here in
 * milliseconds and dB, at more than one ptime.
 *
 * Also pinned: the on/off rules (the phone's voice path, the app-owned
 * device, the suppressor request that goes with it), that a switch reaches
 * a call already up on its next frame, and that each call's TX and RX stay
 * paired — including after the far end holds and resumes us, when baresip
 * rebuilds only the TX side.
 *
 * Links src/audio_processing.c against libre only.  baresip's filter
 * registry, logging and the two platform hooks are stubbed below, and
 * tmr_jiffies is wrapped (-Wl,--wrap) so time is the test's to move.
 *
 * Built a second time as test_audio_processing_webrtc with the WebRTC
 * filter compiled in (BSDK_TEST_WEBRTC): that run loads the real library
 * through src/webrtc_shim.c and checks the echo is actually removed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "../../src/echosdk_internal.h"
#include <rem_au.h>
#include <rem_aulevel.h>
#include <rem_auframe.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                       \
	if (cond) { g_pass++; }                                         \
	else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__);  \
	       printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

/* ── Stubs ───────────────────────────────────────────────────────────────── */

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

static struct list g_aufiltl = LIST_INIT;

struct list *baresip_aufiltl(void) { return &g_aufiltl; }

void aufilt_register(struct list *aufiltl, struct aufilt *af)
{
	af->enabled = true;
	list_append(aufiltl, &af->le, af);
}

void aufilt_unregister(struct aufilt *af)
{
	list_unlink(&af->le);
}

static bool g_has_aec;     /* the phone's voice path is under us */
static bool g_external;    /* app-owned device selected */

bool bsdk_platform_has_aec(void)        { return g_has_aec; }
bool bsdk_audio_external_selected(void) { return g_external; }

static uint64_t g_now_ms;
uint64_t __wrap_tmr_jiffies(void);
uint64_t __wrap_tmr_jiffies(void) { return g_now_ms; }

/* ── A call: every registered filter's TX and RX state ──────────────────── */

#define MAXF 8

struct tcall {
	const struct audio      *au;
	struct aufilt_enc_st    *enc[MAXF];
	struct aufilt_dec_st    *dec[MAXF];
	const struct aufilt     *af[MAXF];
	int n;
};

/* What baresip's aufilt_setup does for a new call. */
static void tcall_open_tx(struct tcall *c)
{
	struct le *le;
	int i = 0;
	LIST_FOREACH(&g_aufiltl, le) {
		struct aufilt *af = le->data;
		struct aufilt_prm prm = {0};
		void *ctx = NULL;   /* never shared: see the hold/resume test */
		c->af[i] = af;
		if (af->encupdh) {
			int err = af->encupdh(&c->enc[i], &ctx, af, &prm, c->au);
			if (!err && c->enc[i])
				c->enc[i]->af = af;
		}
		i++;
	}
	c->n = i;
}

static void tcall_open_rx(struct tcall *c)
{
	struct le *le;
	int i = 0;
	LIST_FOREACH(&g_aufiltl, le) {
		struct aufilt *af = le->data;
		struct aufilt_prm prm = {0};
		void *ctx = NULL;
		if (af->decupdh) {
			int err = af->decupdh(&c->dec[i], &ctx, af, &prm, c->au);
			if (!err && c->dec[i])
				c->dec[i]->af = af;
		}
		i++;
	}
	c->n = i;
}

static void tcall_open(struct tcall *c, const struct audio *au)
{
	memset(c, 0, sizeof(*c));
	c->au = au;
	tcall_open_tx(c);
	tcall_open_rx(c);
}

static void tcall_close_tx(struct tcall *c)
{
	for (int i = 0; i < c->n; i++)
		c->enc[i] = mem_deref(c->enc[i]);
}

static void tcall_close(struct tcall *c)
{
	tcall_close_tx(c);
	for (int i = 0; i < c->n; i++)
		c->dec[i] = mem_deref(c->dec[i]);
}

static void tcall_tx(struct tcall *c, int16_t *p, size_t n,
                     uint32_t srate, uint32_t ch)
{
	struct auframe af;
	auframe_init(&af, AUFMT_S16LE, p, n, srate, ch);
	for (int i = 0; i < c->n; i++)
		if (c->enc[i] && c->af[i]->ench)
			c->af[i]->ench(c->enc[i], &af);
}

/* RX walks the chain in reverse, as aureceiver does.  Returns what the
 * filters returned, OR'd: an RX error drops the frame in baresip. */
static int tcall_rx(struct tcall *c, int16_t *p, size_t n,
                    uint32_t srate, uint32_t ch)
{
	struct auframe af;
	int err = 0;
	auframe_init(&af, AUFMT_S16LE, p, n, srate, ch);
	for (int i = c->n - 1; i >= 0; i--)
		if (c->dec[i] && c->af[i]->dech)
			err |= c->af[i]->dech(c->dec[i], &af);
	return err;
}

/* ── Signals ─────────────────────────────────────────────────────────────── */

#define SR 48000u

static uint32_t g_rng = 1;

static float urand(void)
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return (float)(g_rng >> 8) / 16777216.0f * 2.0f - 1.0f;
}

/* White noise at an RMS level in dBFS. */
static void gen_noise(int16_t *p, size_t n, float dbfs)
{
	float a = 32768.0f * powf(10.0f, dbfs / 20.0f) * 1.7320508f;
	for (size_t i = 0; i < n; i++)
		p[i] = (int16_t)(a * urand());
}

static double level_db(const int16_t *p, size_t n)
{
	double s = 0;
	for (size_t i = 0; i < n; i++)
		s += (double)p[i] * p[i];
	if (s <= 0)
		return -100;
	return 10 * log10(s / n / (32768.0 * 32768.0));
}

/* One TX frame of noise at dbfs through the call; returns output − input. */
static double tx_frame(struct tcall *c, size_t n, float dbfs)
{
	int16_t buf[SR / 1000 * 60];
	gen_noise(buf, n, dbfs);
	double in = level_db(buf, n);
	tcall_tx(c, buf, n, SR, 1);
	return level_db(buf, n) - in;
}

static void rx_frame(struct tcall *c, size_t n, float dbfs)
{
	int16_t buf[SR / 1000 * 60];
	gen_noise(buf, n, dbfs);
	tcall_rx(c, buf, n, SR, 1);
}

static void init(bool ns, bool agc, echosdk_aec_mode_t mode)
{
	g_has_aec  = false;
	g_external = false;
	bsdk_audio_processing_init(ns, agc, mode, 1.0f, 0.0f, 0.0f);
}

static int g_au_ids[4];   /* distinct addresses standing in for struct audio */
#define AU(i) ((const struct audio *)&g_au_ids[i])

/* ── NS gate ─────────────────────────────────────────────────────────────── */

static void test_ns_timing(unsigned ptime)
{
	struct tcall c;
	size_t n = SR / 1000 * ptime;
	unsigned t;
	double att;

	init(true, false, ECHOSDK_AEC_OFF);
	tcall_open(&c, AU(0));

	/* Start of call: noise only.  The floor used to start at 0 and climb
	 * for ~37 s before the gate attenuated anything. */
	for (t = 0; t < 1000; t += ptime)
		att = tx_frame(&c, n, -50.0f);
	CHECK(att <= -15.0, "ptime %u: noise 1 s into the call only %.1f dB down",
	      ptime, att);
	for (; t < 2000; t += ptime)
		tx_frame(&c, n, -50.0f);

	/* A word 20 dB above the noise.  The gate used to take 200 ms to get
	 * within 6 dB of open. */
	double worst_onset = 0;
	for (t = 0; t < 400; t += ptime) {
		att = tx_frame(&c, n, -30.0f);
		if (t >= 30 && att < worst_onset)
			worst_onset = att;
	}
	CHECK(worst_onset > -1.0,
	      "ptime %u: word still %.1f dB down 30 ms after onset",
	      ptime, worst_onset);

	/* Word ends: the tail (and the gaps between syllables) are held
	 * open, then the noise goes back down. */
	double worst_tail = 0, late = 0;
	for (t = 0; t < 700; t += ptime) {
		att = tx_frame(&c, n, -50.0f);
		if (t + ptime <= 150 && att < worst_tail)
			worst_tail = att;
		if (t >= 500)
			late = att;
	}
	CHECK(worst_tail > -1.0, "ptime %u: word tail cut by %.1f dB within 150 ms",
	      ptime, worst_tail);
	CHECK(late <= -15.0, "ptime %u: noise only %.1f dB down 500 ms after the word",
	      ptime, late);

	/* Mute: zeros through the chain.  They must not drag the floor down —
	 * that used to leave the gate open for ~40 s after unmute. */
	int16_t z[SR / 1000 * 60];
	for (t = 0; t < 3000; t += ptime) {
		memset(z, 0, n * sizeof(z[0]));
		tcall_tx(&c, z, n, SR, 1);
	}
	double after_mute = -1000;   /* least attenuation seen */
	for (t = 0; t < 1000; t += ptime) {
		att = tx_frame(&c, n, -50.0f);
		if (t >= 300 && att > after_mute)
			after_mute = att;
	}
	CHECK(after_mute <= -15.0,
	      "ptime %u: noise only %.1f dB down 300 ms+ after unmute",
	      ptime, after_mute);

	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* Rising noise (walking into a louder room) is followed, not gated as if
 * it were speech forever. */
static void test_ns_follows_rising_noise(void)
{
	struct tcall c;
	size_t n = SR / 1000 * 20;
	double att = 0;

	init(true, false, ECHOSDK_AEC_OFF);
	tcall_open(&c, AU(0));
	for (int t = 0; t < 2000; t += 20)
		tx_frame(&c, n, -60.0f);
	for (int t = 0; t < 3000; t += 20)
		att = tx_frame(&c, n, -45.0f);
	CHECK(att <= -15.0, "noise 15 dB louder still only %.1f dB down after 3 s",
	      att);
	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* ── Live switches ───────────────────────────────────────────────────────── */

static void test_ns_live_toggle(void)
{
	struct tcall c;
	size_t n = SR / 1000 * 20;
	int16_t buf[960], ref[960];
	double att = 0;

	init(true, false, ECHOSDK_AEC_OFF);
	tcall_open(&c, AU(0));
	for (int t = 0; t < 2000; t += 20)
		att = tx_frame(&c, n, -50.0f);
	CHECK(att <= -15.0, "gate not closed on noise (%.1f dB)", att);

	/* Off mid-call: the very next frame passes untouched.  With
	 * aufilt_enable() this reached only the next call. */
	bsdk_ap_set_ns(false);
	gen_noise(buf, n, -50.0f);
	memcpy(ref, buf, sizeof(ref));
	tcall_tx(&c, buf, n, SR, 1);
	CHECK(memcmp(buf, ref, n * sizeof(buf[0])) == 0,
	      "set_ns(false) did not reach the next frame");

	/* Back on: fresh state, and working again within a second. */
	bsdk_ap_set_ns(true);
	att = tx_frame(&c, n, -50.0f);
	CHECK(att > -1.0, "re-enabled gate did not start from fresh state (%.1f)",
	      att);
	for (int t = 0; t < 1000; t += 20)
		att = tx_frame(&c, n, -50.0f);
	CHECK(att <= -15.0, "re-enabled gate not working after 1 s (%.1f dB)", att);

	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* ── Rules: the phone's voice path, the app-owned device ─────────────────── */

static void test_voice_path_veto(void)
{
	struct bsdk_ap_effective e;
	struct tcall c;
	size_t n = SR / 1000 * 20;
	int16_t buf[960], ref[960];

	init(true, true, ECHOSDK_AEC_SUPPRESSOR);
	g_has_aec = true;

	bsdk_ap_effective(&e);
	CHECK(!e.ns && !e.agc && !e.suppressor,
	      "stacked on the voice path: ns=%d agc=%d supp=%d",
	      e.ns, e.agc, e.suppressor);

	/* ...and nothing touches the samples. */
	tcall_open(&c, AU(0));
	bool untouched = true;
	for (int t = 0; t < 2000; t += 20) {
		gen_noise(buf, n, t < 1000 ? -50.0f : -30.0f);
		memcpy(ref, buf, sizeof(ref));
		tcall_tx(&c, buf, n, SR, 1);
		if (memcmp(buf, ref, n * sizeof(buf[0])))
			untouched = false;
	}
	CHECK(untouched, "TX changed on the voice path with every filter vetoed");

	/* The app takes the device: the phone's processing went with our
	 * driver, so the app's NS/AGC choices apply again. */
	g_external = true;
	bsdk_ap_effective(&e);
	CHECK(e.ns && e.agc, "app-owned device: ns=%d agc=%d", e.ns, e.agc);

	/* A recorder that reports no voice path lifts the veto too. */
	g_external = false;
	g_has_aec  = false;
	bsdk_ap_effective(&e);
	CHECK(e.ns && e.agc && e.suppressor,
	      "no voice path: ns=%d agc=%d supp=%d", e.ns, e.agc, e.suppressor);

	tcall_close(&c);
	bsdk_audio_processing_close();
}

static void test_suppressor_rules(void)
{
	struct bsdk_ap_effective e;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);

	bsdk_ap_effective(&e);
	CHECK(e.suppressor, "desktop: suppressor off");

	g_has_aec = true;
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "on top of the platform canceller");

	/* Taking the device never switches it on by itself... */
	g_external = true;
	bsdk_ap_external_changed(true);
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "app-owned device switched the suppressor on");

	/* ...asking for it does. */
	bsdk_ap_set_aec(true);
	bsdk_ap_effective(&e);
	CHECK(e.suppressor, "requested while app-owned: still off");

	/* Platform device back: off, and the request does not survive. */
	g_external = false;
	bsdk_ap_external_changed(false);
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "platform device back: suppressor stacked on it");
	g_external = true;
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "old request came back with the app-owned device");

	/* Plain on/off still works on the desktop. */
	g_external = false;
	g_has_aec  = false;
	bsdk_ap_set_aec(false);
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "set_aec(false): still on");
	bsdk_ap_set_aec(true);
	bsdk_ap_effective(&e);
	CHECK(e.suppressor, "set_aec(true): still off");

	bsdk_audio_processing_close();
}

/* ── Suppressor ──────────────────────────────────────────────────────────── */

/* Each call ducks for its own far end, not for whichever call last had
 * audio (the level used to be one global). */
static void test_suppressor_per_call(void)
{
	struct tcall a, b;
	size_t n = SR / 1000 * 20;
	double att_a = 0, att_b = 0;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	tcall_open(&a, AU(0));
	tcall_open(&b, AU(1));

	for (int t = 0; t < 300; t += 20) {
		g_now_ms += 20;
		rx_frame(&a, n, -20.0f);    /* A's far end talks */
		rx_frame(&b, n, -100.0f);   /* B's is silent     */
		att_a = tx_frame(&a, n, -30.0f);
		att_b = tx_frame(&b, n, -30.0f);
	}
	CHECK(att_a <= -15.0, "far end talking: A only %.1f dB down", att_a);
	CHECK(att_b > -0.5, "B ducked %.1f dB for A's far end", att_b);

	tcall_close(&a);
	tcall_close(&b);
	bsdk_audio_processing_close();
}

/* Far end stops: TX is held down while its echo can still arrive, then
 * back quickly.  No RX frames at all afterwards (DTX, hold) — the level
 * must age by the clock, not wait for a frame that never comes. */
static void test_suppressor_release(unsigned ptime)
{
	struct tcall c;
	size_t n = SR / 1000 * ptime;
	double worst_held = -1000, worst_late = 0;   /* least / most attenuation */
	unsigned t;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	tcall_open(&c, AU(0));

	for (t = 0; t < 500; t += ptime) {
		g_now_ms += ptime;
		rx_frame(&c, n, -20.0f);
		tx_frame(&c, n, -30.0f);
	}
	for (t = 0; t < 600; t += ptime) {
		g_now_ms += ptime;
		double att = tx_frame(&c, n, -30.0f);
		if (t + ptime <= 120 && att > worst_held)
			worst_held = att;
		if (t >= 350 && att < worst_late)
			worst_late = att;
	}
	CHECK(worst_held <= -12.0,
	      "ptime %u: echo window let TX through (%.1f dB)", ptime, worst_held);
	CHECK(worst_late > -1.0,
	      "ptime %u: TX still %.1f dB down 350 ms after the far end stopped",
	      ptime, worst_late);

	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* A far end with steady background noise: after its speech, the level
 * settles on the noise, not somewhere above it. */
static void test_suppressor_settles_on_far_noise(void)
{
	struct tcall c;
	size_t n = SR / 1000 * 20;
	double att = 0;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	tcall_open(&c, AU(0));
	for (int t = 0; t < 500; t += 20) {
		g_now_ms += 20;
		rx_frame(&c, n, -20.0f);
		tx_frame(&c, n, -30.0f);
	}
	for (int t = 0; t < 2000; t += 20) {
		g_now_ms += 20;
		rx_frame(&c, n, -40.0f);
		att = tx_frame(&c, n, -30.0f);
	}
	/* −40 dBFS maps to 16.5 × 4/23 ≈ 2.9 dB. */
	CHECK(att > -3.5 && att < -2.3,
	      "far-end noise at -40 dBFS ducks TX %.1f dB, expected ~-2.9", att);
	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* The far end holds us and resumes: baresip rebuilds only the TX chain,
 * with a fresh ctx.  TX must still find its RX. */
static void test_hold_resume_pairing(void)
{
	struct tcall c;
	size_t n = SR / 1000 * 20;
	double att = 0;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	tcall_open(&c, AU(2));

	tcall_close_tx(&c);   /* stop_tx: only tx->filtl is flushed */
	tcall_open_tx(&c);    /* resume: encupdh again, ctx == NULL */

	for (int t = 0; t < 300; t += 20) {
		g_now_ms += 20;
		rx_frame(&c, n, -20.0f);
		att = tx_frame(&c, n, -30.0f);
	}
	CHECK(att <= -15.0, "after hold/resume TX lost its RX (%.1f dB)", att);

	tcall_close(&c);
	bsdk_audio_processing_close();
}

static void test_suppression_level_zero(void)
{
	struct tcall c;
	size_t n = SR / 1000 * 20;
	double att = 0;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	bsdk_aec_floor_store(1.0f);   /* aec_suppression_level = 0 */
	tcall_open(&c, AU(0));
	for (int t = 0; t < 300; t += 20) {
		g_now_ms += 20;
		rx_frame(&c, n, -20.0f);
		att = tx_frame(&c, n, -30.0f);
	}
	CHECK(att > -0.1, "level 0 still attenuates %.2f dB", att);
	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* ── AGC ─────────────────────────────────────────────────────────────────── */

/* Same rise per second whatever the frame size (was per frame). */
static void test_agc_rate(unsigned ptime)
{
	struct tcall c;
	size_t n = SR / 1000 * ptime;
	double att = 0;

	init(false, true, ECHOSDK_AEC_OFF);
	tcall_open(&c, AU(0));
	for (unsigned t = 0; t < 1020; t += ptime)
		att = tx_frame(&c, n, -35.0f);
	CHECK(fabs(att - 8.6) < 0.8,
	      "ptime %u: AGC rose %.1f dB in 1 s, expected ~8.6", ptime, att);
	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* ── Registry lifetime ───────────────────────────────────────────────────── */

/* Every call's shared state goes with its last filter state.  A leaked
 * registry entry would be found again by the next call that happens to get
 * the same struct audio address — with the old call's far-end level in it. */
static void test_registry_entry_dies_with_call(void)
{
	struct tcall c;
	size_t n = SR / 1000 * 20;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	tcall_open(&c, AU(0));
	for (int t = 0; t < 300; t += 20) {
		g_now_ms += 20;
		rx_frame(&c, n, -20.0f);
		tx_frame(&c, n, -30.0f);
	}
	tcall_close(&c);

	/* Same address, same instant: nothing of the old call may remain. */
	tcall_open(&c, AU(0));
	double att = tx_frame(&c, n, -30.0f);
	CHECK(att > -0.5, "new call inherited the old one's far end (%.1f dB)",
	      att);
	tcall_close(&c);
	bsdk_audio_processing_close();
}

/* ── WebRTC ──────────────────────────────────────────────────────────────── */

#ifdef BSDK_TEST_WEBRTC

#include "../../src/webrtc_apm.h"

#define ECHO_DELAY_MS 150
#define HIST (SR * 20)

static int16_t g_far[HIST];

/* Far end: noise into RX.  Near end: nothing but its echo, 150 ms later —
 * the far-end reference point sits upstream of the jitter buffer.  Returns
 * echo removed over the last half, in dB. */
static double run_echo(struct tcall *c, unsigned ms, size_t frame)
{
	static size_t pos;
	size_t delay = SR / 1000 * ECHO_DELAY_MS;
	double in_e = 0, out_e = 0;
	int16_t buf[SR / 1000 * 60];

	for (unsigned t = 0; t < ms; t += (unsigned)(frame * 1000 / SR)) {
		gen_noise(buf, frame, -20.0f);
		for (size_t i = 0; i < frame; i++)
			g_far[(pos + i) % HIST] = buf[i];
		CHECK(tcall_rx(c, buf, frame, SR, 1) == 0,
		      "RX filter returned an error");

		for (size_t i = 0; i < frame; i++) {
			size_t k = (pos + i + HIST - delay) % HIST;
			buf[i] = pos + i >= delay ? g_far[k] / 2 : 0;
		}
		double e_in = 0, e_out = 0;
		for (size_t i = 0; i < frame; i++)
			e_in += (double)buf[i] * buf[i];
		tcall_tx(c, buf, frame, SR, 1);
		for (size_t i = 0; i < frame; i++)
			e_out += (double)buf[i] * buf[i];
		if (t >= ms / 2) {
			in_e  += e_in;
			out_e += e_out;
		}
		pos += frame;
	}
	return 10 * log10(in_e / (out_e + 1e-9));
}

static void test_webrtc(void)
{
	struct bsdk_ap_effective e;
	struct tcall c;
	size_t n20 = SR / 1000 * 20;

	if (!bsdk_webrtc_lib_available()) {
		printf("  SKIP test_webrtc: library not loadable\n");
		return;
	}

	init(false, false, ECHOSDK_AEC_WEBRTC);
	bsdk_ap_effective(&e);
	CHECK(e.webrtc && e.webrtc_aec && !e.webrtc_ns && !e.suppressor,
	      "WEBRTC mode: webrtc=%d aec=%d ns=%d supp=%d",
	      e.webrtc, e.webrtc_aec, e.webrtc_ns, e.suppressor);

	tcall_open(&c, AU(3));

	double erle = run_echo(&c, 10000, n20);
	printf("  webrtc: %.1f dB of echo removed\n", erle);
	CHECK(erle > 15.0, "WebRTC removed only %.1f dB of echo", erle);

	/* Off mid-call: the echo passes on the next frames. */
	bsdk_ap_set_aec(false);
	bsdk_ap_effective(&e);
	CHECK(!e.webrtc, "AEC and NS off, WebRTC still running");
	erle = run_echo(&c, 2000, n20);
	CHECK(fabs(erle) < 0.5, "AEC off mid-call still removed %.1f dB", erle);

	/* NS alone keeps WebRTC running, without its canceller. */
	bsdk_ap_set_ns(true);
	bsdk_ap_effective(&e);
	CHECK(e.webrtc && !e.webrtc_aec && e.webrtc_ns && !e.ns,
	      "NS only: webrtc=%d aec=%d wns=%d gate=%d",
	      e.webrtc, e.webrtc_aec, e.webrtc_ns, e.ns);

	/* Back on: cancelling again. */
	bsdk_ap_set_ns(false);
	bsdk_ap_set_aec(true);
	erle = run_echo(&c, 8000, n20);
	CHECK(erle > 15.0, "AEC back on mid-call: only %.1f dB", erle);

	/* A frame that is not whole 10 ms blocks passes through untouched. */
	int16_t buf[SR / 1000 * 25], ref[SR / 1000 * 25];
	size_t n25 = SR / 1000 * 25;
	gen_noise(buf, n25, -30.0f);
	memcpy(ref, buf, sizeof(ref));
	tcall_tx(&c, buf, n25, SR, 1);
	CHECK(memcmp(buf, ref, sizeof(ref)) == 0, "25 ms frame was modified");
	CHECK(tcall_rx(&c, buf, n25, SR, 1) == 0, "RX error on a 25 ms frame");

	tcall_close(&c);
	bsdk_audio_processing_close();
}

#endif /* BSDK_TEST_WEBRTC */

int main(void)
{
	static const unsigned ptimes[] = {10, 20, 60};

	libre_init();

	for (size_t i = 0; i < RE_ARRAY_SIZE(ptimes); i++)
		test_ns_timing(ptimes[i]);
	test_ns_follows_rising_noise();
	test_ns_live_toggle();
	test_voice_path_veto();
	test_suppressor_rules();
	test_suppressor_per_call();
	for (size_t i = 0; i < RE_ARRAY_SIZE(ptimes); i++)
		test_suppressor_release(ptimes[i]);
	test_suppressor_settles_on_far_noise();
	test_hold_resume_pairing();
	test_suppression_level_zero();
	for (size_t i = 0; i < RE_ARRAY_SIZE(ptimes); i++)
		test_agc_rate(ptimes[i]);
#ifdef BSDK_TEST_WEBRTC
	test_webrtc();
#endif
	test_registry_entry_dies_with_call();

	printf("%d passed, %d failed\n", g_pass, g_fail);
	libre_close();
	return g_fail ? 1 : 0;
}
