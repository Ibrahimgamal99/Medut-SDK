/**
 * @file audio_processing.c
 * TX/RX audio filters: WebRTC AEC/NS, mic gain, NS, AGC, AEC half-duplex
 * suppressor, and speaker gain.
 *
 * Filter chain (encode / TX path, in registration order):
 *   bsdk_webrtc → bsdk_mic_gain → bsdk_ns → bsdk_agc → bsdk_aec
 * Decode / RX path (baresip walks dech in reverse registration order):
 *   bsdk_spk_gain → bsdk_aec → bsdk_webrtc
 *
 * bsdk_webrtc    — WebRTC APM (AEC3 + NS + high-pass), desktop Linux builds
 *                  with ECHOSDK_WITH_WEBRTC_AEC.  First on TX so it sees the
 *                  raw microphone, last on RX so its far-end reference is what
 *                  the speaker plays.  Registered only for ECHOSDK_AEC_WEBRTC.
 * bsdk_mic_gain  — fixed dB scalar on the TX path (pre-boost before NS/AGC).
 * bsdk_ns        — broadband noise gate on TX: attenuates the pauses, leaves
 *                  speech alone.  Not a spectral suppressor; with WebRTC in
 *                  the chain, WebRTC's own NS runs instead.
 * bsdk_agc       — RMS-based gain normaliser targeting −20 dBFS.
 * bsdk_aec       — half-duplex echo suppressor: attenuates TX when RX is loud.
 *                  This is NOT acoustic echo cancellation.
 * bsdk_spk_gain  — fixed dB scalar on the RX path (post-jitter, pre-playback).
 *
 * On/off
 * ──────
 * Every filter is registered once at init and stays in every call's chain;
 * each one decides per frame whether to run, from bsdk_ap_effective().  That
 * is what makes the runtime setters take effect mid-call: baresip reads
 * aufilt.enabled only when it builds a call's chain, and rebuilds it only on
 * a codec change, so aufilt_enable() would reach the next call, not this one.
 * A filter that is switched back on starts from fresh state.
 *
 * The inputs are atomics written on the re thread (the setters in audio.c
 * dispatch there, which keeps them in order) and read on the audio threads.
 * Only inputs are stored, never derived on/off results, so two setters can
 * never leave a stale combination behind.
 *
 * Per-call state
 * ──────────────
 * The suppressor and bsdk_webrtc pair each call's TX side with its RX side
 * through a registry keyed by the struct audio both setup handlers receive.
 * baresip's shared `ctx` is not enough: when the far end holds and resumes
 * us, only the TX chain is rebuilt, and the new encoder gets ctx == NULL.
 * The registry is touched only from setup handlers and filter destructors,
 * all of which run on the re thread.
 *
 * Time
 * ────
 * Every time constant is in seconds, applied per frame as
 * a = 1 − exp(−dt/τ) with dt taken from the frame itself, so behaviour is the
 * same at 10, 20 or 60 ms ptime.
 */

#include "echosdk_internal.h"
#include <rem_au.h>
#include <rem_aulevel.h>
#include <rem_auframe.h>
#include <string.h>
#include <math.h>

#if defined(ECHOSDK_PROFILE_DESKTOP) && defined(ECHOSDK_HAS_WEBRTC_AEC)
#define BSDK_WEBRTC 1
#include "webrtc_apm.h"
#endif

/* ── Shared atomic gain state ───────────────────────────────────────────── */

static uint32_t g_mic_gain_bits  = 0x3F800000u;  /* float 1.0f — unity */
static uint32_t g_spk_gain_bits  = 0x3F800000u;  /* float 1.0f — unity */
/* aec_suppression_level=1.0 → floor=0.15 (−16.5 dB), the former hardcoded default.
 * aec_suppression_level=0.0 → floor=1.0 (no attenuation).
 * Internal formula: floor = 1.0f - level * 0.85f  (see bsdk_aec_floor_store). */
static uint32_t g_aec_floor_bits = 0x3E19999Au;  /* float 0.15f */

static inline float load_f(uint32_t *p)
{
	uint32_t b = re_atomic_rlx(p);
	float v;
	memcpy(&v, &b, 4);
	return v;
}

static inline void store_f(uint32_t *p, float v)
{
	uint32_t b;
	memcpy(&b, &v, 4);
	re_atomic_rlx_set(p, b);
}

void bsdk_mic_gain_store(float linear) { store_f(&g_mic_gain_bits, linear); }
void bsdk_spk_gain_store(float linear) { store_f(&g_spk_gain_bits, linear); }
void bsdk_aec_floor_store(float floor) { store_f(&g_aec_floor_bits, floor); }

/* ── Policy inputs ──────────────────────────────────────────────────────── */

enum aec_backend {
	BACKEND_OFF = 0,
	BACKEND_SUPPRESSOR,
	BACKEND_WEBRTC,
};

static RE_ATOMIC unsigned s_backend;        /* fixed at init */
static RE_ATOMIC bool     s_webrtc_fallback;/* WEBRTC asked for, library absent */
static RE_ATOMIC bool     s_user_ns;
static RE_ATOMIC bool     s_user_agc;
static RE_ATOMIC bool     s_aec_on;
/* The app asked for the suppressor while it owns the device on a platform
 * whose own driver cancels echo.  The one way the suppressor runs there:
 * taking the device over never switches it on by itself. */
static RE_ATOMIC bool     s_supp_external_req;
/* Bumped whenever a WebRTC config input changes; bsdk_webrtc reapplies its
 * APM config when the generation it applied is behind. */
static RE_ATOMIC uint32_t s_webrtc_gen;

/* The phone's own voice path (AEC/NS/AGC) is under us right now. */
static bool voice_path_active(void)
{
	return bsdk_platform_has_aec() && !bsdk_audio_external_selected();
}

void bsdk_ap_effective(struct bsdk_ap_effective *e)
{
	unsigned backend = re_atomic_rlx(&s_backend);
	bool aec_on = re_atomic_rlx(&s_aec_on);
	bool ns     = re_atomic_rlx(&s_user_ns);
	bool agc    = re_atomic_rlx(&s_user_agc);
	bool vp     = voice_path_active();

	e->webrtc_aec = backend == BACKEND_WEBRTC && aec_on;
	e->webrtc_ns  = backend == BACKEND_WEBRTC && ns;
	/* With AEC off, WebRTC keeps running for its NS: bsdk_ns is not in
	 * play in WEBRTC mode, so stopping it would drop NS too. */
	e->webrtc     = e->webrtc_aec || e->webrtc_ns;

	/* Not on top of the phone's own NS/AGC.  Stacked there they remove no
	 * noise that is still in the signal and only cost speech. */
	e->ns  = ns && !vp && backend != BACKEND_WEBRTC;
	e->agc = agc && !vp;

	e->suppressor = backend == BACKEND_SUPPRESSOR && aec_on &&
	                (!bsdk_platform_has_aec() ||
	                 (bsdk_audio_external_selected() &&
	                  re_atomic_rlx(&s_supp_external_req)));
}

void bsdk_ap_log_chain(const char *why)
{
	struct bsdk_ap_effective e;
	bsdk_ap_effective(&e);

	bool vp       = voice_path_active();
	bool user_ns  = re_atomic_rlx(&s_user_ns);
	bool user_agc = re_atomic_rlx(&s_user_agc);
	bool aec_on   = re_atomic_rlx(&s_aec_on);
	unsigned backend = re_atomic_rlx(&s_backend);

	const char *ns = e.webrtc_ns ? "webrtc"
	               : e.ns ? "gate"
	               : (user_ns && vp) ? "off (platform voice path)"
	               : "off";
	const char *agc = e.agc ? "on"
	                : (user_agc && vp) ? "off (platform voice path)"
	                : "off";
	const char *aec = e.webrtc_aec ? "webrtc"
	                : e.suppressor
	                  ? (re_atomic_rlx(&s_webrtc_fallback)
	                     ? "suppressor (webrtc unavailable)" : "suppressor")
	                : (aec_on && backend == BACKEND_SUPPRESSOR &&
	                   bsdk_platform_has_aec())
	                  ? (bsdk_audio_external_selected()
	                     ? "off (app-owned device; set_aec_mode to enable)"
	                     : "off (platform canceller)")
	                : "off";

	/* Say what the microphone actually goes through.  Every one of these is
	 * capable of taking the TX path down by 20 dB, so "what is touching the
	 * mic" is the first question worth answering when a caller reports that
	 * the far end cannot hear them — on a device, from a log, without a
	 * debugger. */
	info("EchoSDK: TX chain [%s]: mic_gain=%s ns=%s agc=%s aec=%s\n",
	     why ? why : "",
	     load_f(&g_mic_gain_bits) != 1.0f ? "on" : "unity/bypass",
	     ns, agc, aec);
}

void bsdk_ap_set_ns(bool on)
{
	re_atomic_rlx_set(&s_user_ns, on);
	re_atomic_rlx_add(&s_webrtc_gen, 1u);
	bsdk_ap_log_chain("set_ns");
}

void bsdk_ap_set_agc(bool on)
{
	re_atomic_rlx_set(&s_user_agc, on);
	bsdk_ap_log_chain("set_agc");
}

void bsdk_ap_set_aec(bool on)
{
	re_atomic_rlx_set(&s_aec_on, on);
	re_atomic_rlx_set(&s_supp_external_req,
	                  on && bsdk_audio_external_selected());
	re_atomic_rlx_add(&s_webrtc_gen, 1u);
	bsdk_ap_log_chain("set_aec");
}

void bsdk_ap_external_changed(bool external)
{
	/* The platform device is back, and its canceller with it: a
	 * suppressor the app asked for while it owned the device goes. */
	if (!external)
		re_atomic_rlx_set(&s_supp_external_req, false);
	bsdk_ap_log_chain(external ? "app-owned device" : "platform device");
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

#define DB_MIN       (-100.0f)
#define FULL_SCALE_SQ (32768.0f * 32768.0f)

static inline float frame_dt(const struct auframe *af)
{
	if (!af->srate || !af->ch || !af->sampc)
		return 0.020f;
	return (float)af->sampc / ((float)af->srate * (float)af->ch);
}

/* One-pole coefficient for time constant tau over dt. */
static inline float coef(float dt, float tau)
{
	return 1.0f - expf(-dt / tau);
}

static float frame_level_db(const int16_t *p, size_t n)
{
	if (!n)
		return DB_MIN;
	float sum_sq = 0.0f;
	for (size_t i = 0; i < n; i++)
		sum_sq += (float)p[i] * (float)p[i];
	float ms = sum_sq / (float)n;
	if (ms <= 0.0f)
		return DB_MIN;
	float db = 10.0f * log10f(ms / FULL_SCALE_SQ);
	return db < DB_MIN ? DB_MIN : db;
}

static inline float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }

static inline int16_t sat16(float v)
{
	return (v > 32767.0f) ? 32767 : (v < -32768.0f) ? -32768 : (int16_t)v;
}

static inline void apply_gain(int16_t *p, size_t n, float g)
{
	for (size_t i = 0; i < n; i++) {
		int32_t s = (int32_t)((float)p[i] * g);
		p[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
}

/* Gain moving from g0 to g1 across the frame, so a change never lands as a
 * step at a frame edge (an audible click at 50 steps a second). */
static void apply_ramp(int16_t *p, size_t n, float g0, float g1)
{
	if (g0 == 1.0f && g1 == 1.0f)
		return;
	if (g0 == g1) {
		apply_gain(p, n, g1);
		return;
	}
	float step = (g1 - g0) / (float)n;
	float g = g0;
	for (size_t i = 0; i < n; i++) {
		g += step;
		p[i] = sat16((float)p[i] * g);
	}
}

/* ── Per-call registry ──────────────────────────────────────────────────── */

#define RBUF_MAX 960   /* one 10 ms block at 48 kHz stereo */

struct ap_call {
	struct le le;
	const struct audio *au;

	/* Suppressor: far-end level envelope.  Written by RX, read by TX; the
	 * pair is not updated as one, which at worst mis-ages one frame. */
	uint32_t rx_db_bits;    /* float dBFS, atomic */
	uint32_t rx_ms;         /* tmr_jiffies() when rx_db was set, atomic */

#ifdef BSDK_WEBRTC
	struct bsdk_apm *apm;   /* NULL when the library would not create one */
	bool apm_tried;

	/* capture side — TX thread only */
	uint32_t cfg_gen;
	bool     c_active;
	bool     c_warned;

	/* render side — RX thread only */
	int16_t  rbuf[RBUF_MAX];
	int16_t  rscratch[RBUF_MAX];
	size_t   rbuf_n;
	uint32_t r_srate, r_ch;
	bool     r_warned;
#endif
};

static struct list s_calls;   /* re thread only */

static void ap_call_destructor(void *arg)
{
	struct ap_call *c = arg;
	list_unlink(&c->le);
#ifdef BSDK_WEBRTC
	if (c->apm)
		bsdk_apm_destroy(c->apm);
#endif
}

/* The call's shared state, referenced.  au == NULL (no audio object to pair
 * on) gets a private one. */
static struct ap_call *ap_call_get(const struct audio *au)
{
	struct le *le;

	if (au) {
		LIST_FOREACH(&s_calls, le) {
			struct ap_call *c = le->data;
			if (c->au == au)
				return mem_ref(c);
		}
	}

	struct ap_call *c = mem_zalloc(sizeof(*c), ap_call_destructor);
	if (!c)
		return NULL;
	c->au = au;
	store_f(&c->rx_db_bits, DB_MIN);
	if (au)
		list_append(&s_calls, &c->le, c);
	return c;
}

/* ── bsdk_mic_gain (encode-only) ────────────────────────────────────────── */

struct mic_gain_enc_st { struct aufilt_enc_st base; };

static void mic_gain_enc_dtor(void *arg) { (void)arg; }

static int mic_gain_encupd(struct aufilt_enc_st **stp, void **ctx,
                            const struct aufilt *af, struct aufilt_prm *prm,
                            const struct audio *au)
{
	(void)ctx; (void)af; (void)prm; (void)au;
	*stp = mem_deref(*stp);
	struct mic_gain_enc_st *s = mem_zalloc(sizeof(*s), mic_gain_enc_dtor);
	if (!s) return ENOMEM;
	*stp = (struct aufilt_enc_st *)s;
	return 0;
}

static int mic_gain_encode(struct aufilt_enc_st *st, struct auframe *af)
{
	(void)st;
	if (af->fmt != AUFMT_S16LE) return 0;
	float g = load_f(&g_mic_gain_bits);
	if (g == 1.0f) return 0;  /* fast-path bypass — no-op at unity */
	apply_gain((int16_t *)af->sampv, af->sampc, g);
	return 0;
}

/* ── NS (noise gate) ────────────────────────────────────────────────────── */

/* Noise floor: the minimum frame level over the last NS_WINS windows of
 * NS_WIN_S each (minimum statistics).  Seeds itself in one window and follows
 * rising noise within NS_WINS × NS_WIN_S. */
#define NS_WIN_S        0.25f
#define NS_WINS         8
/* Frames below this are muted or digital silence (the voice path emits it
 * between words): they say nothing about the noise and must not drag the
 * floor down — that is what used to leave the gate open ~40 s after mute. */
#define NS_SILENCE_DB   (-80.0f)
/* A floor this high is someone talking without a pause, not noise. */
#define NS_FLOOR_MAX_DB (-40.0f)
/* Frame level above the floor: closed at or below CLOSE, open at OPEN. */
#define NS_CLOSE_DB     4.0f
#define NS_OPEN_DB      10.0f
#define NS_FLOOR_GAIN_DB (-20.0f)
/* Opens fast so word onsets survive; closes slowly, after a hold, so word
 * tails and the gaps between syllables do. */
#define NS_ATTACK_TAU_S 0.005f
#define NS_HOLD_S       0.200f
#define NS_RELEASE_DB_PER_S (-NS_FLOOR_GAIN_DB / 0.150f)

struct ns_enc_st {
	struct aufilt_enc_st base;
	bool  active;
	float gain_db;
	float hold_s;
	float win_min[NS_WINS];
	int   win_n;          /* windows filled, up to NS_WINS */
	int   win_idx;        /* next slot to write */
	float cur_min;        /* running minimum of the current window */
	float cur_s;          /* seconds into the current window */
	bool  cur_valid;      /* the current window saw a non-silent frame */
};

static void ns_enc_dtor(void *arg) { (void)arg; }

static void ns_reset(struct ns_enc_st *s)
{
	s->gain_db   = 0.0f;
	s->hold_s    = NS_HOLD_S;
	s->win_n     = 0;
	s->win_idx   = 0;
	s->cur_min   = 0.0f;
	s->cur_s     = 0.0f;
	s->cur_valid = false;
}

static int ns_encupd(struct aufilt_enc_st **stp, void **ctx,
                     const struct aufilt *af, struct aufilt_prm *prm,
                     const struct audio *au)
{
	(void)ctx; (void)af; (void)prm; (void)au;
	*stp = mem_deref(*stp);
	struct ns_enc_st *s = mem_zalloc(sizeof(*s), ns_enc_dtor);
	if (!s) return ENOMEM;
	ns_reset(s);
	*stp = (struct aufilt_enc_st *)s;
	return 0;
}

/* Floor estimate, or false while there is none yet (first window). */
static bool ns_floor(const struct ns_enc_st *s, float *floor_db)
{
	if (!s->win_n)
		return false;
	float m = s->win_min[0];
	for (int i = 1; i < s->win_n; i++)
		if (s->win_min[i] < m)
			m = s->win_min[i];
	if (s->cur_valid && s->cur_min < m)
		m = s->cur_min;
	*floor_db = m < NS_FLOOR_MAX_DB ? m : NS_FLOOR_MAX_DB;
	return true;
}

static int ns_encode(struct aufilt_enc_st *st, struct auframe *af)
{
	if (af->fmt != AUFMT_S16LE) return 0;
	struct ns_enc_st *s = (struct ns_enc_st *)st;
	int16_t *p = (int16_t *)af->sampv;
	size_t   n = af->sampc;
	if (!n) return 0;

	struct bsdk_ap_effective e;
	bsdk_ap_effective(&e);
	if (!e.ns) {
		s->active = false;
		return 0;
	}
	if (!s->active) {
		ns_reset(s);
		s->active = true;
	}

	float dt    = frame_dt(af);
	float level = frame_level_db(p, n);

	if (level > NS_SILENCE_DB) {
		if (!s->cur_valid || level < s->cur_min)
			s->cur_min = level;
		s->cur_valid = true;
	}
	s->cur_s += dt;
	if (s->cur_s >= NS_WIN_S) {
		/* A window of pure silence keeps the floor it had. */
		if (s->cur_valid) {
			s->win_min[s->win_idx] = s->cur_min;
			s->win_idx = (s->win_idx + 1) % NS_WINS;
			if (s->win_n < NS_WINS)
				s->win_n++;
		}
		s->cur_s     = 0.0f;
		s->cur_valid = false;
	}

	/* No floor yet (first window): open.  Silence: nothing to gate, and
	 * no reason to move. */
	float floor_db, target_db = 0.0f;
	bool  speech = false;
	if (level <= NS_SILENCE_DB) {
		target_db = s->gain_db;
	}
	else if (ns_floor(s, &floor_db)) {
		float snr = level - floor_db;
		if (snr >= NS_OPEN_DB) {
			target_db = 0.0f;
			speech    = true;
		}
		else if (snr <= NS_CLOSE_DB) {
			target_db = NS_FLOOR_GAIN_DB;
		}
		else {
			target_db = NS_FLOOR_GAIN_DB *
			            (NS_OPEN_DB - snr) / (NS_OPEN_DB - NS_CLOSE_DB);
		}
	}

	float g0 = db_to_lin(s->gain_db);
	if (target_db >= s->gain_db) {
		s->gain_db += (target_db - s->gain_db) * coef(dt, NS_ATTACK_TAU_S);
		if (speech)
			s->hold_s = NS_HOLD_S;
	}
	else if (s->hold_s > 0.0f) {
		s->hold_s -= dt;
	}
	else {
		s->gain_db -= NS_RELEASE_DB_PER_S * dt;
		if (s->gain_db < target_db)
			s->gain_db = target_db;
	}

	apply_ramp(p, n, g0, db_to_lin(s->gain_db));
	return 0;
}

/* ── AGC ────────────────────────────────────────────────────────────────── */

#define AGC_TARGET_DB     (-20.0f)
#define AGC_HI_DB         (AGC_TARGET_DB + 1.76f)   /* 1.5× power  */
#define AGC_LO_DB         (AGC_TARGET_DB - 1.74f)   /* 0.67× power */
#define AGC_MIN_INPUT_DB  (-50.0f)                  /* speech, not room */
#define AGC_DOWN_DB_PER_S 70.6f
#define AGC_UP_DB_PER_S   8.6f
#define AGC_GAIN_MIN_DB   (-20.0f)
#define AGC_GAIN_MAX_DB   20.0f

struct agc_enc_st {
	struct aufilt_enc_st base;
	bool  active;
	float gain_db;
};

static void agc_enc_dtor(void *arg) { (void)arg; }

static int agc_encupd(struct aufilt_enc_st **stp, void **ctx,
                      const struct aufilt *af, struct aufilt_prm *prm,
                      const struct audio *au)
{
	(void)ctx; (void)af; (void)prm; (void)au;
	*stp = mem_deref(*stp);
	struct agc_enc_st *s = mem_zalloc(sizeof(*s), agc_enc_dtor);
	if (!s) return ENOMEM;
	*stp = (struct aufilt_enc_st *)s;
	return 0;
}

static int agc_encode(struct aufilt_enc_st *st, struct auframe *af)
{
	if (af->fmt != AUFMT_S16LE) return 0;
	struct agc_enc_st *s = (struct agc_enc_st *)st;
	int16_t *p = (int16_t *)af->sampv;
	size_t   n = af->sampc;
	if (!n) return 0;

	struct bsdk_ap_effective e;
	bsdk_ap_effective(&e);
	if (!e.agc) {
		s->active = false;
		return 0;
	}
	if (!s->active) {
		s->gain_db = 0.0f;
		s->active  = true;
	}

	float dt    = frame_dt(af);
	float level = frame_level_db(p, n);
	float g0    = db_to_lin(s->gain_db);

	if (level > AGC_MIN_INPUT_DB) {
		float out = level + s->gain_db;
		if (out > AGC_HI_DB)
			s->gain_db -= AGC_DOWN_DB_PER_S * dt;
		else if (out < AGC_LO_DB)
			s->gain_db += AGC_UP_DB_PER_S * dt;
	}
	if (s->gain_db < AGC_GAIN_MIN_DB) s->gain_db = AGC_GAIN_MIN_DB;
	if (s->gain_db > AGC_GAIN_MAX_DB) s->gain_db = AGC_GAIN_MAX_DB;

	apply_ramp(p, n, g0, db_to_lin(s->gain_db));
	return 0;
}

/* ── AEC (half-duplex echo suppressor) ──────────────────────────────────── */

/* Far-end level at the filter → TX attenuation, interpolated in dB:
 * none at or below RX_LOW, the full floor at or above RX_HIGH. */
#define SUPP_RX_LOW_DB   (-44.0f)
#define SUPP_RX_HIGH_DB  (-21.0f)
/* The RX filter point is upstream of the jitter buffer and the device, so
 * the echo reaches the mic ~60–150 ms after a frame passes here.  Hold the
 * level that long before letting it decay, or the echo tail leaks through. */
#define SUPP_HOLD_MS     150u
/* While the peak is held, a frame this close to it counts as the far end
 * still talking and restarts the hold.  Without the margin only a new peak
 * would, and in steady speech the hold would run from some older, louder
 * frame.  Once the envelope decays it is not refreshed — a steady far-end
 * noise floor would otherwise keep it pinned 6 dB above the real level. */
#define SUPP_REFRESH_DB  6.0f
/* Then decay fast enough that TX is back within ~1 dB ~300 ms after the far
 * end stops: 150 dB/s also matches a typical room's echo tail. */
#define SUPP_RELEASE_DB_PER_S 150.0f
#define SUPP_TAU_DOWN_S  0.005f
#define SUPP_TAU_UP_S    0.030f

struct aec_enc_st {
	struct aufilt_enc_st base;
	struct ap_call *call;
	bool  active;
	float gain;
};
struct aec_dec_st {
	struct aufilt_dec_st base;
	struct ap_call *call;
};

static void aec_enc_dtor(void *arg)
{
	struct aec_enc_st *s = arg;
	mem_deref(s->call);
}

static void aec_dec_dtor(void *arg)
{
	struct aec_dec_st *s = arg;
	mem_deref(s->call);
}

/* The far-end envelope at time now_ms: the stored peak, held, then decayed.
 * With nothing arriving (DTX, hold) it keeps decaying, so a silent far end
 * can never leave TX ducked. */
static float supp_envelope(struct ap_call *c, uint32_t now_ms)
{
	float    db = load_f(&c->rx_db_bits);
	uint32_t t  = re_atomic_rlx(&c->rx_ms);
	uint32_t age = now_ms - t;

	if (db <= DB_MIN || age <= SUPP_HOLD_MS)
		return db;
	float v = db - SUPP_RELEASE_DB_PER_S * (float)(age - SUPP_HOLD_MS) / 1000.0f;
	return v < DB_MIN ? DB_MIN : v;
}

static int aec_encupd(struct aufilt_enc_st **stp, void **ctx,
                      const struct aufilt *af, struct aufilt_prm *prm,
                      const struct audio *au)
{
	(void)ctx; (void)af; (void)prm;
	*stp = mem_deref(*stp);
	struct aec_enc_st *s = mem_zalloc(sizeof(*s), aec_enc_dtor);
	if (!s) return ENOMEM;
	s->call = ap_call_get(au);
	s->gain = 1.0f;
	if (!s->call) {
		mem_deref(s);
		return ENOMEM;
	}
	*stp = (struct aufilt_enc_st *)s;
	return 0;
}

static int aec_decupd(struct aufilt_dec_st **stp, void **ctx,
                      const struct aufilt *af, struct aufilt_prm *prm,
                      const struct audio *au)
{
	(void)ctx; (void)af; (void)prm;
	*stp = mem_deref(*stp);
	struct aec_dec_st *s = mem_zalloc(sizeof(*s), aec_dec_dtor);
	if (!s) return ENOMEM;
	s->call = ap_call_get(au);
	if (!s->call) {
		mem_deref(s);
		return ENOMEM;
	}
	*stp = (struct aufilt_dec_st *)s;
	return 0;
}

static int aec_encode(struct aufilt_enc_st *st, struct auframe *af)
{
	if (af->fmt != AUFMT_S16LE) return 0;
	struct aec_enc_st *s = (struct aec_enc_st *)st;
	int16_t *p = (int16_t *)af->sampv;
	size_t   n = af->sampc;
	if (!n) return 0;

	struct bsdk_ap_effective e;
	bsdk_ap_effective(&e);
	if (!e.suppressor) {
		s->active = false;
		return 0;
	}
	if (!s->active) {
		s->gain   = 1.0f;
		s->active = true;
	}

	/* aec_suppression_level=0 → floor=1.0 (no TX attenuation)
	 * aec_suppression_level=1 → floor=0.15 (−16.5 dB, current default)
	 * The mapping is inverted because g_aec_floor_bits is a minimum gain,
	 * not a suppression amount.  See bsdk_aec_floor_store() in audio.c. */
	float floor_gain = load_f(&g_aec_floor_bits);
	float floor_db   = floor_gain >= 1.0f ? 0.0f : 20.0f * log10f(floor_gain);
	float rx         = supp_envelope(s->call, (uint32_t)tmr_jiffies());

	float atten_db;
	if (rx <= SUPP_RX_LOW_DB)
		atten_db = 0.0f;
	else if (rx >= SUPP_RX_HIGH_DB)
		atten_db = floor_db;
	else
		atten_db = floor_db * (rx - SUPP_RX_LOW_DB) /
		           (SUPP_RX_HIGH_DB - SUPP_RX_LOW_DB);

	float target = db_to_lin(atten_db);
	float dt     = frame_dt(af);
	float g0     = s->gain;
	float tau    = target < s->gain ? SUPP_TAU_DOWN_S : SUPP_TAU_UP_S;

	s->gain += (target - s->gain) * coef(dt, tau);
	apply_ramp(p, n, g0, s->gain);
	return 0;
}

static int aec_decode(struct aufilt_dec_st *st, struct auframe *af)
{
	struct aec_dec_st *s = (struct aec_dec_st *)st;
	if (af->fmt != AUFMT_S16LE || !af->sampc)
		return 0;

	/* Tracked whether or not the suppressor is on, so switching it on
	 * mid-call starts from the far end's real level. */
	float    db  = frame_level_db((const int16_t *)af->sampv, af->sampc);
	uint32_t now = (uint32_t)tmr_jiffies();
	float    cur = supp_envelope(s->call, now);
	bool     held = now - re_atomic_rlx(&s->call->rx_ms) <= SUPP_HOLD_MS;

	if (db >= cur) {
		re_atomic_rlx_set(&s->call->rx_ms, now);
		store_f(&s->call->rx_db_bits, db);
	}
	else if (held && db >= cur - SUPP_REFRESH_DB) {
		re_atomic_rlx_set(&s->call->rx_ms, now);
	}
	return 0;
}

/* ── bsdk_webrtc (WebRTC APM: AEC3 + NS) ────────────────────────────────── */

#ifdef BSDK_WEBRTC

struct webrtc_enc_st {
	struct aufilt_enc_st base;
	struct ap_call *call;
};
struct webrtc_dec_st {
	struct aufilt_dec_st base;
	struct ap_call *call;
};

static void webrtc_enc_dtor(void *arg)
{
	struct webrtc_enc_st *s = arg;
	mem_deref(s->call);
}

static void webrtc_dec_dtor(void *arg)
{
	struct webrtc_dec_st *s = arg;
	mem_deref(s->call);
}

/* Setup must never fail for a reason of ours: an error here stops baresip
 * building the rest of the chain (audio.c aufilt_setup breaks out), which
 * would take every later filter off this call.  A call whose APM will not
 * come up gets a pass-through state and a warning. */
static struct ap_call *webrtc_call_get(const struct audio *au)
{
	struct ap_call *c = ap_call_get(au);
	if (!c || c->apm_tried)
		return c;

	c->apm_tried = true;
	c->apm = bsdk_apm_create();
	if (!c->apm) {
		warning("EchoSDK: WebRTC APM would not start; "
		        "this call has no echo cancellation\n");
		return c;
	}

	struct bsdk_ap_effective e;
	bsdk_ap_effective(&e);
	c->cfg_gen = re_atomic_rlx(&s_webrtc_gen);
	bsdk_apm_configure(c->apm, e.webrtc_aec, e.webrtc_ns);
	return c;
}

static int webrtc_encupd(struct aufilt_enc_st **stp, void **ctx,
                         const struct aufilt *af, struct aufilt_prm *prm,
                         const struct audio *au)
{
	(void)ctx; (void)af; (void)prm;
	*stp = mem_deref(*stp);
	struct webrtc_enc_st *s = mem_zalloc(sizeof(*s), webrtc_enc_dtor);
	if (!s) return ENOMEM;
	s->call = webrtc_call_get(au);
	if (!s->call) {
		mem_deref(s);
		return ENOMEM;
	}
	*stp = (struct aufilt_enc_st *)s;
	return 0;
}

static int webrtc_decupd(struct aufilt_dec_st **stp, void **ctx,
                         const struct aufilt *af, struct aufilt_prm *prm,
                         const struct audio *au)
{
	(void)ctx; (void)af; (void)prm;
	*stp = mem_deref(*stp);
	struct webrtc_dec_st *s = mem_zalloc(sizeof(*s), webrtc_dec_dtor);
	if (!s) return ENOMEM;
	s->call = webrtc_call_get(au);
	if (!s->call) {
		mem_deref(s);
		return ENOMEM;
	}
	*stp = (struct aufilt_dec_st *)s;
	return 0;
}

/* 10 ms of samples at the frame's rate, or 0 if that is not an APM rate. */
static size_t webrtc_block(const struct auframe *af)
{
	switch (af->srate) {
	case 8000: case 16000: case 32000: case 48000: break;
	default: return 0;
	}
	if (af->ch < 1 || af->ch > 2)
		return 0;
	return (size_t)af->srate / 100 * af->ch;
}

static int webrtc_encode(struct aufilt_enc_st *st, struct auframe *af)
{
	struct webrtc_enc_st *s = (struct webrtc_enc_st *)st;
	struct ap_call *c = s->call;

	if (af->fmt != AUFMT_S16LE || !c->apm || !af->sampc)
		return 0;

	struct bsdk_ap_effective e;
	bsdk_ap_effective(&e);
	if (!e.webrtc) {
		c->c_active = false;
		return 0;
	}

	uint32_t gen = re_atomic_rlx(&s_webrtc_gen);
	if (!c->c_active || gen != c->cfg_gen) {
		bsdk_apm_configure(c->apm, e.webrtc_aec, e.webrtc_ns);
		c->cfg_gen  = gen;
		c->c_active = true;
	}

	size_t block = webrtc_block(af);
	if (!block || af->sampc % block) {
		if (!c->c_warned) {
			warning("EchoSDK: webrtc: TX frame of %zu samples at "
			        "%u Hz/%u ch is not whole 10 ms blocks; "
			        "passing it through\n",
			        af->sampc, af->srate, af->ch);
			c->c_warned = true;
		}
		return 0;
	}

	int16_t *p = (int16_t *)af->sampv;
	for (size_t i = 0; i < af->sampc; i += block) {
		int r = bsdk_apm_capture(c->apm, p + i, af->srate, af->ch);
		if (r && !c->c_warned) {
			warning("EchoSDK: webrtc: capture error %d\n", r);
			c->c_warned = true;
		}
	}

	/* TX errors only log in baresip, but at 50 lines a second: never. */
	return 0;
}

/* Feeds the far end to the APM in whole 10 ms blocks, carrying any remainder
 * to the next frame.  Never changes the frame, and always returns 0: an RX
 * filter error makes baresip drop the frame, i.e. the far end's audio. */
static int webrtc_decode(struct aufilt_dec_st *st, struct auframe *af)
{
	struct webrtc_dec_st *s = (struct webrtc_dec_st *)st;
	struct ap_call *c = s->call;

	if (af->fmt != AUFMT_S16LE || !c->apm || !af->sampc)
		return 0;

	struct bsdk_ap_effective e;
	bsdk_ap_effective(&e);
	if (!e.webrtc) {
		c->rbuf_n = 0;
		return 0;
	}

	size_t block = webrtc_block(af);
	if (!block || block > RBUF_MAX) {
		if (!c->r_warned) {
			warning("EchoSDK: webrtc: RX at %u Hz/%u ch is not an "
			        "APM format; no echo reference\n",
			        af->srate, af->ch);
			c->r_warned = true;
		}
		return 0;
	}
	if (af->srate != c->r_srate || af->ch != c->r_ch) {
		c->r_srate = af->srate;
		c->r_ch    = af->ch;
		c->rbuf_n  = 0;
	}

	const int16_t *src = af->sampv;
	size_t n = af->sampc;

	if (c->rbuf_n) {
		size_t take = block - c->rbuf_n;
		if (take > n)
			take = n;
		memcpy(c->rbuf + c->rbuf_n, src, take * sizeof(*src));
		c->rbuf_n += take;
		src += take;
		n   -= take;
		if (c->rbuf_n == block) {
			(void)bsdk_apm_render(c->apm, c->rbuf, c->rscratch,
			                      af->srate, af->ch);
			c->rbuf_n = 0;
		}
	}
	while (n >= block) {
		(void)bsdk_apm_render(c->apm, src, c->rscratch,
		                      af->srate, af->ch);
		src += block;
		n   -= block;
	}
	if (n) {
		memcpy(c->rbuf, src, n * sizeof(*src));
		c->rbuf_n = n;
	}
	return 0;
}

static struct aufilt g_webrtc_filter = {
	.name    = "bsdk_webrtc",
	.encupdh = webrtc_encupd,
	.ench    = webrtc_encode,
	.decupdh = webrtc_decupd,
	.dech    = webrtc_decode,
};

#endif /* BSDK_WEBRTC */

/* ── bsdk_spk_gain (decode-only) ────────────────────────────────────────── */

struct spk_gain_dec_st { struct aufilt_dec_st base; };

static void spk_gain_dec_dtor(void *arg) { (void)arg; }

static int spk_gain_decupd(struct aufilt_dec_st **stp, void **ctx,
                            const struct aufilt *af, struct aufilt_prm *prm,
                            const struct audio *au)
{
	(void)ctx; (void)af; (void)prm; (void)au;
	*stp = mem_deref(*stp);
	struct spk_gain_dec_st *s = mem_zalloc(sizeof(*s), (mem_destroy_h *)spk_gain_dec_dtor);
	if (!s) return ENOMEM;
	*stp = (struct aufilt_dec_st *)s;
	return 0;
}

static int spk_gain_decode(struct aufilt_dec_st *st, struct auframe *af)
{
	(void)st;
	if (af->fmt != AUFMT_S16LE) return 0;
	float g = load_f(&g_spk_gain_bits);
	if (g == 1.0f) return 0;  /* fast-path bypass — no-op at unity */
	apply_gain((int16_t *)af->sampv, af->sampc, g);
	return 0;
}

/* ── Registration ───────────────────────────────────────────────────────── */

static struct aufilt g_mic_gain_filter = {
	.name    = "bsdk_mic_gain",
	.encupdh = mic_gain_encupd,
	.ench    = mic_gain_encode,
};

static struct aufilt g_ns_filter = {
	.name    = "bsdk_ns",
	.encupdh = ns_encupd,
	.ench    = ns_encode,
};

static struct aufilt g_agc_filter = {
	.name    = "bsdk_agc",
	.encupdh = agc_encupd,
	.ench    = agc_encode,
};

static struct aufilt g_aec_filter = {
	.name    = "bsdk_aec",
	.encupdh = aec_encupd,
	.ench    = aec_encode,
	.decupdh = aec_decupd,
	.dech    = aec_decode,
};

static struct aufilt g_spk_gain_filter = {
	.name    = "bsdk_spk_gain",
	.decupdh = spk_gain_decupd,
	.dech    = spk_gain_decode,
};

/* Which echo control init settles on.  WEBRTC without the library (not
 * compiled in, or absent at runtime) falls back to the suppressor rather
 * than leaving the call with none. */
static enum aec_backend pick_backend(echosdk_aec_mode_t mode)
{
	switch (mode) {
	case ECHOSDK_AEC_OFF:
		return BACKEND_OFF;
	case ECHOSDK_AEC_WEBRTC:
#ifdef BSDK_WEBRTC
		if (bsdk_webrtc_lib_available())
			return BACKEND_WEBRTC;
		warning("EchoSDK: %s not found; WebRTC AEC unavailable, "
		        "using the built-in suppressor\n", ECHOSDK_WEBRTC_SONAME);
#else
		warning("EchoSDK: WebRTC AEC not in this build; "
		        "using the built-in suppressor\n");
#endif
		re_atomic_rlx_set(&s_webrtc_fallback, true);
		return BACKEND_SUPPRESSOR;
	case ECHOSDK_AEC_SUPPRESSOR:
	default:
		return BACKEND_SUPPRESSOR;
	}
}

void bsdk_audio_processing_init(bool ns, bool agc,
                                echosdk_aec_mode_t aec_mode,
                                float aec_suppression_level,
                                float mic_db, float spk_db)
{
	struct list *fl = baresip_aufiltl();

	/* A retried init must not find ours still linked: list_append would
	 * refuse them and leave them ahead of auconv/auresamp, which were
	 * registered afresh. */
	bsdk_audio_processing_close();

	/* Seed atomic state from config before registering filters — all of
	 * it, so nothing carries over from a previous init. */
	store_f(&g_mic_gain_bits, powf(10.0f, mic_db / 20.0f));
	store_f(&g_spk_gain_bits, powf(10.0f, spk_db / 20.0f));
	/* floor = 1.0 - level * 0.85; level=1.0 → 0.15 (default) */
	store_f(&g_aec_floor_bits, 1.0f - aec_suppression_level * 0.85f);

	re_atomic_rlx_set(&s_webrtc_fallback, false);
	re_atomic_rlx_set(&s_backend, (unsigned)pick_backend(aec_mode));
	re_atomic_rlx_set(&s_user_ns,  ns);
	re_atomic_rlx_set(&s_user_agc, agc);
	re_atomic_rlx_set(&s_aec_on,   aec_mode != ECHOSDK_AEC_OFF);
	re_atomic_rlx_set(&s_supp_external_req, false);
	re_atomic_rlx_add(&s_webrtc_gen, 1u);

	/* PROTECTED BY RE_MAIN — aufilt_register modifies the aufiltl list.
	 * Registration also enables; whether each filter runs is decided per
	 * frame (see the file comment), so nothing is ever disabled here. */
#ifdef BSDK_WEBRTC
	if (re_atomic_rlx(&s_backend) == BACKEND_WEBRTC)
		aufilt_register(fl, &g_webrtc_filter);  /* TX first, RX last */
#endif
	aufilt_register(fl, &g_mic_gain_filter);  /* TX: raw pre-boost */
	aufilt_register(fl, &g_ns_filter);
	aufilt_register(fl, &g_agc_filter);
	aufilt_register(fl, &g_aec_filter);
	aufilt_register(fl, &g_spk_gain_filter);  /* RX only */

	bsdk_ap_log_chain("init");
}

void bsdk_audio_processing_close(void)
{
	/* PROTECTED BY RE_MAIN */
#ifdef BSDK_WEBRTC
	aufilt_unregister(&g_webrtc_filter);
#endif
	aufilt_unregister(&g_mic_gain_filter);
	aufilt_unregister(&g_ns_filter);
	aufilt_unregister(&g_agc_filter);
	aufilt_unregister(&g_aec_filter);
	aufilt_unregister(&g_spk_gain_filter);
}
