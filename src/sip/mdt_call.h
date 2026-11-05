/**
 * @file mdt_call.h  Private header of the Medut call layer (src/sip/)
 *
 * One SIP call: its sipsess, SDP offer/answer, media streams, transfer
 * subscription and the events it raises.  sip/call.c is the call itself;
 * sip/call_api.c is what core/mdt.h offers the SDK on top of it.
 *
 * Adapted from baresip's include/baresip.h and src/core.h (BSD-3-Clause,
 * Copyright (C) 2010 Alfred E. Heggestad); see THIRD_PARTY_NOTICES.
 */

#ifndef MDT_CALL_H
#define MDT_CALL_H

#include "mdt_sip.h"
#include "../media/mdt_media.h"

enum {
	CALL_LINENUM_MIN = 1,
	CALL_LINENUM_MAX = 256
};

/* What a call reports to its owner (call_api.c), through mdt_call_event_h. */
enum mdt_call_event {
	MDT_CALL_EVENT_INCOMING,
	MDT_CALL_EVENT_OUTGOING,
	MDT_CALL_EVENT_RINGING,
	MDT_CALL_EVENT_PROGRESS,
	MDT_CALL_EVENT_ANSWERED,
	MDT_CALL_EVENT_ESTABLISHED,
	MDT_CALL_EVENT_CLOSED,
	MDT_CALL_EVENT_TRANSFER,
	MDT_CALL_EVENT_TRANSFER_FAILED,
	MDT_CALL_EVENT_MENC,
};

struct mdt_call_prm {
	struct sa laddr;          /* local address of the SDP      */
	int af;                   /* address family of the media   */
	bool use_rtp;
};

typedef void (mdt_call_event_h)(struct mdt_call *call,
				 enum mdt_call_event ev,
				 const char *str, void *arg);
typedef void (mdt_call_dtmf_h)(struct mdt_call *call, char key, void *arg);

bool mdt_call_peer_cancelled(const struct mdt_call *call);

/* media/rtpstat.c: the X-RTP-Stat value of the BYE */
int mdt_rtpstat_print(struct re_printf *pf, const struct mdt_call *call);

/* sip/call.c */
int mdt_call_update_media(struct mdt_call *call);
int mdt_call_streams_alloc(struct mdt_call *call);
int mdt_call_alloc(struct mdt_call **callp, const struct mdt_config *cfg, struct list *lst,
	       const char *local_name, const char *local_uri,
	       struct mdt_ua *ua, const struct mdt_call_prm *prm,
	       const struct sip_msg *msg, struct mdt_call *xcall,
	       struct dnsc *dnsc,
	       mdt_call_event_h *eh, void *arg);
void mdt_call_set_custom_hdrs(struct mdt_call *call, const struct list *hdrs);
int mdt_call_invite(struct mdt_call *call, const struct pl *paddr);
int mdt_call_modify(struct mdt_call *call);
void mdt_call_endf(struct mdt_call *call, uint16_t scode, const char *reason,
		 const char *fmt, ...);
void mdt_call_end(struct mdt_call *call, uint16_t scode, const char *reason);
int mdt_call_progress_dir(struct mdt_call *call, enum sdp_dir adir);
int mdt_call_respond(struct mdt_call *call, uint16_t scode);
int mdt_call_hold(struct mdt_call *call, bool hold);
int mdt_call_sdp_get(const struct mdt_call *call, struct mbuf **descp, bool offer);
bool mdt_call_refresh_allowed(const struct mdt_call *call);
bool mdt_call_reinvite_abandon(struct mdt_call *call);
const char *mdt_call_id(const struct mdt_call *call);
const char *mdt_call_peeruri(const struct mdt_call *call);
const char *mdt_call_contacturi(const struct mdt_call *call);
const char *mdt_call_localuri(const struct mdt_call *call);
const char *mdt_call_peername(const struct mdt_call *call);
const char *mdt_call_diverteruri(const struct mdt_call *call);
int mdt_call_send_digit(struct mdt_call *call, char key);
struct mdt_ua *mdt_call_get_ua(const struct mdt_call *call);
int mdt_call_sess_accept(struct mdt_call *call, struct sipsess_sock *sess_sock,
		const struct sip_msg *msg);
bool mdt_call_sess_cmp(const struct mdt_call *call, const struct sip_msg *msg);
uint32_t mdt_call_duration(const struct mdt_call *call);
uint32_t mdt_call_setup_duration(const struct mdt_call *call);
struct mdt_audio *mdt_call_audio(const struct mdt_call *call);
struct list *mdt_call_streaml(const struct mdt_call *call);
int mdt_call_reset_transp(struct mdt_call *call, const struct sa *laddr);
const struct sa *mdt_call_laddr(const struct mdt_call *call);
int mdt_call_sipfrag(struct mdt_call *call, uint16_t scode,
			const char *reason, ...);
int mdt_call_transfer(struct mdt_call *call, const char *uri);
int mdt_call_replace_transfer(struct mdt_call *call, struct mdt_call *source_call);
bool mdt_call_peer_cancelled(const struct mdt_call *call);
uint16_t mdt_call_scode(const struct mdt_call *call);
enum mdt_call_state mdt_call_state(const struct mdt_call *call);
void mdt_call_set_handlers(struct mdt_call *call, mdt_call_event_h *eh,
		       mdt_call_dtmf_h *dtmfh, void *arg);
void mdt_call_set_xrtpstat(struct mdt_call *call);
bool mdt_call_is_onhold(const struct mdt_call *call);
bool mdt_call_is_remote_hold(const struct mdt_call *call);
bool mdt_call_got_offer(const struct mdt_call *call);
bool mdt_call_is_outgoing(const struct mdt_call *call);
void mdt_call_enable_rtp_timeout(struct mdt_call *call, uint32_t timeout_ms);
uint32_t mdt_call_linenum(const struct mdt_call *call);
struct mdt_call *mdt_call_find_linenum(const struct list *calls, uint32_t linenum);
void mdt_call_set_mdir(struct mdt_call *call, enum sdp_dir a);
const struct pl *mdt_call_user_data(const struct mdt_call *call);
int mdt_call_set_user_data(struct mdt_call *call, const struct pl *user_data);
enum sip_transp mdt_call_transp(const struct mdt_call *call);
enum sdp_neg_state mdt_call_sdp_neg_state(const struct mdt_call *call);

#endif /* MDT_CALL_H */
