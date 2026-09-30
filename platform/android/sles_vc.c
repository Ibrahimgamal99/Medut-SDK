/**
 * @file sles_vc.c  Android OpenSLES audio driver, voice-communication tuned
 *
 * baresip's stock `opensles` module records with the default (generic) input
 * preset and plays on the media stream, so the platform's built-in echo
 * cancellation and noise suppression never engage.  This driver is EchoSDK's
 * own replacement (third_party is never patched):
 *
 *   recorder — requests SL_IID_ANDROIDCONFIGURATION and sets
 *              SL_ANDROID_KEY_RECORDING_PRESET =
 *              SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION before
 *              Realize(), which routes capture through the device's
 *              hardware/HAL AEC + NS + AGC voice path.
 *   player   — sets SL_ANDROID_KEY_STREAM_TYPE = SL_ANDROID_STREAM_VOICE
 *              so playback uses the voice-call volume/routing domain
 *              (matches AudioManager.MODE_IN_COMMUNICATION).
 *
 * Registered directly via ausrc_register()/auplay_register() as "sles_vc"
 * (no baresip module-table entry needed).  modules_init.c prefers this
 * driver and falls back to the stock "opensles" module if init fails.
 *
 * Both configuration keys are best-effort: if SetConfiguration is rejected
 * (some vendor libs), the stream still opens with defaults — identical
 * behavior to the stock module, just without the voice tuning.
 */

#include <string.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <re.h>
#include <rem.h>
#include "../../src/echosdk_internal.h"

/* Queue depth per direction, in PTIME slices.
 *
 * Capture gets four.  Two is the minimum that works at all — one buffer in
 * flight, one for AudioFlinger to write into — which leaves no headroom for a
 * scheduling hiccup longer than one PTIME, and at PTIME=10 with an 8 kHz
 * capture that is a 160-byte slice against a HAL that would rather work in
 * 20 ms at 48 kHz.  Four keeps three free at all times, and costs only queue
 * the recorder draws on when it is behind: OpenSL completes a buffer when it
 * is full either way, so depth here is not added latency.
 *
 * What prompted it, from the device on 2026-08-31: recurring "tx aubuf
 * underrun" from baresip plus a TX level reading exactly -127 dBov — all-zero
 * PCM — through most ticks of a call whose RX level moved normally.  Capture
 * was running and delivering frames, and a fraction of those frames were
 * digital silence.  Starvation is the likeliest reading of that and this is
 * the cheap, safe response to it, but it is not proof: TX mute produces the
 * identical -127 (baresip mutes in ausrc_read_handler(), upstream of where the
 * level is measured), so validate against a call whose mute state is known —
 * the example app now prints it next to the level for exactly this reason.
 *
 * Playback stays at two.  Its queue is primed full before the player starts,
 * so every extra buffer there IS 10 ms of mouth-to-ear latency, and nothing in
 * the evidence points at the playback side. */
#define N_REC_BUFFERS  4
#define N_PLAY_BUFFERS 2
#define PTIME 10

static SLObjectItf s_engine_obj = NULL;
static SLEngineItf s_engine     = NULL;

static struct ausrc  *s_ausrc  = NULL;
static struct auplay *s_auplay = NULL;

/* ── Recorder ────────────────────────────────────────────────────────────── */

struct ausrc_st {
	int16_t      *sampv[N_REC_BUFFERS];
	size_t        sampc;
	uint8_t       buffer_id;
	ausrc_read_h *rh;
	void         *arg;
	struct ausrc_prm prm;
	bool          first_frame;   /* logged once — see rec_bq_callback */

	SLObjectItf                   rec_obj;
	SLRecordItf                   rec;
	SLAndroidSimpleBufferQueueItf rec_bq;
};

static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->rec_obj) {
		SLuint32 state;
		if (SL_RESULT_SUCCESS ==
		    (*st->rec_obj)->GetState(st->rec_obj, &state) &&
		    SL_OBJECT_STATE_UNREALIZED != state)
			(*st->rec_obj)->Destroy(st->rec_obj);
	}

	for (int i = 0; i < N_REC_BUFFERS; i++)
		mem_deref(st->sampv[i]);
}

static int start_recording(struct ausrc_st *st);

static void rec_bq_callback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
	struct ausrc_st *st = context;
	struct auframe af;
	SLresult r;
	(void)bq;

	/* A recorder that opens and then never delivers is the failure mode
	 * worth naming: SetRecordState() succeeds without RECORD_AUDIO, and the
	 * queue then simply never completes a buffer.  There is no error to
	 * report anywhere — the call comes up, the far end hears nothing, and
	 * the only trace is baresip's "tx aubuf underrun".  So say plainly, once,
	 * that capture actually produced a frame; the absence of this line is
	 * the diagnosis. */
	if (!st->first_frame) {
		st->first_frame = true;
		info("sles_vc: capture delivering (%u Hz, %u ch)\n",
		     st->prm.srate, st->prm.ch);
	}

	auframe_init(&af, AUFMT_S16LE, st->sampv[st->buffer_id], st->sampc,
	             st->prm.srate, st->prm.ch);
	af.timestamp = tmr_jiffies_usec();

	st->rh(&af, st->arg);

	/* Hand the buffer we just drained back to the recorder and advance to
	 * the next one, which is already queued.  Keeping every buffer but the
	 * one in flight enqueued is the point: with a single buffer in the queue
	 * the recorder has nowhere to write between a completion callback and
	 * the next Enqueue, and AudioFlinger drops that slice of input — a
	 * steady stream of small gaps in what the far end hears.
	 *
	 * This is the only place the queue gets re-primed, so a dropped Enqueue
	 * is terminal: the callback fires when a buffer completes, and with the
	 * queue empty no buffer ever completes again. Capture then goes silent
	 * for the rest of the call with the recorder still "started" as far as
	 * AudioFlinger is concerned — no error anywhere, the far end just stops
	 * hearing us. Android returns an error here when the recorder is mid
	 * transition (audio route change, focus loss, transport reset), which is
	 * exactly when a call is most likely to be interrupted.
	 *
	 * So check it, and re-prime the queue rather than dying quietly. */
	r = (*st->rec_bq)->Enqueue(st->rec_bq, st->sampv[st->buffer_id],
	                           (unsigned int)(st->sampc * 2));
	if (SL_RESULT_SUCCESS == r) {
		st->buffer_id = (st->buffer_id + 1) % N_REC_BUFFERS;
		return;
	}

	warning("sles_vc: capture Enqueue failed (0x%x) — restarting\n",
	        (unsigned)r);

	if (start_recording(st))
		warning("sles_vc: capture restart failed; mic is now silent\n");
}

static int create_recorder(struct ausrc_st *st, struct ausrc_prm *prm)
{
	SLDataLocator_IODevice loc_dev = {SL_DATALOCATOR_IODEVICE,
	                                  SL_IODEVICE_AUDIOINPUT,
	                                  SL_DEFAULTDEVICEID_AUDIOINPUT,
	                                  NULL};
	SLDataSource audio_src = {&loc_dev, NULL};

	SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
		SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, N_REC_BUFFERS
	};
	/* Capture masks are NOT the playback masks.  Mono capture must be
	 * SL_SPEAKER_FRONT_LEFT: Android maps the OpenSL positional mask onto an
	 * AUDIO_CHANNEL_IN_* mask, and FRONT_CENTER has no input equivalent — it
	 * converts to 0, which the platform reports as
	 *
	 *   W libOpenSLES: Conversion from OpenSL ES positional channel mask 0x4
	 *                  to Android mask 0 loses channels
	 *
	 * before falling back to a guess based on the channel count.  FRONT_LEFT
	 * maps cleanly to AUDIO_CHANNEL_IN_MONO and needs no guess.  (The stock
	 * baresip opensles module uses FRONT_CENTER here, which is where this
	 * came from.) */
	int chmask = prm->ch > 1
		? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT
		: SL_SPEAKER_FRONT_LEFT;
	SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, prm->ch,
	                               prm->srate * 1000,
	                               SL_PCMSAMPLEFORMAT_FIXED_16,
	                               SL_PCMSAMPLEFORMAT_FIXED_16,
	                               chmask,
	                               SL_BYTEORDER_LITTLEENDIAN};
	SLDataSink audio_snk = {&loc_bq, &format_pcm};

	/* ANDROIDCONFIGURATION is requested (non-required) so the
	 * recording preset can be set before Realize(). */
	const SLInterfaceID id[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
	                             SL_IID_ANDROIDCONFIGURATION};
	const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
	SLresult r;

	r = (*s_engine)->CreateAudioRecorder(s_engine, &st->rec_obj,
	                                     &audio_src, &audio_snk,
	                                     2, id, req);
	if (SL_RESULT_SUCCESS != r) {
		warning("sles_vc: CreateAudioRecorder failed: %d\n", (int)r);
		return ENODEV;
	}

	/* Voice-communication preset — must precede Realize(). */
	SLAndroidConfigurationItf cfg = NULL;
	bool voice_path = false;
	r = (*st->rec_obj)->GetInterface(st->rec_obj,
	                                 SL_IID_ANDROIDCONFIGURATION, &cfg);
	if (SL_RESULT_SUCCESS == r) {
		SLuint32 preset = SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION;
		r = (*cfg)->SetConfiguration(cfg,
		                             SL_ANDROID_KEY_RECORDING_PRESET,
		                             &preset, sizeof(preset));
		if (SL_RESULT_SUCCESS != r)
			warning("sles_vc: voice-comm preset rejected (%d); "
			        "recording with default preset\n", (int)r);
		else
			voice_path = true;
	}
	else {
		cfg = NULL;
		warning("sles_vc: no ANDROIDCONFIGURATION itf (%d); "
		        "recording with default preset\n", (int)r);
	}

	r = (*st->rec_obj)->Realize(st->rec_obj, SL_BOOLEAN_FALSE);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	/* A SetConfiguration that returned success is not proof the preset
	 * took: read it back.  A vendor that cannot answer gets the benefit of
	 * the doubt — SetConfiguration did not refuse. */
	if (voice_path && cfg) {
		SLuint32 got = 0;
		SLuint32 size = sizeof(got);
		r = (*cfg)->GetConfiguration(cfg, SL_ANDROID_KEY_RECORDING_PRESET,
		                             &size, &got);
		if (SL_RESULT_SUCCESS == r &&
		    got != SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION) {
			warning("sles_vc: recorder runs preset %u, not "
			        "voice-communication\n", (unsigned)got);
			voice_path = false;
		}
	}

	/* Without the preset there is no platform AEC/NS/AGC under us, and the
	 * SDK's own filters are all there is (audio_processing.c). */
	bsdk_platform_voice_path_report(voice_path);

	r = (*st->rec_obj)->GetInterface(st->rec_obj, SL_IID_RECORD,
	                                 &st->rec);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	r = (*st->rec_obj)->GetInterface(st->rec_obj,
	                                 SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
	                                 &st->rec_bq);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	r = (*st->rec_bq)->RegisterCallback(st->rec_bq, rec_bq_callback, st);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	return 0;
}

static int start_recording(struct ausrc_st *st)
{
	SLresult r;

	(*st->rec)->SetRecordState(st->rec, SL_RECORDSTATE_STOPPED);
	(*st->rec_bq)->Clear(st->rec_bq);

	/* Prime every buffer, not just the first: the recorder must always have
	 * a free buffer to write into while the callback is draining another. */
	st->buffer_id = 0;
	for (int i = 0; i < N_REC_BUFFERS; i++) {
		r = (*st->rec_bq)->Enqueue(st->rec_bq, st->sampv[i],
		                           (unsigned int)(st->sampc * 2));
		if (SL_RESULT_SUCCESS != r)
			return ENODEV;
	}

	r = (*st->rec)->SetRecordState(st->rec, SL_RECORDSTATE_RECORDING);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	return 0;
}

static int recorder_alloc(struct ausrc_st **stp, const struct ausrc *as,
                          struct ausrc_prm *prm, const char *device,
                          ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err = 0;
	(void)device;
	(void)errh;

	if (!stp || !as || !prm || !rh)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("sles_vc: record: unsupported sample format (%s)\n",
		        aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->rh  = rh;
	st->arg = arg;
	st->prm = *prm;

	st->sampc = prm->srate * prm->ch * PTIME / 1000;
	for (int i = 0; i < N_REC_BUFFERS; i++) {
		st->sampv[i] = mem_zalloc(2 * st->sampc, NULL);
		if (!st->sampv[i]) {
			err = ENOMEM;
			goto out;
		}
	}

	err = create_recorder(st, prm);
	if (err)
		goto out;

	err = start_recording(st);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}

/* ── Player ──────────────────────────────────────────────────────────────── */

struct auplay_st {
	auplay_write_h *wh;
	void           *arg;
	int16_t        *sampv[N_PLAY_BUFFERS];
	size_t          sampc;
	uint8_t         buffer_id;
	struct auplay_prm prm;

	SLObjectItf                   mix_obj;
	SLObjectItf                   play_obj;
	SLPlayItf                     play;
	SLAndroidSimpleBufferQueueItf play_bq;
};

static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	if (st->play_obj)
		(*st->play_obj)->Destroy(st->play_obj);
	if (st->mix_obj)
		(*st->mix_obj)->Destroy(st->mix_obj);

	for (int i = 0; i < N_PLAY_BUFFERS; i++)
		mem_deref(st->sampv[i]);
}

static int start_player(struct auplay_st *st);

static void play_bq_callback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
	struct auplay_st *st = context;
	struct auframe af;
	SLresult r;

	auframe_init(&af, AUFMT_S16LE, st->sampv[st->buffer_id], st->sampc,
	             st->prm.srate, st->prm.ch);

	st->wh(&af, st->arg);

	/* Same one-way door as the capture path: the queue is only re-primed
	 * here, so a dropped Enqueue silences playback permanently. */
	r = (*st->play_bq)->Enqueue(bq, st->sampv[st->buffer_id],
	                            (unsigned int)(st->sampc * 2));
	if (SL_RESULT_SUCCESS != r) {
		warning("sles_vc: playback Enqueue failed (0x%x) — restarting\n",
		        (unsigned)r);
		if (start_player(st))
			warning("sles_vc: playback restart failed; speaker is "
			        "now silent\n");
		return;
	}

	st->buffer_id = (st->buffer_id + 1) % N_PLAY_BUFFERS;
}

static int create_player(struct auplay_st *st, struct auplay_prm *prm)
{
	SLresult r;

	/* Output mix */
	r = (*s_engine)->CreateOutputMix(s_engine, &st->mix_obj, 0, NULL,
	                                 NULL);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;
	r = (*st->mix_obj)->Realize(st->mix_obj, SL_BOOLEAN_FALSE);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
		SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, N_PLAY_BUFFERS
	};
	uint32_t ch_mask = prm->ch == 2
		? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT
		: SL_SPEAKER_FRONT_CENTER;
	SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, prm->ch,
	                               prm->srate * 1000,
	                               SL_PCMSAMPLEFORMAT_FIXED_16,
	                               SL_PCMSAMPLEFORMAT_FIXED_16,
	                               ch_mask,
	                               SL_BYTEORDER_LITTLEENDIAN};
	SLDataSource audio_src = {&loc_bufq, &format_pcm};

	SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX,
	                                      st->mix_obj};
	SLDataSink audio_snk = {&loc_outmix, NULL};

	const SLInterfaceID ids[2] = {SL_IID_BUFFERQUEUE,
	                              SL_IID_ANDROIDCONFIGURATION};
	const SLboolean req[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};

	r = (*s_engine)->CreateAudioPlayer(s_engine, &st->play_obj,
	                                   &audio_src, &audio_snk,
	                                   2, ids, req);
	if (SL_RESULT_SUCCESS != r) {
		warning("sles_vc: CreateAudioPlayer failed: %d\n", (int)r);
		return ENODEV;
	}

	/* Voice stream type — must precede Realize(). */
	SLAndroidConfigurationItf cfg;
	r = (*st->play_obj)->GetInterface(st->play_obj,
	                                  SL_IID_ANDROIDCONFIGURATION, &cfg);
	if (SL_RESULT_SUCCESS == r) {
		SLint32 stream_type = SL_ANDROID_STREAM_VOICE;
		r = (*cfg)->SetConfiguration(cfg, SL_ANDROID_KEY_STREAM_TYPE,
		                             &stream_type,
		                             sizeof(stream_type));
		if (SL_RESULT_SUCCESS != r)
			warning("sles_vc: voice stream type rejected (%d); "
			        "playing on default stream\n", (int)r);
	}

	r = (*st->play_obj)->Realize(st->play_obj, SL_BOOLEAN_FALSE);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	r = (*st->play_obj)->GetInterface(st->play_obj, SL_IID_PLAY,
	                                  &st->play);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	r = (*st->play_obj)->GetInterface(st->play_obj, SL_IID_BUFFERQUEUE,
	                                  &st->play_bq);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	r = (*st->play_bq)->RegisterCallback(st->play_bq, play_bq_callback,
	                                     st);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	return 0;
}

static int start_player(struct auplay_st *st)
{
	SLresult r;

	st->buffer_id = 0;
	for (int i = 0; i < N_PLAY_BUFFERS; i++) {
		r = (*st->play_bq)->Enqueue(st->play_bq, st->sampv[i],
		                            (unsigned int)(st->sampc * 2));
		if (SL_RESULT_SUCCESS != r)
			return ENODEV;
	}
	/* Both buffers are queued as silence; the callback fires as the
	 * first drains and starts pulling real audio. Keep buffer_id at 0
	 * so the callback writes into the buffer that drains next. */

	r = (*st->play)->SetPlayState(st->play, SL_PLAYSTATE_PLAYING);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	return 0;
}

static int player_alloc(struct auplay_st **stp, const struct auplay *ap,
                        struct auplay_prm *prm, const char *device,
                        auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err = 0;
	(void)device;

	if (!stp || !ap || !prm || !wh)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("sles_vc: play: unsupported sample format (%s)\n",
		        aufmt_name(prm->fmt));
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->wh  = wh;
	st->arg = arg;
	st->prm = *prm;

	st->sampc = prm->srate * prm->ch * PTIME / 1000;
	for (int i = 0; i < N_PLAY_BUFFERS; i++) {
		st->sampv[i] = mem_zalloc(2 * st->sampc, NULL);
		if (!st->sampv[i]) {
			err = ENOMEM;
			goto out;
		}
	}

	err = create_player(st, prm);
	if (err)
		goto out;

	err = start_player(st);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bsdk_sles_vc_init(void)
{
	SLEngineOption opts[] = {
		{ (SLuint32)SL_ENGINEOPTION_THREADSAFE,
		  (SLuint32)SL_BOOLEAN_TRUE },
	};
	SLresult r;
	int err;

	r = slCreateEngine(&s_engine_obj, 1, opts, 0, NULL, NULL);
	if (SL_RESULT_SUCCESS != r)
		return ENODEV;

	r = (*s_engine_obj)->Realize(s_engine_obj, SL_BOOLEAN_FALSE);
	if (SL_RESULT_SUCCESS != r)
		goto err_out;

	r = (*s_engine_obj)->GetInterface(s_engine_obj, SL_IID_ENGINE,
	                                  &s_engine);
	if (SL_RESULT_SUCCESS != r)
		goto err_out;

	err  = ausrc_register(&s_ausrc, baresip_ausrcl(), "sles_vc",
	                      recorder_alloc);
	err |= auplay_register(&s_auplay, baresip_auplayl(), "sles_vc",
	                       player_alloc);
	if (err)
		goto err_out;

	return 0;

 err_out:
	bsdk_sles_vc_close();
	return ENODEV;
}

void bsdk_sles_vc_close(void)
{
	s_ausrc  = mem_deref(s_ausrc);
	s_auplay = mem_deref(s_auplay);

	if (s_engine_obj) {
		(*s_engine_obj)->Destroy(s_engine_obj);
		s_engine_obj = NULL;
		s_engine     = NULL;
	}
}
