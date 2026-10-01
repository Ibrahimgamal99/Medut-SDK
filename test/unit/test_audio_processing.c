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
 * device and the voice path its engine reports, the suppressor request that
 * goes with it and that set_aec is not), that a switch reaches
 * a call already up on its next frame, that each call's TX and RX stay
 * paired — including after the far end holds and resumes us, when baresip
 * rebuilds only the TX side — and that with two calls up each TX ducks for
 * the louder far end, since the speaker plays both.
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

static char g_last_info[512];   /* the last info() line, for the log tests */

void _info(bool safe, const char *fmt, ...)
{
	va_list ap;
	(void)safe;
	va_start(ap, fmt);
	(void)re_vsnprintf(g_last_info, sizeof(g_last_info), fmt, ap);
	va_end(ap);
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

	/* ...nor does set_aec(true), which apps call to mean "keep echo
	 * control on" — on iOS that put it on top of VoiceProcessingIO... */
	bsdk_ap_set_aec(true);
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "set_aec(true) while app-owned switched it on");

	/* ...asking for it with set_aec_mode does. */
	bsdk_ap_set_aec_mode(true);
	bsdk_ap_effective(&e);
	CHECK(e.suppressor, "requested while app-owned: still off");

	/* The request outlives a plain off/on, but not set_aec_mode(OFF). */
	bsdk_ap_set_aec(false);
	bsdk_ap_set_aec(true);
	bsdk_ap_effective(&e);
	CHECK(e.suppressor, "set_aec off/on dropped the request");
	bsdk_ap_set_aec_mode(false);
	bsdk_ap_set_aec(true);
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "set_aec_mode(OFF) left the request in place");
	bsdk_ap_set_aec_mode(true);

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

	/* A desktop app on its own device gets it only by asking, too: no
	 * platform canceller says nothing about the app's device. */
	g_external = true;
	bsdk_ap_external_changed(true);
	bsdk_ap_effective(&e);
	CHECK(!e.suppressor, "desktop, app-owned, not asked: suppressor on");
	bsdk_ap_set_aec_mode(true);
	bsdk_ap_effective(&e);
	CHECK(e.suppressor, "desktop, app-owned, asked: suppressor off");
	g_external = false;
	bsdk_ap_external_changed(false);

	bsdk_audio_processing_close();
}

/* external on/off x voice path on/off x explicit request on/off.  Off the
 * app-owned device, "voice path" is the platform driver's and a request
 * cannot be made (set_aec_mode records one only while app-owned). */
static void test_voice_path_matrix(void)
{
	struct bsdk_ap_effective e;

	for (int ext = 0; ext < 2; ext++)
	for (int vp = 0; vp < 2; vp++)
	for (int req = 0; req < 2; req++) {
		init(true, true, ECHOSDK_AEC_SUPPRESSOR);
		g_external = ext;
		g_has_aec  = !ext && vp;
		bsdk_ap_external_changed(ext);
		bsdk_ap_external_voice_path(ext && vp);
		bsdk_ap_set_aec_mode(true);
		if (!req)
			bsdk_ap_set_aec_mode(false), bsdk_ap_set_aec(true);
		bsdk_ap_effective(&e);

		bool want_ns   = !vp;
		bool want_supp = ext ? req : !vp;
		CHECK(e.ns == want_ns && e.agc == want_ns && e.suppressor == want_supp,
		      "ext=%d vp=%d req=%d: ns=%d agc=%d supp=%d, want %d/%d/%d",
		      ext, vp, req, e.ns, e.agc, e.suppressor,
		      want_ns, want_ns, want_supp);
		bsdk_audio_processing_close();
	}
	g_external = false;
	g_has_aec  = false;
}

/* What the log says on an iPhone with NS, AGC and AEC all on and the app's
 * VPIO engine running — the line a field report is read from. */
static void test_tx_chain_log_app_voice_path(void)
{
	init(true, true, ECHOSDK_AEC_SUPPRESSOR);
	g_has_aec  = true;           /* audiounit was the SDK's own driver */
	g_external = true;
	bsdk_ap_external_changed(true);
	CHECK(strstr(g_last_info, "ns=gate agc=on "
	             "aec=off (app-owned device; set_aec_mode to enable)") != NULL,
	      "app-owned, raw mic: \"%s\"", g_last_info);

	bsdk_ap_external_voice_path(true);
	CHECK(strstr(g_last_info, "[app voice path up]") &&
	      strstr(g_last_info, "ns=off (app-owned voice path) "
	             "agc=off (app-owned voice path) "
	             "aec=off (app-owned voice path)") != NULL,
	      "app-owned, VPIO: \"%s\"", g_last_info);

	/* set_aec(true), what the plugin calls, changes none of it. */
	bsdk_ap_set_aec(true);
	CHECK(strstr(g_last_info, "aec=off (app-owned voice path)") != NULL,
	      "set_aec(true) on VPIO: \"%s\"", g_last_info);

	g_external = false;
	g_has_aec  = false;
	bsdk_audio_processing_close();
}

/* The app's voice-path report lasts only while its device does. */
static void test_voice_path_reset(void)
{
	struct bsdk_ap_effective e;

	init(true, true, ECHOSDK_AEC_SUPPRESSOR);
	g_external = true;
	bsdk_ap_external_changed(true);
	bsdk_ap_external_voice_path(true);
	bsdk_ap_effective(&e);
	CHECK(!e.ns && !e.agc, "app voice path: ns=%d agc=%d", e.ns, e.agc);

	/* Platform device back, then the app's again with its engine not yet
	 * up: a raw microphone until the engine says otherwise. */
	g_external = false;
	bsdk_ap_external_changed(false);
	g_external = true;
	bsdk_ap_external_changed(true);
	bsdk_ap_effective(&e);
	CHECK(e.ns && e.agc, "voice path survived the platform device: ns=%d agc=%d",
	      e.ns, e.agc);

	/* ...and a re-init forgets it. */
	bsdk_ap_external_voice_path(true);
	bsdk_audio_processing_close();
	init(true, true, ECHOSDK_AEC_SUPPRESSOR);
	g_external = true;
	bsdk_ap_effective(&e);
	CHECK(e.ns && e.agc, "voice path survived re-init: ns=%d agc=%d",
	      e.ns, e.agc);

	g_external = false;
	bsdk_audio_processing_close();
}

/* ── Suppressor ──────────────────────────────────────────────────────────── */

/* Two calls up at once: the speaker plays both far ends, so each call's
 * microphone hears both echoes, and each TX ducks for the louder one — not
 * for whichever call's frame came last (the level used to be one global, so
 * a silent far end's frame un-ducked a loud one's). */
static void test_suppressor_across_calls(void)
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
	CHECK(att_b <= -15.0, "A's far end in B's mic: B only %.1f dB down", att_b);

	/* B is held: no RX frames.  A's far end stops too; both come back,
	 * B's own envelope having aged out with no frame to refresh it. */
	for (int t = 0; t < 600; t += 20) {
		g_now_ms += 20;
		rx_frame(&a, n, -100.0f);
		att_a = tx_frame(&a, n, -30.0f);
		att_b = tx_frame(&b, n, -30.0f);
	}
	CHECK(att_a > -1.0 && att_b > -1.0,
	      "both far ends quiet: A %.1f, B %.1f dB down", att_a, att_b);

	/* A gone: its slot no longer counts.  B's far end alone ducks B. */
	for (int t = 0; t < 300; t += 20) {
		g_now_ms += 20;
		rx_frame(&a, n, -20.0f);
		tx_frame(&a, n, -30.0f);
	}
	tcall_close(&a);
	att_b = tx_frame(&b, n, -30.0f);
	CHECK(att_b > -1.0, "B ducked %.1f dB for a call that has ended", att_b);

	tcall_close(&b);
	bsdk_audio_processing_close();
}

/* More calls than envelope slots: the extra one still ducks for its own far
 * end, and for every call that has a slot. */
static void test_suppressor_slots_full(void)
{
	enum { NCALLS = 9 };
	static int ids[NCALLS];
	struct tcall c[NCALLS];
	size_t n = SR / 1000 * 20;
	double att = 0;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	for (int i = 0; i < NCALLS; i++)
		tcall_open(&c[i], (const struct audio *)&ids[i]);

	for (int t = 0; t < 300; t += 20) {
		g_now_ms += 20;
		rx_frame(&c[NCALLS - 1], n, -20.0f);
		att = tx_frame(&c[NCALLS - 1], n, -30.0f);
	}
	CHECK(att <= -15.0, "call past the slots: own far end only %.1f dB", att);

	for (int t = 0; t < 600; t += 20) {
		g_now_ms += 20;
		rx_frame(&c[NCALLS - 1], n, -100.0f);
		rx_frame(&c[0], n, -20.0f);
		att = tx_frame(&c[NCALLS - 1], n, -30.0f);
	}
	CHECK(att <= -15.0, "call past the slots: call 0's far end only %.1f dB",
	      att);

	for (int i = 0; i < NCALLS; i++)
		tcall_close(&c[i]);
	bsdk_audio_processing_close();
}

/* The RX thread stores a newer timestamp than the clock TX read a moment
 * before.  The age then reads -1 ms; unsigned, that was 49 days, and the
 * envelope decayed to nothing for that frame: one frame of echo through. */
static void test_suppressor_age_race(void)
{
	struct tcall c;
	size_t n = SR / 1000 * 20;
	double att = 0;

	init(false, false, ECHOSDK_AEC_SUPPRESSOR);
	tcall_open(&c, AU(0));
	for (int t = 0; t < 300; t += 20) {
		g_now_ms += 20;
		rx_frame(&c, n, -20.0f);
		tx_frame(&c, n, -30.0f);
	}
	g_now_ms += 20;
	rx_frame(&c, n, -20.0f);
	g_now_ms -= 1;               /* TX read the clock first */
	att = tx_frame(&c, n, -30.0f);
	CHECK(att <= -15.0, "RX stored a newer stamp: TX only %.1f dB down", att);
	g_now_ms += 1;

	tcall_close(&c);
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

/* The init path clamps like the setter, NaN included: a NaN floor made every
 * ducked frame NaN, i.e. whatever the float-to-int16 cast gives for it. */
static void test_suppression_level_clamped_at_init(void)
{
	static const struct { float lvl; double lo, hi; } cases[] = {
		{ NAN,  -0.1,  0.1 },   /* → 0: no attenuation */
		{ -1.f, -0.1,  0.1 },
		{ 2.0f, -17.0, -15.0 }, /* → 1: −16.5 dB floor */
	};
	size_t n = SR / 1000 * 20;

	for (size_t k = 0; k < RE_ARRAY_SIZE(cases); k++) {
		struct tcall c;
		double att = 0;

		g_has_aec  = false;
		g_external = false;
		bsdk_audio_processing_init(false, false, ECHOSDK_AEC_SUPPRESSOR,
		                           cases[k].lvl, 0.0f, 0.0f);
		tcall_open(&c, AU(0));
		for (int t = 0; t < 300; t += 20) {
			g_now_ms += 20;
			rx_frame(&c, n, -20.0f);
			att = tx_frame(&c, n, -30.0f);
		}
		CHECK(att >= cases[k].lo && att <= cases[k].hi,
		      "init level %g: TX %.2f dB, want [%.1f, %.1f]",
		      (double)cases[k].lvl, att, cases[k].lo, cases[k].hi);
		tcall_close(&c);
		bsdk_audio_processing_close();
	}
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
	test_voice_path_matrix();
	test_voice_path_reset();
	test_tx_chain_log_app_voice_path();
	test_suppressor_across_calls();
	test_suppressor_slots_full();
	test_suppressor_age_race();
	for (size_t i = 0; i < RE_ARRAY_SIZE(ptimes); i++)
		test_suppressor_release(ptimes[i]);
	test_suppressor_settles_on_far_noise();
	test_hold_resume_pairing();
	test_suppression_level_zero();
	test_suppression_level_clamped_at_init();
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
