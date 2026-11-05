/**
 * @file rtprecv.c  Generic RTP stream receiver
 *
 * Copyright (C) 2023 Alfred E. Heggestad, Christian Spielberger
 *
 * Adapted from baresip's src/rtprecv.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */
#include <string.h>
#include <time.h>
#include <re_atomic.h>
#include <re.h>
#include "mdt_media.h"

/** Magic number */
#define MAGIC 0x00511eb3

/* Receive */
struct mdt_rtp_receiver {
#ifndef RELEASE
	uint32_t magic;                /**< Magic number for debugging       */
#endif
	/* Data protected by mtx */
	char *name;                    /**< Media name                       */
	struct mdt_metric *metric;         /**< Metrics for receiving            */
	struct mdt_jbuf *jbuf;             /**< Jitter Buffer for incoming RTP   */
	bool enabled;                  /**< True if enabled                  */
	uint64_t ts_last;              /**< Timestamp of last recv RTP pkt   */
	uint32_t ssrc;                 /**< Incoming synchronization source  */
	bool ssrc_set;                 /**< Incoming SSRC is set             */
	bool ssrc_changed;             /**< SSRC changed since last poll     */
	uint32_t pseq;                 /**< Sequence number for incoming RTP */
	bool pseq_set;                 /**< True if sequence number is set   */
	bool rtp_estab;                /**< True if RTP stream established   */
	RE_ATOMIC bool run;            /**< True if RX thread is running     */
	RE_ATOMIC bool stop;           /**< True if RX thread should stop    */
	bool start_rtcp;               /**< Start RTCP flag                  */
	char *cname;                   /**< Canonical Name for RTCP send     */
	struct sa rtcp_peer;           /**< RTCP address of Peer             */
	bool pinhole;                  /**< Open RTCP NAT pinhole flag       */
	mtx_t *mtx;                    /**< Mutex protects above fields      */

	/* Unprotected data */
	struct mdt_stream *strm;           /**< Stream                           */
	struct rtp_sock *rtp;          /**< RTP Socket                       */
	mdt_stream_pt_h *pth;              /**< Stream payload type handler      */
	mdt_stream_rtp_h *rtph;            /**< Stream RTP handler               */
	mdt_stream_rtpestab_h *rtpestabh;  /**< RTP established handler          */
	void *arg;                     /**< Stream argument                  */
	void *sessarg;                 /**< Session argument                 */
	thrd_t thr;                    /**< RX thread                        */
	struct tmr tmr;                /**< Timer for stopping RX thread     */
	int pt;                        /**< Previous payload type            */
	int pt_tel;                    /**< Payload type for tel event       */
	uint32_t srate;                /**< Receiver Samplerate              */
	struct tmr tmr_decode;         /**< Decode Timer                     */
};


enum work_type {
	WORK_RTCP,
	WORK_RTPESTAB,
	WORK_PTCHANGED,
	WORK_MNATCONNH,
};


struct work {
	enum work_type type;
	struct mdt_rtp_receiver *rx;
	union {
		struct rtcp_msg *rtcp;
		struct {
			uint8_t pt;
			struct mbuf *mb;
		} pt;
		struct {
			struct sa raddr1;
			struct sa raddr2;
		} mnat;
	} u;
};

static bool rtprecv_consume_ssrc_change(struct mdt_rtp_receiver *rx,
                    uint32_t *ssrc);


static void async_work_main(int err, void *arg);
static void work_destructor(void *arg);


/*
 * functions that run in RX thread (if "rxmode thread" is configured)
 */


static void pass_rtcp_work(struct mdt_rtp_receiver *rx, struct rtcp_msg *msg)
{
	struct work *w;

	if (!re_atomic_rlx(&rx->run)) {
		mdt_stream_process_rtcp(rx->strm, msg);
		return;
	}

	w = mem_zalloc(sizeof(*w), work_destructor);
	if (!w)
		return;

	w->type    = WORK_RTCP;
	w->rx      = rx;
	w->u.rtcp  = mem_ref(msg);
	re_thread_async_main_id((intptr_t)rx, NULL, async_work_main, w);
}


static int pass_pt_work(struct mdt_rtp_receiver *rx, uint8_t pt, struct mbuf *mb)
{
	struct work *w;

	if (!re_atomic_rlx(&rx->run))
		return rx->pth(pt, mb, rx->arg);

	w = mem_zalloc(sizeof(*w), work_destructor);
	if (!w)
		return ENOMEM;

	w->type    = WORK_PTCHANGED;
	w->rx      = rx;
	w->u.pt.pt = pt;
	w->u.pt.mb = mbuf_dup(mb);

	return re_thread_async_main_id((intptr_t)rx, NULL, async_work_main, w);
}


static void pass_rtpestab_work(struct mdt_rtp_receiver *rx)
{
	struct work *w;

	if (!re_atomic_rlx(&rx->run)) {
		rx->rtpestabh(rx->strm, rx->sessarg);
		return;
	}

	w = mem_zalloc(sizeof(*w), work_destructor);
	w->type = WORK_RTPESTAB;
	w->rx   = rx;

	re_thread_async_main_id((intptr_t)rx, NULL, async_work_main, w);
}


static void pass_mnat_work(struct mdt_rtp_receiver *rx, const struct sa *raddr1,
			   const struct sa *raddr2)
{
	struct work *w;

	if (!re_atomic_rlx(&rx->run)) {
		mdt_stream_mnat_connected(rx->strm, raddr1, raddr2);
		return;
	}

	w = mem_zalloc(sizeof(*w), work_destructor);
	w->type = WORK_MNATCONNH;
	w->rx   = rx;
	sa_cpy(&w->u.mnat.raddr1, raddr1);
	sa_cpy(&w->u.mnat.raddr2, raddr2);

	re_thread_async_main_id((intptr_t)rx, NULL, async_work_main, w);
}


static void rtprecv_periodic(void *arg)
{
	struct mdt_rtp_receiver *rx = arg;

	if (re_atomic_rlx(&rx->stop)) {
		tmr_cancel(&rx->tmr_decode);
		udp_thread_detach(rtp_sock(rx->rtp));
		udp_thread_detach(rtcp_sock(rx->rtp));
		re_cancel();
		return;
	}

	mtx_lock(rx->mtx);
	bool pinhole    = rx->pinhole;
	mtx_unlock(rx->mtx);
	tmr_start(&rx->tmr, 10, rtprecv_periodic, rx);
	mtx_lock(rx->mtx);
	if (rx->start_rtcp) {
		int err = 0;
		rx->start_rtcp = false;
		rtcp_start(rx->rtp, rx->cname, &rx->rtcp_peer);
		mtx_unlock(rx->mtx);
		if (pinhole) {
			err = rtcp_send_app(rx->rtp, "PING",
					    (void *)"PONG", 4);
		}
		if (err) {
			MDT_WARN("rtprecv: rtcp_send_app failed (%m)\n",
				err);
		}
	}
	else {
		mtx_unlock(rx->mtx);
	}
}


static int rtprecv_thread(void *arg)
{
	struct mdt_rtp_receiver *rx = arg;

	int err = re_thread_init();
	if (err)
		return err;

	MDT_INFO("rtp_receiver: RTP RX thread started\n");
	tmr_start(&rx->tmr, 10, rtprecv_periodic, rx);

	err = udp_thread_attach(rtp_sock(rx->rtp));
	if (err) {
		MDT_WARN("rtp_receiver: could not attach to RTP socket (%m)\n",
			err);
		return err;
	}

	err = udp_thread_attach(rtcp_sock(rx->rtp));
	if (err) {
		MDT_WARN("rtp_receiver: could not attach to RTCP socket (%m)\n",
			err);
		return err;
	}

	mdt_rtprecv_enable(rx, true);
	err = re_main(NULL);

	tmr_cancel(&rx->tmr);
	re_thread_close();
	return err;
}


static int lostcalc(struct mdt_rtp_receiver *rx, uint16_t seq)
{
	const uint16_t delta = seq - rx->pseq;
	int lostc;

	if (rx->pseq == (uint32_t)-1)
		lostc = 0;
	else if (delta == 0)
		return -1;
	else if (delta < 3000)
		lostc = delta - 1;
	else if (delta < 0xff9c)
		lostc = 0;
	else
		return -2;

	rx->pseq = seq;
	return lostc;
}


static void handle_rtp(struct mdt_rtp_receiver *rx, const struct rtp_header *hdr,
		      struct mbuf *mb, unsigned lostc)
{
	struct rtpext extv[8];
	size_t extc = 0;

	/* RFC 5285 -- A General Mechanism for RTP Header Extensions */
	if (hdr->ext && hdr->x.len && mb) {

		const size_t pos = mb->pos;
		const size_t end = mb->end;
		const size_t ext_stop = mb->pos;
		size_t i;
		int err;

		if (hdr->x.type != RTPEXT_TYPE_MAGIC) {
			MDT_DBG("rtprecv: unknown ext type ignored (0x%04x)\n",
			     hdr->x.type);
			goto handler;
		}

		size_t ext_len = hdr->x.len*sizeof(uint32_t);
		if (mb->pos < ext_len) {
			MDT_WARN("rtp_receiver: corrupt rtp packet,"
				" not enough space for rtpext of %zu bytes\n",
				ext_len);
			return;
		}

		mb->pos = mb->pos - ext_len;
		mb->end = ext_stop;

		for (i=0; i<RE_ARRAY_SIZE(extv) && mbuf_get_left(mb); i++) {

			err = rtpext_decode(&extv[i], mb);
			if (err) {
				MDT_WARN("rtp_receiver: rtpext_decode failed "
					"(%m)\n", err);
				return;
			}
		}

		extc = i;

		mb->pos = pos;
		mb->end = end;
	}

handler:
	/* (No declaration right after a label: C23 only, and Apple clang says
	 * so.) */
	rx->rtph(hdr, extv, extc, mb, lostc,
		 rtprecv_consume_ssrc_change(rx, NULL), rx->arg);

	return;
}


static void decode_frames(struct mdt_rtp_receiver *rx);
static void decode_tmr(void *arg)
{
	struct mdt_rtp_receiver *rx = arg;

	decode_frames(rx);
}

static void rtprecv_resync(struct mdt_rtp_receiver *rx,
			 const struct rtp_header *hdr)
{
	if (!rx || !hdr)
		return;

	mdt_jbuf_flush(rx->jbuf);

	mtx_lock(rx->mtx);
	rx->ssrc = hdr->ssrc;
	rx->ssrc_set = true;
	rx->pseq = (uint16_t)(hdr->seq - 1);
	rx->pseq_set = true;
	rx->pt = -1;
	rx->pt_tel = -1;
	rx->ssrc_changed = true;
	mtx_unlock(rx->mtx);
}


/**
 * Decode RTP packets
 *
 * @param s The stream
 */
static void decode_frames(struct mdt_rtp_receiver *rx)
{
	struct rtp_header hdr;
	void *mb;
	int lostc;
	int32_t delay;
	int err;

	if (!rx || !rx->jbuf)
		return;

	uint32_t n = 1;

	do {
		err = mdt_jbuf_get(rx->jbuf, &hdr, &mb);
		if (err == EAGAIN)
			++n;
		else if (err)
			break;

		lostc = lostcalc(rx, hdr.seq);

		handle_rtp(rx, &hdr, mb, lostc > 0 ? lostc : 0);
		mem_deref(mb);
	} while (--n);

	delay = mdt_jbuf_next_play(rx->jbuf);
	if (delay < 0)
		delay = 10; /* Fallback time */

	tmr_start(&rx->tmr_decode, delay, decode_tmr, rx);
}


static bool rtprecv_filter_pt(struct mdt_rtp_receiver *rx,
			      const struct rtp_header *hdr)
{
	bool handle;

	handle = hdr->pt != rx->pt;
	if (rx->pt_tel != -1)
		handle |= hdr->pt == rx->pt_tel;

	if (!handle)
		return false;

	const struct sdp_format *lc;
	lc = sdp_media_lformat(mdt_stream_sdpmedia(rx->strm), hdr->pt);
	if (lc && !str_casecmp(lc->name, "telephone-event")) {
		rx->pt_tel = hdr->pt;
	}

	rx->pt = hdr->pt;
	return true;
}


void mdt_rtprecv_decode(const struct sa *src, const struct rtp_header *hdr,
		     struct mbuf *mb, void *arg)
{
	struct mdt_rtp_receiver *rx = arg;
	uint32_t ssrc0;
	bool ssrc_changed = false;
	int err = 0;

	if (!rx)
		return;

	MDT_MAGIC_CHECK(rx);
	mtx_lock(rx->mtx);
	if (!rx->enabled) {
		mtx_unlock(rx->mtx);
		return;
	}

	if (rtp_pt_is_rtcp(hdr->pt)) {
		MDT_DBG("rtprecv: drop incoming RTCP packet on RTP port"
		     " (pt=%u)\n", hdr->pt);
		mtx_unlock(rx->mtx);
		return;
	}

	rx->ts_last = tmr_jiffies();

	mdt_metric_add_packet(rx->metric, mbuf_get_left(mb));

	if (!rx->rtp_estab) {
		rx->rtp_estab = true;
		if (rx->rtpestabh) {
			MDT_DBG("rtprecv: incoming rtp for '%s' established, "
			      "receiving from %J\n", rx->name, src);
			pass_rtpestab_work(rx);
		}

		tmr_start(&rx->tmr_decode, 0, decode_tmr, rx);
	}

	ssrc0 = rx->ssrc;
	if (!rx->pseq_set) {
		rx->ssrc = hdr->ssrc;
		rx->ssrc_set = true;
		rx->pseq = hdr->seq - 1;
		rx->pseq_set = true;
	}
	else if (hdr->ssrc != ssrc0) {

		MDT_DBG("rtprecv: %s: SSRC changed 0x%x -> 0x%x"
		     " (%zu bytes from %J)\n",
		     rx->name, ssrc0, hdr->ssrc,
		     mbuf_get_left(mb), src);

		ssrc_changed = true;
	}
	mtx_unlock(rx->mtx);

	if (ssrc_changed)
		rtprecv_resync(rx, hdr);

	if (rtprecv_filter_pt(rx, hdr)) {
		err = pass_pt_work(rx, hdr->pt, mb);
		if (err)
			return;

		if (hdr->pt == rx->pt_tel)
			return;
	}

	if (rx->jbuf) {
		/* Put frame in Jitter Buffer */
		err = mdt_jbuf_put(rx->jbuf, hdr, mb);
		if (err) {
			MDT_INFO("rtprecv: %s: dropping %zu bytes from %J"
			     " [seq=%u, ts=%u] (%m)\n",
			     rx->name, mb->end,
			     src, hdr->seq, hdr->ts, err);
			mdt_metric_inc_err(rx->metric);
		}
	}
	else {
		(void)handle_rtp(rx, hdr, mb, 0);
	}
}


void mdt_rtprecv_handle_rtcp(const struct sa *src, struct rtcp_msg *msg,
			  void *arg)
{
	struct mdt_rtp_receiver *rx = arg;
	(void)src;

	MDT_MAGIC_CHECK(rx);
	mtx_lock(rx->mtx);
	if (!rx->enabled) {
		mtx_unlock(rx->mtx);
		return;
	}

	rx->ts_last = tmr_jiffies();
	mtx_unlock(rx->mtx);

	pass_rtcp_work(rx, msg);
}


void mdt_rtprecv_mnat_connected_handler(const struct sa *raddr1,
				     const struct sa *raddr2, void *arg)
{
	struct mdt_rtp_receiver *rx = arg;

	MDT_MAGIC_CHECK(rx);

	pass_mnat_work(rx, raddr1, raddr2);
}


int mdt_rtprecv_start_rtcp(struct mdt_rtp_receiver *rx, const char *cname,
		       const struct sa *peer, bool pinhole)
{
	int err;

	if (!rx)
		return EINVAL;

	mtx_lock(rx->mtx);
	if (peer)
		rx->rtcp_peer = *peer;


	rx->cname = mem_deref(rx->cname);
	err = str_dup(&rx->cname, cname);
	rx->start_rtcp = true;
	rx->pinhole = pinhole;
	mtx_unlock(rx->mtx);

	return err;
}


/*
 * functions that run in main thread
 */

void mdt_rtprecv_set_socket(struct mdt_rtp_receiver *rx, struct rtp_sock *rtp)
{
	mtx_lock(rx->mtx);
	rx->rtp = rtp;
	mtx_unlock(rx->mtx);
}


void mdt_rtprecv_set_ssrc(struct mdt_rtp_receiver *rx, uint32_t ssrc)
{
	if (!rx)
		return;

	mtx_lock(rx->mtx);
	if (rx->ssrc_set) {
		if (ssrc != rx->ssrc) {
			MDT_DBG("rtprecv: receive: SSRC changed: %x -> %x\n",
			     rx->ssrc, ssrc);
			rx->ssrc = ssrc;
		}
	}
	else {
		MDT_DBG("rtprecv: receive: setting SSRC: %x\n", ssrc);
		rx->ssrc = ssrc;
		rx->ssrc_set = true;
	}
	mtx_unlock(rx->mtx);
}


uint64_t mdt_rtprecv_ts_last(struct mdt_rtp_receiver *rx)
{
	if (!rx)
		return 0;

	uint64_t ts_last;

	mtx_lock(rx->mtx);
	ts_last = rx->ts_last;
	mtx_unlock(rx->mtx);

	return ts_last;
}


void mdt_rtprecv_set_ts_last(struct mdt_rtp_receiver *rx, uint64_t ts_last)
{
	if (!rx)
		return;

	mtx_lock(rx->mtx);
	rx->ts_last = ts_last;
	mtx_unlock(rx->mtx);
}


void mdt_rtprecv_flush(struct mdt_rtp_receiver *rx)
{
	if (!rx)
		return;

	mdt_jbuf_flush(rx->jbuf);
}


void mdt_rtprecv_enable(struct mdt_rtp_receiver *rx, bool enable)
{
	if (!rx)
		return;

	mtx_lock(rx->mtx);
	rx->enabled = enable;
	mtx_unlock(rx->mtx);
}


int mdt_rtprecv_get_ssrc(struct mdt_rtp_receiver *rx, uint32_t *ssrc)
{
	int err;

	if (!rx || !ssrc)
		return EINVAL;

	mtx_lock(rx->mtx);
	if (rx->ssrc_set) {
		*ssrc = rx->ssrc;
		err = 0;
	}
	else
		err = ENOENT;
	mtx_unlock(rx->mtx);

	return err;
}


static bool rtprecv_consume_ssrc_change(struct mdt_rtp_receiver *rx,
					uint32_t *ssrc)
{
	bool changed = false;

	if (!rx)
		return false;

	mtx_lock(rx->mtx);
	if (rx->ssrc_changed) {
		rx->ssrc_changed = false;
		if (rx->ssrc_set && ssrc)
			*ssrc = rx->ssrc;
		changed = true;
	}
	else if (ssrc && rx->ssrc_set) {
		*ssrc = rx->ssrc;
	}
	mtx_unlock(rx->mtx);

	return changed;
}

struct mdt_jbuf *mdt_rtprecv_jbuf(struct mdt_rtp_receiver *rx)
{
	return rx ? rx->jbuf : NULL;
}


void mdt_rtprecv_enable_mux(struct mdt_rtp_receiver *rx, bool enable)
{
	if (!rx)
		return;

	mtx_lock(rx->mtx);
	rtcp_enable_mux(rx->rtp, enable);
	mtx_unlock(rx->mtx);
}


static void destructor(void *arg)
{
	struct mdt_rtp_receiver *rx = arg;

	if (re_atomic_rlx(&rx->run)) {
		mdt_rtprecv_enable(rx, false);
		re_atomic_rlx_set(&rx->stop, true);
		thrd_join(rx->thr, NULL);
		re_atomic_rlx_set(&rx->run, false);
		re_thread_async_main_cancel((intptr_t)rx);
	}
	else {
		udp_thread_detach(rtp_sock(rx->rtp));
		udp_thread_detach(rtcp_sock(rx->rtp));
	}

	tmr_cancel(&rx->tmr_decode);

	mem_deref(rx->metric);
	mem_deref(rx->name);
	mem_deref(rx->mtx);
	mem_deref(rx->jbuf);
	mem_deref(rx->cname);
}


int mdt_rtprecv_alloc(struct mdt_rtp_receiver **rxp,
		   struct mdt_stream *strm,
		   const char *name,
		   const struct mdt_config_avt *cfg,
		   mdt_stream_rtp_h *rtph,
		   mdt_stream_pt_h *pth, void *arg)
{
	struct mdt_rtp_receiver *rx;
	int err;

	if (!rxp || !str_isset(name))
		return EINVAL;

	rx = mem_zalloc(sizeof(*rx), destructor);
	if (!rx)
		return ENOMEM;

	MDT_MAGIC_INIT(rx);
	rx->strm   = strm;
	rx->rtph   = rtph;
	rx->pth    = pth;
	rx->arg    = arg;
	rx->pseq   = -1;
	rx->pt     = -1;
	rx->pt_tel = -1;

	err  = str_dup(&rx->name, name);
	err |= mutex_alloc(&rx->mtx);
	if (err)
		goto out;

	/* Audio Jitter buffer */
	if (mdt_stream_type(strm) == MDT_MEDIA_AUDIO &&
	    cfg->audio.jbtype != MDT_JBUF_OFF && cfg->audio.jbuf_del.max) {

		err = mdt_jbuf_alloc(&rx->jbuf, cfg->audio.jbuf_del.min,
				 cfg->audio.jbuf_del.max, cfg->audio.jbuf_sz);
		if (err)
			goto out;

		err = mdt_jbuf_set_type(rx->jbuf, cfg->audio.jbtype);
		if (err)
			goto out;
	}

	err = mdt_jbuf_set_id(rx->jbuf, name);
	if (err)
		goto out;

	rx->metric = mdt_metric_alloc();
	if (!rx->metric)
		err = ENOMEM;
	else
		err = mdt_metric_init(rx->metric);

out:
	if (err)
		mem_deref(rx);
	else
		*rxp = rx;

	return err;
}


int mdt_rtprecv_start_thread(struct mdt_rtp_receiver *rx)
{
	int err;

	if (!rx)
		return EINVAL;

	if (re_atomic_rlx(&rx->run))
		return 0;

	udp_thread_detach(rtp_sock(rx->rtp));
	udp_thread_detach(rtcp_sock(rx->rtp));
	re_atomic_rlx_set(&rx->run, true);
	err = thread_create_name(&rx->thr,
				 "RX thread",
				 rtprecv_thread, rx);
	if (err) {
		re_atomic_rlx_set(&rx->run, false);
		udp_thread_attach(rtp_sock(rx->rtp));
		udp_thread_attach(rtcp_sock(rx->rtp));
	}

	return err;
}


bool mdt_rtprecv_running(const struct mdt_rtp_receiver *rx)
{
	if (!rx)
		return false;

	return re_atomic_rlx(&rx->run);
}


void mdt_rtprecv_set_handlers(struct mdt_rtp_receiver *rx,
			   mdt_stream_rtpestab_h *rtpestabh, void *arg)
{
	if (!rx)
		return;

	mtx_lock(rx->mtx);
	rx->rtpestabh     = rtpestabh;
	rx->sessarg       = arg;
	mtx_unlock(rx->mtx);
}


struct mdt_metric *mdt_rtprecv_metric(struct mdt_rtp_receiver *rx)
{
	if (!rx)
		return NULL;

	/* it is allowed to return metric because it is thread safe */
	return rx->metric;
}


static void work_destructor(void *arg)
{
	struct work *w = arg;

	switch (w->type) {
		case WORK_RTCP:
			mem_deref(w->u.rtcp);
			break;
		case WORK_PTCHANGED:
			mem_deref(w->u.pt.mb);
			break;
		default:
			break;
	}
}


static void async_work_main(int err, void *arg)
{
	struct work *w = arg;
	struct mdt_rtp_receiver *rx = w->rx;
	(void)err;

	switch (w->type) {
		case WORK_RTCP:
			mdt_stream_process_rtcp(rx->strm, w->u.rtcp);
			break;
		case WORK_PTCHANGED:
			rx->pth(w->u.pt.pt, w->u.pt.mb, rx->arg);
			break;
		case WORK_RTPESTAB:
			rx->rtpestabh(rx->strm, rx->sessarg);
			break;
		case WORK_MNATCONNH:
			mdt_stream_mnat_connected(rx->strm,
					      &w->u.mnat.raddr1,
					      &w->u.mnat.raddr2);
			break;
		default:
			break;
	}

	mem_deref(w);
}


void mdt_rtprecv_set_srate(struct mdt_rtp_receiver *rx, uint32_t srate)
{
	if (!rx)
		return;

	rx->srate = srate;
	mdt_jbuf_set_srate(rx->jbuf, srate);
}
