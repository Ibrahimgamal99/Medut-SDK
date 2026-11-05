/**
 * @file aureceiver.c  Audio stream receiver
 *
 * Copyright (C) 2023 Alfred E. Heggestad, Christian Spielberger
 *
 * Adapted from baresip's src/aureceiver.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */
#include <string.h>
#include <stdlib.h>
#include <re.h>
#include "mdt_media.h"
#include <re_atomic.h>
#include <rem.h>


/**
 * Audio receive pipeline
 *
 \verbatim

 Processing decoder pipeline:

       .--------.   .-------.   .--------.   .--------.
 |\    |        |   |       |   |        |   |        |
 | |<--| auplay |<--| aubuf |<--| aufilt |<--| decode |<--- RTP
 |/    |        |   |       |   |        |   |        |
       '--------'   '-------'   '--------'   '--------'

 \endverbatim
 */

enum {
	JITTER_EMA_COEFF   = 128,     /**< Jitter EMA coefficient            */
};


struct mdt_audio_recv {
	enum aufmt fmt;               /**< Decoder sample format             */
	void *sampv;                  /**< Decoder sample buffer             */
	size_t sampvsz;               /**< Decoder sample buffer size        */
	const struct mdt_config_audio *cfg;  /**< Audio configuration            */
	struct mdt_audec_state *dec;      /**< Audio decoder state (optional)    */
	const struct mdt_aucodec *ac;     /**< Current audio decoder             */
	struct aubuf *aubuf;          /**< Audio buffer before auplay        */
	mtx_t *aubuf_mtx;             /**< Mutex for aubuf allocation        */
	uint32_t ssrc;                /**< Incoming synchronization source   */
	struct list filtl;            /**< Audio filters in decoding order   */
	uint64_t t;                   /**< Last auframe push time            */
	uint32_t ptime;               /**< Packet time for receiving [us]    */

	double level_last;            /**< Last audio level value [dBov]     */
	bool level_set;               /**< True if level_last is set         */
	struct mdt_timestamp_recv ts_recv;/**< Receive timestamp state           */
	uint8_t extmap_aulevel;       /**< ID Range 1-14 inclusive           */
	int pt;                       /**< Payload type of audio codec       */

	uint64_t n_discard;           /**< Nbr of discarded packets          */
	RE_ATOMIC uint64_t latency;   /**< Latency in [ms]                   */

#ifndef RELEASE
	struct {
		uint64_t t;           /**< Last auframe push time            */
		int32_t jitter;       /**< Auframe push jitter [us]          */
		int32_t dmax;         /**< Max deviation [us]                */
	} stats;
#endif

	mtx_t *mtx;

	const struct mdt_auplay *ap;      /**< Audio player module               */
	struct mdt_auplay_st *auplay;     /**< Audio player                      */
	struct mdt_auplay_prm auplay_prm; /**< Audio player parameters           */
	char *module;                 /**< Audio player module name          */
	char *device;                 /**< Audio player device name          */
	enum aufmt play_fmt;          /**< Sample format for audio playback  */
	bool done_first;              /**< First auplay write done flag      */
	/* burst concealment, see aurecv_plc_playout() */
	size_t plc_level;             /**< aubuf bytes after last real frame */
	size_t plc_frame_sz;          /**< Bytes of that frame               */
	uint64_t plc_next_ts;         /**< Timestamp just after it           */
	uint64_t plc_start_ts;        /**< plc_next_ts when it was pushed    */
	unsigned plc_run;             /**< Frames concealed at playout since */
	size_t plc_read_sz;           /**< Bytes the device read then        */
};


static void destructor(void *arg)
{
	struct mdt_audio_recv *ar = arg;

	mem_deref(ar->dec);
	mem_deref(ar->aubuf);
	mem_deref(ar->aubuf_mtx);
	mem_deref(ar->sampv);
	mem_deref(ar->mtx);
	list_flush(&ar->filtl);
	mem_deref(ar->module);
	mem_deref(ar->device);
}


static int aurecv_process_decfilt(struct mdt_audio_recv *ar, struct auframe *af)
{
	int err = 0;

	/* Process exactly one audio-frame in reverse list order */
	for (struct le *le = ar->filtl.tail; le; le = le->prev) {
		struct mdt_aufilt_dec_st *st = le->data;

		if (st->af && st->af->dech)
			err = st->af->dech(st, af);

		if (err)
			break;
	}

	return err;
}


static int aurecv_alloc_aubuf(struct mdt_audio_recv *ar, const struct auframe *af)
{
	size_t min_sz;
	size_t max_sz;
	size_t sz;
	const struct mdt_config_audio *cfg = ar->cfg;
	int err;

	sz = aufmt_sample_size(cfg->play_fmt);
	min_sz = sz * au_calc_nsamp(af->srate, af->ch, cfg->buffer.min);
	max_sz = sz * au_calc_nsamp(af->srate, af->ch, cfg->buffer.max);

	MDT_DBG("mdt_audio_recv: create audio buffer"
	      " [%u - %u ms]"
	      " [%zu - %zu bytes]\n",
	      (unsigned) cfg->buffer.min, (unsigned) cfg->buffer.max,
	      min_sz, max_sz);

	mtx_lock(ar->aubuf_mtx);
	err = aubuf_alloc(&ar->aubuf, min_sz, max_sz);
	if (err) {
		MDT_WARN("mdt_audio_recv: aubuf alloc error (%m)\n", err);
		goto out;
	}

	struct pl *id = pl_alloc_str("aureceiver");
	if (!id) {
		ar->aubuf = mem_deref(ar->aubuf);
		err = ENOMEM;
		goto out;
	}

	aubuf_set_id(ar->aubuf, id);
	mem_deref(id);

out:
	mtx_unlock(ar->aubuf_mtx);

	return err;
}


static int aurecv_push_aubuf(struct mdt_audio_recv *ar, const struct auframe *af)
{
	int err;

	if (!ar->aubuf) {
		err = aurecv_alloc_aubuf(ar, af);
		if (err)
			return err;
	}

#ifndef RELEASE
	int32_t d, da;
	uint64_t t;
	t = tmr_jiffies_usec();
	if (ar->stats.t) {
		d = (int32_t) (int64_t) ((t - ar->stats.t) - ar->ptime);
		da = abs(d);
		ar->stats.dmax = max(ar->stats.dmax, da);
		ar->stats.jitter += (da - ar->stats.jitter) / JITTER_EMA_COEFF;
	}

	ar->stats.t = t;
#endif
	err = aubuf_write_auframe(ar->aubuf, af);
	if (err)
		return err;

	uint64_t bpms = (uint64_t)af->srate * af->ch *
			aufmt_sample_size(af->fmt) / 1000;
	if (bpms)
		re_atomic_rlx_set(&ar->latency,
				  aubuf_cur_size(ar->aubuf) / bpms);

	return 0;
}


static int aurecv_plc_frame(struct mdt_audio_recv *ar,
			    const struct rtp_header *hdr,
			    const uint8_t *buf, size_t len)
{
	struct auframe af;
	size_t sampc = ar->sampvsz / aufmt_sample_size(ar->fmt);
	int err;
	const struct mdt_aucodec *ac = ar->ac;

	err = ac->plch(ar->dec, ar->fmt, ar->sampv, &sampc, buf, len);
	if (err) {
		MDT_WARN("mdt_audio_recv: %s codec PLC error: %m\n",
			ac->name, err);
		return err;
	}

	auframe_init(&af, ar->fmt, ar->sampv, sampc, ac->srate, ac->ch);
	af.timestamp = ((uint64_t) hdr->ts) * AUDIO_TIMEBASE / ac->crate;

	err = aurecv_process_decfilt(ar, &af);
	if (err)
		return err;

	return aurecv_push_aubuf(ar, &af);
}



/* how many of `lostc` lost frames to conceal.  None that
 * playout concealment has already played: again, they would only add delay.
 * Of the rest, always the last — the arriving packet's FEC rebuilds it, for
 * at most a frame of extra buffer — and otherwise as many as put the playout
 * buffer back at its level after the last real frame, less the one the
 * arriving packet brings.  Rounded to the nearest frame, not down: a packet
 * that arrives a little early, or a device that reads in bursts, leaves part
 * of a frame, and rounding that away lost the frame FEC could rebuild. */
static unsigned aurecv_plc_needed(struct mdt_audio_recv *ar, unsigned lostc)
{
	size_t cur, deficit, n;
	unsigned left;

	if (!ar->aubuf || !ar->plc_frame_sz)
		return lostc;             /* nothing to measure against yet */

	if (ar->plc_run >= lostc)
		return 0;
	left = lostc - ar->plc_run;

	cur = aubuf_cur_size(ar->aubuf);
	deficit = cur < ar->plc_level
		? (ar->plc_level - cur + ar->plc_frame_sz / 2) /
		  ar->plc_frame_sz
		: 0;
	n = deficit > 1 ? deficit - 1 : 1;

	return n < left ? (unsigned)n : left;
}


/* the device wants `need` bytes and the buffer holds less.
 * Decode concealment into it now, rather than let it run dry into silence
 * and conceal the gap only when — and after — the next packet arrives.  Up
 * to 1 s of it after the last real frame: past that the stream has stopped
 * (hold, a dead path), it is not a burst.  Called from the device thread
 * with aubuf_mtx held; never blocks on the receiver, which the network
 * thread holds while it decodes. */
static void aurecv_plc_playout(struct mdt_audio_recv *ar, size_t need)
{
	const struct mdt_aucodec *ac;

	if (mtx_trylock(ar->mtx) != thrd_success)
		return;

	ac = ar->ac;
	if (!ac || !ac->plch || !ar->dec || !ar->plc_frame_sz)
		goto out;                 /* no real frame yet, or cannot conceal */

	ar->plc_read_sz = need;

	for (int i = 0; i < 4 && aubuf_cur_size(ar->aubuf) < need; i++) {
		struct auframe af;
		size_t sampc = ar->sampvsz / aufmt_sample_size(ar->fmt);

		if (ar->plc_next_ts - ar->plc_start_ts >= AUDIO_TIMEBASE)
			break;
		if (ac->plch(ar->dec, ar->fmt, ar->sampv, &sampc, NULL, 0) ||
		    !sampc)
			break;

		auframe_init(&af, ar->fmt, ar->sampv, sampc, ac->srate, ac->ch);
		af.timestamp = ar->plc_next_ts;
		ar->plc_next_ts += (uint64_t)(sampc / ac->ch) * AUDIO_TIMEBASE /
		                   ac->srate;

		if (aurecv_process_decfilt(ar, &af) ||
		    aurecv_push_aubuf(ar, &af))
			break;
		++ar->plc_run;
	}

 out:
	mtx_unlock(ar->mtx);
}


static int aurecv_stream_decode(struct mdt_audio_recv *ar,
				const struct rtp_header *hdr,
				struct mbuf *mb, unsigned lostc)
{
	struct auframe af;
	bool marker = hdr->m;
	int err = 0;
	const struct mdt_aucodec *ac = ar->ac;

	/* No decoder set */
	if (!ac)
		return 0;

	/* playout concealment has already played this slot.
	 * Decoded, the packet would be spliced into the concealment in
	 * timestamp order and its length would stay on the delay.  Bounded:
	 * the RTP timestamp is not unwrapped here, and without the bound a
	 * wrap mid-call would discard every packet after it.
	 *
	 * Only while the buffer held a whole device read after the last real
	 * frame: then running dry means this packet was late.  A shallower
	 * buffer ran dry because the device reads more at once than it
	 * holds, and the packet is what lets it grow to fit — dropped, the
	 * next read conceals again, and the one after, for the whole call. */
	uint64_t ts = ((uint64_t) hdr->ts) * AUDIO_TIMEBASE / ac->crate;
	if (ar->plc_run && ts < ar->plc_next_ts &&
	    ar->plc_next_ts - ts < AUDIO_TIMEBASE &&
	    ar->plc_level >= ar->plc_read_sz) {
		++ar->n_discard;
		goto out;
	}

	/*
	 * Packet Loss Concealment (PLC)
	 *
	 * One or more packets were lost before this one arrived. Generate
	 * a concealment frame for each lost packet.
	 *
	 * Only the last lost packet can potentially be recovered using
	 * in-band FEC data carried in the current (just received) packet
	 * -- e.g. Opus -- so it is the only one handed the current
	 * packet's payload. Earlier losses fall back to plain
	 * concealment, since no coded data is available for them.
	 */
	if (lostc && ac->plch) {
		/* only the last aurecv_plc_needed() of them. */
		for (unsigned i = lostc - aurecv_plc_needed(ar, lostc);
		     i < lostc; i++) {

			bool last = (i == lostc - 1);

			err = aurecv_plc_frame(ar, hdr,
					last ? mbuf_buf(mb) : NULL,
					last ? mbuf_get_left(mb) : 0);
			if (err)
				goto out;
		}
	}

	size_t sampc = ar->sampvsz / aufmt_sample_size(ar->fmt);

	if (mbuf_get_left(mb)) {

		err = ac->dech(ar->dec,
				   ar->fmt, ar->sampv, &sampc,
				   marker, mbuf_buf(mb), mbuf_get_left(mb));
		if (err) {
			MDT_WARN("mdt_audio_recv: %s codec decode %zu bytes: %m\n",
				ac->name, mbuf_get_left(mb), err);
			goto out;
		}
	}
	else {
		/* PLC might be done in filters below or above within Codec */
		sampc = 0;
	}

	auframe_init(&af, ar->fmt, ar->sampv, sampc, ac->srate, ac->ch);
	af.timestamp = ((uint64_t) hdr->ts) * AUDIO_TIMEBASE / ac->crate;

	err = aurecv_process_decfilt(ar, &af);
	if (err)
		goto out;

	err = aurecv_push_aubuf(ar, &af);
	/* where burst concealment measures and continues. */
	if (!err && ar->aubuf && af.sampc && af.srate && af.ch) {
		ar->plc_level    = aubuf_cur_size(ar->aubuf);
		ar->plc_frame_sz = auframe_size(&af);
		ar->plc_next_ts  = af.timestamp +
			(uint64_t)(af.sampc / af.ch) * AUDIO_TIMEBASE / af.srate;
		ar->plc_start_ts = ar->plc_next_ts;
		ar->plc_run      = 0;
	}
 out:
	return err;
}


void mdt_aurecv_reset(struct mdt_audio_recv *ar)
{
	if (!ar)
		return;

	mtx_lock(ar->mtx);
	ar->ts_recv.is_set = false;
	ar->ts_recv.num_wraps = 0;
	aubuf_flush(ar->aubuf);
	mtx_unlock(ar->mtx);
}

/* Handle incoming stream data from the network */
void mdt_aurecv_receive(struct mdt_audio_recv *ar, const struct rtp_header *hdr,
		    struct rtpext *extv, size_t extc,
		    struct mbuf *mb, unsigned lostc)
{
	bool discard = false;
	int wrap;

	if (!mb)
		return;

	mtx_lock(ar->mtx);
	if (hdr->pt != ar->pt) {
		mtx_unlock(ar->mtx);
		return;
	}


	/* RFC 5285 -- A General Mechanism for RTP Header Extensions */
	const struct rtpext *ext = rtpext_find(extv, extc, ar->extmap_aulevel);
	if (ext) {
		ar->level_last = -(double)(ext->data[0] & 0x7f);
		ar->level_set = true;
	}

	/* Save timestamp for incoming RTP packets */

	if (!ar->ts_recv.is_set)
		mdt_timestamp_set(&ar->ts_recv, hdr->ts);

	wrap = mdt_timestamp_wrap(hdr->ts, ar->ts_recv.last);

	switch (wrap) {

	case -1:
		MDT_WARN("mdt_audio_recv: rtp timestamp wraps backwards"
			" (delta = %d) -- discard\n",
			(int32_t)(ar->ts_recv.last - hdr->ts));
		discard = true;
		break;

	case 0:
		break;

	case 1:
		++ar->ts_recv.num_wraps;
		break;

	default:
		break;
	}

	ar->ts_recv.last = hdr->ts;

	if (discard) {
		++ar->n_discard;
		goto out;
	}

	(void)aurecv_stream_decode(ar, hdr, mb, lostc);

out:
	mtx_unlock(ar->mtx);
}


void mdt_aurecv_set_extmap(struct mdt_audio_recv *ar, uint8_t aulevel)
{
	if (!ar)
		return;

	mtx_lock(ar->mtx);
	ar->extmap_aulevel = aulevel;
	mtx_unlock(ar->mtx);
}


int mdt_aurecv_set_module(struct mdt_audio_recv *ar, const char *module)
{
	if (!ar)
		return EINVAL;

	ar->module = mem_deref(ar->module);
	return str_dup(&ar->module, module);
}


int mdt_aurecv_set_device(struct mdt_audio_recv *ar, const char *device)
{
	if (!ar)
		return EINVAL;

	ar->device = mem_deref(ar->device);
	return str_dup(&ar->device, device);
}


int mdt_aurecv_alloc(struct mdt_audio_recv **aupp, const struct mdt_config_audio *cfg,
		 size_t sampc, uint32_t ptime)
{
	struct mdt_audio_recv *ar;
	int err;

	if (!aupp)
		return EINVAL;

	ar = mem_zalloc(sizeof(*ar), destructor);
	if (!ar)
		return ENOMEM;

	ar->cfg = cfg;
	ar->fmt   = cfg->dec_fmt;
	ar->play_fmt = cfg->play_fmt;
	ar->sampvsz = sampc * aufmt_sample_size(ar->fmt);
	ar->sampv   = mem_zalloc(ar->sampvsz, NULL);
	ar->ptime   = ptime * 1000;
	ar->pt      = -1;
	if (!ar->sampv) {
		err = ENOMEM;
		goto out;
	}

	err  = mutex_alloc(&ar->mtx);
	err |= mutex_alloc(&ar->aubuf_mtx);

out:
	if (err)
		mem_deref(ar);
	else
		*aupp = ar;

	return err;
}


void mdt_aurecv_flush(struct mdt_audio_recv *ar)
{
	if (!ar)
		return;

	mtx_lock(ar->mtx);
	aubuf_flush(ar->aubuf);

	/* Reset audio filter chain */
	list_flush(&ar->filtl);
	mtx_unlock(ar->mtx);
}


int mdt_aurecv_decoder_set(struct mdt_audio_recv *ar,
		       const struct mdt_aucodec *ac, int pt, const char *params)
{
	int err = 0;

	if (!ar || !ac)
		return EINVAL;

	MDT_INFO("mdt_audio_recv: Set audio decoder: %s %uHz %dch\n",
	     ac->name, ac->srate, ac->ch);

	mtx_lock(ar->mtx);
	if (ac != ar->ac) {
		ar->ac = ac;
		ar->dec = mem_deref(ar->dec);
		/* nothing to conceal from until the new
		 * codec's first frame. */
		ar->plc_level    = 0;
		ar->plc_frame_sz = 0;
		ar->plc_next_ts  = 0;
		ar->plc_start_ts = 0;
		ar->plc_run      = 0;
		ar->plc_read_sz  = 0;
	}

	if (ac->decupdh) {
		err = ac->decupdh(&ar->dec, ac, params);
		if (err) {
			MDT_WARN("mdt_audio_recv: alloc decoder: %m\n", err);
			goto out;
		}
	}

	ar->pt = pt;

out:
	mtx_unlock(ar->mtx);
	return err;
}


int mdt_aurecv_payload_type(const struct mdt_audio_recv *ar)
{
	if (!ar)
		return -1;

	return ar->pt;
}


int mdt_aurecv_filt_append(struct mdt_audio_recv *ar, struct mdt_aufilt_dec_st *decst)
{
	if (!ar || !decst)
		return EINVAL;

	mtx_lock(ar->mtx);
	list_append(&ar->filtl, &decst->le, decst);
	mtx_unlock(ar->mtx);

	return 0;
}


bool mdt_aurecv_filt_empty(const struct mdt_audio_recv *ar)
{
	bool empty;
	if (!ar)
		return false;

	mtx_lock(ar->mtx);
	empty = list_isempty(&ar->filtl);
	mtx_unlock(ar->mtx);

	return empty;
}


/* the decoder's state, for codec-specific counters that
 * only it keeps (modules/opus/decode.c: the FEC/PLC split). */
struct mdt_audec_state *mdt_aurecv_decoder(const struct mdt_audio_recv *ar)
{
	return ar ? ar->dec : NULL;
}


const struct mdt_aucodec *mdt_aurecv_codec(const struct mdt_audio_recv *ar)
{
	const struct mdt_aucodec *ac;

	if (!ar)
		return NULL;

	mtx_lock(ar->mtx);
	ac = ar->ac;
	mtx_unlock(ar->mtx);
	return ac;
}


static void aurecv_read(struct mdt_audio_recv *ar, struct auframe *af)
{
	if (!ar || mtx_trylock(ar->aubuf_mtx) != thrd_success)
		return;

	if (ar->aubuf) {
		/* conceal now rather than run dry */
		if (aubuf_cur_size(ar->aubuf) < auframe_size(af))
			aurecv_plc_playout(ar, auframe_size(af));
		aubuf_read_auframe(ar->aubuf, af);
	}
	else
		memset(af->sampv, 0, auframe_size(af));

	mtx_unlock(ar->aubuf_mtx);
}


void mdt_aurecv_stop(struct mdt_audio_recv *ar)
{
	if (!ar)
		return;

	ar->auplay = mem_deref(ar->auplay);
	mtx_lock(ar->mtx);
	ar->ac = NULL;
	mtx_unlock(ar->mtx);
}


void mdt_aurecv_stop_auplay(struct mdt_audio_recv *ar)
{
	if (!ar)
		return;

	ar->auplay = mem_deref(ar->auplay);
}


static void check_plframe(struct auframe *af1, struct auframe *af2)
{
	if ((af1->srate && af1->srate != af2->srate) ||
	    (af1->ch    && af1->ch    != af2->ch   )) {
		MDT_WARN("mdt_audio_recv: srate/ch of frame %u/%u vs "
			"player %u/%u. Use module auresamp!\n",
			af1->srate, af1->ch,
			af2->srate, af2->ch);
	}

	if (af1->fmt != af2->fmt) {
		MDT_WARN("mdt_audio_recv: invalid sample formats (%s -> %s). "
			"%s\n",
			aufmt_name(af1->fmt), aufmt_name(af2->fmt),
			af1->fmt == AUFMT_S16LE ?
			"Use module auconv!" : "");
	}
}


/*
 * Write samples to Audio Player.
 *
 * @note This function has REAL-TIME properties
 *
 * @note The application is responsible for filling in silence in
 *       the case of underrun
 *
 * @note This function may be called from any thread
 *
 * @note The sample format is set in ar->play_fmt
 */
static void auplay_write_handler(struct auframe *af, void *arg)
{
	struct mdt_audio_recv *ar = arg;

	if (!ar->done_first) {
		struct auframe afr;
		memset(&afr, 0, sizeof(afr));
		afr = *af;

		aurecv_read(ar, af);

		check_plframe(&afr, af);
		ar->done_first = true;
		return;
	}

	aurecv_read(ar, af);
}


int mdt_aurecv_start_player(struct mdt_audio_recv *ar, struct list *auplayl)
{
	const struct mdt_aucodec *ac = mdt_aurecv_codec(ar);
	uint32_t srate_dsp;
	uint32_t channels_dsp;
	int err = 0;

	if (!ac)
		return 0;

	srate_dsp    = ac->srate;
	channels_dsp = ac->ch;

	if (ar->cfg->srate_play && ar->cfg->srate_play != srate_dsp) {
		srate_dsp = ar->cfg->srate_play;
	}
	if (ar->cfg->channels_play && ar->cfg->channels_play != channels_dsp) {
		channels_dsp = ar->cfg->channels_play;
	}

	/* Start Audio Player */
	if (!ar->auplay && mdt_auplay_find(auplayl, NULL)) {

		struct mdt_auplay_prm prm;

		prm.srate      = srate_dsp;
		prm.ch         = channels_dsp;
		prm.ptime      = ar->ptime / 1000;
		prm.fmt        = ar->play_fmt;

		ar->auplay_prm = prm;
		err = mdt_auplay_alloc(&ar->auplay, auplayl,
				   ar->module,
				   &prm, ar->device,
				   auplay_write_handler, ar);
		if (err) {
			MDT_WARN("mdt_audio_recv: start_player failed (%s.%s): "
				"%m\n",
				ar->module, ar->device, err);
			return err;
		}

		ar->ap = mdt_auplay_find(auplayl, ar->module);

		MDT_INFO("mdt_audio_recv: player started with sample format %s\n",
		     aufmt_name(ar->play_fmt));
	}

	return 0;
}


bool mdt_aurecv_player_started(const struct mdt_audio_recv *ar)
{
	return ar ? ar->auplay != NULL : false;
}


int mdt_aurecv_print_pipeline(struct re_printf *pf, const struct mdt_audio_recv *ar)
{
	struct mbuf *mb;
	struct le *le;
	int err;

	if (!ar)
		return 0;

	mb = mbuf_alloc(32);
	if (!mb)
		return ENOMEM;

	err = re_hprintf(pf, "audio rx pipeline:  %10s",
			 ar->ap ? ar->ap->name : "(play)");
	err |= mbuf_printf(mb, " <--- aubuf");
	mtx_lock(ar->mtx);
	for (le = list_head(&ar->filtl); le; le = le->next) {
		struct mdt_aufilt_dec_st *st = le->data;

		if (st->af->dech)
			err |= mbuf_printf(mb, " <--- %s", st->af->name);
	}
	mtx_unlock(ar->mtx);

	err |= mbuf_printf(mb, " <--- %s",
			   ar->ac ? ar->ac->name : "(decoder)");

	if (err)
		goto out;

	err = re_hprintf(pf, "%b", mb->buf, mb->pos);
out:
	mem_deref(mb);
	return err;
}
