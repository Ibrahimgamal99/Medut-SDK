/**
 * @file record.c  Per-call WAV audio recording (mixed RX+TX)
 *
 * RX frames drive writes. Each RX frame is clip-summed with the most recent
 * TX frame before being written, producing a single mixed WAV file containing
 * both sides of the conversation. The WAV header is finalized on stop.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "medut_internal.h"

/* ── WAV header (44 bytes, PCM S16LE) ───────────────────────────────────── */

static void wav_write_header(FILE *f, uint32_t srate, uint8_t ch,
                              uint32_t data_bytes)
{
	uint16_t channels    = ch;
	uint16_t bits        = 16;
	uint32_t byte_rate   = srate * channels * 2;
	uint16_t block_align = (uint16_t)(channels * 2);
	uint32_t riff_size   = 36 + data_bytes;
	uint32_t fmt_size    = 16;
	uint16_t audio_fmt   = 1; /* PCM */

	fwrite("RIFF",       1, 4, f);
	fwrite(&riff_size,   4, 1, f);
	fwrite("WAVE",       1, 4, f);
	fwrite("fmt ",       1, 4, f);
	fwrite(&fmt_size,    4, 1, f);
	fwrite(&audio_fmt,   2, 1, f);
	fwrite(&channels,    2, 1, f);
	fwrite(&srate,       4, 1, f);
	fwrite(&byte_rate,   4, 1, f);
	fwrite(&block_align, 2, 1, f);
	fwrite(&bits,        2, 1, f);
	fwrite("data",       1, 4, f);
	fwrite(&data_bytes,  4, 1, f);
}

/* ── Sample-level clip-sum mix ───────────────────────────────────────────── */

static void mix_s16(int16_t *dst, const int16_t *src, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int32_t v = (int32_t)dst[i] + (int32_t)src[i];
		if      (v >  32767) v =  32767;
		else if (v < -32768) v = -32768;
		dst[i] = (int16_t)v;
	}
}

/* ── Internal frame writer (called from audio thread via media tap) ──────── */

void bsdk_record_write_frame(struct medut_call *lc, medut_media_dir_t dir,
                              const int16_t *pcm, size_t samples,
                              uint32_t srate, uint8_t ch)
{
	mtx_lock(&lc->rec_lock);

	if (!lc->rec_active || !lc->rec_file) {
		mtx_unlock(&lc->rec_lock);
		return;
	}

	/* Capture format from the very first frame */
	if (!lc->rec_srate) {
		lc->rec_srate = srate;
		lc->rec_ch    = ch;
	}

	/* Skip frames whose format doesn't match what we committed to */
	if (srate != lc->rec_srate || ch != lc->rec_ch) {
		mtx_unlock(&lc->rec_lock);
		return;
	}

	if (dir == MEDUT_MEDIA_DIR_TX) {
		/* Store TX frame so the next RX write can mix it in */
		size_t cap = sizeof(lc->rec_tx_buf) / sizeof(lc->rec_tx_buf[0]);
		lc->rec_tx_count = samples < cap ? samples : cap;
		memcpy(lc->rec_tx_buf, pcm, lc->rec_tx_count * sizeof(int16_t));
		mtx_unlock(&lc->rec_lock);
		return;
	}

	/* RX frame — write WAV header on the very first write */
	if (!lc->rec_hdr_written) {
		wav_write_header(lc->rec_file, lc->rec_srate, lc->rec_ch, 0);
		lc->rec_hdr_written = true;
	}

	/* Copy RX into a stack buffer so we can mix without modifying pcm */
	int16_t mix[4096];
	size_t  count = samples < 4096 ? samples : 4096;
	memcpy(mix, pcm, count * sizeof(int16_t));

	/* Clip-sum the last TX frame if it has the same length */
	if (lc->rec_tx_count == count)
		mix_s16(mix, lc->rec_tx_buf, count);

	size_t bytes = count * sizeof(int16_t);
	fwrite(mix, 1, bytes, lc->rec_file);
	lc->rec_data_bytes += (uint32_t)bytes;

	mtx_unlock(&lc->rec_lock);
}

/* ── record_start ────────────────────────────────────────────────────────── */

typedef struct {
	struct medut_call *lc;
	char                 path[512];
	int                  result;
} rec_start_ctx_t;

static void record_start_fn(void *arg)
{
	rec_start_ctx_t *ctx = arg;
	struct medut_call *lc = ctx->lc;

	mtx_lock(&lc->rec_lock);

	if (lc->rec_active) {
		mtx_unlock(&lc->rec_lock);
		ctx->result = EALREADY;
		return;
	}

	lc->rec_file = fopen(ctx->path, "wb");
	if (!lc->rec_file) {
		mtx_unlock(&lc->rec_lock);
		ctx->result = errno ? errno : EIO;
		return;
	}

	lc->rec_data_bytes  = 0;
	lc->rec_srate       = 0;
	lc->rec_ch          = 0;
	lc->rec_hdr_written = false;
	lc->rec_tx_count    = 0;
	lc->rec_active      = true;

	mtx_unlock(&lc->rec_lock);
	ctx->result = 0;
}

int medut_call_record_start(medut_call_handle_t call, const char *path)
{
	if (!call || !path) return MEDUT_ERR_INVAL;

	rec_start_ctx_t ctx = {.lc = call, .result = 0};
	str_ncpy(ctx.path, path, sizeof(ctx.path));

	return bsdk_dispatch_sync(record_start_fn, &ctx);
}

/* ── record_stop ─────────────────────────────────────────────────────────── */

static void record_stop_fn(void *arg)
{
	struct medut_call *lc = arg;

	mtx_lock(&lc->rec_lock);
	lc->rec_active     = false;
	FILE    *f         = lc->rec_file;
	uint32_t data_bytes = lc->rec_data_bytes;
	uint32_t srate     = lc->rec_srate;
	uint8_t  ch        = lc->rec_ch;
	lc->rec_file       = NULL;
	mtx_unlock(&lc->rec_lock);

	if (f) {
		if (srate) {
			rewind(f);
			wav_write_header(f, srate, ch, data_bytes);
			fflush(f);
		}
		fclose(f);
	}
}

int medut_call_record_stop(medut_call_handle_t call)
{
	if (!call) return MEDUT_ERR_INVAL;
	return bsdk_dispatch_sync(record_stop_fn, call);
}
