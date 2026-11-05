/**
 * @file audio.c  Audio stream
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 * \ref GenericAudioStream
 *
 * Adapted from baresip's src/audio.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <time.h>
#include <re.h>
#include "mdt_media.h"
#include <re_atomic.h>
#include <rem.h>


/** Magic number */
#define MAGIC 0x000a0d10


/**
 * \page GenericAudioStream Generic Audio Stream
 *
 * Implements a generic audio stream. The application can allocate multiple
 * instances of a audio stream, mapping it to a particular SDP media line.
 * The audio object has a DSP sound card sink and source, and an audio encoder
 * and decoder. A particular audio object is mapped to a generic media
 * stream object. Each audio channel has an optional audio filtering chain.
 *
 *<pre>
 *            write  read
 *              |    /|\
 *             \|/    |
 * .------.   .---------.    .-------.
 * |filter|<--|  audio  |--->|encoder|
 * '------'   |         |    |-------|
 *            | object  |--->|decoder|
 *            '---------'    '-------'
 *              |    /|\
 *              |     |
 *             \|/    |
 *         .------. .-----.
 *         |auplay| |ausrc|
 *         '------' '-----'
 *</pre>
 */

enum {
	MAX_SRATE       = 48000,  /* Maximum sample rate in [Hz] */
	MAX_CHANNELS    =     2,  /* Maximum number of channels  */
	MAX_PTIME       =    60,  /* Maximum packet time in [ms] */

	AUDIO_SAMPSZ    = MAX_SRATE * MAX_CHANNELS * MAX_PTIME / 1000,
};


/**
 * Audio transmit/encoder
 *
 *
 \verbatim

 Processing encoder pipeline:

 .    .-------.   .-------.   .--------.   .--------.
 |    |       |   |       |   |        |   |        |
 |O-->| ausrc |-->| aubuf |-->| aufilt |-->| encode |---> RTP
 |    |       |   |       |   |        |   |        |
 '    '-------'   '-------'   '--------'   '--------'

 \endverbatim
 *
 */
struct autx {
	const struct mdt_ausrc *as;       /**< Audio Source module             */
	struct mdt_ausrc_st *ausrc;       /**< Audio Source state              */
	struct mdt_ausrc_prm ausrc_prm;   /**< Audio Source parameters         */
	const struct mdt_aucodec *ac;     /**< Current audio encoder           */
	struct mdt_auenc_state *enc;      /**< Audio encoder state (optional)  */
	struct aubuf *aubuf;          /**< Packetize outgoing stream       */
	size_t aubuf_maxsz;           /**< Maximum aubuf size in [bytes]   */
	RE_ATOMIC bool aubuf_started; /**< Aubuf was started flag          */
	struct list filtl;            /**< Audio filters in encoding order */
	struct mbuf *mb;              /**< Buffer for outgoing RTP packets */
	struct mbuf *mb_telev;        /**< Buffer for Telephony Events     */
	char *module;                 /**< Audio source module name        */
	char *device;                 /**< Audio source device name        */
	void *sampv;                  /**< Sample buffer                   */
	uint32_t ptime;               /**< Packet time for sending         */
	uint64_t ts_ext;              /**< Ext. Timestamp for outgoing RTP */
	uint32_t ts_base;             /**< First timestamp sent            */
	uint32_t ts_tel;              /**< Timestamp for Telephony Events  */
	size_t psize;                 /**< Packet size for sending         */
	bool marker;                  /**< Marker bit for outgoing RTP     */
	RE_ATOMIC bool muted;         /**< Audio source is muted           */
	int cur_key;                  /**< Currently transmitted event     */
	enum aufmt src_fmt;           /**< Sample format for audio source  */
	enum aufmt enc_fmt;           /**< Sample format for encoder       */

	struct {
		uint64_t aubuf_overrun;
		uint64_t aubuf_underrun;
	} stats;

	struct {
		thrd_t tid;           /**< Audio transmit thread           */
		RE_ATOMIC bool run;   /**< Audio transmit thread running   */
	} thr;

	mtx_t *mtx;
};


struct mdt_audio_recv;


/** Generic Audio stream */
struct mdt_audio {
	MDT_MAGIC_DECL                    /**< Magic number for debugging      */
	struct autx tx;               /**< Transmit                        */
	struct mdt_audio_recv *aur;       /**< Audio Receiver                  */
	struct mdt_stream *strm;          /**< Generic media stream            */
	struct telev *telev;          /**< Telephony events                */
	struct mdt_config_audio cfg;      /**< Audio configuration             */
	bool started;                 /**< Stream is started flag          */
	bool level_enabled;           /**< Audio level RTP ext. enabled    */
	uint8_t extmap_aulevel;       /**< ID Range 1-14 inclusive         */
	mdt_audio_event_h *eventh;        /**< Event handler                   */
	mdt_audio_level_h *levelh;        /**< Audio level handler             */
	mdt_audio_err_h *errh;            /**< Audio error handler             */
	void *arg;                    /**< Handler argument                */
};


/* RFC 6464 */
static const char *uri_aulevel = "urn:ietf:params:rtp-hdrext:ssrc-audio-level";


static void stop_tx(struct autx *tx, struct mdt_audio *a)
{
	if (!tx || !a)
		return;

	mdt_stream_enable_tx(a->strm, false);
	if (re_atomic_rlx(&tx->thr.run)) {
		re_atomic_rlx_set(&tx->thr.run, false);
		thrd_join(tx->thr.tid, NULL);
	}

	/* audio source must be stopped first */
	tx->ausrc = mem_deref(tx->ausrc);
	tx->aubuf = mem_deref(tx->aubuf);

	list_flush(&tx->filtl);
}


static void audio_destructor(void *arg)
{
	struct mdt_audio *a = arg;

	MDT_DBG("audio: destroyed (started=%d)\n", a->started);

	stop_tx(&a->tx, a);
	mdt_stream_enable_rx(a->strm, false);
	mdt_aurecv_stop(a->aur);

	mem_deref(a->tx.enc);
	mem_deref(a->tx.aubuf);
	mem_deref(a->tx.mb);
	mem_deref(a->tx.mb_telev);
	mem_deref(a->tx.sampv);
	mem_deref(a->tx.module);
	mem_deref(a->tx.device);

	list_flush(&a->tx.filtl);

	mem_deref(a->strm);
	mem_deref(a->telev);
	mem_deref(a->aur);

	mem_deref(a->tx.mtx);
}


static bool aucodec_equal(const struct mdt_aucodec *a, const struct mdt_aucodec *b)
{
	if (!a || !b)
		return false;

	return a->srate == b->srate && a->ch == b->ch;
}


static int add_audio_codec(struct sdp_media *m, struct mdt_aucodec *ac)
{
	if (ac->crate < 8000) {
		MDT_WARN("audio: illegal clock rate %u\n", ac->crate);
		return EINVAL;
	}

	if (ac->ch == 0 || ac->pch == 0) {
		MDT_WARN("audio: illegal channels for audio codec '%s'\n",
			ac->name);
		return EINVAL;
	}

	return sdp_format_add(NULL, m, false, ac->pt, ac->name, ac->crate,
			      ac->pch, ac->fmtp_ench, ac->fmtp_cmph, ac, false,
			      "%s", ac->fmtp);
}


static int append_rtpext(struct mdt_audio *au, struct mbuf *mb,
			 enum aufmt fmt, const void *sampv, size_t sampc)
{
	uint8_t data[1];
	double level;
	int err;

	/* audio level must be calculated from the audio samples that
	 * are actually sent on the network. */
	level = aulevel_calc_dbov(fmt, sampv, sampc);

	data[0] = (int)-level & 0x7f;

	err = rtpext_encode(mb, au->extmap_aulevel, 1, data);
	if (err) {
		MDT_WARN("audio: rtpext_encode failed (%m)\n", err);
		return err;
	}

	return err;
}


/*
 * Encode audio and send via stream
 *
 * @note This function has REAL-TIME properties
 */
static void encode_rtp_send(struct mdt_audio *a, struct autx *tx,
			    struct auframe *af)
{
	size_t frame_size;  /* number of samples per channel */
	size_t sampc_rtp;
	size_t len;
	size_t ext_len = 0;
	uint32_t ts_delta = 0;
	bool marker = tx->marker;
	int err;

	if (!tx->ac || !tx->ac->ench)
		return;

	if (tx->ac->srate != af->srate || tx->ac->ch != af->ch) {
		MDT_WARN("audio: srate/ch of frame %u/%u vs audio codec %u/%u. "
			"Use module auresamp!\n",
			af->srate, af->ch, tx->ac->srate, tx->ac->ch);
		return;
	}

	tx->mb->pos = tx->mb->end = MDT_STREAM_PRESZ;

	if (a->level_enabled) {

		/* skip the extension header */
		tx->mb->pos += RTPEXT_HDR_SIZE;

		err = append_rtpext(a, tx->mb, af->fmt,
				    af->sampv, af->sampc);
		if (err)
			return;

		ext_len = tx->mb->pos - MDT_STREAM_PRESZ;

		/* write the Extension header at the beginning */
		tx->mb->pos = MDT_STREAM_PRESZ;

		err = rtpext_hdr_encode(tx->mb, ext_len - RTPEXT_HDR_SIZE);
		if (err)
			return;

		tx->mb->pos = MDT_STREAM_PRESZ + ext_len;
		tx->mb->end = MDT_STREAM_PRESZ + ext_len;
	}

	len = mbuf_get_space(tx->mb);

	err = tx->ac->ench(tx->enc, &marker, mbuf_buf(tx->mb), &len,
			   af->fmt, af->sampv, af->sampc);

	if ((err & 0xffff0000) == 0x00010000) {

		/* MPA needs some special treatment here */

		ts_delta = err & 0xffff;
		af->sampc = 0;
	}
	else if (err) {
		MDT_WARN("audio: %s encode error: %zu samples (%m)\n",
			tx->ac->name, af->sampc, err);
		goto out;
	}

	tx->mb->pos = MDT_STREAM_PRESZ;
	tx->mb->end = MDT_STREAM_PRESZ + ext_len + len;

	if (mbuf_get_left(tx->mb)) {

		uint32_t rtp_ts = tx->ts_ext & 0xffffffff;

		if (len) {
			mtx_lock(a->tx.mtx);
			err = mdt_stream_send(a->strm, ext_len!=0, marker, -1,
					  rtp_ts, tx->mb);
			mtx_unlock(a->tx.mtx);
			if (err)
				goto out;
		}

		if (ts_delta) {
			mtx_lock(a->tx.mtx);
			tx->ts_ext += ts_delta;
			mtx_unlock(a->tx.mtx);
			goto out;
		}
	}

	/* Convert from audio samplerate to RTP clockrate */
	sampc_rtp = af->sampc * tx->ac->crate / tx->ac->srate;

	/* The RTP clock rate used for generating the RTP timestamp is
	 * independent of the number of channels and the encoding
	 * However, MPA support variable packet durations. Thus, MPA
	 * should update the ts according to its current internal state.
	 */
	frame_size = sampc_rtp / tx->ac->ch;

	mtx_lock(a->tx.mtx);
	tx->ts_ext += (uint32_t)frame_size;
	mtx_unlock(a->tx.mtx);

 out:
	/* a frame the encoder dropped (Opus DTX) leaves the
	 * marker set, for the first packet after the gap (RFC 7587 3.1.3). */
	tx->marker = (len == 0);
}


/*
 * @note This function has REAL-TIME properties
 */
static void poll_aubuf_tx(struct mdt_audio *a)
{
	struct autx *tx = &a->tx;
	struct auframe af;
	size_t sampc;
	size_t sz;
	struct le *le;
	uint32_t srate;
	uint8_t ch;
	int err = 0;

	mtx_lock(tx->mtx);
	sz = aufmt_sample_size(tx->src_fmt);
	if (!sz) {
		mtx_unlock(tx->mtx);
		return;
	}

	sampc = tx->psize / sz;
	srate = tx->ausrc_prm.srate;
	ch = tx->ausrc_prm.ch;

	/* timed read from audio-buffer */
	auframe_init(&af, tx->src_fmt, tx->sampv, sampc, srate, ch);
	aubuf_read_auframe(tx->aubuf, &af);

	/* Process exactly one audio-frame in list order */
	for (le = tx->filtl.head; le; le = le->next) {
		struct mdt_aufilt_enc_st *st = le->data;

		if (st->af && st->af->ench)
			err |= st->af->ench(st, &af);
	}
	mtx_unlock(tx->mtx);
	if (err) {
		MDT_WARN("audio: aufilter encode: %m\n", err);
	}

	if (af.fmt != tx->enc_fmt) {
		MDT_WARN("audio: tx: invalid sample formats (%s -> %s). %s\n",
			aufmt_name(af.fmt), aufmt_name(tx->enc_fmt),
			tx->enc_fmt == AUFMT_S16LE ? "Use module auconv!" : ""
			);
	}

	/* Encode and send */
	encode_rtp_send(a, tx, &af);
}


static void check_telev(struct mdt_audio *a, struct autx *tx)
{
	const struct sdp_format *fmt;
	struct mbuf *mb = tx->mb_telev;
	bool marker = false;
	int err;

	mb->pos = mb->end = MDT_STREAM_PRESZ;

	mtx_lock(tx->mtx);
	err = telev_poll(a->telev, &marker, mb);
	mtx_unlock(tx->mtx);
	if (err)
		return;

	if (marker)
		tx->ts_tel = (uint32_t)tx->ts_ext;

	fmt = sdp_media_rformat(mdt_stream_sdpmedia(mdt_audio_strm(a)), telev_rtpfmt);
	if (!fmt)
		return;

	mb->pos = MDT_STREAM_PRESZ;
	mtx_lock(a->tx.mtx);
	err = mdt_stream_send(a->strm, false, marker, fmt->pt, tx->ts_tel, mb);
	mtx_unlock(a->tx.mtx);
	if (err) {
		MDT_WARN("audio: telev: mdt_stream_send %m\n", err);
	}
}


/*
 * Read samples from Audio Source
 *
 * @note This function has REAL-TIME properties
 *
 * @note This function may be called from any thread
 */
static void ausrc_read_handler(struct auframe *af, void *arg)
{
	struct mdt_audio *a = arg;
	struct autx *tx = &a->tx;

	mtx_lock(tx->mtx);
	enum aufmt fmt = tx->src_fmt;
	size_t psize = tx->psize;
	mtx_unlock(tx->mtx);

	if (fmt != af->fmt) {
		MDT_WARN("audio: ausrc format mismatch:"
			" expected=%d(%s), actual=%d(%s)\n",
			fmt, aufmt_name(fmt),
			af->fmt, aufmt_name(af->fmt));
		return;
	}

	if (re_atomic_rlx(&tx->muted))
		auframe_mute(af);

	if (aubuf_cur_size(tx->aubuf) >= tx->aubuf_maxsz) {

		mtx_lock(tx->mtx);
		uint64_t aubuf_overrun = ++tx->stats.aubuf_overrun;
		mtx_unlock(tx->mtx);

		MDT_DBG("audio: tx aubuf overrun (total %llu)\n",
		      aubuf_overrun);
	}

	(void)aubuf_write_auframe(tx->aubuf, af);

	if (!re_atomic_rlx(&tx->aubuf_started) &&
	    (aubuf_cur_size(tx->aubuf) >= psize)) {
		re_atomic_rlx_set(&tx->aubuf_started, true);
	}
}


static void ausrc_error_handler(int err, const char *str, void *arg)
{
	struct mdt_audio *a = arg;
	MDT_MAGIC_CHECK(a);

	if (!err) {
		MDT_INFO("audio: ausrc - %s\n", str);
	}
	if (a->errh)
		a->errh(err, str, a->arg);
}


static void handle_telev(struct mdt_audio *a, struct mbuf *mb)
{
	int event, digit;
	bool end;

	if (telev_recv(a->telev, mb, &event, &end))
		return;

	digit = telev_code2digit(event);
	if (digit >= 0 && a->eventh)
		a->eventh(digit, end, a->arg);
}


static int stream_pt_handler(uint8_t pt, struct mbuf *mb, void *arg)
{
	struct mdt_audio *a = arg;
	const struct sdp_format *lc;

	lc = sdp_media_lformat(mdt_stream_sdpmedia(a->strm), pt);

	/* Telephone event? */
	if (lc && !str_casecmp(lc->name, "telephone-event")) {
		handle_telev(a, mb);
		return ENODATA;
	}

	if (!lc)
		return ENOENT;

	int ptc = mdt_aurecv_payload_type(a->aur);
	if (ptc == (int) pt)
		return 0;

	if (ptc != -1)
		MDT_INFO("Audio decoder changed payload %d -> %u\n", ptc, pt);

	return mdt_audio_decoder_set(a, lc->data, lc->pt, lc->params);
}


/**
 * Stream receive handler for audio is called from RX thread if enabled
 */
static void stream_recv_handler(const struct rtp_header *hdr,
				struct rtpext *extv, size_t extc,
				struct mbuf *mb, unsigned lostc,
				bool new_source,
				void *arg)
{
	struct mdt_audio *a = arg;

	MDT_MAGIC_CHECK(a);

	if (!a->aur)
		return;

	if (new_source)
		mdt_aurecv_reset(a->aur);

	mdt_aurecv_receive(a->aur, hdr, extv, extc, mb, lostc);
}


static int add_telev_codec(struct mdt_audio *a)
{
	struct sdp_media *m = mdt_stream_sdpmedia(mdt_audio_strm(a));
	struct sdp_format *sf;
	uint32_t pt = a->cfg.telev_pt;
	char pts[11];
	bool add = !sdp_media_lformat(m, pt);
	int err;

	(void)re_snprintf(pts, sizeof(pts), "%u", pt);

	/* Use payload-type 101 if available, for CiscoGW interop */
	err = sdp_format_add(&sf, m, false,
			     add ? pts : NULL,
			     telev_rtpfmt, TELEV_SRATE, 1, NULL,
			     NULL, NULL, false, "0-15");
	if (err)
		return err;

	return 0;
}


/**
 * Allocate an audio stream
 *
 * @param ap         Pointer to allocated audio stream object
 * @param streaml    List of streams
 * @param mdt_stream_prm Stream parameters
 * @param cfg        Global configuration
 * @param acc        User-Agent account
 * @param sdp_sess   SDP Session
 * @param mnat       Media NAT (optional)
 * @param mdt_mnat_sess  Media NAT session (optional)
 * @param menc       Media Encryption (optional)
 * @param mdt_menc_sess  Media Encryption session (optional)
 * @param ptime      Packet time in [ms]
 * @param aucodecl   List of audio codecs
 * @param offerer    True if SDP offerer, false if SDP answerer
 * @param eventh     Event handler
 * @param levelh     Audio level handler
 * @param errh       Error handler
 * @param arg        Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_audio_alloc(struct mdt_audio **ap, struct list *streaml,
		const struct mdt_stream_param *mdt_stream_prm,
		const struct mdt_config *cfg,
		struct sdp_session *sdp_sess,
		const struct mdt_mnat *mnat, struct mdt_mnat_sess *mdt_mnat_sess,
		const struct mdt_menc *menc, struct mdt_menc_sess *mdt_menc_sess,
		uint32_t ptime, const struct list *aucodecl, bool offerer,
		mdt_audio_event_h *eventh, mdt_audio_level_h *levelh,
		mdt_audio_err_h *errh, void *arg)
{
	struct mdt_audio *a;
	struct autx *tx;
	struct le *le;
	uint32_t minptime = ptime;
	int err;

	if (!ap || !cfg)
		return EINVAL;

	if (ptime < 1 || ptime > MAX_PTIME) {
		MDT_WARN("audio: ptime %ums out of range (%ums - %ums)\n",
			ptime, 1, MAX_PTIME);
		return ENOTSUP;
	}

	a = mem_zalloc(sizeof(*a), audio_destructor);
	if (!a)
		return ENOMEM;

	MDT_MAGIC_INIT(a);

	a->cfg = cfg->audio;
	tx = &a->tx;

	tx->src_fmt = cfg->audio.src_fmt;
	tx->enc_fmt = cfg->audio.enc_fmt;

	err = mdt_stream_alloc(&a->strm, streaml,
			   mdt_stream_prm, &cfg->avt, sdp_sess,
			   MDT_MEDIA_AUDIO,
			   mnat, mdt_mnat_sess, menc, mdt_menc_sess, offerer,
			   stream_recv_handler, NULL, stream_pt_handler,
			   a);
	if (err)
		goto out;

	err = mdt_aurecv_alloc(&a->aur, &a->cfg, AUDIO_SAMPSZ, ptime);
	if (err)
		goto out;

	if (cfg->avt.rtp_bw.max) {
		sdp_media_set_lbandwidth(mdt_stream_sdpmedia(a->strm),
					 SDP_BANDWIDTH_AS,
					 AUDIO_BANDWIDTH / 1000);
	}

	/* Audio codecs */
	for (le = list_head(aucodecl); le; le = le->next) {

		struct mdt_aucodec *ac = le->data;

		if (ac->ptime)
			minptime = min(minptime, ac->ptime);

		err = add_audio_codec(mdt_stream_sdpmedia(a->strm), ac);
		if (err)
			goto out;
	}

	err  = sdp_media_set_lattr(mdt_stream_sdpmedia(a->strm), true,
				   "minptime", "%u", minptime);
	err |= sdp_media_set_lattr(mdt_stream_sdpmedia(a->strm), true,
				   "ptime", "%u", ptime);
	if (err)
		goto out;


	tx->mb = mbuf_alloc(MDT_STREAM_PRESZ + 4096);
	tx->mb_telev = mbuf_alloc(MDT_STREAM_PRESZ + 64);
	tx->sampv = mem_zalloc(AUDIO_SAMPSZ * aufmt_sample_size(tx->enc_fmt),
			       NULL);

	if (!tx->mb || !tx->mb_telev || !tx->sampv) {
		err = ENOMEM;
		goto out;
	}

	err = telev_alloc(&a->telev, ptime);
	if (err)
		goto out;

	err = add_telev_codec(a);
	if (err)
		goto out;

	/* The driver every stream uses (src/audio/device.c). */
	err  = str_dup(&tx->module, mdt_audio_driver());
	err |= str_dup(&tx->device, mdt_audio_dev(true));
	if (err)
		goto out;

	tx->ptime  = ptime;
	tx->ts_ext = tx->ts_base = rand_u16();
	tx->marker = true;

	err  = mdt_aurecv_set_module(a->aur, mdt_audio_driver());
	err |= mdt_aurecv_set_device(a->aur, mdt_audio_dev(false));
	if (err)
		goto out;

	err  = mutex_alloc(&tx->mtx);
	if (err)
		goto out;

	a->eventh  = eventh;
	a->levelh  = levelh;
	a->errh    = errh;
	a->arg     = arg;

 out:
	if (err)
		mem_deref(a);
	else
		*ap = a;

	return err;
}


int mdt_audio_enable_level(struct mdt_audio *au)
{
	if (!au)
		return EINVAL;

	au->extmap_aulevel = mdt_stream_generate_extmap_id(au->strm);
	mdt_aurecv_set_extmap(au->aur, au->extmap_aulevel);

	return sdp_media_set_lattr(mdt_stream_sdpmedia(au->strm), false,
				   "extmap",
				   "%u %s",
				   au->extmap_aulevel, uri_aulevel);
}



struct mdt_audec_state *mdt_aurecv_decoder(const struct mdt_audio_recv *ar);


/* the receive decoder's state, for the codec-specific
 * counters only it keeps (src/api/stats.c: the Opus FEC/PLC split). */
struct mdt_audec_state *mdt_audio_decoder_state(const struct mdt_audio *a);
struct mdt_audec_state *mdt_audio_decoder_state(const struct mdt_audio *a)
{
	return a ? mdt_aurecv_decoder(a->aur) : NULL;
}


static int tx_thread(void *arg)
{
	struct mdt_audio *a = arg;
	struct autx *tx = &a->tx;
	uint64_t ts = 0;

	mtx_lock(tx->mtx);
	while (re_atomic_rlx(&tx->thr.run)) {
		uint64_t now;

		mtx_unlock(tx->mtx);
		sys_msleep(4);
		mtx_lock(tx->mtx);

		if (!re_atomic_rlx(&tx->aubuf_started)) {
			mtx_unlock(tx->mtx);
			goto loop;
		}

		if (!re_atomic_rlx(&tx->thr.run))
			break;

		size_t psize = tx->psize;
		uint32_t ptime = tx->ptime;

		mtx_unlock(tx->mtx);

		now = tmr_jiffies();
		if (!ts)
			ts = now;

		if (ts > now)
			goto loop;

		/* Now is the time to send */

		if (aubuf_cur_size(tx->aubuf) >= psize) {

			poll_aubuf_tx(a);
		}
		else {
			mtx_lock(tx->mtx);
			uint64_t aubuf_underrun = ++tx->stats.aubuf_underrun;
			tx->ts_ext += (ptime * tx->ac->crate / 1000);
			mtx_unlock(tx->mtx);

			MDT_DBG("audio: thread: tx aubuf underrun"
			      " (total %llu)\n", aubuf_underrun);
		}

		ts += ptime;

		/* Exact timing: send Telephony-Events from here.
		 * Be aware check_telev sets tx->mtx, so it must released!
		 */
		check_telev(a, tx);

loop:
		mtx_lock(tx->mtx);
	}

	mtx_unlock(tx->mtx);
	return 0;
}


static void aufilt_param_set(struct mdt_aufilt_prm *prm,
			     const struct mdt_aucodec *ac, enum aufmt fmt)
{
	prm->srate      = ac->srate;
	prm->ch         = ac->ch;
	prm->fmt        = fmt;
}


static int autx_print_pipeline(struct re_printf *pf, const struct autx *autx)
{
	struct le *le;
	int err;

	if (!autx)
		return 0;

	err = re_hprintf(pf, "audio tx pipeline:  %10s",
			 autx->as ? autx->as->name : "(src)");

	err |= re_hprintf(pf, " ---> aubuf");
	for (le = list_head(&autx->filtl); le; le = le->next) {
		struct mdt_aufilt_enc_st *st = le->data;

		if (st->af->ench)
			err |= re_hprintf(pf, " ---> %s", st->af->name);
	}

	err |= re_hprintf(pf, " ---> %s",
			  autx->ac ? autx->ac->name : "(encoder)");

	return err;
}


/**
 * Setup the audio-filter chain
 *
 * must be called before auplay/ausrc-alloc
 *
 * @param a Audio object
 *
 * @return 0 if success, otherwise errorcode
 */
static int aufilt_setup(struct mdt_audio *a, struct list *aufiltl)
{
	struct mdt_aufilt_prm encprm, plprm;
	struct autx *tx = &a->tx;
	struct le *le;
	bool update_enc, update_dec;
	int err = 0;

	/* wait until we have both Encoder and Decoder */
	if (!tx->ac || !mdt_aurecv_codec(a->aur))
		return 0;

	update_dec = mdt_aurecv_filt_empty(a->aur);
	update_enc = list_isempty(&tx->filtl);

	aufilt_param_set(&encprm, tx->ac, tx->enc_fmt);
	aufilt_param_set(&plprm, mdt_aurecv_codec(a->aur), a->cfg.play_fmt);
	if (a->cfg.srate_play && a->cfg.srate_play != plprm.srate) {
		plprm.srate = a->cfg.srate_play;
	}

	if (a->cfg.channels_play && a->cfg.channels_play != plprm.ch) {
		plprm.ch = a->cfg.channels_play;
	}

	/* Audio filters */
	for (le = list_head(aufiltl); le; le = le->next) {
		struct mdt_aufilt *af = le->data;
		struct mdt_aufilt_enc_st *encst = NULL;
		struct mdt_aufilt_dec_st *decst = NULL;
		void *ctx = NULL;

		if (!af->enabled)
			continue;

		if (af->encupdh && update_enc) {
			err = af->encupdh(&encst, &ctx, af, &encprm, a);
			if (err) {
				MDT_WARN("audio: error in encode audio-filter"
					" '%s' (%m)\n", af->name, err);
			}
			else {
				encst->af = af;
				mtx_lock(tx->mtx);
				list_append(&tx->filtl, &encst->le, encst);
				mtx_unlock(tx->mtx);
			}
		}

		if (af->decupdh && update_dec) {
			err = af->decupdh(&decst, &ctx, af, &plprm, a);
			if (err) {
				MDT_WARN("audio: error in decode audio-filter"
					" '%s' (%m)\n", af->name, err);
			}
			else {
				decst->af = af;
				mdt_aurecv_filt_append(a->aur, decst);
			}
		}

		if (err) {
			MDT_WARN("audio: audio-filter '%s'"
				" update failed (%m)\n", af->name, err);
			break;
		}
	}

	return 0;
}


static int start_source(struct autx *tx, struct mdt_audio *a, struct list *ausrcl)
{
	const struct mdt_aucodec *ac = tx->ac;
	uint32_t srate_dsp;
	uint32_t channels_dsp;
	int err;

	if (!ac)
		return 0;

	srate_dsp    = ac->srate;
	channels_dsp = ac->ch;

	if (a->cfg.srate_src && a->cfg.srate_src != srate_dsp) {
		srate_dsp = a->cfg.srate_src;
	}
	if (a->cfg.channels_src && a->cfg.channels_src != channels_dsp) {
		channels_dsp = a->cfg.channels_src;
	}

	/* Start Audio Source */
	if (!tx->ausrc && mdt_ausrc_find(ausrcl, NULL)) {

		size_t sz;
		size_t psize_alloc;
		struct mdt_ausrc_prm prm = {
			.srate      = srate_dsp,
			.ch         = channels_dsp,
			.ptime      = tx->ptime,
			.fmt        = tx->src_fmt
		};

		tx->ausrc_prm = prm;

		sz = aufmt_sample_size(tx->src_fmt);

		psize_alloc = sz * au_calc_nsamp(prm.srate, prm.ch, prm.ptime);
		tx->psize = psize_alloc;
		tx->aubuf_maxsz = tx->psize * 30;

		if (!tx->aubuf) {
			err = aubuf_alloc(&tx->aubuf, tx->psize,
					  tx->aubuf_maxsz);
			if (err)
				return err;
		}

		err = mdt_ausrc_alloc(&tx->ausrc, ausrcl,
				  tx->module,
				  &prm, tx->device,
				  ausrc_read_handler, ausrc_error_handler, a);
		if (err) {
			MDT_WARN("audio: start_source failed (%s.%s): %m\n",
				tx->module, tx->device, err);
			return err;
		}

		mtx_lock(tx->mtx);
		/* recalculate and resize aubuf if mdt_ausrc_alloc changes prm */
		tx->src_fmt = prm.fmt;
		sz = aufmt_sample_size(tx->src_fmt);
		tx->psize = sz * au_calc_nsamp(prm.srate, prm.ch, prm.ptime);
		if (psize_alloc != tx->psize) {
			tx->ausrc_prm = prm;
			tx->aubuf_maxsz = tx->psize * 30;
			err = aubuf_resize(tx->aubuf, tx->psize,
					   tx->aubuf_maxsz);
			if (err) {
				mtx_unlock(tx->mtx);
				return err;
			}
		}

		mtx_unlock(tx->mtx);
		tx->as = mdt_ausrc_find(ausrcl, tx->module);

		if (!re_atomic_rlx(&tx->thr.run)) {
			re_atomic_rlx_set(&tx->thr.run, true);
			err = thread_create_name(&tx->thr.tid,
						 "Audio TX",
						 tx_thread, a);
			if (err) {
				re_atomic_rlx_set(&tx->thr.run,
						  false);
				return err;
			}
		}

		MDT_INFO("audio: source started with sample format %s\n",
		     aufmt_name(tx->src_fmt));
	}

	mdt_stream_enable_tx(a->strm, true);

	return 0;
}


static void audio_flush_filters(struct mdt_audio *a)
{
	struct autx *tx = &a->tx;

	mdt_aurecv_flush(a->aur);

	mtx_lock(a->tx.mtx);
	list_flush(&tx->filtl);
	mtx_unlock(a->tx.mtx);
}


/**
 * Update audio object and start/stop according to media direction
 *
 * @param a Audio object
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_audio_update(struct mdt_audio *a)
{
	struct list *aufiltl = mdt_aufiltl();
	struct sdp_media *m;
	enum sdp_dir dir = SDP_INACTIVE;
	const struct sdp_format *sc = NULL;
	int err = 0;

	if (!a)
		return EINVAL;

	MDT_DBG("audio: update\n");
	m = mdt_stream_sdpmedia(mdt_audio_strm(a));

	if (!sdp_media_disabled(m)) {
		dir = sdp_media_dir(m);
		sc = sdp_media_rformat(m, NULL);
	}

	if (!sc || !sc->data) {
		MDT_INFO("audio: stream is disabled\n");
		mdt_audio_stop(a);
		return 0;
	}

	if (dir & SDP_RECVONLY)
		err |= mdt_audio_decoder_set(a, sc->data, sc->pt, sc->rparams);

	if (dir & SDP_SENDONLY)
		err |= mdt_audio_encoder_set(a, sc->data, sc->pt, sc->params);

	if (err) {
		MDT_WARN("audio: start error (%m)\n", err);
		return err;
	}

	/* Audio filter */
	if (!list_isempty(aufiltl)) {

		err = aufilt_setup(a, aufiltl);
		if (err)
			return err;
	}

	if (dir & SDP_RECVONLY) {
		mdt_stream_enable_rx(a->strm, true);
	}
	else {
		mdt_stream_enable_rx(a->strm, false);
		mdt_aurecv_stop(a->aur);
	}

	if (dir & SDP_SENDONLY) {
		err |= start_source(&a->tx, a, mdt_ausrcl());
	}
	else {
		stop_tx(&a->tx, a);
	}

	if (a->tx.ac && mdt_aurecv_codec(a->aur)) {

		if (!a->started) {
			MDT_INFO("%H\n%H\n",
			     autx_print_pipeline, &a->tx,
			     mdt_aurecv_print_pipeline, a->aur);
		}

		a->started = true;
	}

	return err;
}


/**
 * Stop the audio playback and recording
 *
 * @param a Audio object
 */
void mdt_audio_stop(struct mdt_audio *a)
{
	if (!a)
		return;

	stop_tx(&a->tx, a);
	mdt_stream_enable_rx(a->strm, false);
	mdt_aurecv_stop(a->aur);
	a->started = false;
}


/**
 * Set the audio encoder used
 *
 * @note The audio source has to be started separately
 *
 * @param a      Audio object
 * @param ac     Audio codec to use
 * @param pt_tx  Payload type for sending
 * @param params Optional encoder parameters
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_audio_encoder_set(struct mdt_audio *a, const struct mdt_aucodec *ac,
		      int pt_tx, const char *params)
{
	struct autx *tx;
	int err = 0;

	if (!a || !ac)
		return EINVAL;

	tx = &a->tx;

	if (ac != tx->ac) {
		MDT_INFO("audio: Set audio encoder: %s %uHz %dch\n",
		     ac->name, ac->srate, ac->ch);

		/* Should the source be stopped first? */
		if (!aucodec_equal(ac, tx->ac)) {
			tx->ausrc = mem_deref(tx->ausrc);
			audio_flush_filters(a);
			aubuf_flush(tx->aubuf);
		}

		tx->enc = mem_deref(tx->enc);
		tx->ac = ac;

		if (!list_isempty(mdt_aufiltl())) {
			err = aufilt_setup(a, mdt_aufiltl());
			if (err)
				return err;
		}
	}

	if (ac->encupdh) {
		struct mdt_auenc_param prm;

		prm.bitrate = 0;        /* auto */

		err = ac->encupdh(&tx->enc, ac, &prm, params);
		if (err) {
			MDT_WARN("audio: alloc encoder: %m\n", err);
			return err;
		}
	}

	mdt_stream_set_srate(a->strm, ac->crate, 0);

	mtx_lock(a->tx.mtx);
	mdt_stream_update_encoder(a->strm, pt_tx);
	mtx_unlock(a->tx.mtx);

	telev_set_srate(a->telev, ac->crate);

	/* use a codec-specific ptime */
	if (ac->ptime)
		tx->ptime = ac->ptime;

	return err;
}


/**
 * Set the audio decoder used
 *
 * @note Starts also the player if not already running
 *
 * @param a      Audio object
 * @param ac     Audio codec to use
 * @param pt     Payload type for receiving
 * @param params Optional decoder parameters
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_audio_decoder_set(struct mdt_audio *a, const struct mdt_aucodec *ac,
		      int pt, const char *params)
{
	int err = 0;
	bool reset = false;

	if (!a || !ac)
		return EINVAL;

	if (ac != mdt_aurecv_codec(a->aur)) {
		struct sdp_media *m;

		m = mdt_stream_sdpmedia(mdt_audio_strm(a));
		reset  = !aucodec_equal(ac, mdt_aurecv_codec(a->aur));
		reset |= !(sdp_media_dir(m) & SDP_RECVONLY);
		if (reset) {
			mdt_aurecv_stop(a->aur);
			audio_flush_filters(a);
			mdt_stream_flush(a->strm);
		}
	}

	err = mdt_aurecv_decoder_set(a->aur, ac, pt, params);
	if (err)
		return err;

	if (reset && !list_isempty(mdt_aufiltl())) {
		err = aufilt_setup(a, mdt_aufiltl());
		if (err) {
			return err;
		}
	}

	mdt_stream_set_srate(a->strm, 0, ac->crate);
	if (reset || !mdt_aurecv_player_started(a->aur))
		err |= mdt_aurecv_start_player(a->aur, mdt_auplayl());

	return err;
}


/**
 * Get the RTP Stream object from an Audio object
 *
 * @param au  Audio object
 *
 * @return RTP Stream object
 */
struct mdt_stream *mdt_audio_strm(const struct mdt_audio *au)
{
	return au ? au->strm : NULL;
}


int mdt_audio_send_digit(struct mdt_audio *a, char key)
{
	int err = 0;

	if (!a)
		return EINVAL;

	if (key != KEYCODE_REL) {
		int event = telev_digit2code(key);
		MDT_INFO("audio: send DTMF digit: '%c'\n", key);

		if (event == -1) {
			MDT_WARN("audio: invalid DTMF digit (0x%02x)\n", key);
			return EINVAL;
		}

		mtx_lock(a->tx.mtx);
		err = telev_send(a->telev, event, false);
		mtx_unlock(a->tx.mtx);

	}
	else if (a->tx.cur_key && a->tx.cur_key != KEYCODE_REL) {
		/* Key release */
		MDT_INFO("audio: send DTMF digit end: '%c'\n", a->tx.cur_key);
		mtx_lock(a->tx.mtx);
		err = telev_send(a->telev,
				 telev_digit2code(a->tx.cur_key), true);
		mtx_unlock(a->tx.mtx);
	}

	a->tx.cur_key = key;

	return err;
}


/**
 * Mute the audio stream source (i.e. Microphone)
 *
 * @param a      Audio stream
 * @param muted  True to mute, false to un-mute
 */
void mdt_audio_mute(struct mdt_audio *a, bool muted)
{
	if (!a)
		return;

	re_atomic_rlx_set(&a->tx.muted, muted);
}


/**
 * Get the mute state of an audio source
 *
 * @param a      Audio stream
 *
 * @return True if muted, otherwise false
 */
bool mdt_audio_ismuted(const struct mdt_audio *a)
{
	if (!a)
		return false;

	return re_atomic_rlx(&a->tx.muted);
}


static bool extmap_handler(const char *name, const char *value, void *arg)
{
	struct mdt_audio *a = arg;
	struct sdp_extmap extmap;
	int err;
	(void)name;

	MDT_MAGIC_CHECK(a);

	err = sdp_extmap_decode(&extmap, value);
	if (err) {
		MDT_WARN("audio: sdp_extmap_decode error (%m)\n", err);
		return false;
	}

	if (0 == pl_strcasecmp(&extmap.name, uri_aulevel)) {

		if (extmap.id < RTPEXT_ID_MIN || extmap.id > RTPEXT_ID_MAX) {
			MDT_WARN("audio: extmap id out of range (%u)\n",
				extmap.id);
			return false;
		}

		a->extmap_aulevel = extmap.id;
		mdt_aurecv_set_extmap(a->aur, a->extmap_aulevel);

		err = sdp_media_set_lattr(mdt_stream_sdpmedia(a->strm), true,
					  "extmap",
					  "%u %s",
					  a->extmap_aulevel,
					  uri_aulevel);
		if (err)
			return false;

		a->level_enabled = true;
		MDT_INFO("audio: client-to-mixer audio levels enabled\n");
	}

	return false;
}


void mdt_audio_sdp_attr_decode(struct mdt_audio *a)
{
	const char *attr;

	if (!a)
		return;

	/* This is probably only meaningful for audio data, but
	   may be used with other media types if it makes sense. */
	attr = sdp_media_rattr(mdt_stream_sdpmedia(a->strm), "ptime");
	if (attr) {
		struct autx *tx = &a->tx;
		uint32_t ptime_tx = atoi(attr);

		if (ptime_tx && ptime_tx != a->tx.ptime
		    && ptime_tx <= MAX_PTIME) {

			MDT_INFO("audio: peer changed ptime_tx %ums -> %ums\n",
			     a->tx.ptime, ptime_tx);

			tx->ptime = ptime_tx;

			if (tx->ac) {
				size_t sz;

				sz = aufmt_sample_size(tx->src_fmt);

				tx->psize = sz * au_calc_nsamp(tx->ac->srate,
							    tx->ac->ch,
							    ptime_tx);
			}

			sdp_media_set_lattr(mdt_stream_sdpmedia(a->strm), true,
					    "ptime", "%u", ptime_tx);
		}
	}

	/* Client-to-Mixer Audio Level Indication */
	if (a->cfg.level) {
		sdp_media_rattr_apply(mdt_stream_sdpmedia(a->strm),
				      "extmap",
				      extmap_handler, a);
	}
}


/**
 * Set the audio source state to a new audio source module and device.
 * The current audio source will be stopped.
 *
 * @param au     Audio object
 * @param mod    Audio source module (NULL to stop)
 * @param device Audio source device name
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_audio_set_source(struct mdt_audio *au, const char *mod, const char *device)
{
	struct autx *tx;
	int err;

	if (!au)
		return EINVAL;

	tx = &au->tx;

	/* stop the audio device first */
	tx->ausrc = mem_deref(tx->ausrc);

	if (str_isset(mod)) {

		err = mdt_ausrc_alloc(&tx->ausrc, mdt_ausrcl(),
				  mod, &tx->ausrc_prm, device,
				  ausrc_read_handler, ausrc_error_handler, au);
		if (err) {
			MDT_WARN("audio: set_source failed (%s.%s): %m\n",
				mod, device, err);
			return err;
		}

		tx->as = mdt_ausrc_find(mdt_ausrcl(), mod);
	}

	return 0;
}


/**
 * Set the audio player state to a new audio player module and device.
 * The current audio player will be stopped.
 *
 * @param a      Audio object
 * @param mod    Audio player module (NULL to stop)
 * @param device Audio player device name
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_audio_set_player(struct mdt_audio *a, const char *mod, const char *device)
{
	int err;

	if (!a)
		return EINVAL;

	mdt_aurecv_stop_auplay(a->aur);
	if (!str_isset(mod))
		return 0;

	err  = mdt_aurecv_set_module(a->aur, mod);
	err |= mdt_aurecv_set_device(a->aur, device);
	if (err)
		goto out;

	err = mdt_aurecv_start_player(a->aur, mdt_auplayl());
out:
	if (err) {
		MDT_WARN("audio: set player failed (%s.%s): %m\n",
			mod, device, err);
		return err;
	}

	return 0;
}


/**
 * Set the bitrate for the audio encoder
 *
 * @param au      Audio object
 * @param bitrate Encoder bitrate in [bits/s]
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_audio_set_bitrate(struct mdt_audio *au, uint32_t bitrate)
{
	struct autx *tx;
	const struct mdt_aucodec *ac;
	int err;

	if (!au)
		return EINVAL;

	tx = &au->tx;

	ac = tx->ac;

	MDT_INFO("audio: set bitrate for encoder '%s' to %u bits/s\n",
	     ac ? ac->name : "?",
	     bitrate);

	if (ac) {

		if (ac->encupdh) {
			struct mdt_auenc_param prm;

			prm.bitrate = bitrate;

			err = ac->encupdh(&tx->enc, ac, &prm, NULL);
			if (err) {
				MDT_WARN("audio: encupdh error: %m\n", err);
				return err;
			}
		}

	}
	else {
		MDT_INFO("audio: set_bitrate: no audio encoder\n");
	}

	return 0;
}


/**
 * Get audio codec of audio stream
 *
 * @param au    Audio object
 * @param tx    True to get transmit codec, false to get receive codec
 *
 * @return      Audio codec if success, otherwise NULL
 */
const struct mdt_aucodec *mdt_audio_codec(const struct mdt_audio *au, bool tx)
{
	if (!au)
		return NULL;

	return tx ? au->tx.ac : mdt_aurecv_codec(au->aur);
}

