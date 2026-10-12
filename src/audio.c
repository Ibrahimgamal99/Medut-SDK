/**
 * @file audio.c  Audio device control, mute, and runtime quality settings
 */

#include "medut_internal.h"
#include <re_udp.h>
#include <math.h>


/* ── medut_audio_mute ──────────────────────────────────────────────────── */

typedef struct { struct medut_call *lc; bool mute; int result; } mute_ctx_t;

static void mute_fn(void *arg)
{
	mute_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;
	if (!lc->bc) { ctx->result = ENOENT; return; }

	struct audio *au = call_audio(lc->bc);
	if (!au) { ctx->result = ENOENT; return; }

	audio_mute(au, ctx->mute);
	ctx->result = 0;
}

int medut_audio_mute(medut_call_handle_t call, bool mute)
{
	if (!call) return MEDUT_ERR_INVAL;
	mute_ctx_t ctx = {.lc = call, .mute = mute, .result = 0};
	int err = bsdk_dispatch_sync(mute_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── medut_audio_is_muted ──────────────────────────────────────────────── */

typedef struct { struct medut_call *lc; bool result; } is_muted_ctx_t;

static void is_muted_fn(void *arg)
{
	is_muted_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;
	if (!lc->bc) { ctx->result = false; return; }
	struct audio *au = call_audio(lc->bc);
	ctx->result = au ? audio_ismuted(au) : false;
}

bool medut_audio_is_muted(medut_call_handle_t call)
{
	if (!call) return false;
	is_muted_ctx_t ctx = {.lc = call, .result = false};
	bsdk_dispatch_sync(is_muted_fn, &ctx);
	return ctx.result;
}

/* ── medut_audio_mute_rx ───────────────────────────────────────────────── */

static void mute_rx_fn(void *arg)
{
	mute_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;
	if (!lc->bc) { ctx->result = ENOENT; return; }

	struct audio *au = call_audio(lc->bc);
	if (!au) { ctx->result = ENOENT; return; }

	struct stream *strm = audio_strm(au);
	if (!strm) { ctx->result = ENOENT; return; }

	ctx->result = stream_enable_rx(strm, !ctx->mute);
}

int medut_audio_mute_rx(medut_call_handle_t call, bool mute)
{
	if (!call) return MEDUT_ERR_INVAL;
	mute_ctx_t ctx = {.lc = call, .mute = mute, .result = 0};
	int err = bsdk_dispatch_sync(mute_rx_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── medut_audio_set_input_device ──────────────────────────────────────── */

typedef struct { const char *name; int result; } device_ctx_t;

typedef struct {
	struct medut_call *lc;
	const char *mod;
	const char *dev;
	bool        is_input;
} update_call_dev_ctx_t;

static void update_call_device(struct medut_call *lc, void *arg)
{
	update_call_dev_ctx_t *ctx = arg;
	if (lc->state != MEDUT_CALL_ESTABLISHED || !lc->bc)
		return;
	struct audio *au = call_audio(lc->bc);
	if (!au)
		return;
	if (ctx->is_input)
		audio_set_source(au, ctx->mod, ctx->dev);
	else
		audio_set_player(au, ctx->mod, ctx->dev);
}

static void set_input_fn(void *arg)
{
	device_ctx_t *ctx = arg;
	struct config *bc = conf_config();
	if (ctx->name)
		str_ncpy(bc->audio.src_dev, ctx->name,
		         sizeof(bc->audio.src_dev));
	else
		bc->audio.src_dev[0] = '\0';

	/* Also switch any established calls to the new device */
	update_call_dev_ctx_t uctx = {
		.mod      = bc->audio.src_mod,
		.dev      = bc->audio.src_dev,
		.is_input = true,
	};
	bsdk_call_foreach(update_call_device, &uctx);
	ctx->result = 0;
}

int medut_audio_set_input_device(const char *name)
{
	device_ctx_t ctx = {.name = name, .result = 0};
	int err = bsdk_dispatch_sync(set_input_fn, &ctx);
	return err ? err : ctx.result;
}

static void set_output_fn(void *arg)
{
	device_ctx_t *ctx = arg;
	struct config *bc = conf_config();
	if (ctx->name)
		str_ncpy(bc->audio.play_dev, ctx->name,
		         sizeof(bc->audio.play_dev));
	else
		bc->audio.play_dev[0] = '\0';

	/* Also switch any established calls to the new device */
	update_call_dev_ctx_t uctx = {
		.mod      = bc->audio.play_mod,
		.dev      = bc->audio.play_dev,
		.is_input = false,
	};
	bsdk_call_foreach(update_call_device, &uctx);
	ctx->result = 0;
}

int medut_audio_set_output_device(const char *name)
{
	device_ctx_t ctx = {.name = name, .result = 0};
	int err = bsdk_dispatch_sync(set_output_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── App-owned audio device ──────────────────────────────────────────────── */

static void use_external_fn(void *arg)
{
	bool *enable = arg;
	struct config *bc = conf_config();
	const char *mod;

	if (*enable) {
		mod = "external";
	}
	else {
		mod = bsdk_platform_audio_mod();
		if (!mod) {
			/* Nothing to go back to — this build has no platform
			 * device compiled in. Leaving "external" in place beats
			 * pointing the stack at a module that does not exist. */
			warning("Medut: no platform audio device to restore; "
			        "staying on the app-owned device\n");
			return;
		}
	}

	str_ncpy(bc->audio.src_mod,  mod, sizeof(bc->audio.src_mod));
	str_ncpy(bc->audio.play_mod, mod, sizeof(bc->audio.play_mod));

	/* The device name is the platform module's business, not the app's. */
	bc->audio.src_dev[0]  = '\0';
	bc->audio.play_dev[0] = '\0';

	/* Move any call that is already up, the same way a device switch does:
	 * without this the change only takes effect on the next call. */
	update_call_dev_ctx_t src = {
		.mod = bc->audio.src_mod, .dev = bc->audio.src_dev,
		.is_input = true,
	};
	bsdk_call_foreach(update_call_device, &src);

	update_call_dev_ctx_t play = {
		.mod = bc->audio.play_mod, .dev = bc->audio.play_dev,
		.is_input = false,
	};
	bsdk_call_foreach(update_call_device, &play);

	info("Medut: audio device -> '%s'\n", mod);

	/* The audio filters read this per frame, and from it decide what runs
	 * on this very call (audio_processing.c):
	 *
	 * Going back to the platform device puts its hardware canceller, NS and
	 * AGC back in the path, so the SDK's own versions stop: stacked, the
	 * suppressor only half-duplexes a call the hardware had already made
	 * full-duplex, and the gate and AGC only cost speech.
	 *
	 * Deliberately one-directional for the suppressor: taking the device
	 * does NOT switch it on, on any platform.  An app that displaces our
	 * drivers is expected to capture through VOICE_COMMUNICATION /
	 * VoiceProcessingIO itself, and silently ducking its TX by 16.5 dB
	 * would be the SDK fighting the platform — the exact failure this
	 * feature exists to avoid.  Apps that want the fallback ask for it with
	 * medut_set_aec_mode().  NS and AGC follow what the app's engine
	 * reports through medut_audio_external_set_voice_path(). */
	bsdk_audio_external_sync();
	bsdk_ap_external_changed(*enable);
}

int medut_audio_use_external(bool enable)
{
	bool flag = enable;
	return bsdk_dispatch_sync(use_external_fn, &flag);
}

/* Not dispatched: an atomic the filters read per frame, written from the
 * app's device start/stop, which must not wait on the SIP thread. */
void medut_audio_external_set_voice_path(bool active)
{
	bsdk_ap_external_voice_path(active);
}

static RE_ATOMIC bool s_external_selected;

bool bsdk_audio_external_selected(void)
{
	return re_atomic_rlx(&s_external_selected);
}

void bsdk_audio_external_sync(void)
{
	re_atomic_rlx_set(&s_external_selected,
	                  0 == str_cmp(conf_config()->audio.src_mod, "external"));
}

/* ── Audio device enumeration ────────────────────────────────────────────── */

/* Both struct ausrc and struct auplay start with: le, name, dev_list.
 * Use this layout to read dev_list without casting to the full type. */
struct audio_mod_hdr {
	struct le    le;
	const char  *name;
	struct list  dev_list;
};

typedef struct {
	medut_audio_device_t *devices;
	int                     max_count;
	int                     count;
	bool                    is_input;
} list_dev_ctx_t;

/* Called per mediadev entry within a module */
static bool fill_mediadev(struct le *le, void *arg)
{
	list_dev_ctx_t *ctx = arg;
	if (ctx->count >= ctx->max_count)
		return true;  /* stop */
	struct mediadev *md = le->data;
	medut_audio_device_t *d = &ctx->devices[ctx->count++];
	str_ncpy(d->name, md->name, sizeof(d->name));
	d->description[0] = '\0';
	d->is_default = ctx->is_input ? md->src.is_default : md->play.is_default;
	return false;  /* continue */
}

/* Called per audio module (ausrc or auplay) */
static bool fill_module_devices(struct le *le, void *arg)
{
	list_dev_ctx_t *ctx = arg;
	struct audio_mod_hdr *mod = le->data;
	if (list_isempty(&mod->dev_list)) {
		/* Module registered but no enumerated devices yet — expose the
		 * module name itself as a usable device identifier. */
		if (ctx->count < ctx->max_count) {
			medut_audio_device_t *d = &ctx->devices[ctx->count++];
			str_ncpy(d->name, mod->name, sizeof(d->name));
			d->description[0] = '\0';
			d->is_default = true;  /* single entry = default */
		}
	} else {
		list_apply(&mod->dev_list, false, fill_mediadev, ctx);
	}
	return ctx->count >= ctx->max_count;  /* stop if full */
}

typedef struct {
	medut_audio_device_t *devices;
	int                     max_count;
	int                     result;
	bool                    is_input;
} enum_dev_ctx_t;

static void enum_input_fn(void *arg)
{
	enum_dev_ctx_t *ctx = arg;
	list_dev_ctx_t lctx = {
		.devices   = ctx->devices,
		.max_count = ctx->max_count,
		.count     = 0,
		.is_input  = true,
	};
	list_apply(baresip_ausrcl(), false, fill_module_devices, &lctx);
	ctx->result = lctx.count;
}

static void enum_output_fn(void *arg)
{
	enum_dev_ctx_t *ctx = arg;
	list_dev_ctx_t lctx = {
		.devices   = ctx->devices,
		.max_count = ctx->max_count,
		.count     = 0,
		.is_input  = false,
	};
	list_apply(baresip_auplayl(), false, fill_module_devices, &lctx);
	ctx->result = lctx.count;
}

int medut_audio_list_input_devices(medut_audio_device_t *devices, int max_count)
{
	if (!devices || max_count <= 0) return MEDUT_ERR_INVAL;
	enum_dev_ctx_t ctx = {
		.devices   = devices,
		.max_count = max_count,
		.result    = 0,
		.is_input  = true,
	};
	int err = bsdk_dispatch_sync(enum_input_fn, &ctx);
	return err ? err : ctx.result;
}

int medut_audio_list_output_devices(medut_audio_device_t *devices, int max_count)
{
	if (!devices || max_count <= 0) return MEDUT_ERR_INVAL;
	enum_dev_ctx_t ctx = {
		.devices   = devices,
		.max_count = max_count,
		.result    = 0,
		.is_input  = false,
	};
	int err = bsdk_dispatch_sync(enum_output_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── Runtime audio processing toggles ───────────────────────────────────── */

/* Each setter records an input for audio_processing.c on the re thread,
 * which keeps concurrent setters in order; every filter reads the inputs per
 * frame, so the change reaches calls already up on their next frame.
 *
 * Not aufilt_enable(): baresip reads that flag only when it builds a call's
 * filter chain, and rebuilds only on a codec change — so it would reach the
 * next call, never this one. */
static void set_ns_fn(void *arg)  { bsdk_ap_set_ns(*(bool *)arg); }
static void set_agc_fn(void *arg) { bsdk_ap_set_agc(*(bool *)arg); }
static void set_aec_fn(void *arg) { bsdk_ap_set_aec(*(bool *)arg); }
static void set_aec_mode_fn(void *arg) { bsdk_ap_set_aec_mode(*(bool *)arg); }

void medut_set_aec(bool enable)
{
	/* Re-enables the echo control init settled on (the aec_mode, or the
	 * suppressor if WEBRTC fell back); disables it when enable=false.
	 * Back-compat shim for the former bool aec API — so it does not go
	 * through set_aec_mode's ENOTSUP for a WEBRTC that fell back.  Not a
	 * request for the suppressor on an app-owned device: apps call this
	 * to mean "keep echo control on", and on iOS that put the suppressor
	 * on top of VoiceProcessingIO. */
	bool on = enable && g_bsdk.cfg.aec_mode != MEDUT_AEC_OFF;
	bsdk_dispatch_sync(set_aec_fn, &on);
}

int medut_set_aec_mode(medut_aec_mode_t mode)
{
	/* Only AEC_OFF ↔ init_mode transitions are valid at runtime.
	 * Switching between SUPPRESSOR and WEBRTC requires re-init. */
	if (mode != MEDUT_AEC_OFF && mode != g_bsdk.cfg.aec_mode) {
		warning("Medut: set_aec_mode: cannot switch from %d to %d at runtime "
		        "(only OFF ↔ init mode transitions allowed)\n",
		        (int)g_bsdk.cfg.aec_mode, (int)mode);
		return EINVAL;
	}

#if !defined(MEDUT_PROFILE_DESKTOP) || !defined(MEDUT_HAS_WEBRTC_AEC)
	if (mode == MEDUT_AEC_WEBRTC) {
		warning("Medut: set_aec_mode: WEBRTC AEC not available "
		        "(requires desktop build with MEDUT_WITH_WEBRTC_AEC=ON)\n");
		return ENOTSUP;
	}
#endif

	bool on = mode != MEDUT_AEC_OFF;
	return bsdk_dispatch_sync(set_aec_mode_fn, &on);
}

void medut_set_aec_suppression_level(float level)
{
	/* Negated so NaN clamps to 0 too: NaN fails every comparison. */
	if (!(level >= 0.0f)) level = 0.0f;
	if (level > 1.0f) level = 1.0f;
	/* aec_suppression_level=0 → floor=1.0 (no attenuation)
	 * aec_suppression_level=1 → floor=0.15 (−16.5 dB, default)
	 * Mapping inverted because floor is a minimum gain, not a suppression amount. */
	bsdk_aec_floor_store(1.0f - level * 0.85f);
	g_bsdk.cfg.aec_suppression_level = level;
}

void medut_set_ns(bool enable)
{
	bsdk_dispatch_sync(set_ns_fn, &enable);
	g_bsdk.cfg.ns = enable;
}

void medut_set_agc(bool enable)
{
	bsdk_dispatch_sync(set_agc_fn, &enable);
	g_bsdk.cfg.agc = enable;
}

void medut_set_mic_gain_db(float db)
{
	if (db < -20.0f) db = -20.0f;
	if (db >  20.0f) db =  20.0f;
	bsdk_mic_gain_store(powf(10.0f, db / 20.0f));
	g_bsdk.cfg.mic_gain_db = db;
}

void medut_set_speaker_gain_db(float db)
{
	if (db < -20.0f) db = -20.0f;
	if (db >  20.0f) db =  20.0f;
	bsdk_spk_gain_store(powf(10.0f, db / 20.0f));
	g_bsdk.cfg.speaker_gain_db = db;
}

/* ── Per-call DSCP ───────────────────────────────────────────────────────── */

typedef struct {
	struct medut_call *lc;
	uint8_t              dscp;
	int                  result;
} dscp_ctx_t;

static void set_dscp_rtp_fn(void *arg)
{
	dscp_ctx_t *ctx = arg;
	if (!ctx->lc->bc) { ctx->result = ENOENT; return; }

	struct audio *au = call_audio(ctx->lc->bc);
	if (!au)          { ctx->result = ENOENT; return; }

	struct stream *strm = audio_strm(au);
	if (!strm)        { ctx->result = ENOENT; return; }

	struct rtp_sock *rs = stream_rtp_sock(strm);
	if (!rs)          { ctx->result = ENOENT; return; }

	/* rtp_sock() returns the underlying struct udp_sock* as void* */
	struct udp_sock *us = rtp_sock(rs);
	if (!us)          { ctx->result = ENOENT; return; }

	ctx->result = udp_settos(us, ctx->dscp);
}

int medut_call_set_dscp_rtp(medut_call_handle_t call, uint8_t dscp)
{
	if (!call) return MEDUT_ERR_INVAL;
	dscp_ctx_t ctx = { .lc = call, .dscp = dscp, .result = 0 };
	int err = bsdk_dispatch_sync(set_dscp_rtp_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── Global jitter buffer bounds ─────────────────────────────────────────── */

typedef struct {
	medut_jbuf_type_t type;
	uint32_t            min_ms;
	uint32_t            max_ms;
} jbuf_ctx_t;

static void set_jbuf_fn(void *arg)
{
	jbuf_ctx_t *ctx = arg;
	bsdk_jbuf_configure(ctx->type, ctx->min_ms, ctx->max_ms);
}

void medut_set_jitter_buffer(uint32_t min_ms, uint32_t max_ms)
{
	jbuf_ctx_t ctx = { g_bsdk.cfg.jbuf_type, min_ms, max_ms };
	bsdk_dispatch_sync(set_jbuf_fn, &ctx);
	g_bsdk.cfg.jitter_buffer_min_ms = min_ms;
	g_bsdk.cfg.jitter_buffer_max_ms = max_ms;
}

/* ── medut_set_jitter_buffer_type ──────────────────────────────────────── */

void medut_set_jitter_buffer_type(medut_jbuf_type_t type)
{
	/* The bounds' defaults depend on the type, so re-apply them too. */
	jbuf_ctx_t ctx = { type, g_bsdk.cfg.jitter_buffer_min_ms,
	                   g_bsdk.cfg.jitter_buffer_max_ms };
	bsdk_dispatch_sync(set_jbuf_fn, &ctx);
	g_bsdk.cfg.jbuf_type = type;
}
