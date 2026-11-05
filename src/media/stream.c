/**
 * @file stream.c  Generic Media Stream
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's src/stream.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */
#include <string.h>
#include <time.h>
#include <re.h>
#include "mdt_media.h"
#include <re_atomic.h>


/** Magic number */
#define MAGIC 0x00511ea3


enum {
	RTP_RECV_SIZE = 8192,
	RTP_CHECK_INTERVAL = 1000,  /* how often to check for RTP [ms] */
	PORT_DISCARD = 9,
};


/* Transmit */
struct mdt_sender {
	struct mdt_metric *metric; /**< Metrics for transmit            */
	struct sa raddr_rtp;   /**< Remote RTP address              */
	struct sa raddr_rtcp;  /**< Remote RTCP address             */
	int pt_enc;            /**< Payload type for encoding       */
	RE_ATOMIC bool enabled;/**< True if enabled                 */
	mtx_t *lock;
};


/* Receive */
struct mdt_rtp_receiver;


struct rxmain {
	struct tmr tmr_rtp;    /**< Timer for detecting RTP timeout  */
	uint32_t rtp_timeout;  /**< RTP Timeout value in [ms]        */
	struct tmr tmr_rec;    /**< Timer for rtp_receiver start     */
	bool use_rxthread;     /**< Use RX thread flag               */
};


/** Defines a generic media stream */
struct mdt_stream {
#ifndef RELEASE
	uint32_t magic;          /**< Magic number for debugging            */
#endif
	struct le le;            /**< Linked list element                   */
	struct mdt_config_avt cfg;   /**< Stream configuration                  */
	struct sdp_media *sdp;   /**< SDP Media line                        */
	enum sdp_dir ldir;       /**< SDP direction of the stream           */
	struct rtp_sock *rtp;    /**< RTP Socket                            */
	struct rtcp_stats rtcp_stats;/**< RTCP statistics                   */
	const struct mdt_mnat *mnat; /**< Media NAT traversal module            */
	struct mdt_mnat_media *mns;  /**< Media NAT traversal state             */
	const struct mdt_menc *menc; /**< Media encryption module               */
	struct mdt_menc_sess *mencs; /**< Media encryption session state        */
	struct mdt_menc_media *mes;  /**< Media Encryption media state          */
	enum mdt_media_type type;    /**< Media type, e.g. audio/video          */
	char *cname;             /**< RTCP Canonical end-point identifier   */
	char *peer;              /**< Peer URI/name or identifier           */
	char *mid;               /**< Media stream identification           */
	bool rtcp_mux;           /**< RTP/RTCP multiplex supported by peer  */
	bool terminated;         /**< Stream is terminated flag             */
	RE_ATOMIC bool hold;     /**< Stream is on-hold (local)             */
	bool mnat_connected;     /**< Media NAT is connected                */
	bool menc_secure;        /**< Media stream is secure                */
	mdt_stream_rtcp_h *rtcph;    /**< Stream RTCP handler                   */
	void *arg;               /**< Handler argument                      */
	mdt_stream_mnatconn_h *mnatconnh;/**< Medianat connected handler        */
	mdt_stream_rtcp_h *sessrtcph;    /**< Stream RTCP handler               */
	mdt_stream_error_h *errorh;  /**< Stream error handler                  */
	void *sess_arg;          /**< Session handlers argument             */

	uint8_t extmap_counter;

	struct mdt_sender tx;

	struct mdt_rtp_receiver *rx;
	struct rxmain rxm;
};


static void print_rtp_stats(const struct mdt_stream *s)
{
	uint32_t tx_n_packets = mdt_metric_n_packets(s->tx.metric);
	uint32_t rx_n_packets = mdt_metric_n_packets(mdt_rtprecv_metric(s->rx));
	bool started = tx_n_packets > 0 || rx_n_packets > 0;

	if (!started)
		return;

	MDT_INFO("\n%-9s       Transmit:     Receive:\n"
	     "packets:        %7u      %7u\n"
	     "avg. bitrate:   %7.1f      %7.1f  (kbit/s)\n"
	     "errors:         %7d      %7d\n"
	     ,
	     sdp_media_name(s->sdp),
	     tx_n_packets, rx_n_packets,
	     1.0*mdt_metric_avg_bitrate(s->tx.metric)/1000.0,
	     1.0*mdt_metric_avg_bitrate(mdt_rtprecv_metric(s->rx))/1000.0,
	     mdt_metric_n_err(s->tx.metric),
	     mdt_metric_n_err(mdt_rtprecv_metric(s->rx))
	     );

	if (s->rtcp_stats.tx.sent || s->rtcp_stats.rx.sent) {

		MDT_INFO("pkt.report:     %7u      %7u\n"
		     "lost:           %7d      %7d\n"
		     "jitter:         %7.1f      %7.1f  (ms)\n",
		     s->rtcp_stats.tx.sent, s->rtcp_stats.rx.sent,
		     s->rtcp_stats.tx.lost, s->rtcp_stats.rx.lost,
		     1.0*s->rtcp_stats.tx.jit/1000,
		     1.0*s->rtcp_stats.rx.jit/1000);
	}
}


static void stream_destructor(void *arg)
{
	struct mdt_stream *s = arg;

	if (s->cfg.rtp_stats)
		print_rtp_stats(s);

	mem_deref(s->tx.metric);

	tmr_cancel(&s->rxm.tmr_rtp);
	tmr_cancel(&s->rxm.tmr_rec);
	mem_deref(s->rx);
	list_unlink(&s->le);
	mem_deref(s->sdp);
	mem_deref(s->mes);
	mem_deref(s->mencs);
	mem_deref(s->mns);
	mem_deref(s->rtp);
	mem_deref(s->cname);
	mem_deref(s->peer);
	mem_deref(s->mid);
	mem_deref(s->tx.lock);
}


static const char *media_name(enum mdt_media_type type)
{
	switch (type) {

	case MDT_MEDIA_AUDIO: return "audio";
	default:          return "???";
	}
}


static bool mnat_ready(const struct mdt_stream *strm)
{
	if (strm->mnat && strm->mnat->wait_connected)
		return strm->mnat_connected;
	else
		return true;
}


/**
 * Enable TX stream
 *
 * @param strm   Stream object
 * @param enable True to enable, false to disable
 *
 * @return 0 if success and otherwise errorcode
 */
int mdt_stream_enable_tx(struct mdt_stream *strm, bool enable)
{
	if (!strm)
		return EINVAL;

	if (!enable) {
		MDT_DBG("stream: disable %s RTP sender\n",
		      media_name(strm->type));

		re_atomic_rls_set(&strm->tx.enabled, false);
		return 0;
	}

	if (!mdt_stream_is_ready(strm))
		return EAGAIN;

	if (!(sdp_media_rdir(strm->sdp) & SDP_SENDONLY))
		return ENOTSUP;

	if (sdp_media_ldir(strm->sdp) == SDP_RECVONLY)
		return ENOTSUP;

	if (sdp_media_ldir(strm->sdp) == SDP_INACTIVE)
		return ENOTSUP;

	MDT_DBG("stream: enable %s RTP sender\n", media_name(strm->type));
	re_atomic_rls_set(&strm->tx.enabled, true);

	return 0;
}


static void stream_start_receiver(void *arg)
{
	struct mdt_stream *s = arg;
	mdt_rtprecv_start_thread(s->rx);
}


/**
 * Enable RX stream
 *
 * @param strm   Stream object
 * @param enable True to enable, false to disable
 *
 * @return 0 if success and otherwise errorcode
 */
int mdt_stream_enable_rx(struct mdt_stream *strm, bool enable)
{
	if (!strm)
		return EINVAL;

	if (!enable) {
		MDT_DBG("stream: disable %s RTP receiver\n",
		      media_name(strm->type));

		mdt_rtprecv_enable(strm->rx, false);
		return 0;
	}

	if (!(sdp_media_dir(strm->sdp) & SDP_RECVONLY))
		return ENOTSUP;

	MDT_DBG("stream: enable %s RTP receiver\n", media_name(strm->type));

	if (strm->rtp && strm->cfg.rxmode == RECEIVE_MODE_THREAD &&
	    strm->type == MDT_MEDIA_AUDIO && !mdt_rtprecv_running(strm->rx)) {
		{
			strm->rxm.use_rxthread = true;
			tmr_start(&strm->rxm.tmr_rec, 1, stream_start_receiver,
				  strm);
			return 0;
		}
	}

	mdt_rtprecv_enable(strm->rx, true);
	return 0;
}


static void stream_close(struct mdt_stream *strm, int err)
{
	mdt_stream_error_h *errorh = strm->errorh;

	strm->terminated = true;
	mdt_stream_enable(strm, false);
	strm->errorh = NULL;

	strm->rx = mem_deref(strm->rx);
	if (errorh)
		errorh(strm, err, strm->sess_arg);
}


static void check_rtp_handler(void *arg)
{
	struct mdt_stream *strm = arg;
	const uint64_t now = tmr_jiffies();
	uint64_t ts_last;
	int diff_ms;

	MDT_MAGIC_CHECK(strm);

	tmr_start(&strm->rxm.tmr_rtp, RTP_CHECK_INTERVAL,
		  check_rtp_handler, strm);

	/* If no RTP was received at all, check later */
	ts_last = mdt_rtprecv_ts_last(strm->rx);
	if (!ts_last)
		return;

	/* We are in sendrecv mode, check when the last RTP packet
	 * was received.
	 */
	if (sdp_media_dir(strm->sdp) == SDP_SENDRECV) {

		diff_ms = (int)(now - ts_last);

		if (diff_ms > 100) {
			MDT_DBG("stream: last \"%s\" RTP packet: %d "
			      "milliseconds\n",
			      sdp_media_name(strm->sdp), diff_ms);
		}

		/* check for large jumps in time */
		if (diff_ms > (3600 * 1000)) {
			mdt_rtprecv_set_ts_last(strm->rx, 0);
			return;
		}

		if (diff_ms > (int)strm->rxm.rtp_timeout) {

			MDT_INFO("stream: no %s RTP packets received for"
			     " %d milliseconds\n",
			     sdp_media_name(strm->sdp), diff_ms);

			stream_close(strm, ETIMEDOUT);
		}
	}
	else {
		MDT_DBG("check_rtp: not checking \"%s\" RTP (dir=%s)\n",
			  sdp_media_name(strm->sdp),
			  sdp_dir_name(sdp_media_dir(strm->sdp)));
	}
}


void mdt_stream_process_rtcp(struct mdt_stream *strm, struct rtcp_msg *msg)
{
	if (msg->hdr.pt == RTCP_SR && msg->hdr.count) {
		(void)rtcp_stats(strm->rtp, msg->r.sr.ssrc, &strm->rtcp_stats);
	}
	else if (msg->hdr.pt == RTCP_RR) { /* maybe rtx.ssrc RFC 4588 */
		(void)rtcp_stats(strm->rtp, msg->r.rr.ssrc, &strm->rtcp_stats);
	}

	if (strm->rtcph)
		strm->rtcph(strm, msg, strm->arg);

	if (strm->sessrtcph)
		strm->sessrtcph(strm, msg, strm->sess_arg);
}


static int stream_sock_alloc(struct mdt_stream *s, int af)
{
	struct sa laddr;
	uint8_t tos;
	int err;

	if (!s)
		return EINVAL;

	/* we listen on all interfaces */
	sa_init(&laddr, af);

	err = rtp_listen(&s->rtp, IPPROTO_UDP, &laddr,
			 s->cfg.rtp_ports.min, s->cfg.rtp_ports.max,
			 true, mdt_rtprecv_decode, mdt_rtprecv_handle_rtcp, s->rx);
	if (err) {
		MDT_WARN("stream: rtp_listen failed: af=%s ports=%u-%u"
			" (%m)\n", net_af2name(af),
			s->cfg.rtp_ports.min, s->cfg.rtp_ports.max, err);
		return err;
	}

	tos = s->cfg.rtp_tos;
	(void)udp_settos(rtp_sock(s->rtp), tos);
	(void)udp_settos(rtcp_sock(s->rtp), tos);

	udp_rxsz_set(rtp_sock(s->rtp), RTP_RECV_SIZE);

	udp_sockbuf_set(rtp_sock(s->rtp), 65536);

	mdt_rtprecv_set_socket(s->rx, s->rtp);
	return 0;
}


/**
 * Start media encryption
 *
 * @param strm   Stream object
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_stream_start_mediaenc(struct mdt_stream *strm)
{
	int err;

	if (!strm)
		return EINVAL;

	if (strm->menc && strm->menc->mediah) {
		struct sa raddr_rtp;
		struct sa raddr_rtcp;

		MDT_INFO("stream: %s: starting mediaenc '%s' (wait_secure=%d)\n",
		     media_name(strm->type), strm->menc->id,
		     strm->menc->wait_secure);

		mtx_lock(strm->tx.lock);
		sa_cpy(&raddr_rtp,  &strm->tx.raddr_rtp);
		sa_cpy(&raddr_rtcp, &strm->tx.raddr_rtcp);
		mtx_unlock(strm->tx.lock);

		err = strm->menc->mediah(&strm->mes, strm->mencs, strm->rtp,
				 rtp_sock(strm->rtp),
				 strm->rtcp_mux ? NULL : rtcp_sock(strm->rtp),
				 &raddr_rtp,
				 strm->rtcp_mux ? NULL : &raddr_rtcp,
					 strm->sdp, strm);
		if (err) {
			MDT_WARN("stream: start mediaenc error: %m\n", err);
			return err;
		}
	}

	return 0;
}



void mdt_stream_mnat_connected(struct mdt_stream *strm, const struct sa *raddr1,
			   const struct sa *raddr2)
{
	MDT_INFO("stream: '%s' mnat '%s' connected: raddr %J %J\n",
	     media_name(strm->type),
	     strm->mnat->id, raddr1, raddr2);

	mtx_lock(strm->tx.lock);
	strm->tx.raddr_rtp = *raddr1;

	if (strm->rtcp_mux)
		strm->tx.raddr_rtcp = *raddr1;
	else if (raddr2)
		strm->tx.raddr_rtcp = *raddr2;
	mtx_unlock(strm->tx.lock);

	strm->mnat_connected = true;

	if (strm->mnatconnh)
		strm->mnatconnh(strm, strm->sess_arg);

	mdt_stream_enable_tx(strm, true);
}


static int sender_init(struct mdt_sender *tx)
{
	int err;

	tx->metric = mdt_metric_alloc();
	if (!tx->metric)
		return ENOMEM;

	err = mdt_metric_init(tx->metric);

	tx->pt_enc = -1;

	return err;
}


int mdt_stream_alloc(struct mdt_stream **sp, struct list *streaml,
		 const struct mdt_stream_param *prm,
		 const struct mdt_config_avt *cfg,
		 struct sdp_session *sdp_sess,
		 enum mdt_media_type type,
		 const struct mdt_mnat *mnat, struct mdt_mnat_sess *mdt_mnat_sess,
		 const struct mdt_menc *menc, struct mdt_menc_sess *mdt_menc_sess,
		 bool offerer,
		 mdt_stream_rtp_h *rtph, mdt_stream_rtcp_h *rtcph, mdt_stream_pt_h *pth,
		 void *arg)
{
	struct mdt_stream *s;
	int err;

	if (!sp || !prm || !cfg || !rtph || !pth)
		return EINVAL;

	s = mem_zalloc(sizeof(*s), NULL);
	if (!s)
		return ENOMEM;

	MDT_MAGIC_INIT(s);

	err = mutex_alloc(&s->tx.lock);
	if (err)
		goto out;

	mem_destructor(s, stream_destructor);

	err  = sender_init(&s->tx);
	if (err)
		goto out;

	s->cfg = *cfg;
	s->cfg.rtcp_mux = prm->rtcp_mux;

	s->type   = type;
	s->rtcph  = rtcph;
	s->arg    = arg;
	s->ldir   = SDP_SENDRECV;

	if (prm->use_rtp) {
		err = mdt_rtprecv_alloc(&s->rx, s, media_name(type), cfg,
			       rtph, pth, arg);
		if (err) {
			MDT_WARN("stream: failed to create receiver"
				" for media '%s' (%m)\n",
				media_name(type), err);
			goto out;
		}

		tmr_init(&s->rxm.tmr_rtp);
		tmr_init(&s->rxm.tmr_rec);
		err = stream_sock_alloc(s, prm->af);
		if (err) {
			MDT_WARN("stream: failed to create socket"
				" for media '%s' (%m)\n",
				media_name(type), err);
			goto out;
		}
	}

	err = str_dup(&s->cname, prm->cname);
	if (err)
		goto out;

	if (prm->peer) {
		err = str_dup(&s->peer, prm->peer);
		if (err)
			goto out;
	}

	err = sdp_media_add(&s->sdp, sdp_sess, media_name(type),
			    s->rtp ? sa_port(rtp_local(s->rtp)) : PORT_DISCARD,
			    (menc && menc->sdp_proto) ? menc->sdp_proto :
			    sdp_proto_rtpavp);
	if (err)
		goto out;

	/* RFC 5506 */
	if (offerer || sdp_media_rattr(s->sdp, "rtcp-rsize"))
		err |= sdp_media_set_lattr(s->sdp, true, "rtcp-rsize", NULL);

	/* RFC 5576 */
	err |= sdp_media_set_lattr(s->sdp, true,
				   "ssrc", "%u cname:%s",
				   rtp_sess_ssrc(s->rtp), prm->cname);

	/* RFC 5761 */
	if (s->cfg.rtcp_mux &&
	    (offerer || sdp_media_rattr(s->sdp, "rtcp-mux"))) {

		err |= sdp_media_set_lattr(s->sdp, true, "rtcp-mux", NULL);
	}

	if (offerer) {
		uint32_t ix = list_count(streaml);

		err |= sdp_media_set_lattr(s->sdp, true, "mid", "%u",
					   ix);

		re_sdprintf(&s->mid, "%u", ix);
	}

	if (err)
		goto out;

	if (mnat && s->rtp) {
		s->mnat = mnat;
		err = mnat->mediah(&s->mns, mdt_mnat_sess,
				   rtp_sock(s->rtp),
				   s->cfg.rtcp_mux ? NULL : rtcp_sock(s->rtp),
				   s->sdp,
				   mdt_rtprecv_mnat_connected_handler, s->rx);
		if (err)
			goto out;
	}

	if (menc && s->rtp) {
		s->menc  = menc;
		s->mencs = mem_ref(mdt_menc_sess);

		err = mdt_stream_start_mediaenc(s);
		if (err)
			goto out;
	}

	list_append(streaml, &s->le, s);

 out:
	if (err)
		mem_deref(s);
	else
		*sp = s;

	return err;
}



uint8_t mdt_stream_generate_extmap_id(struct mdt_stream *strm)
{
	uint8_t id;

	if (!strm)
		return 0;

	id = ++strm->extmap_counter;

	if (id > RTPEXT_ID_MAX)
		return 0;

	return id;
}


/**
 * Get the sdp object from the stream
 *
 * @param strm Stream object
 *
 * @return SDP media object
 */
struct sdp_media *mdt_stream_sdpmedia(const struct mdt_stream *strm)
{
	return strm ? strm->sdp : NULL;
}


/**
 * Write stream data to the network
 *
 * @param s		Stream object
 * @param ext		Extension bit
 * @param marker	Marker bit
 * @param pt		Payload type
 * @param ts		Timestamp
 * @param mb		Payload buffer
 *
 * @return int	0 if success, errorcode otherwise
 */
int mdt_stream_send(struct mdt_stream *s, bool ext, bool marker, int pt, uint32_t ts,
		struct mbuf *mb)
{
	int err = 0;

	if (!s)
		return EINVAL;

	if (!re_atomic_acq(&s->tx.enabled))
		return 0;

	if (re_atomic_rlx(&s->hold))
		return 0;

	mdt_metric_add_packet(s->tx.metric, mbuf_get_left(mb));

	if (pt < 0) {
		mtx_lock(s->tx.lock);
		pt = s->tx.pt_enc;
		mtx_unlock(s->tx.lock);
	}

	if (pt >= 0) {
		mtx_lock(s->tx.lock);
		err = rtp_send(s->rtp, &s->tx.raddr_rtp, ext, marker, pt, ts,
			       tmr_jiffies_rt_usec(), mb);
		mtx_unlock(s->tx.lock);
		if (err)
			mdt_metric_inc_err(s->tx.metric);
	}

	return err;
}


static void stream_remote_set(struct mdt_stream *s)
{
	const char *rmid, *rssrc;

	if (!s)
		return;

	/* RFC 5576 */
	rssrc = sdp_media_rattr(s->sdp, "ssrc");
	if (rssrc) {
		struct pl num;

		if (0 == re_regex(rssrc, str_len(rssrc), "[0-9]+", &num))
			mdt_rtprecv_set_ssrc(s->rx, pl_u32(&num));
	}

	/* RFC 5761 */
	if (s->cfg.rtcp_mux && sdp_media_rattr(s->sdp, "rtcp-mux")) {

		if (!s->rtcp_mux)
			MDT_INFO("%s: RTP/RTCP multiplexing enabled\n",
			     sdp_media_name(s->sdp));
		s->rtcp_mux = true;

		sdp_media_set_lattr(s->sdp, true, "rtcp-mux", NULL);
	}

	/* RFC 5888 */
	rmid = sdp_media_rattr(s->sdp, "mid");
	if (rmid) {
		s->mid = mem_deref(s->mid);

		str_dup(&s->mid, rmid);

		sdp_media_set_lattr(s->sdp, true, "mid", "%s", rmid);
	}

	mdt_rtprecv_enable_mux(s->rx, s->rtcp_mux);

	mtx_lock(s->tx.lock);
	sa_cpy(&s->tx.raddr_rtp, sdp_media_raddr(s->sdp));

	if (s->rtcp_mux)
		s->tx.raddr_rtcp = s->tx.raddr_rtp;
	else
		sdp_media_raddr_rtcp(s->sdp, &s->tx.raddr_rtcp);
	mtx_unlock(s->tx.lock);
}


/**
 * Update the media stream
 *
 * @param s Stream object
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_stream_update(struct mdt_stream *s)
{
	const struct sdp_format *fmt;
	int err = 0;

	if (!s)
		return EINVAL;

	MDT_INFO("stream: update '%s'\n", media_name(s->type));

	/* disable rx/tx stream for updates */
	mdt_stream_enable(s, false);

	fmt = sdp_media_rformat(s->sdp, NULL);

	mtx_lock(s->tx.lock);
	s->tx.pt_enc = fmt ? fmt->pt : -1;
	mtx_unlock(s->tx.lock);

	if (mdt_sdp_media_has_media(s->sdp)) {

		stream_remote_set(s);
	}

	if (s->mencs && mnat_ready(s)) {

		err = mdt_stream_start_mediaenc(s);
		if (err) {
			MDT_WARN("stream: mediaenc update: %m\n", err);
			return err;
		}
	}

	mdt_stream_enable(s, true);

	return 0;
}


void mdt_stream_update_encoder(struct mdt_stream *s, int pt_enc)
{
	if (!s)
		return;

	if (pt_enc >= 0) {
		mtx_lock(s->tx.lock);
		s->tx.pt_enc = pt_enc;
		mtx_unlock(s->tx.lock);
	}
}


void mdt_stream_hold(struct mdt_stream *s, bool hold)
{
	enum sdp_dir dir;

	if (!s)
		return;

	re_atomic_rlx_set(&s->hold, hold);
	dir = s->ldir;

	if (hold) {
		switch (s->ldir) {
			case SDP_RECVONLY:
				dir = SDP_INACTIVE;
				break;
			case SDP_SENDRECV:
				dir = SDP_SENDONLY;
				break;
			default:
				break;
		}
	}

	sdp_media_set_ldir(s->sdp, dir);
	mdt_stream_flush(s);
}


void mdt_stream_set_ldir(struct mdt_stream *s, enum sdp_dir dir)
{
	if (!s)
		return;

	s->ldir = dir;

	if (dir == SDP_INACTIVE)
		sdp_media_set_disabled(s->sdp, true);
	else
		sdp_media_set_disabled(s->sdp, false);

	sdp_media_set_ldir(s->sdp, dir);

	mdt_stream_flush(s);
}


enum sdp_dir mdt_stream_ldir(const struct mdt_stream *s)
{
	return s ? s->ldir : SDP_INACTIVE;
}


void mdt_stream_set_srate(struct mdt_stream *s, uint32_t srate_tx, uint32_t srate_rx)
{
	if (!s)
		return;

	if (srate_tx)
		rtcp_set_srate_tx(s->rtp, srate_tx);
	if (srate_rx) {
		rtcp_set_srate_rx(s->rtp, srate_rx);
		mdt_rtprecv_set_srate(s->rx, srate_rx);
	}
}


void mdt_stream_flush(struct mdt_stream *s)
{
	if (!s)
		return;

	mdt_rtprecv_flush(s->rx);

	if (s->type == MDT_MEDIA_AUDIO)
		rtp_clear(s->rtp);
}


void mdt_stream_enable_rtp_timeout(struct mdt_stream *strm, uint32_t timeout_ms)
{
	struct sdp_media *m;

	if (!strm)
		return;

	m = mdt_stream_sdpmedia(strm);
	if (!mdt_sdp_media_has_media(m))
		return;

	if (sdp_media_disabled(m))
		return;

	const struct sdp_format *sc = sdp_media_rformat(m, NULL);
	if (!sc || !sc->data)
		return;

	strm->rxm.rtp_timeout = timeout_ms;

	tmr_cancel(&strm->rxm.tmr_rtp);

	if (timeout_ms) {

		MDT_INFO("stream: Enable RTP timeout (%u milliseconds)\n",
		     timeout_ms);

		mdt_rtprecv_set_ts_last(strm->rx, tmr_jiffies());
		tmr_start(&strm->rxm.tmr_rtp, 10, check_rtp_handler, strm);
	}
}


/**
 * Set optional session handlers
 *
 * @param strm      Stream object
 * @param mnatconnh Media NAT connected handler
 * @param rtpestabh Incoming RTP established handler
 * @param rtcph     Incoming RTCP message handler
 * @param errorh    Error handler
 * @param arg       Handler argument
 */
void mdt_stream_set_session_handlers(struct mdt_stream *strm,
				 mdt_stream_mnatconn_h *mnatconnh,
				 mdt_stream_rtpestab_h *rtpestabh,
				 mdt_stream_rtcp_h *rtcph,
				 mdt_stream_error_h *errorh, void *arg)
{
	if (!strm)
		return;

	strm->mnatconnh  = mnatconnh;
	strm->sessrtcph  = rtcph;
	strm->errorh     = errorh;
	strm->sess_arg   = arg;

	mdt_rtprecv_set_handlers(strm->rx, rtpestabh, arg);
}


/**
 * Get the RTCP Statistics from a media stream
 *
 * @param strm Stream object
 *
 * @return RTCP Statistics
 */
const struct rtcp_stats *mdt_stream_rtcp_stats(const struct mdt_stream *strm)
{
	return strm ? &strm->rtcp_stats : NULL;
}


/**
 * Get the Jitter Buffer Statistics from a media stream
 *
 * @param strm Stream object
 * @param stat Pointer to statistics storage
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_stream_jbuf_stats(const struct mdt_stream *strm, struct mdt_jbuf_stat *stat)
{
	if (!strm)
		return EINVAL;

	return mdt_jbuf_stats(mdt_rtprecv_jbuf(strm->rx), stat);
}


/**
 * Get the number of transmitted RTP packets
 *
 * @param strm Stream object
 *
 * @return Number of transmitted RTP packets
 */
uint32_t mdt_stream_metric_get_tx_n_packets(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_n_packets(strm->tx.metric) : 0;
}


/**
 * Get the number of transmitted RTP bytes
 *
 * @param strm Stream object
 *
 * @return Number of transmitted RTP bytes
 */
uint32_t mdt_stream_metric_get_tx_n_bytes(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_n_bytes(strm->tx.metric) : 0;
}


/**
 * Get the number of transmission errors
 *
 * @param strm Stream object
 *
 * @return Number of transmission errors
 */
uint32_t mdt_stream_metric_get_tx_n_err(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_n_err(strm->tx.metric) : 0;
}


/**
 * Get current transmitted RTP bitrate
 *
 * @param strm Stream object
 *
 * @return Current transmitted RTP bitrate
 */
uint32_t mdt_stream_metric_get_tx_bitrate(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_bitrate(strm->tx.metric) : 0;
}


/**
 * Get average transmitted RTP bitrate
 *
 * @param strm Stream object
 *
 * @return Average transmitted RTP bitrate
 */
double mdt_stream_metric_get_tx_avg_bitrate(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_avg_bitrate(strm->tx.metric) : 0.0;
}


/**
 * Get the number of received RTP packets
 *
 * @param strm Stream object
 *
 * @return Number of received RTP packets
 */
uint32_t mdt_stream_metric_get_rx_n_packets(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_n_packets(mdt_rtprecv_metric(strm->rx)) : 0;
}


/**
 * Get the number of received RTP bytes
 *
 * @param strm Stream object
 *
 * @return Number of received RTP bytes
 */
uint32_t mdt_stream_metric_get_rx_n_bytes(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_n_bytes(mdt_rtprecv_metric(strm->rx)) : 0;
}


/**
 * Get the number of receive errors
 *
 * @param strm Stream object
 *
 * @return Number of receive errors
 */
uint32_t mdt_stream_metric_get_rx_n_err(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_n_err(mdt_rtprecv_metric(strm->rx)) : 0;
}


/**
 * Get current received RTP bitrate
 *
 * @param strm Stream object
 *
 * @return Current received RTP bitrate
 */
uint32_t mdt_stream_metric_get_rx_bitrate(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_bitrate(mdt_rtprecv_metric(strm->rx)) : 0;
}


/**
 * Get average received RTP bitrate
 *
 * @param strm Stream object
 *
 * @return Average received RTP bitrate
 */
double mdt_stream_metric_get_rx_avg_bitrate(const struct mdt_stream *strm)
{
	return strm ? mdt_metric_avg_bitrate(mdt_rtprecv_metric(strm->rx)) : 0.0;
}


bool mdt_stream_is_ready(const struct mdt_stream *strm)
{
	if (!strm)
		return false;

	/* Media NAT */
	if (strm->mnat) {
		if (!mnat_ready(strm))
			return false;
	}

	/* Media Enc */
	if (strm->menc && strm->menc->wait_secure) {

		if (!strm->menc_secure)
			return false;
	}

	mtx_lock(strm->tx.lock);
	if (!sa_isset(&strm->tx.raddr_rtp, SA_ALL)) {
		mtx_unlock(strm->tx.lock);
		return false;
	}
	mtx_unlock(strm->tx.lock);

	if (sdp_media_dir(mdt_stream_sdpmedia(strm)) == SDP_INACTIVE)
		return false;

	return !strm->terminated;
}


/**
 * Set the secure flag on the stream object
 *
 * @param strm   Stream object
 * @param secure True for secure, false for insecure
 */
void mdt_stream_set_secure(struct mdt_stream *strm, bool secure)
{
	if (!strm)
		return;

	strm->menc_secure = secure;

	mdt_stream_enable_tx(strm, true);
}


/**
 * Get the secure flag on the stream object
 *
 * @param strm   Stream object
 *
 * @return True for secure, false for insecure
 */
bool mdt_stream_is_secure(const struct mdt_stream *strm)
{
	return strm ? strm->menc_secure : false;
}


/**
 * Start the media stream RTCP
 *
 * @param strm   Stream object
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_stream_start_rtcp(const struct mdt_stream *strm)
{
	int err = 0;

	if (!strm)
		return EINVAL;

	MDT_DBG("stream: %s: starting RTCP with remote %J\n",
	      media_name(strm->type), &strm->tx.raddr_rtcp);

	if (strm->rxm.use_rxthread) {
		err = mdt_rtprecv_start_rtcp(strm->rx, strm->cname,
					 &strm->tx.raddr_rtcp, !strm->mnat);
	}
	else {
		rtcp_start(strm->rtp, strm->cname, &strm->tx.raddr_rtcp);

		if (!strm->mnat) {
			/* Send a dummy RTCP packet to open NAT pinhole */
			err = rtcp_send_app(strm->rtp, "PING",
					    (void *)"PONG", 4);
			if (err) {
				MDT_WARN("stream: rtcp_send_app failed (%m)\n",
					err);
				return err;
			}
		}
	}

	return err;
}


/**
 * Enable stream (RX and TX)
 *
 * @param strm   Stream object
 * @param enable True to enable, false to disable
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_stream_enable(struct mdt_stream *strm, bool enable)
{
	if (!strm)
		return EINVAL;

	mdt_stream_enable_rx(strm, enable);
	mdt_stream_enable_tx(strm, enable);
	return 0;
}


/**
 * Get the name of the stream type (e.g. audio or video)
 *
 * @param strm Stream object
 *
 * @return Name of stream type
 */
const char *mdt_stream_name(const struct mdt_stream *strm)
{
	if (!strm)
		return NULL;

	return media_name(strm->type);
}


const struct sa *mdt_stream_raddr(const struct mdt_stream *strm)
{
	if (!strm)
		return NULL;

	return &strm->tx.raddr_rtp;
}


enum mdt_media_type mdt_stream_type(const struct mdt_stream *strm)
{
	return strm ? strm->type : (enum mdt_media_type)-1;
}


int mdt_stream_pt_enc(const struct mdt_stream *strm)
{
	int pt;


	if (!strm)
		return -1;

	mtx_lock(strm->tx.lock);
	pt =  strm->tx.pt_enc;
	mtx_unlock(strm->tx.lock);

	return pt;
}


struct rtp_sock *mdt_stream_rtp_sock(const struct mdt_stream *strm)
{
	return strm ? strm->rtp : NULL;
}


int mdt_stream_ssrc_rx(const struct mdt_stream *strm, uint32_t *ssrc)
{
	if (!strm)
		return EINVAL;

	return mdt_rtprecv_get_ssrc(strm->rx, ssrc);
}

