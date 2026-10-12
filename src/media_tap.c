/**
 * @file media_tap.c  PCM media tap via baresip aufilt
 *
 * Registers a global audio filter. On each audio frame (encode=TX, decode=RX),
 * looks up the call owning the stream and calls the consumer's tap callback
 * with raw int16_t PCM. The tap MUST be non-blocking.
 *
 * Note: fmt conversion — tap delivers S16LE only. If baresip uses a different
 * internal format, the frame is skipped (this is logged as a warning once).
 */

#include "medut_internal.h"
#include <rem_au.h>
#include <rem_aulevel.h>
#include <rem_auframe.h>
#include <math.h>
#include <string.h>

/* ── Filter state ────────────────────────────────────────────────────────── */

struct tap_enc_st {
	struct aufilt_enc_st  base;
	const struct audio   *au;
	struct medut_call  *lc;
};

struct tap_dec_st {
	struct aufilt_dec_st  base;
	const struct audio   *au;
	struct medut_call  *lc;
};

/* ── Forward declaration ─────────────────────────────────────────────────── */
static struct medut_call *tap_find_call(const struct audio *au);

/* Compute dBov level from a PCM frame in any supported format.
 * Returns -127.0f for silence/empty/unsupported. */
static float tap_compute_dbov(const void *sampv, size_t sampc, int fmt)
{
	if (!sampv || sampc == 0)
		return -127.0f;

	double sum_sq = 0.0;
	double norm   = 1.0;

	switch (fmt) {
	case AUFMT_S16LE: {
		const int16_t *s = sampv;
		for (size_t i = 0; i < sampc; i++)
			sum_sq += (double)s[i] * (double)s[i];
		norm = 32768.0;
		break;
	}
	case AUFMT_S32LE: {
		const int32_t *s = sampv;
		for (size_t i = 0; i < sampc; i++) {
			double v = (double)s[i];
			sum_sq += v * v;
		}
		norm = 2147483648.0;
		break;
	}
	case AUFMT_FLOAT: {
		const float *s = sampv;
		for (size_t i = 0; i < sampc; i++) {
			double v = (double)s[i];
			sum_sq += v * v;
		}
		norm = 1.0;
		break;
	}
	case AUFMT_S24_3LE: {
		const uint8_t *p = sampv;
		for (size_t i = 0; i < sampc; i++) {
			int32_t v = (int32_t)p[0]
			          | ((int32_t)p[1] << 8)
			          | ((int32_t)(int8_t)p[2] << 16);
			sum_sq += (double)v * (double)v;
			p += 3;
		}
		norm = 8388608.0;
		break;
	}
	default:
		return -127.0f;
	}

	double rms = sqrt(sum_sq / (double)sampc);
	if (rms <= 0.0)
		return -127.0f;
	return (float)(20.0 * log10(rms / norm));
}

/* ── Filter callbacks ────────────────────────────────────────────────────── */

static int tap_encupd(struct aufilt_enc_st **stp, void **ctx,
                       const struct aufilt *af, struct aufilt_prm *prm,
                       const struct audio *au)
{
	(void)ctx; (void)af; (void)prm;
	struct tap_enc_st *st = mem_zalloc(sizeof(*st), NULL);
	if (!st) return ENOMEM;
	st->au = au;
	st->lc = tap_find_call(au);
	*stp = (struct aufilt_enc_st *)st;
	return 0;
}

static int tap_decupd(struct aufilt_dec_st **stp, void **ctx,
                       const struct aufilt *af, struct aufilt_prm *prm,
                       const struct audio *au)
{
	(void)ctx; (void)af; (void)prm;
	struct tap_dec_st *st = mem_zalloc(sizeof(*st), NULL);
	if (!st) return ENOMEM;
	st->au = au;
	st->lc = tap_find_call(au);
	*stp = (struct aufilt_dec_st *)st;
	return 0;
}

struct tap_find_ctx { const struct audio *au; struct medut_call *found; };

static void tap_find_fn(struct medut_call *lc, void *arg)
{
	struct tap_find_ctx *ctx = arg;
	struct audio *got = lc->bc ? call_audio(lc->bc) : NULL;
	if (!ctx->found && got == ctx->au)
		ctx->found = lc;
}

static struct medut_call *tap_find_call(const struct audio *au)
{
	struct tap_find_ctx ctx = {.au = au, .found = NULL};
	bsdk_call_foreach(tap_find_fn, &ctx);
	return ctx.found;
}

static int tap_encode(struct aufilt_enc_st *st, struct auframe *af)
{
	struct tap_enc_st *ts = (struct tap_enc_st *)st;

	if (!ts->lc)
		ts->lc = tap_find_call(ts->au);
	struct medut_call *lc = ts->lc;
	if (!lc)
		return 0;

	float dbov = tap_compute_dbov(af->sampv, af->sampc, af->fmt);
	uint32_t bits; memcpy(&bits, &dbov, 4);
	re_atomic_rlx_set(&lc->tx_level_bits, bits);

	/* Raw-PCM delivery (consumer cb + recording) requires S16LE. */
	if (af->fmt != AUFMT_S16LE) return 0;

	mtx_lock(&lc->tap_lock);
	medut_media_tap_cb_t cb = lc->tap_cb;
	void *ud = lc->tap_userdata;
	mtx_unlock(&lc->tap_lock);

	if (cb) {
		cb(lc, MEDUT_MEDIA_DIR_TX,
		   (const int16_t *)af->sampv,
		   af->sampc, af->srate, af->ch,
		   af->timestamp, ud);
	}
	bsdk_record_write_frame(lc, MEDUT_MEDIA_DIR_TX,
	                        (const int16_t *)af->sampv,
	                        af->sampc, af->srate, af->ch);
	return 0;
}

static int tap_decode(struct aufilt_dec_st *st, struct auframe *af)
{
	struct tap_dec_st *ts = (struct tap_dec_st *)st;

	if (!ts->lc)
		ts->lc = tap_find_call(ts->au);
	struct medut_call *lc = ts->lc;
	if (!lc) return 0;

	float dbov = tap_compute_dbov(af->sampv, af->sampc, af->fmt);
	uint32_t bits; memcpy(&bits, &dbov, 4);
	re_atomic_rlx_set(&lc->rx_level_bits, bits);

	/* Raw-PCM delivery (consumer cb + recording) requires S16LE. */
	if (af->fmt != AUFMT_S16LE) return 0;

	const int16_t *samples = (const int16_t *)af->sampv;
	size_t n = af->sampc;

	mtx_lock(&lc->tap_lock);
	medut_media_tap_cb_t cb = lc->tap_cb;
	void *ud = lc->tap_userdata;
	mtx_unlock(&lc->tap_lock);

	if (cb) {
		cb(lc, MEDUT_MEDIA_DIR_RX,
		   samples, n, af->srate, af->ch,
		   af->timestamp, ud);
	}
	bsdk_record_write_frame(lc, MEDUT_MEDIA_DIR_RX,
	                        samples, n, af->srate, af->ch);
	return 0;
}

/* ── Filter registration ─────────────────────────────────────────────────── */

static struct aufilt s_tap_filter = {
	.name     = "medut_tap",
	.encupdh  = tap_encupd,
	.ench     = tap_encode,
	.decupdh  = tap_decupd,
	.dech     = tap_decode,
};

static bool s_tap_registered = false;

static void ensure_tap_registered(void)
{
	if (s_tap_registered) return;
	aufilt_register(baresip_aufiltl(), &s_tap_filter);
	s_tap_registered = true;
}

void bsdk_tap_global_init(void)
{
	ensure_tap_registered();
}

void bsdk_tap_global_reset(void)
{
	if (s_tap_registered) {
		aufilt_unregister(&s_tap_filter);
		s_tap_registered = false;
	}
}

/* ── Public API ──────────────────────────────────────────────────────────── */

typedef struct {
	struct medut_call    *lc;
	medut_media_tap_cb_t  cb;
	void                   *userdata;
} set_tap_ctx_t;

static void set_tap_fn(void *arg)
{
	set_tap_ctx_t *ctx = arg;
	if (ctx->cb)
		ensure_tap_registered();
	mtx_lock(&ctx->lc->tap_lock);
	ctx->lc->tap_cb       = ctx->cb;
	ctx->lc->tap_userdata = ctx->userdata;
	mtx_unlock(&ctx->lc->tap_lock);
}

int medut_call_set_media_tap(medut_call_handle_t call,
                                medut_media_tap_cb_t cb,
                                void *userdata)
{
	if (!call) return MEDUT_ERR_INVAL;
	set_tap_ctx_t ctx = {.lc = call, .cb = cb, .userdata = userdata};
	return bsdk_dispatch_sync(set_tap_fn, &ctx);
}
