/**
 * @file call.c  Call Control
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's src/call.c (BSD-3-Clause); see THIRD_PARTY_NOTICES.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <re.h>
#include "mdt_call.h"


/** Magic number */
#define MAGIC 0xca11ca11


#define FOREACH_STREAM						\
	for (le = call->streaml.head; le; le = le->next)


/** SIP Call Control object */
struct mdt_call {
	MDT_MAGIC_DECL                /**< Magic number for debugging           */
	struct le le;             /**< Linked list element                  */
	const struct mdt_config *cfg; /**< Global configuration                 */
	struct mdt_ua *ua;            /**< SIP User-agent                       */
	struct sipsess *sess;     /**< SIP Session                          */
	struct sdp_session *sdp;  /**< SDP Session                          */
	struct sipsub *sub;       /**< Call transfer REFER subscription     */
	struct sipnot *not;       /**< REFER/NOTIFY client                  */
	struct mdt_call *xcall;       /**< Cross ref Transfer call              */
	struct list streaml;      /**< List of mediastreams (struct mdt_stream) */
	struct mdt_audio *audio;      /**< Audio stream                         */
	enum mdt_call_state state;    /**< Call state                           */
	char *local_uri;          /**< Local SIP uri                        */
	char *local_name;         /**< Local display name                   */
	char *contact_uri;        /**< Peer Contact SIP Address             */
	char *peer_uri;           /**< Peer SIP Address                     */
	char *peer_name;          /**< Peer display name                    */
	struct sa msg_src;        /**< Peer message source address          */
	char *diverter_uri;       /**< Diverter SIP Address                 */
	char *id;                 /**< Cached session call-id               */
	char *replaces;           /**< Replaces parameter                   */
	struct tmr tmr_inv;       /**< Timer for incoming calls             */
	struct tmr tmr_dtmf;      /**< Timer for incoming DTMF events       */
	struct tmr tmr_reinv;     /**< Timer for outgoing re-INVITES        */
	struct tmr tmr_defer;     /**< Retry of a deferred offer            */
	bool remote_hold;         /**< True if remote has placed us on hold */
	bool peer_cancel;         /**< The caller CANCELled our ringing     */
	time_t time_start;        /**< Time when call started               */
	time_t time_conn;         /**< Time when call initiated             */
	time_t time_stop;         /**< Time when call stopped               */
	bool outgoing;            /**< True if outgoing, false if incoming  */
	bool answered;            /**< True if call has been answered       */
	bool got_offer;           /**< Got SDP Offer from Peer              */
	bool on_hold;             /**< True if call is on hold (local)      */
	bool hold_confirmed;      /**< The peer accepted our local hold     */
	bool offer_out;           /**< An offer of ours awaits its outcome  */
	bool offer_hold;          /**< ... and the local hold it carries    */
	bool modify_deferred;     /**< A change waits for that outcome      */
	bool progr_queued;        /**< True if a progress response is queued*/
	struct mdt_mnat_sess *mnats;  /**< Media NAT session                    */
	bool mnat_wait;           /**< Waiting for MNAT to establish        */
	uint16_t answer_scode;    /**< Answered before MNAT gathered: the
				   *   answer to send once it has (0 none) */
	struct mdt_menc_sess *mencs;  /**< Media encryption session state       */
	int af;                   /**< Preferred Address Family             */
	uint16_t scode;           /**< Termination status code              */
	mdt_call_event_h *eh;         /**< Event handler                        */
	mdt_call_dtmf_h *dtmfh;       /**< DTMF handler                         */
	void *arg;                /**< Handler argument                     */

	struct mdt_config_avt cfg_avt;    /**< AVT config                    */
	struct mdt_config_call cfg_call;  /**< Call config                   */

	uint32_t rtp_timeout_ms;  /**< RTP Timeout in [ms]                  */
	uint32_t linenum;         /**< Line number from 1 to N              */
	struct list custom_hdrs;  /**< List of custom headers if any        */

	enum sdp_dir estadir;      /**< Established audio direction         */
	bool use_rtp;
	struct pl *user_data;      /**< User data related to the call       */
};


static int send_invite(struct mdt_call *call);
static int send_dtmf_info(struct mdt_call *call, char key);


static const char *state_name(enum mdt_call_state st)
{
	switch (st) {

	case MDT_CALL_IDLE:		 return "IDLE";
	case MDT_CALL_INCOMING:	 return "INCOMING";
	case MDT_CALL_OUTGOING:	 return "OUTGOING";
	case MDT_CALL_RINGING:	 return "RINGING";
	case MDT_CALL_EARLY:		 return "EARLY";
	case MDT_CALL_ESTABLISHED:	 return "ESTABLISHED";
	case MDT_CALL_TERMINATED:	 return "TERMINATED";
	case MDT_CALL_TRANSFER:	 return "TRANSFER";
	case MDT_CALL_UNKNOWN:	 return "UNKNOWN";
	default:			 return "???";
	}
}


static void set_state(struct mdt_call *call, enum mdt_call_state st)
{
	call->state = st;
}


static const struct sdp_format *sdp_media_rcodec(const struct sdp_media *m)
{
	const struct list *lst;
	struct le *le;

	if (!m || !sdp_media_rport(m))
		return NULL;

	lst = sdp_media_format_lst(m, false);

	for (le=list_head(lst); le; le=le->next) {

		const struct sdp_format *fmt = le->data;

		if (!fmt->sup)
			continue;

		if (!fmt->data)
			continue;

		return fmt;
	}

	return NULL;
}


static void call_timer_start(struct mdt_call *call)
{
	MDT_DBG("call: timer started\n");
	tmr_cancel(&call->tmr_inv);
	call->time_start = time(NULL);
}


static void call_stream_stop(struct mdt_call *call)
{
	if (!call)
		return;

	call->time_stop = time(NULL);

	mdt_audio_stop(call->audio);

	tmr_cancel(&call->tmr_inv);
}


static void call_event_handler(struct mdt_call *call, enum mdt_call_event ev,
			       const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	if (!call || !call->eh)
		return;

	va_start(ap, fmt);
	(void)re_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	call->eh(call, ev, buf, call->arg);
}


static void invite_timeout(void *arg)
{
	struct mdt_call *call = arg;

	MDT_INFO("%s: Local timeout after %u seconds\n",
	     call->peer_uri, call->cfg_call.local_timeout);

	call_event_handler(call, MDT_CALL_EVENT_CLOSED, "Local timeout");
}


/* Called when all media streams are established */
static void mnat_handler(int err, uint16_t scode, const char *reason,
			 void *arg)
{
	struct mdt_call *call = arg;
	MDT_MAGIC_CHECK(call);

	if (err) {
		MDT_WARN("call: medianat '%s' failed: %m\n",
			call->ua->mnat->id, err);
		call_event_handler(call, MDT_CALL_EVENT_CLOSED, "%m", err);
		return;
	}
	else if (scode) {
		MDT_WARN("call: medianat failed: %u %s\n", scode, reason);
		call_event_handler(call, MDT_CALL_EVENT_CLOSED, "%u %s",
				   scode, reason);
		return;
	}

	MDT_INFO("call: media-nat '%s' established/gathered\n",
	     call->ua->mnat->id);

	/* Re-INVITE */
	if (!call->mnat_wait) {
		MDT_INFO("call: medianat established -- sending Re-INVITE\n");
		(void)mdt_call_modify(call);
		return;
	}

	call->mnat_wait = false;

	switch (call->state) {

	case MDT_CALL_OUTGOING:
		(void)send_invite(call);
		break;

	case MDT_CALL_INCOMING:
		/* The call was reported as it arrived (mdt_call_accept); an
		 * answer given while the media NAT gathered goes out now, with
		 * the candidates in it. */
		if (call->answer_scode) {
			const uint16_t scode = call->answer_scode;

			call->answer_scode = 0;
			err = mdt_call_respond(call, scode);
			if (err) {
				MDT_WARN("call: answer after media-nat: %m\n",
					  err);
				call_event_handler(call,
						   MDT_CALL_EVENT_CLOSED,
						   "%m", err);
			}
		}
		break;

	default:
		break;
	}
}


static int call_apply_sdp(struct mdt_call *call)
{
	struct le *le;
	int err = 0;

	if (!call)
		return EINVAL;

	mdt_audio_sdp_attr_decode(call->audio);

	/* Update each stream */
	FOREACH_STREAM {
		struct mdt_stream *strm = le->data;

		mdt_stream_update(strm);

		if (mdt_stream_is_ready(strm)) {

			mdt_stream_start_rtcp(strm);
		}
	}

	if (call->ua->mnat && call->ua->mnat->updateh && call->mnats)
		err = call->ua->mnat->updateh(call->mnats);

	return err;
}


static int update_streams(struct mdt_call *call)
{
	int err = 0;

	if (!call)
		return EINVAL;

	if (mdt_stream_is_ready(mdt_audio_strm(call->audio)))
		err |= mdt_audio_update(call->audio);
	else
		mdt_audio_stop(call->audio);

	return err;
}


int mdt_call_update_media(struct mdt_call *call)
{
	int err;

	err = call_apply_sdp(call);
	err |= update_streams(call);

	return err;
}


static int update_media(struct mdt_call *call)
{
	int err;

	MDT_DBG("call: update media\n");

	err = mdt_call_update_media(call);

	/* After the media follow the remote SDP: on an outgoing call this
	 * completes the offer/answer, and the negotiation event reads the
	 * codec the encoder now has. */
	bsdk_ev_call_sdp(bsdk_call_find(call), true, !call->got_offer);

	return err;
}


static void print_summary(const struct mdt_call *call)
{
	uint32_t dur = mdt_call_duration(call);
	if (!dur)
		return;

	MDT_INFO("%s: Call with %s terminated (duration: %H)\n",
	     call->local_uri, call->peer_uri, fmt_human_time, &dur);
}


static void call_destructor(void *arg)
{
	struct mdt_call *call = arg;

	if (call->state != MDT_CALL_IDLE)
		print_summary(call);

	call_stream_stop(call);
	list_unlink(&call->le);
	tmr_cancel(&call->tmr_dtmf);
	tmr_cancel(&call->tmr_reinv);
	tmr_cancel(&call->tmr_defer);

	mem_deref(call->sess);
	mem_deref(call->id);
	mem_deref(call->local_uri);
	mem_deref(call->local_name);
	mem_deref(call->contact_uri);
	mem_deref(call->peer_uri);
	mem_deref(call->peer_name);
	mem_deref(call->replaces);
	mem_deref(call->diverter_uri);
	mem_deref(call->audio);
	mem_deref(call->sdp);
	mem_deref(call->mnats);
	mem_deref(call->mencs);
	mem_deref(call->sub);
	mem_deref(call->not);
	mem_deref(call->ua);
	mem_deref(call->user_data);

	list_flush(&call->custom_hdrs);
}


static void audio_event_handler(int key, bool end, void *arg)
{
	struct mdt_call *call = arg;
	MDT_MAGIC_CHECK(call);

	MDT_INFO("received in-band DTMF event: '%c' (end=%d)\n", key, end);

	if (call->dtmfh)
		call->dtmfh(call, end ? KEYCODE_REL : key, call->arg);
}


static void audio_error_handler(int err, const char *str, void *arg)
{
	struct mdt_call *call = arg;
	MDT_MAGIC_CHECK(call);

	if (err) {
		MDT_WARN("call: audio device error: %m (%s)\n", err, str);

		call_stream_stop(call);
		call_event_handler(call, MDT_CALL_EVENT_CLOSED,
			"%s", str);
	}
}


static void menc_event_handler(enum mdt_menc_event event,
			       const char *prm, struct mdt_stream *strm, void *arg)
{
	struct mdt_call *call = arg;
	int err;
	(void)strm;
	MDT_MAGIC_CHECK(call);

	MDT_DBG("call: mediaenc event '%s' (%s)\n", mdt_menc_event_name(event), prm);

	bool secure_rtcp = NULL != strstr(prm, "RTCP");

	switch (event) {

	case MDT_MENC_EVENT_SECURE:
		if (strstr(prm, "audio")) {
			mdt_stream_set_secure(mdt_audio_strm(call->audio), true);
			if (secure_rtcp)
				mdt_stream_start_rtcp(mdt_audio_strm(call->audio));
			err = mdt_audio_update(call->audio);
			if (err) {
				MDT_WARN("call: secure: could not"
					" start audio: %m\n", err);
			}
		}
		else {
			MDT_INFO("call: mediaenc: no match for stream (%s)\n",
			     prm);
		}
		break;

	default:
		break;
	}

	if (str_isset(prm))
		call_event_handler(call, MDT_CALL_EVENT_MENC, "%u,%s", event,
				   prm);
	else
		call_event_handler(call, MDT_CALL_EVENT_MENC, "%u", event);
}


static void menc_error_handler(int err, void *arg)
{
	struct mdt_call *call = arg;
	MDT_MAGIC_CHECK(call);

	MDT_WARN("call: mediaenc '%s' error: %m\n", call->ua->mencp->id, err);

	call_stream_stop(call);
	call_event_handler(call, MDT_CALL_EVENT_CLOSED, "mediaenc failed");
}


static void stream_mnatconn_handler(struct mdt_stream *strm, void *arg)
{
	struct mdt_call *call = arg;
	int err;
	MDT_MAGIC_CHECK(call);

	if (call->mencs) {
		err = mdt_stream_start_mediaenc(strm);
		if (err) {
			call_event_handler(call, MDT_CALL_EVENT_CLOSED,
					   "mediaenc failed %m", err);
		}
	}
	else if (mdt_stream_is_ready(strm)) {

		mdt_stream_start_rtcp(strm);

		err = mdt_audio_update(call->audio);
		if (err) {
			MDT_WARN("call: mnatconn: could not"
				" start audio: %m\n", err);
		}
	}
}


static void stream_rtcp_handler(struct mdt_stream *strm,
				struct rtcp_msg *msg, void *arg)
{
	struct mdt_call *call = arg;

	MDT_MAGIC_CHECK(call);

	(void)strm;

	/* Keep the X-RTP-Stat header of the BYE current. */
	if (msg->hdr.pt == RTCP_SR && call->cfg_avt.rtp_stats)
		mdt_call_set_xrtpstat(call);
}


static void stream_error_handler(struct mdt_stream *strm, int err, void *arg)
{
	struct mdt_call *call = arg;
	MDT_MAGIC_CHECK(call);

	MDT_INFO("call: error in \"%s\" rtp stream (%m)\n",
		sdp_media_name(mdt_stream_sdpmedia(strm)), err);

	call->scode = 701;
	set_state(call, MDT_CALL_TERMINATED);

	call_stream_stop(call);
	call_event_handler(call, MDT_CALL_EVENT_CLOSED, "rtp stream error");
}


static int assign_linenum(uint32_t *linenum, const struct list *lst)
{
	uint32_t num;

	for (num=CALL_LINENUM_MIN; num<CALL_LINENUM_MAX; num++) {

		if (!mdt_call_find_linenum(lst, num)) {
			*linenum = num;
			return 0;
		}
	}

	return ENOENT;
}


/* RFC 5373: a caller that requires auto-answer is refused — the SDK never
 * answers on its own. */
static void call_rfc5373_autoanswer(struct mdt_call *call,
		const struct sip_msg *msg, const char *name)
{
	const struct sip_hdr *hdr;
	struct pl v1;

	hdr = sip_msg_xhdr(msg, name);
	if (!hdr || pl_strcasecmp(&hdr->val, "Auto"))
		return;

	if (!msg_param_exists(&hdr->val, "require", &v1)) {
		MDT_WARN("call: rejected, since %s is not allowed\n", name);
		mdt_call_end(call, 0, NULL);
	}
}


/* An auto-answer request in Call-Info / Alert-Info (answer-after=,
 * info=alert-autoanswer: Polycom, Avaya, Grandstream, Snom, Gigaset,
 * Yealink). */
static bool hdr_has_autoanswer(const struct sip_hdr *hdr)
{
	struct pl v;

	if (!hdr)
		return false;

	if (!msg_param_decode(&hdr->val, "answer-after", &v))
		return true;

	return !msg_param_decode(&hdr->val, "info", &v) &&
	       !pl_strcmp(&v, "alert-autoanswer");
}


static void call_decode_diverter(struct mdt_call *call, const struct sip_msg *msg)
{
	const struct sip_hdr *hdr;
	struct sip_addr addr;
	int err;

	if (!call || !msg)
		return;

	hdr = sip_msg_hdr(msg, SIP_HDR_HISTORY_INFO);
	if (!hdr)
		hdr = sip_msg_xhdr(msg, "Diversion");
	if (!hdr)
		return;

	err = sip_addr_decode(&addr, &hdr->val);
	if (err) {
		MDT_WARN("call: error parsing diverter address: %r\n",
			&hdr->val);
		return;
	}

	err = pl_strdup(&call->diverter_uri, &addr.auri);

	if (err) {
		MDT_WARN("call: could not extract diverter uri");
		return;
	}
}


/* An incoming call that asks to be auto-answered.  A vendor auto-answer
 * header is only noticed; with none, an RFC 5373 one that requires it is
 * refused. */
static void call_check_autoanswer(struct mdt_call *call,
				  const struct sip_msg *msg)
{
	if (!msg)
		return;

	if (hdr_has_autoanswer(sip_msg_hdr(msg, SIP_HDR_CALL_INFO)) ||
	    hdr_has_autoanswer(sip_msg_hdr(msg, SIP_HDR_ALERT_INFO)))
		return;

	call_rfc5373_autoanswer(call, msg, "Answer-Mode");
	call_rfc5373_autoanswer(call, msg, "Priv-Answer-Mode");
}


int mdt_call_streams_alloc(struct mdt_call *call)
{
	if (!call)
		return EINVAL;

	const struct mdt_ua *ua = call->ua;
	struct mdt_stream_param strm_prm;
	struct le *le;
	int err;

	memset(&strm_prm, 0, sizeof(strm_prm));
	strm_prm.use_rtp  = call->use_rtp;
	strm_prm.af	  = call->af;
	strm_prm.cname	  = call->local_uri;
	strm_prm.peer	  = call->peer_uri;
	strm_prm.rtcp_mux = ua->rtcp_mux;

	err = mdt_audio_alloc(&call->audio, &call->streaml, &strm_prm,
			  call->cfg, call->sdp,
			  ua->mnat, call->mnats, ua->mencp, call->mencs,
			  ua->ptime, mdt_ua_aucodecl(ua),
			  !call->got_offer,
			  audio_event_handler, NULL,
			  audio_error_handler, call);
	if (err)
		return err;

	FOREACH_STREAM {
		struct mdt_stream *strm = le->data;

		mdt_stream_set_session_handlers(strm, stream_mnatconn_handler,
					    NULL,
					    stream_rtcp_handler,
					    stream_error_handler, call);
	}

	if (call->cfg->audio.level && !call->got_offer) {
		err = mdt_audio_enable_level(call->audio);
		if (err)
			return err;
	}

	return 0;
}


/**
 * Allocate a new Call state object
 *
 * @param callp       Pointer to allocated Call state object
 * @param cfg         Global configuration
 * @param lst         List of call objects
 * @param local_name  Local display name (optional)
 * @param local_uri   Local SIP uri
 * @param ua          User-Agent
 * @param prm         Call parameters
 * @param msg         SIP message for incoming calls
 * @param xcall       Optional call to inherit properties from
 * @param dnsc        DNS Client
 * @param eh          Call event handler
 * @param arg         Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_alloc(struct mdt_call **callp, const struct mdt_config *cfg, struct list *lst,
	       const char *local_name, const char *local_uri,
	       struct mdt_ua *ua, const struct mdt_call_prm *prm,
	       const struct sip_msg *msg, struct mdt_call *xcall,
	       struct dnsc *dnsc,
	       mdt_call_event_h *eh, void *arg)
{
	struct mdt_call *call;
	int err = 0;

	if (!cfg || !local_uri || !ua || !prm)
		return EINVAL;

	MDT_DBG("call: alloc with params laddr=%j, af=%s, use_rtp=%d\n",
	      &prm->laddr, net_af2name(prm->af), prm->use_rtp);

	call = mem_zalloc(sizeof(*call), call_destructor);
	if (!call)
		return ENOMEM;

	MDT_MAGIC_INIT(call);

	call->cfg_avt = cfg->avt;
	call->cfg_call = cfg->call;

	tmr_init(&call->tmr_inv);
	tmr_init(&call->tmr_reinv);
	tmr_init(&call->tmr_defer);

	call->cfg    = cfg;
	call->ua     = mem_ref(ua);
	call->state  = MDT_CALL_IDLE;
	call->eh     = eh;
	call->arg    = arg;
	call->af     = prm->af;
	call->estadir = SDP_SENDRECV;
	call->use_rtp = prm->use_rtp;
	call_check_autoanswer(call, msg);
	call_decode_diverter(call, msg);

	err = str_dup(&call->local_uri, local_uri);
	if (local_name)
		err |= str_dup(&call->local_name, local_name);

	if (msg) {
		struct sip_addr addr;
		const struct sip_hdr *hdr = sip_msg_hdr(msg, SIP_HDR_CONTACT);
		if (hdr && 0 == sip_addr_decode(&addr, &hdr->val))
			err |= pl_strdup(&call->contact_uri, &addr.auri);

		err |= pl_strdup(&call->peer_uri, &msg->from.auri);
	}

	if (err)
		goto out;

	/* Init SDP info */
	err = sdp_session_alloc(&call->sdp, &prm->laddr);
	if (err)
		goto out;

	/* Check for incoming SDP Offer */
	if (msg && mbuf_get_left(msg->mb))
		call->got_offer = true;

	/* Initialise media NAT handling */
	if (ua->mnat) {
		err = ua->mnat->sessh(&call->mnats, ua->mnat,
				       dnsc, call->af,
				       ua->stun_host,
				       ua->stun_user, ua->stun_pass,
				       call->sdp, !call->got_offer,
				       mnat_handler, call);
		if (err) {
			MDT_WARN("call: medianat session: %m\n", err);
			goto out;
		}
	}
	call->mnat_wait = true;

	/* Media encryption */
	if (ua->mencp) {
		if (ua->mencp->sessh) {
			err = ua->mencp->sessh(&call->mencs, call->sdp,
					       !call->got_offer,
					       menc_event_handler,
					       menc_error_handler, call);
			if (err) {
				MDT_WARN("call: mediaenc session: %m\n", err);
				goto out;
			}
		}
	}

	/* inherit certain properties from original call */
	if (xcall) {
		call->not = mem_ref(xcall->not);
		call->xcall = xcall;
	}

	if (cfg->avt.rtp_timeout) {
		mdt_call_enable_rtp_timeout(call, cfg->avt.rtp_timeout*1000);
	}

	err = assign_linenum(&call->linenum, lst);
	if (err) {
		MDT_WARN("call: could not assign linenumber\n");
		goto out;
	}

	/* NOTE: The new call must always be added to the tail of list,
	 *       which indicates the current call.
	 */
	list_append(lst, &call->le, call);

 out:
	if (err) {
		mem_deref(call);
	}
	else if (callp) {
		*callp = call;
		if (xcall)
			xcall->xcall = call;
	}

	return err;
}


void mdt_call_set_custom_hdrs(struct mdt_call *call, const struct list *hdrs)
{
	struct le *le;

	if (!call)
		return;

	list_flush(&call->custom_hdrs);

	LIST_FOREACH(hdrs, le) {
		struct sip_hdr *hdr = le->data;
		char *buf = NULL;

		if (re_sdprintf(&buf, "%r", &hdr->name))
			return;

		if (mdt_hdrs_add(&call->custom_hdrs, buf,
				    "%r", &hdr->val)) {
			mem_deref(buf);
			return;
		}

		mem_deref(buf);
	}
}


/**
 * Connect an outgoing call to a given SIP uri
 *
 * @param call  Call Object
 * @param paddr SIP address or uri to connect to
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_invite(struct mdt_call *call, const struct pl *paddr)
{
	struct sip_addr addr;
	struct pl rname = PL("Replaces");
	struct pl rval = PL_INIT;
	int err;

	if (!call || !paddr)
		return EINVAL;

	MDT_INFO("call: connecting to '%r'..\n", paddr);

	call->outgoing = true;
	err = str_x64dup(&call->id, rand_u64());
	if (err)
		return err;

	/* if the peer-address is a full SIP address then we need
	 * to parse it and extract the SIP uri part.
	 */
	call->peer_uri = mem_deref(call->peer_uri);
	if (0 == sip_addr_decode(&addr, paddr)) {

		uri_header_get(&addr.uri.headers, &rname, &rval);
		if (pl_isset(&rval))
			err = re_sdprintf(&call->replaces, "%H",
					  uri_header_unescape, &rval);

		addr.uri.headers.l = 0;

		if (pl_isset(&addr.params)) {
			err |= re_sdprintf(&call->peer_uri, "%H%r", uri_encode,
					   &addr.uri, &addr.params);
		}
		else {
			err |= re_sdprintf(&call->peer_uri, "%H", uri_encode,
					   &addr.uri);
		}

		if (pl_isset(&addr.dname))
			pl_strdup(&call->peer_name, &addr.dname);

	}
	else {
		err = pl_strdup(&call->peer_uri, paddr);
	}
	if (err)
		return err;

	set_state(call, MDT_CALL_OUTGOING);
	call_event_handler(call, MDT_CALL_EVENT_OUTGOING, "%s", call->peer_uri);

	/* If we are using asynchronous medianat like STUN/TURN, then
	 * wait until completed before sending the INVITE */
	if (!call->ua->mnat) {
		err = send_invite(call);
	}
	else {
		err = mdt_call_streams_alloc(call);
		if (err)
			return err;

		mdt_call_set_mdir(call, call->estadir);
	}

	return err;
}


static void deferred_handler(void *arg)
{
	struct mdt_call *call = arg;

	if (!call->modify_deferred)
		return;

	if (mdt_call_refresh_allowed(call)) {
		call->modify_deferred = false;
		(void)mdt_call_modify(call);
	}
	else if (!call->offer_out) {
		tmr_start(&call->tmr_defer, 300, deferred_handler, call);
	}
}


/**
 * Update the current call by sending Re-INVITE or UPDATE
 *
 * @param call Call object
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_modify(struct mdt_call *call)
{
	struct mbuf *desc = NULL;
	int err;

	if (!call)
		return EINVAL;

	MDT_DBG("call: modify\n");

	if (mdt_call_refresh_allowed(call)) {
		bsdk_ev_call_sdp(bsdk_call_find(call), false, false);

		err = mdt_call_sdp_get(call, &desc, true);
		if (err)
			return err;

		err = sipsess_modify(call->sess, desc);
		if (err)
			goto out;

		/* libre sends this offer now or, behind a request of ours
		 * still under way, in its place: either way the next outcome
		 * reported (modify_handler) is this offer's. */
		call->offer_out  = true;
		call->offer_hold = call->on_hold;
	}
	else if (sip_dialog_established(sipsess_dialog(call->sess))) {
		/* An offer is in flight: this change goes out when it ends —
		 * ours, reported to modify_handler; the peer's, which reports
		 * nothing to us, by retrying shortly. */
		call->modify_deferred = true;
		if (!call->offer_out)
			tmr_start(&call->tmr_defer, 300, deferred_handler,
				  call);
	}

	err = mdt_call_update_media(call);

 out:
	mem_deref(desc);

	return err;
}


/**
 * Hangup the call
 *
 * @param call   Call to hangup
 * @param scode  Optional status code
 * @param reason Optional reason
 * @param fmt    Formatted headers
 * @param ...    Variable arguments
 */
void mdt_call_endf(struct mdt_call *call, uint16_t scode, const char *reason,
		 const char *fmt, ...)
{
	if (!call)
		return;

	if (call->cfg_avt.rtp_stats)
		mdt_call_set_xrtpstat(call);

	if (call->state == MDT_CALL_INCOMING) {
		if (call->answered) {
			MDT_INFO("call: abort call '%s' with %s\n",
			     sip_dialog_callid(sipsess_dialog(call->sess)),
			     call->peer_uri);
			sipsess_abort(call->sess);
		}
		else {
			if (!scode)
				scode = 486;

			if (!str_isset(reason))
				reason = "Busy Here";

			MDT_INFO("call: rejecting incoming call from %s (%u %s)\n",
			     call->peer_uri, scode, reason);
			va_list ap;
			va_start(ap, fmt);
			(void)sipsess_reject(call->sess, scode, reason,
					     fmt ? "%v" : NULL, fmt, &ap);
			va_end(ap);
		}
	}
	else {
		MDT_INFO("call: terminate call '%s' with %s\n",
		     sip_dialog_callid(sipsess_dialog(call->sess)),
		     call->peer_uri);

		if (call->not)
			mdt_call_sipfrag(call, 487, "Request Terminated");

		call->sess = mem_deref(call->sess);
	}

	set_state(call, MDT_CALL_TERMINATED);

	call_stream_stop(call);
}


/**
 * Hangup the call
 *
 * @param call   Call to hangup
 * @param scode  Optional status code
 * @param reason Optional reason
 */
void mdt_call_end(struct mdt_call *call, uint16_t scode, const char *reason)
{
	mdt_call_endf(call, scode, reason, NULL);
}



/**
 * Send a SIP 183 Session Progress with the given audio direction
 *
 * @param call Call to answer
 * @param adir Audio direction
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_progress_dir(struct mdt_call *call, enum sdp_dir adir)
{
	struct mbuf *desc;
	int err;

	if (!call)
		return EINVAL;

	enum sdp_dir ardir = sdp_media_rdir(mdt_stream_sdpmedia(mdt_audio_strm(
				mdt_call_audio(call))));
	if ((adir & ardir) == SDP_INACTIVE) {
		MDT_DBG("call: progress: audio would become inactive\n");
		return EINVAL;
	}

	tmr_cancel(&call->tmr_inv);

	if (adir != call->estadir)
		mdt_call_set_mdir(call, adir);

	err = mdt_call_sdp_get(call, &desc, false);
	if (err)
		return err;

	err = sipsess_progress(call->sess, 183, "Session Progress",
			       call->ua->rel100,
			       desc, "Allow: %H\r\n%H",
			       mdt_ua_print_allowed, call->ua,
			       mdt_ua_print_require, call->ua);

	call->progr_queued = (err == EAGAIN);
	if (err)
		goto out;

	if (call->got_offer) {
		bsdk_ev_call_sdp(bsdk_call_find(call), false, true);
		err = mdt_call_update_media(call);
	}

	if (err)
		goto out;

out:
	mem_deref(desc);

	return err;
}


static bool call_need_modify(const struct mdt_call *call)
{
	enum sdp_dir adir;

	if (!call)
		return false;

	adir = mdt_stream_ldir(mdt_audio_strm(mdt_call_audio(call)));
	return adir != call->estadir;
}


/**
 * Answer an incoming call
 *
 * @param call  Call to answer
 * @param scode Status code
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_respond(struct mdt_call *call, uint16_t scode)
{
	struct mbuf *desc;
	int err;

	if (!call || !call->sess)
		return EINVAL;


	if (MDT_CALL_INCOMING != call->state) {
		MDT_INFO("call: answer: call is not in incoming state (%s)\n",
		     state_name(call->state));
		return EINVAL;
	}

	if (sipsess_awaiting_prack(call->sess)) {
		MDT_INFO("call: answer: can not answer because we are awaiting a "
		     "PRACK to a 1xx response with SDP\n");
		return EAGAIN;
	}

	/* Answered before the media NAT has gathered — the call rings as it
	 * arrives, not once ICE is ready: the answer goes out the moment it
	 * has (mnat_handler). */
	/* (mnat_wait is set for every call; only one with a media NAT ever
	 * sees it cleared.) */
	if (call->ua->mnat && call->mnat_wait) {
		MDT_INFO("call: answer %u: waiting for media-nat '%s' to"
			  " gather\n", scode, call->ua->mnat->id);
		call->answer_scode = scode;
		return 0;
	}

	MDT_INFO("call: answering call on line %u from %s with %u\n",
			call->linenum, call->peer_uri, scode);

	if (call->got_offer) {
		err = call_apply_sdp(call);
		if (err)
			return err;
	}

	bsdk_ev_call_sdp(bsdk_call_find(call), false, call->got_offer);

	err = sdp_encode(&desc, call->sdp, !call->got_offer);
	if (err)
		return err;

	if (scode >= 200 && scode < 300) {
		err = sipsess_answer(call->sess, scode, "Answering", desc,
				"Allow: %H\r\n"
				"%H", mdt_ua_print_allowed, call->ua,
				mdt_ua_print_supported, call->ua);
	}
	else {
		err = sipsess_answer(call->sess, scode, "Answering", desc,
				"Allow: %H\r\n", mdt_ua_print_allowed, call->ua);
	}

	if (err)
		goto out;

	call->answered = true;

out:
	mem_deref(desc);
	return err;
}


/**
 * Put the current call on hold/resume
 *
 * @param call  Call object
 * @param hold  True to hold, false to resume
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_hold(struct mdt_call *call, bool hold)
{
	struct le *le;

	if (!call || !call->sess)
		return EINVAL;

	if (hold == call->on_hold)
		return 0;

	MDT_INFO("call: %s %s\n", hold ? "hold" : "resume", call->peer_uri);

	call->on_hold = hold;

	FOREACH_STREAM
		mdt_stream_hold(le->data, hold);

	return mdt_call_modify(call);
}


int mdt_call_sdp_get(const struct mdt_call *call, struct mbuf **descp, bool offer)
{
	if (!call)
		return EINVAL;

	return sdp_encode(descp, call->sdp, offer);
}


/**
 * Check if a target refresh (re-INVITE or UPDATE) is currently allowed
 *
 * @param call  Call object
 *
 * @return True if a target refresh is currently allowed, otherwise false
 */
bool mdt_call_refresh_allowed(const struct mdt_call *call)
{
	return call ? sipsess_refresh_allowed(call->sess) : false;
}


/* Withdraw our unanswered re-INVITE (sipsess_reinvite_abandon(), an
 * Medut patch to libre).  True when one was dropped. */
bool mdt_call_reinvite_abandon(struct mdt_call *call)
{
	return call ? sipsess_reinvite_abandon(call->sess) : false;
}


/**
 * Get the session call-id for the call
 *
 * @param call Call object
 *
 * @return Session call-id
 */
const char *mdt_call_id(const struct mdt_call *call)
{
	return call ? call->id : NULL;
}


/**
 * Get the URI of the peer
 *
 * For incoming calls, this is the From header URI of the incoming INVITE.
 *
 * @param call  Call object
 *
 * @return Peer URI
 */
const char *mdt_call_peeruri(const struct mdt_call *call)
{
	return call ? call->peer_uri : NULL;
}


/**
 * Get the Contact URI of the peer
 *
 * For outgoing calls, this is the same as mdt_call_peeruri.
 * For incoming calls, this is the Contact header URI of the incoming INVITE.
 *
 * @param call  Call object
 *
 * @return Peer Contact URI
 */
const char *mdt_call_contacturi(const struct mdt_call *call)
{
	if (!call)
		return NULL;

	return call->outgoing ? call->peer_uri : call->contact_uri;
}


/**
 * Get the local URI of the call
 *
 * @param call  Call object
 *
 * @return Local URI
 */
const char *mdt_call_localuri(const struct mdt_call *call)
{
	return call ? call->local_uri : NULL;
}


/**
 * Get the name of the peer
 *
 * @param call  Call object
 *
 * @return Peer name
 */
const char *mdt_call_peername(const struct mdt_call *call)
{
	return call ? call->peer_name : NULL;
}


/**
 * Get the diverter URI of the call
 *
 * @param call  Call object
 *
 * @return Diverter URI
 */
const char *mdt_call_diverteruri(const struct mdt_call *call)
{
	return call ? call->diverter_uri : NULL;
}


/**
 * Send a DTMF digit to the peer
 *
 * @param call  Call object
 * @param key   DTMF digit to send (KEYCODE_REL for key release)
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_send_digit(struct mdt_call *call, char key)
{
	int err = 0;
	const struct sdp_format *fmt;
	bool info = true;

	if (!call)
		return EINVAL;

	switch (call->ua->dtmf) {
		case MEDUT_DTMF_SIP_INFO:
			info = true;
			break;
		case MEDUT_DTMF_AUTO:
			fmt = sdp_media_rformat(
				mdt_stream_sdpmedia(mdt_audio_strm(call->audio)),
				telev_rtpfmt);
			info = fmt == NULL;
			break;
		case MEDUT_DTMF_RFC4733:
		default:
			info = false;
			break;
	}

	if (info) {
		if (key != KEYCODE_REL) {
			err = send_dtmf_info(call, key);
		}
	}
	else {
		err = mdt_audio_send_digit(call->audio, key);
	}

	return err;
}


/**
 * Get the User-Agent for the call
 *
 * @param call Call object
 *
 * @return User-Agent
 */
struct mdt_ua *mdt_call_get_ua(const struct mdt_call *call)
{
	return call ? call->ua : NULL;
}


/* Tell the SDK the call's hold state: on hold when our hold is confirmed
 * or the peer holds us, and whether a local hold is (still) wanted. */
static void hold_report(struct mdt_call *call)
{
	bsdk_ev_call_hold(bsdk_call_find(call), bsdk_account_find(call->ua),
			  call->hold_confirmed || call->remote_hold,
			  call->on_hold);
}


/* How an offer of ours ended (libre patch V): once per re-INVITE / UPDATE,
 * with its final response, or err and no response.
 *
 * Accepted, the local hold it carried is the call's.  Refused, a hold or
 * resume it carried is undone — the peer keeps the media as it was, and
 * the next offer must not carry it again.  Unanswered (err), the hold stays
 * wanted for the next offer to carry.  A change that waited for this offer
 * goes out now. */
static void modify_handler(int err, const struct sip_msg *msg, void *arg)
{
	struct mdt_call *call = arg;
	const bool had_offer = call->offer_out;
	struct le *le;

	MDT_MAGIC_CHECK(call);

	call->offer_out = false;

	if (had_offer && !err && msg && msg->scode < 300) {
		if (call->hold_confirmed != call->offer_hold) {
			call->hold_confirmed = call->offer_hold;
			hold_report(call);
		}
	}
	else if (had_offer && !err && msg &&
		 call->on_hold != call->hold_confirmed) {
		MDT_INFO("call: %s refused (%u %r): still %s\n",
			  call->on_hold ? "hold" : "resume", msg->scode,
			  &msg->reason,
			  call->hold_confirmed ? "on hold" : "active");

		call->on_hold = call->hold_confirmed;
		FOREACH_STREAM
			mdt_stream_hold(le->data, call->on_hold);

		/* The streams follow the direction again (a refused hold
		 * had stopped the receiver). */
		(void)mdt_call_update_media(call);
		hold_report(call);
	}

	if (call->modify_deferred && mdt_call_refresh_allowed(call)) {
		call->modify_deferred = false;
		(void)mdt_call_modify(call);
	}
}


/*
 * Returns true, if a hold or resume event is detected.
 *
 * If a hold or resume is detected, *held says which.
 *
 * Detects hold/resume transitions per RFC 6337 section 5.3. Only detect
 * transitions in established calls, not during initial call setup.
 *
 * When remote transitions from sendrecv or recvonly to sendonly or inactive,
 * we're placed on hold.
 * When remote transitions from sendonly or inactive to sendrecv or recvonly,
 * we're resumed.
 */
static bool detect_hold_resume(bool *held, struct mdt_call *call,
			       enum sdp_dir old_rdir, bool is_offer)
{
	bool emit_event = false;

	if (!held || !call || call->state != MDT_CALL_ESTABLISHED)
		return false;

	const struct sdp_media *m = mdt_stream_sdpmedia(mdt_audio_strm(call->audio));
	const enum sdp_dir new_rdir = sdp_media_rdir(m);

	bool before_on_hold = (old_rdir == SDP_RECVONLY ||
			       old_rdir == SDP_INACTIVE);
	bool now_on_hold = (new_rdir == SDP_RECVONLY ||
			    new_rdir == SDP_INACTIVE);

	/* For offers, detect based on rdir changes.
	 * For answers, use the tracked remote_hold state because rdir
	 * may have changed when we created an offer. */
	if (is_offer) {
		if (now_on_hold && !before_on_hold) {
			*held = true;
			call->remote_hold = true;
			emit_event = true;
		}
		else if (before_on_hold && !now_on_hold) {
			*held = false;
			call->remote_hold = false;
			emit_event = true;
		}
	}
	else if (call->remote_hold && !now_on_hold) {
		/* Only detect resume events for answers, not hold events. */
		*held = false;
		call->remote_hold = false;
		emit_event = true;
	}

	return emit_event;
}


static int sipsess_offer_handler(struct mbuf **descp,
				 const struct sip_msg *msg, void *arg)
{
	const bool got_offer = (0 != mbuf_get_left(msg->mb));
	struct mdt_call *call = arg;
	enum sdp_dir ardir;
	int err;

	MDT_MAGIC_CHECK(call);

	if (got_offer) {
		bool held;
		const struct sdp_media *m =
			mdt_stream_sdpmedia(mdt_audio_strm(call->audio));
		const enum sdp_dir old_rdir = sdp_media_rdir(m);
		call->got_offer = true;

		/* Decode SDP Offer */
		err = sdp_decode(call->sdp, msg->mb, true);
		if (err) {
			MDT_WARN("call: reinvite: could not decode SDP offer:"
				" %m\n", err);
			return err;
		}

		if (detect_hold_resume(&held, call, old_rdir, true))
			hold_report(call);

		err = update_media(call);
		if (err) {
			MDT_WARN("call: reinvite - update media failed/rejected"
				" (%m)\n", err);
			return err;
		}
	}

	ardir = sdp_media_rdir(mdt_stream_sdpmedia(mdt_audio_strm(mdt_call_audio(call))));

	MDT_INFO("call: got %r%s audio: %s\n", &msg->met,
	     got_offer ? " (SDP Offer)" : "",
	     sdp_dir_name(ardir));

	/* Encode SDP Answer */
	err = sdp_encode(descp, call->sdp, !got_offer);
	if (err)
		return err;

	bsdk_ev_call_sdp(bsdk_call_find(call), false, got_offer);
	return 0;
}


static int sipsess_answer_handler(const struct sip_msg *msg, void *arg)
{
	struct mdt_call *call = arg;
	int err;
	bool held;
	const struct sdp_media *m = mdt_stream_sdpmedia(mdt_audio_strm(call->audio));
	const enum sdp_dir old_rdir = sdp_media_rdir(m);

	MDT_MAGIC_CHECK(call);

	MDT_DBG("call: got SDP answer (%zu bytes)\n", mbuf_get_left(msg->mb));

	call->got_offer = false;
	if (!pl_strcmp(&msg->cseq.met, "INVITE") &&
	    msg->scode >= 200 && msg->scode < 300 &&
	    mdt_call_state(call) != MDT_CALL_ESTABLISHED)
		call_event_handler(call, MDT_CALL_EVENT_ANSWERED, "%s",
                                   call->peer_uri);

	if (msg_ctype_cmp(&msg->ctyp, "multipart", "mixed"))
		(void)sdp_decode_multipart(&msg->ctyp.params, msg->mb);

	err = sdp_decode(call->sdp, msg->mb, false);
	if (err) {
		MDT_WARN("call: could not decode SDP answer: %m\n", err);
		return err;
	}

	if (detect_hold_resume(&held, call, old_rdir, false))
		hold_report(call);

	err = update_media(call);
	if (err)
		return err;

	return 0;
}


static void set_established_mdir(void *arg)
{
	struct mdt_call *call = arg;
	if (!call)
		return;
	MDT_MAGIC_CHECK(call);

	if (call_need_modify(call)) {
		mdt_call_set_mdir(call, call->estadir);
		mdt_call_modify(call);
	}
}


static uint32_t randwait(uint32_t minwait, uint32_t maxwait)
{

	return minwait + rand_u16() % (maxwait - minwait);
}



static void sipsess_estab_handler(const struct sip_msg *msg, void *arg)
{
	struct mdt_call *call = arg;
	uint32_t wait;
	(void)msg;

	MDT_MAGIC_CHECK(call);

	if (call->state == MDT_CALL_ESTABLISHED)
		return;

	set_state(call, MDT_CALL_ESTABLISHED);

	if (call->got_offer)
		(void)update_streams(call);

	call_timer_start(call);

	if (call->rtp_timeout_ms) {

		struct le *le;

		FOREACH_STREAM {
			struct mdt_stream *strm = le->data;
			mdt_stream_enable_rtp_timeout(strm, call->rtp_timeout_ms);
		}
	}

	/* the transferor will hangup this call */
	if (call->not) {
		(void)mdt_call_sipfrag(call, 200, "OK");
	}

	wait = mdt_call_is_outgoing(call) ? 150 : 0;
	wait += randwait(50, 150);

	/* modify call after mdt_call_event_established handlers are executed */
	tmr_start(&call->tmr_reinv, wait, set_established_mdir, call);

	/* must be done last, the handler might deref this call */
	call_event_handler(call, MDT_CALL_EVENT_ESTABLISHED, "%s", call->peer_uri);
}


static void dtmfend_handler(void *arg)
{
	struct mdt_call *call = arg;

	if (call->dtmfh)
		call->dtmfh(call, KEYCODE_REL, call->arg);
}


static void sipsess_send_info_handler(int err, const struct sip_msg *msg,
				void *arg)
{
	(void)arg;

	if (err)
		MDT_WARN("call: sending DTMF INFO failed (%m)", err);
	else if (msg && msg->scode != 200)
		MDT_WARN("call: sending DTMF INFO failed (scode: %d)",
				msg->scode);
}


static void sipsess_info_handler(struct sip *sip, const struct sip_msg *msg,
				 void *arg)
{
	struct mdt_call *call = arg;

	if (msg_ctype_cmp(&msg->ctyp, "application", "dtmf-relay")) {

		struct pl body, sig, dur;
		int err;

		pl_set_mbuf(&body, msg->mb);

		err  = re_regex(body.p, body.l,
		       "Signal=[ ]*[0-9*#a-d]+", NULL, &sig);
		err |= re_regex(body.p, body.l,
		       "Duration=[ ]*[0-9]+", NULL, &dur);

		if (err || !pl_isset(&sig) || sig.l == 0) {
			(void)sip_reply(sip, msg, 400, "Bad Request");
		}
		else {
			char s = toupper(sig.p[0]);
			uint32_t duration = pl_u32(&dur);

			MDT_INFO("call: received SIP INFO DTMF: '%c' "
			     "(duration=%r)\n", s, &dur);

			(void)sip_reply(sip, msg, 200, "OK");

			if (call->dtmfh) {
				tmr_start(&call->tmr_dtmf, duration,
					  dtmfend_handler, call);
				call->dtmfh(call, s, call->arg);
			}
		}
	}
	else if (!mbuf_get_left(msg->mb)) {
		(void)sip_reply(sip, msg, 200, "OK");
	}
	else {
		(void)sip_reply(sip, msg, 488, "Not Acceptable Here");
	}
}


static void sipnot_close_handler(int err, const struct sip_msg *msg,
				 void *arg)
{
	struct mdt_call *call = arg;

	call->not = mem_deref(call->not);

	if (err)
		call_event_handler(call, MDT_CALL_EVENT_TRANSFER_FAILED,
				   "%m", err);
	else if (msg && msg->scode >= 300)
		call_event_handler(call, MDT_CALL_EVENT_TRANSFER_FAILED,
				   "%u %r", msg->scode, &msg->reason);
}


static void sipsess_refer_handler(struct sip *sip, const struct sip_msg *msg,
				  void *arg)
{
	struct mdt_call *call = arg;
	const struct sip_hdr *hdr;
	int err;

	/* get the transfer target */
	hdr = sip_msg_hdr(msg, SIP_HDR_REFER_TO);
	if (!hdr) {
		MDT_WARN("call: bad REFER request from %r\n", &msg->from.auri);
		(void)sip_reply(sip, msg, 400, "Missing Refer-To header");
		return;
	}

	/* The REFER creates an implicit subscription.
	 * Reply 202 to the REFER request
	 */
	call->not = mem_deref(call->not);
	err = sipevent_accept(&call->not, g_sip.evsock, msg,
			      sipsess_dialog(call->sess), NULL,
			      202, "Accepted", 60, 60, 60,
			      mdt_ua_cuser(call->ua), "message/sipfrag",
			      mdt_ua_auth, call->ua, true,
			      sipnot_close_handler, call,
			      "Allow: %H\r\n", mdt_ua_print_allowed, call->ua);
	if (err) {
		MDT_WARN("call: refer: sipevent_accept failed: %m\n", err);
		return;
	}

	(void)mdt_call_sipfrag(call, 100, "Trying");

	set_state(call, MDT_CALL_TRANSFER);
	call_event_handler(call, MDT_CALL_EVENT_TRANSFER, "%r", &hdr->val);
}


static void xfer_cleanup(struct mdt_call *call, const char *reason)
{
	if (call->xcall->state == MDT_CALL_TRANSFER) {
		set_state(call->xcall, MDT_CALL_ESTABLISHED);
		call_event_handler(call->xcall, MDT_CALL_EVENT_TRANSFER_FAILED,
                                   "%s", reason);
	}

	call->xcall->xcall = NULL;
}


static void sipsess_close_handler(int err, const struct sip_msg *msg,
				  void *arg)
{
	struct mdt_call *call = arg;
	char reason[128] = "";
	const struct sip_hdr *reason_hdr = NULL;

	MDT_MAGIC_CHECK(call);

	/* libre ends a session the caller CANCELled with ECONNRESET and the
	 * CANCEL request — the same error a dead socket gives, told apart
	 * only by that request. */
	if (err == ECONNRESET && msg && msg->req &&
	    !pl_strcmp(&msg->met, "CANCEL")) {
		const struct sip_hdr *rh = sip_msg_hdr(msg, SIP_HDR_REASON);

		MDT_INFO("%s: cancelled by the caller\n", call->peer_uri);
		call->peer_cancel = true;

		if (call->xcall)
			xfer_cleanup(call, "Cancelled");

		call_stream_stop(call);

		/* The text of the CANCEL's Reason (RFC 3326), e.g. "Call
		 * completed elsewhere", is what the app can show; a Reason
		 * without one is kept for the record. */
		if (rh) {
			struct pl text;

			if (0 == msg_param_decode(&rh->val, "text", &text)) {
				if (text.l >= 2 && text.p[0] == '"' &&
				    text.p[text.l - 1] == '"') {
					++text.p;
					text.l -= 2;
				}
				call_event_handler(call,
						MDT_CALL_EVENT_CLOSED,
						"%r", &text);
			}
			else {
				call_event_handler(call,
						MDT_CALL_EVENT_CLOSED,
						"Cancelled by caller (%r)",
						&rh->val);
			}
		}
		else {
			call_event_handler(call, MDT_CALL_EVENT_CLOSED,
						"Cancelled by caller");
		}
		return;
	}

	if (err) {
		MDT_INFO("%s: session closed: %m\n", call->peer_uri, err);

		(void)re_snprintf(reason, sizeof(reason), "%m", err);

		if (call->not) {
			(void)mdt_call_sipfrag(call, 500, "%m", err);
		}
	}
	else if (msg) {

		call->scode = msg->scode;

		(void)re_snprintf(reason, sizeof(reason), "%u %r",
				  msg->scode, &msg->reason);

		MDT_INFO("%s: session closed: %u %r\n",
		     call->peer_uri, msg->scode, &msg->reason);

		if (call->not) {
			(void)mdt_call_sipfrag(call, msg->scode,
						  "%r", &msg->reason);
		}
	}
	else {
		MDT_INFO("%s: session closed\n", call->peer_uri);
	}

	if (call->xcall)
		xfer_cleanup(call, reason);

	call_stream_stop(call);

	if (msg)
		reason_hdr = sip_msg_hdr(msg, SIP_HDR_REASON);

	if (reason_hdr) {
		MDT_INFO("Cancel reason: %r\n", &reason_hdr->val);
		call_event_handler(call, MDT_CALL_EVENT_CLOSED, "%s,%r",
						   reason, &reason_hdr->val);
	}
	else {
		call_event_handler(call, MDT_CALL_EVENT_CLOSED, "%s", reason);
	}
}


static void prack_handler(const struct sip_msg *msg, void *arg)
{
	struct mdt_call *call = arg;

	if (!msg || !call)
		return;

	if (call->progr_queued && !call->answered)
		(void)mdt_call_progress_dir(call, call->estadir);

	return;
}


static bool have_common_audio_codecs(const struct mdt_call *call)
{
	const struct sdp_format *sc;
	struct mdt_aucodec *ac;

	sc = sdp_media_rcodec(mdt_stream_sdpmedia(mdt_audio_strm(call->audio)));
	if (!sc)
		return false;

	ac = sc->data;  /* note: this will exclude telephone-event */

	return ac != NULL;
}



static bool valid_addressfamily(struct mdt_call *call, const struct mdt_stream *strm)
{
	struct sdp_media *m;
	const struct sa *raddr;
	m = mdt_stream_sdpmedia(strm);
	raddr = sdp_media_raddr(m);

	if (sa_isset(raddr, SA_ADDR) &&  sa_af(raddr) != call->af) {
		MDT_INFO("call: incompatible address-family for %s"
				" (local=%s, remote=%s)\n",
				sdp_media_name(m),
				net_af2name(call->af),
				net_af2name(sa_af(raddr)));

		return false;
	}

	return true;
}


/**
 * Find a call given the value of a Replaces header
 *
 * @param calls   List of calls
 * @param hdr     Value of Replaces header
 *
 * @return Call object if found, NULL if not found
 */
static struct mdt_call *call_find_replaces(const struct list *calls,
				       const struct pl *hdr)
{
	struct le *le;
	struct sip_msg msg = { 0 };
	struct mdt_call *ret = NULL;

	if (!calls || !pl_isset(hdr))
		return NULL;

	msg.req = true;
	msg.callid.p = hdr->p;
	msg.callid.l = hdr->l;

	if (pl_strchr(&msg.callid, ';')) {
		msg.callid.l = (size_t)(pl_strchr(&msg.callid, ';')
				- msg.callid.p);
	}

	if (!pl_isset(&msg.callid))
		return NULL;

	(void)re_regex(hdr->p, hdr->l, ";to-tag=[^; ]+", &msg.to.tag);
	(void)re_regex(hdr->p, hdr->l, ";from-tag=[^; ]+", &msg.from.tag);

	for (le = list_head(calls); le; le = le->next) {
		struct mdt_call *call = le->data;

		if (pl_isset(&msg.from.tag) && pl_isset(&msg.to.tag) &&
		    sip_dialog_cmp(sipsess_dialog(call->sess), &msg)) {
			ret = call;
			break;
		}
		else if (pl_isset(&msg.to.tag) &&
			 sip_dialog_cmp_half(sipsess_dialog(call->sess),
					     &msg)) {
			ret = call;
			break;
		}
		else if (0 == pl_strcmp(&msg.callid, call->id)) {
			ret = call;
			break;
		}
	}

	return ret;
}


int mdt_call_sess_accept(struct mdt_call *call, struct sipsess_sock *sess_sock,
		const struct sip_msg *msg)
{
	const struct sip_hdr *hdr;
	int err;

	if (!call || !msg)
		return EINVAL;

	call->outgoing = false;
	if (pl_isset(&msg->from.dname)) {
		err = pl_strdup(&call->peer_name, &msg->from.dname);
		if (err)
			return err;
	}

	err = mdt_call_streams_alloc(call);
	if (err)
		return err;

	if (call->got_offer) {

		err = sdp_decode(call->sdp, msg->mb, true);
		if (err)
			return err;

		/*
		 * Each media description in the SDP answer MUST
		 * use the same network type as the corresponding
		 * media description in the offer.
		 *
		 * See RFC 6157
		 */
		if (!valid_addressfamily(call, mdt_audio_strm(call->audio))) {
			sip_treply(NULL, g_sip.sip, msg, 488,
				   "Not Acceptable Here");

			call_event_handler(call, MDT_CALL_EVENT_CLOSED,
					   "Wrong address family");
			return 0;
		}

		/* Check if we have any common audio codecs, after the SDP
		 * offer has been parsed
		 */
		if (!have_common_audio_codecs(call)) {
			MDT_INFO("call: no common audio codecs - rejected\n");

			sip_treply(NULL, g_sip.sip, msg,
				   488, "Not Acceptable Here");

			call_event_handler(call, MDT_CALL_EVENT_CLOSED,
					   "No common audio codecs");

			return 0;
		}

		bsdk_ev_call_sdp(bsdk_call_find(call), true, false);
	}

	hdr = sip_msg_hdr(msg, SIP_HDR_REPLACES);
	if (hdr && pl_isset(&hdr->val)) {
		struct mdt_call *rcall = call_find_replaces(&call->ua->calls,
							&hdr->val);
		if (!rcall) {
			MDT_INFO("call: Replaces header present, but could not "
				"find matching call %r\n", &hdr->val);

			sip_treply(NULL, g_sip.sip, msg,
				   481, "Call/Transaction Does Not Exist");

			call_event_handler(call, MDT_CALL_EVENT_CLOSED,
				"Replaces header without matching dialog.");

			return 0;
		}
		else {
			call_stream_stop(rcall);
			call_event_handler(rcall, MDT_CALL_EVENT_CLOSED,
				"%r replaced", &hdr->val);
		}
	}

	err = sipsess_accept(&call->sess, sess_sock, msg, 180, "Ringing",
			     call->ua->rel100,
			     mdt_ua_cuser(call->ua), "application/sdp", NULL,
			     mdt_ua_auth, call->ua, true,
			     sipsess_offer_handler, sipsess_answer_handler,
			     sipsess_estab_handler, sipsess_info_handler,
			     sipsess_refer_handler,
			     sipsess_close_handler,
			     call, "Allow: %H\r\n%H",
			     mdt_ua_print_allowed, call->ua,
			     mdt_ua_print_require, call->ua);

	if (err) {
		MDT_WARN("call: sipsess_accept: %m\n", err);
		return err;
	}

	err = str_dup(&call->id,
		      sip_dialog_callid(sipsess_dialog(call->sess)));
	if (err)
		return err;

	set_state(call, MDT_CALL_INCOMING);

	err = sipsess_set_prack_handler(call->sess, prack_handler);
	if (err)
		return err;

	err = sipsess_set_modify_handler(call->sess, modify_handler);
	if (err)
		return err;

	/* New call */
	if (call->cfg_call.local_timeout) {
		tmr_start(&call->tmr_inv, call->cfg_call.local_timeout*1000,
			  invite_timeout, call);
	}

	call->msg_src = msg->src;

	call->estadir = mdt_stream_ldir(mdt_audio_strm(mdt_call_audio(call)));

	/* Ring now, whether or not a media NAT is still gathering: the
	 * caller already has our 180, and ICE only has to be ready for the
	 * answer (mdt_call_respond waits for it).  Ringing once it was ready
	 * left the phone silent for as long as gathering took — 1.7 s on a
	 * slow DNS answer for the STUN server (2026-11-04). */
	call_event_handler(call, MDT_CALL_EVENT_INCOMING, "%s",
			   call->peer_uri);

	return 0;
}


bool mdt_call_sess_cmp(const struct mdt_call *call, const struct sip_msg *msg)
{
	if (!call || !msg)
		return false;

	return sipsess_msg(call->sess) == msg;
}


static void sipsess_progr_handler(const struct sip_msg *msg, void *arg)
{
	struct mdt_call *call = arg;
	bool media;

	MDT_MAGIC_CHECK(call);

	MDT_INFO("call: SIP Progress: %u %r (%r/%r)\n",
	     msg->scode, &msg->reason, &msg->ctyp.type, &msg->ctyp.subtype);

	call->msg_src = msg->src;

	if (msg->scode <= 100)
		return;

	/* check for 18x and content-type
	 *
	 * 1. start media-stream if application/sdp
	 * 2. play local ringback tone if not
	 *
	 * we must also handle changes to/from 180 and 183,
	 * so we reset the media-stream/ringback each time.
	 */
	if (msg_ctype_cmp(&msg->ctyp, "application", "sdp")
	    && mbuf_get_left(msg->mb)
	    && !sdp_decode(call->sdp, msg->mb, false)) {
		media = true;
	}
	else if (msg_ctype_cmp(&msg->ctyp, "multipart", "mixed") &&
		 !sdp_decode_multipart(&msg->ctyp.params, msg->mb) &&
		 !sdp_decode(call->sdp, msg->mb, false)) {
		media = true;
	}
	else
		media = false;

	switch (msg->scode) {

	case 180:
		set_state(call, MDT_CALL_RINGING);
		break;

	case 183:
		set_state(call, MDT_CALL_EARLY);

		break;
	}

	if (media) {
		mem_ref(call);
		call_event_handler(call, MDT_CALL_EVENT_PROGRESS, "%s",
                                   call->peer_uri);
		mem_deref(call);
	}
	else {
		call_stream_stop(call);
		call_event_handler(call, MDT_CALL_EVENT_RINGING, "%s",
                                   call->peer_uri);
	}
}


static void redirect_handler(const struct sip_msg *msg, const char *uri,
	void *arg)
{
	struct mdt_call *call = arg;

	(void)call;
	MDT_INFO("call: redirect (%u) to %s\n", msg->scode, uri);
}


static int sipsess_desc_handler(struct mbuf **descp, const struct sa *src,
				const struct sa *dst, void *arg)
{
	struct mdt_call *call = arg;
	int err;
	(void) dst;

	MDT_MAGIC_CHECK(call);
	call->af     = sa_af(src);
	if (!call->ua->mnat)
		sdp_session_set_laddr(call->sdp, src);

	if (list_isempty(&call->streaml)) {
		err = mdt_call_streams_alloc(call);
		if (err)
			return err;

		mdt_call_set_mdir(call, call->estadir);
		bsdk_ev_call_sdp(bsdk_call_find(call), false, false);
	}

	err = mdt_call_sdp_get(call, descp, true);
	if (err)
		return err;
#if 0
	MDT_INFO("- - - - - S D P - O f f e r - - - - -\n"
	     "%b"
	     "- - - - - - - - - - - - - - - - - - -\n",
	     (*descp)->buf, (*descp)->end);
#endif

	return 0;
}


static int call_print_replaces(struct re_printf *pf, const struct mdt_call *call) {
	int err = 0;

	if (!call || !call->replaces)
		return 0;

	err = re_hprintf(pf, "Replaces: %s\r\n", call->replaces);

	return err;
}


static int send_invite(struct mdt_call *call)
{
	const char *routev[1];
	int err;

	routev[0] = call->ua->outbound;

	if (!list_isempty(&call->streaml))
		bsdk_ev_call_sdp(bsdk_call_find(call), false, false);

	err = sipsess_connect(&call->sess, g_sip.sock,
			      call->peer_uri,
			      call->local_name,
			      call->local_uri,
			      mdt_ua_cuser(call->ua),
			      routev[0] ? routev : NULL,
			      routev[0] ? 1 : 0,
			      "application/sdp",
			      mdt_ua_auth, call->ua, true,
			      call->id,
			      sipsess_desc_handler,
			      sipsess_offer_handler, sipsess_answer_handler,
			      sipsess_progr_handler, sipsess_estab_handler,
			      sipsess_info_handler,
			      sipsess_refer_handler,
			      sipsess_close_handler, call,
			      "Allow: %H\r\n%H%H%H%H",
			      mdt_ua_print_allowed, call->ua,
			      mdt_ua_print_supported, call->ua,
			      mdt_ua_print_require, call->ua,
			      call_print_replaces, call,
			      mdt_hdrs_print, &call->custom_hdrs);
	if (err) {
		MDT_WARN("call: sipsess_connect: %m\n", err);
		return err;
	}

	err = sipsess_set_redirect_handler(call->sess, redirect_handler);
	if (err)
		return err;

	err = sipsess_set_prack_handler(call->sess, prack_handler);
	if (err)
		return err;

	err = sipsess_set_modify_handler(call->sess, modify_handler);
	if (err)
		return err;

	/* save call setup timer */
	call->time_conn = time(NULL);

	return 0;
}


static int send_dtmf_info(struct mdt_call *call, char key)
{
	struct mbuf *body;
	int err;

	if ((key < '0' || key > '9') &&
	    (key < 'a' || key > 'd') &&
	    (key < 'A' || key > 'D') &&
	    (key != '*') &&
	    (key != '#'))
		return EINVAL;

	body = mbuf_alloc(25);
	mbuf_printf(body, "Signal=%c\r\nDuration=250\r\n", key);
	mbuf_set_pos(body, 0);

	err = sipsess_info(call->sess, "application/dtmf-relay", body,
			   sipsess_send_info_handler, call);
	if (err) {
		MDT_WARN("call: sipsess_info for DTMF failed (%m)\n", err);
		goto out;
	}

 out:
	mem_deref(body);

	return err;
}



/**
 * Get the current call duration in seconds
 *
 * @param call  Call object
 *
 * @return Duration in seconds
 */
uint32_t mdt_call_duration(const struct mdt_call *call)
{
	if (!call || !call->time_start)
		return 0;

	return (uint32_t)(time(NULL) - call->time_start);
}


/**
 * Get the current call setup time in seconds
 *
 * @param call  Call object
 *
 * @return Call setup in seconds
 */
uint32_t mdt_call_setup_duration(const struct mdt_call *call)
{
	if (!call || !call->time_conn || call->time_conn <= 0 )
		return 0;

	return (uint32_t)(call->time_start - call->time_conn);
}


/**
 * Get the audio object for the current call
 *
 * @param call  Call object
 *
 * @return Audio object
 */
struct mdt_audio *mdt_call_audio(const struct mdt_call *call)
{
	return call ? call->audio : NULL;
}



/**
 * Get the list of media streams for the current call
 *
 * @param call  Call object
 *
 * @return List of media streams
 */
struct list *mdt_call_streaml(const struct mdt_call *call)
{
	return call ? (struct list *)&call->streaml : NULL;
}


int mdt_call_reset_transp(struct mdt_call *call, const struct sa *laddr)
{
	if (!call)
		return EINVAL;

	sdp_session_set_laddr(call->sdp, laddr);

	return mdt_call_modify(call);
}


const struct sa *mdt_call_laddr(const struct mdt_call *call)
{
	if (!call)
		return NULL;

	return sdp_session_laddr(call->sdp);
}


/**
 * Send a SIP NOTIFY with a SIP message fragment
 *
 * @param call   Call object
 * @param scode  SIP Status code
 * @param reason Formatted SIP Reason phrase
 * @param ...    Variable arguments
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_sipfrag(struct mdt_call *call, uint16_t scode,
			const char *reason, ...)
{
	struct mbuf *mb;
	va_list ap;
	int err;

	if (!call)
		return EINVAL;

	mb = mbuf_alloc(512);
	if (!mb)
		return ENOMEM;

	va_start(ap, reason);
	(void)mbuf_printf(mb, "SIP/2.0 %u %v\r\n", scode, reason, &ap);
	va_end(ap);

	mb->pos = 0;

	if (scode >= 200) {
		err = sipevent_notify(call->not, mb, SIPEVENT_TERMINATED,
				      SIPEVENT_NORESOURCE, 0);

		call->not = mem_deref(call->not);
	}
	else {
		err = sipevent_notify(call->not, mb, SIPEVENT_ACTIVE,
				      SIPEVENT_NORESOURCE, 0);
	}

	mem_deref(mb);

	return err;
}


static void sipsub_notify_handler(struct sip *sip, const struct sip_msg *msg,
				  void *arg)
{
	struct mdt_call *call = arg;
	struct pl scode, reason;
	uint32_t sc;

	if (re_regex((char *)mbuf_buf(msg->mb), mbuf_get_left(msg->mb),
		     "SIP/2.0 [0-9]+ [^\r\n]+", &scode, &reason)) {
		(void)sip_reply(sip, msg, 400, "Bad sipfrag");
		return;
	}

	(void)sip_reply(sip, msg, 200, "OK");

	sc = pl_u32(&scode);

	if (sc >= 300) {
		MDT_INFO("call: transfer failed: %u %r\n", sc, &reason);
		call_event_handler(call, MDT_CALL_EVENT_TRANSFER_FAILED,
				   "%u %r", sc, &reason);
	}
	else if (sc >= 200) {
		call_event_handler(call, MDT_CALL_EVENT_CLOSED, "Call transfered");
	}
}


static void sipsub_close_handler(int err, const struct sip_msg *msg,
				 const struct sipevent_substate *substate,
				 void *arg)
{
	struct mdt_call *call = arg;

	(void)substate;

	call->sub = mem_deref(call->sub);

	if (err) {
		MDT_INFO("call: subscription closed: %m\n", err);
	}
	else if (msg && msg->scode >= 300) {
		MDT_INFO("call: transfer failed: %u %r\n",
		     msg->scode, &msg->reason);
		call_event_handler(call, MDT_CALL_EVENT_TRANSFER_FAILED,
				   "%u %r", msg->scode, &msg->reason);
	}
}


static int normalize_uri(char **out, const char *uri, const struct uri *luri)
{
	struct uri uri2;
	struct pl pl;
	int err;

	if (!out || !uri || !luri)
		return EINVAL;

	pl_set_str(&pl, uri);

	if (0 == uri_decode(&uri2, &pl)) {

		err = str_dup(out, uri);
	}
	else {
		uri2 = *luri;

		uri2.user     = pl;
		uri2.params   = pl_null;

		err = re_sdprintf(out, "%H", uri_encode, &uri2);
	}

	return err;
}


/**
 * Transfer the call to a target SIP uri
 *
 * @param call  Call object
 * @param uri   Target SIP uri
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_transfer(struct mdt_call *call, const char *uri)
{
	char *nuri;
	int err;

	if (!call || !uri)
		return EINVAL;

	err = normalize_uri(&nuri, uri, &call->ua->luri);
	if (err)
		return err;

	MDT_INFO("transferring call to %s\n", nuri);

	call->sub = mem_deref(call->sub);
	err = sipevent_drefer(&call->sub, g_sip.evsock,
			      sipsess_dialog(call->sess), mdt_ua_cuser(call->ua),
			      mdt_ua_auth, call->ua, true,
			      sipsub_notify_handler, sipsub_close_handler,
			      call,
		              "Refer-To: %s\r\nReferred-by: %s\r\n",
			      nuri, call->ua->aor);
	if (err) {
		MDT_WARN("call: sipevent_drefer: %m\n", err);
	}

	mem_deref(nuri);

	return err;
}


/**
 * Transfer the call to a target SIP uri and replace the source call
 *
 * @param call        Call object
 * @param source_call Source call object
 *
 * @return 0 if success, otherwise errorcode
 */
int mdt_call_replace_transfer(struct mdt_call *call, struct mdt_call *source_call)
{
	int err;

	if (!call || !source_call)
		return EINVAL;

	MDT_INFO("transferring call to %s\n", source_call->peer_uri);

	call->sub = mem_deref(call->sub);

	err = sipevent_drefer(&call->sub, g_sip.evsock,
		sipsess_dialog(call->sess), mdt_ua_cuser(call->ua),
		mdt_ua_auth, call->ua, true,
		sipsub_notify_handler, sipsub_close_handler, call,
	"Refer-To: <%s?Replaces=%s%%3Bto-tag%%3D%s%%3Bfrom-tag%%3D%s>\r\n"
		"Referred-By: %s\r\n",
		source_call->peer_uri,
		source_call->id,
		sip_dialog_rtag(sipsess_dialog(source_call->sess)),
		sip_dialog_ltag(sipsess_dialog(source_call->sess)),
		call->ua->aor);

	if (err) {
		MDT_WARN("call: sipevent_drefer: %m\n", err);
	}

	return err;
}


/* Did the caller CANCEL this (incoming) call? */
bool mdt_call_peer_cancelled(const struct mdt_call *call)
{
	return call ? call->peer_cancel : false;
}


/**
 * Get the SIP status code for the outgoing call
 *
 * @param call Call object
 *
 * @return SIP Status code
 */
uint16_t mdt_call_scode(const struct mdt_call *call)
{
	return call ? call->scode : 0;
}


/**
 * Get state of the call
 *
 * @param call Call object
 *
 * @return Call state or MDT_CALL_UNKNOWN if call object is NULL
 */
enum mdt_call_state mdt_call_state(const struct mdt_call *call)
{
	if (!call)
		return MDT_CALL_UNKNOWN;

	return call->state;
}


/**
 * Set the callback handlers for a call object
 *
 * @param call  Call object
 * @param eh    Event handler
 * @param dtmfh DTMF handler
 * @param arg   Handler argument
 */
void mdt_call_set_handlers(struct mdt_call *call, mdt_call_event_h *eh,
		       mdt_call_dtmf_h *dtmfh, void *arg)
{
	if (!call)
		return;

	if (eh)
		call->eh    = eh;

	if (dtmfh)
		call->dtmfh = dtmfh;

	if (arg)
		call->arg   = arg;
}


void mdt_call_set_xrtpstat(struct mdt_call *call)
{
	if (!call)
		return;

	sipsess_set_close_headers(call->sess,
				  "X-RTP-Stat: %H\r\n",
				  mdt_rtpstat_print, call);
}


/**
 * Check if a call is locally on hold
 *
 * @param call Call object
 *
 * @return True if on hold (local), otherwise false
 */
bool mdt_call_is_onhold(const struct mdt_call *call)
{
	return call ? call->on_hold : false;
}


bool mdt_call_is_remote_hold(const struct mdt_call *call)
{
	return call ? call->remote_hold : false;
}


/* The peer's offer is the one being answered (an INVITE with SDP). */
bool mdt_call_got_offer(const struct mdt_call *call)
{
	return call ? call->got_offer : false;
}


/**
 * Check if a call has direction outgoing
 *
 * @param call Call object
 *
 * @return True if outgoing, otherwise false
 */
bool mdt_call_is_outgoing(const struct mdt_call *call)
{
	return call ? call->outgoing : false;
}


/**
 * Enable RTP timeout for a call
 *
 * @param call       Call object
 * @param timeout_ms RTP Timeout in [milliseconds]
 */
void mdt_call_enable_rtp_timeout(struct mdt_call *call, uint32_t timeout_ms)
{
	if (!call)
		return;

	call->rtp_timeout_ms = timeout_ms;
}


/**
 * Get the line number for this call
 *
 * @param call Call object
 *
 * @return Line number from 1 to N
 */
uint32_t mdt_call_linenum(const struct mdt_call *call)
{
	return call ? call->linenum : 0;
}


/**
 * Find the call with a given line number
 *
 * @param calls   List of calls
 * @param linenum Line number from 1 to N
 *
 * @return Call object if found, NULL if not found
 */
struct mdt_call *mdt_call_find_linenum(const struct list *calls, uint32_t linenum)
{
	struct le *le;

	for (le = list_head(calls); le; le = le->next) {
		struct mdt_call *call = le->data;

		if (linenum == call->linenum)
			return call;
	}

	return NULL;
}


/**
 * Set the audio direction of the SDP media line
 *
 * @param call Call object
 * @param a    Audio SDP direction
 */
void mdt_call_set_mdir(struct mdt_call *call, enum sdp_dir a)
{
	if (!call)
		return;

	mdt_stream_set_ldir(mdt_audio_strm(mdt_call_audio(call)), a);
}


/**
 * Get the user data for the call
 *
 * @param call Call object
 *
 * @return Call's user data
 */
const struct pl *mdt_call_user_data(const struct mdt_call *call)
{
	return call ? call->user_data : NULL;
}


/**
 * Set the user data of the call
 *
 * @param call Call object
 * @param user_data User data to be set
 * @return int
 */
int mdt_call_set_user_data(struct mdt_call *call, const struct pl *user_data)
{
	if (!call)
		return EINVAL;

	call->user_data = mem_deref(call->user_data);
	if (!pl_isset(user_data))
		return 0;

	call->user_data = pl_alloc_dup(user_data);
	return call->user_data ? 0 : ENOMEM;
}


/**
 * Get the SIP transport protocol used for this call
 *
 * @param call Call object
 *
 * @return Transport protocol
 */
enum sip_transp mdt_call_transp(const struct mdt_call *call)
{
	return call ? sip_dialog_tp(sipsess_dialog(call->sess))
		: SIP_TRANSP_NONE;
}


/*
 * Get the SDP negotiation state of the call
 *
 * @param call Call object
 *
 * @return SDP negotiation state
 */
enum sdp_neg_state mdt_call_sdp_neg_state(const struct mdt_call *call)
{
	return call ? sipsess_sdp_neg_state(call->sess) : SDP_NEG_NONE;
}
