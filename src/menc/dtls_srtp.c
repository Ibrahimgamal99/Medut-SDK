/**
 * @file dtls_srtp.c DTLS-SRTP media encryption
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/dtls_srtp/dtls_srtp.c (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */

#include <re.h>
#include <string.h>
#include "../media/mdt_media.h"
#include "dtls_srtp.h"


/**
 * @defgroup dtls_srtp dtls_srtp
 *
 * DTLS-SRTP media encryption module
 *
 * This module implements end-to-end media encryption using DTLS-SRTP
 * which is now mandatory for WebRTC endpoints.
 *
 * An account uses it with MEDUT_MEDIA_ENC_DTLS_SRTP.
 *
 *
 * Internally the protocol stack diagram looks something like this:
 *
 \verbatim
 *                    application
 *                        |
 *                        |
 *            [DTLS]   [SRTP]
 *                \      /
 *                 \    /
 *                  \  /
 *                   \/
 *              ( TURN/ICE )
 *                   |
 *                   |
 *                [socket]
 \endverbatim
 *
 */

struct mdt_menc_sess {
	struct sdp_session *sdp;
	bool offerer;
	mdt_menc_event_h *eventh;
	mdt_menc_error_h *errorh;
	void *arg;
};

/* media */
/* bound on one DTLS handshake, and how many times it may be
 * restarted before we accept that the peer is not going to answer.
 *
 * The first bound is much shorter than the rest, because the first thing that
 * goes wrong is nearly always a single dropped ClientHello.  We send it the
 * moment ICE nominates a pair, which is the moment *our* check succeeded — the
 * peer may not have nominated yet, and until it does it will not accept a
 * packet from us.  OpenSSL then retransmits on its own schedule, 1 s and 2 s,
 * so one drop costs 3 s of dead air on an answered call.  Measured on-device
 * (2026-08-31, inbound over WSS on cellular): 3.11 s from ClientHello to
 * `established`, against 45 ms on a path where nothing was dropped.
 *
 * 1200 ms is chosen to sit above any plausible RTT-plus-processing on a
 * cellular path — so a handshake that is merely slow is left alone — and below
 * the point where OpenSSL's own backoff has cost more than a fresh association
 * would.  Later attempts back off: by then the evidence points at a path that
 * is not going to carry DTLS at all, and hammering it buys nothing. */
#define MEDUT_DTLS_HANDSHAKE_MS       1200
#define MEDUT_DTLS_HANDSHAKE_LATER_MS 2500
#define MEDUT_DTLS_MAX_RESTARTS          4

struct dtls_srtp {
	struct comp compv[2];
	struct tmr tmr_hs;      /* handshake deadline */
	struct sa raddr_rtp;    /* kept for a restart */
	struct sa raddr_rtcp;   /* kept for a restart */
	unsigned hs_restarts;   /* restarts spent */
	const struct mdt_menc_sess *sess;
	struct sdp_media *sdpm;
	const struct mdt_stream *strm;   /**< pointer to parent */
	bool started;
	bool active;
	bool mux;
};

/* RFC 4145 */
enum setup {
	SETUP_ACTIVE = 0,
	SETUP_PASSIVE,
	SETUP_ACTPASS,
};

static struct tls *tls;
static const char* srtp_profiles =
	"SRTP_AES128_CM_SHA1_80:"
	"SRTP_AES128_CM_SHA1_32:"
	"SRTP_AEAD_AES_128_GCM:"
	"SRTP_AEAD_AES_256_GCM";


static enum setup setup_decode(const char *setup)
{
	if (0 == str_casecmp(setup, "active")) return SETUP_ACTIVE;
	if (0 == str_casecmp(setup, "passive")) return SETUP_PASSIVE;
	if (0 == str_casecmp(setup, "actpass")) return SETUP_ACTPASS;

	return (enum setup)-1;
}


static void sess_destructor(void *arg)
{
	struct mdt_menc_sess *sess = arg;

	mem_deref(sess->sdp);
}


static void destructor(void *arg)
{
	struct dtls_srtp *st = arg;
	size_t i;

	tmr_cancel(&st->tmr_hs);  /* it closes over `st` */

	for (i=0; i<2; i++) {
		struct comp *c = &st->compv[i];

		mem_deref(c->uh_srtp);
		mem_deref(c->tls_conn);
		mem_deref(c->dtls_sock);
		mem_deref(c->app_sock);  /* must be freed last */
		mem_deref(c->tx);
		mem_deref(c->rx);
	}

	mem_deref(st->sdpm);
}


static bool verify_fingerprint(const struct sdp_session *sess,
			       const struct sdp_media *media,
			       struct tls_conn *tc)
{
	struct pl hash;
	uint8_t md_sdp[32], md_dtls[32];
	size_t sz_sdp = sizeof(md_sdp);
	size_t sz_dtls;
	enum tls_fingerprint type;
	int err;

	if (sdp_fingerprint_decode(sdp_media_session_rattr(media, sess,
							   "fingerprint"),
				   &hash, md_sdp, &sz_sdp))
		return false;

	if (0 == pl_strcasecmp(&hash, "sha-256")) {
		type = TLS_FINGERPRINT_SHA256;
		sz_dtls = 32;
	}
	else {
		MDT_WARN("dtls_srtp: unknown fingerprint '%r'\n", &hash);
		return false;
	}

	err = tls_peer_fingerprint(tc, type, md_dtls, sizeof(md_dtls));
	if (err) {
		MDT_WARN("dtls_srtp: could not get DTLS fingerprint (%m)\n",
			err);
		return false;
	}

	if (sz_sdp != sz_dtls || 0 != memcmp(md_sdp, md_dtls, sz_sdp)) {
		MDT_WARN("dtls_srtp: %r fingerprint mismatch\n", &hash);
		MDT_INFO("SDP:  %w\n", md_sdp, sz_sdp);
		MDT_INFO("DTLS: %w\n", md_dtls, sz_dtls);
		return false;
	}

	MDT_INFO("dtls_srtp: verified %r fingerprint OK\n", &hash);

	return true;
}


static int session_alloc(struct mdt_menc_sess **sessp,
			 struct sdp_session *sdp, bool offerer,
			 mdt_menc_event_h *eventh, mdt_menc_error_h *errorh,
			 void *arg)
{
	struct mdt_menc_sess *sess;
	int err;

	if (!sessp || !sdp)
		return EINVAL;

	sess = mem_zalloc(sizeof(*sess), sess_destructor);
	if (!sess)
		return ENOMEM;

	sess->sdp     = mem_ref(sdp);
	sess->offerer = offerer;
	sess->eventh  = eventh;
	sess->errorh  = errorh;
	sess->arg     = arg;

	/* RFC 4145 */
	/* the answerer takes the passive DTLS role.
	 *
	 * RFC 5763 s5 lets the answerer pick either role when the offer says
	 * `actpass`, and upstream picks `active` — which makes us the side that
	 * sends the ClientHello.  On an inbound call that is the one thing this
	 * SDK cannot do reliably: our media leaves from whatever address the NAT
	 * in front of us maps it to, ICE learns that address as peer-reflexive
	 * rather than from the SDP, and a peer that filters on its candidate
	 * list (Asterisk: "DTLS packet from <ip:port> dropped. Source not in ICE
	 * active candidate list.") drops the handshake we started.
	 *
	 * Measured on-device (2026-08-31, inbound over WSS behind a NAT with two
	 * egress addresses): every inbound call whose selected candidate differed
	 * from the signalled one came up silent — 9 of 9 — while the one outbound
	 * call in the same capture, where the peer answers `active` and therefore
	 * connects to us, handshook without trouble.  The direction of the
	 * handshake is the only thing that differs between them.
	 *
	 * Passive puts an inbound call on the outbound path: the peer sends
	 * first, into the mapping our own ICE checks just opened, and our reply
	 * goes back down the 5-tuple it arrived on.  Nothing then depends on the
	 * peer having been told which of our addresses is the real one.
	 *
	 * The offer decides the answer, so media_alloc() revises this once the
	 * remote `setup` is known: an offer that says `passive` still has to be
	 * answered with `active`. */
	err = sdp_session_set_lattr(sdp, true, "setup",
				    offerer ? "actpass" : "passive");
	if (err)
		goto out;

	/* RFC 4572 */
	err = sdp_session_set_lattr(sdp, true, "fingerprint", "SHA-256 %H",
				    mdt_dtls_print_sha256_fingerprint, tls);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(sess);
	else
		*sessp = sess;

	return err;
}


static size_t get_master_keylen(enum srtp_suite suite)
{
	switch (suite) {

	case SRTP_AES_CM_128_HMAC_SHA1_32: return 16+14;
	case SRTP_AES_CM_128_HMAC_SHA1_80: return 16+14;
	case SRTP_AES_128_GCM:             return 16+12;
	case SRTP_AES_256_GCM:             return 32+12;
	default: return 0;
	}
}


static void dtls_estab_handler(void *arg)
{
	struct comp *comp = arg;
	const struct dtls_srtp *ds = comp->ds;
	enum srtp_suite suite;
	uint8_t cli_key[32+12], srv_key[32+12];
	char buf[32] = "";
	size_t keylen;
	int err;

	MDT_DBG("dtls_srtp: established: cipher=%s\n",
	      tls_cipher_name(comp->tls_conn));

	if (!verify_fingerprint(ds->sess->sdp, ds->sdpm, comp->tls_conn)) {
		MDT_WARN("dtls_srtp: could not verify remote fingerprint\n");
		if (ds->sess->errorh)
			ds->sess->errorh(EPIPE, ds->sess->arg);
		return;
	}

	err = tls_srtp_keyinfo(comp->tls_conn, &suite,
			       cli_key, sizeof(cli_key),
			       srv_key, sizeof(srv_key));
	if (err) {
		MDT_WARN("dtls_srtp: could not get SRTP keyinfo (%m)\n", err);
		return;
	}

	comp->negotiated = true;

	/* the handshake landed — stand the watchdog down. */
	tmr_cancel(&((struct dtls_srtp *)ds)->tmr_hs);

	MDT_INFO("dtls_srtp: ---> DTLS-SRTP complete (%s/%s) Profile=%s\n",
	     sdp_media_name(ds->sdpm),
	     comp->is_rtp ? "RTP" : "RTCP", srtp_suite_name(suite));

	keylen = get_master_keylen(suite);

	err |= mdt_srtp_stream_add(&comp->tx, suite,
			       ds->active ? cli_key : srv_key, keylen, true);
	err |= mdt_srtp_stream_add(&comp->rx, suite,
			       ds->active ? srv_key : cli_key, keylen, false);
	if (err)
		return;

	err |= mdt_srtp_install(comp);
	if (err) {
		MDT_WARN("dtls_srtp: mdt_srtp_install: %m\n", err);
	}

	if (ds->sess->eventh) {
		/* under rtcp-mux this handshake keys RTCP too
		 * and there is no RTCP component to report it, while call.c
		 * starts RTCP only on an event that names it. */
		if (re_snprintf(buf, sizeof(buf), "%s,%s",
				sdp_media_name(ds->sdpm),
				!comp->is_rtp ? "RTCP" :
				ds->mux ? "RTP,RTCP" : "RTP"))
			ds->sess->eventh(MDT_MENC_EVENT_SECURE, buf,
					 (struct mdt_stream *)ds->strm,
					 ds->sess->arg);
		else
			MDT_WARN("dtls_srtp: failed to print secure"
				" event arguments\n");
	}
}


static void dtls_close_handler(int err, void *arg)
{
	struct comp *comp = arg;

	MDT_INFO("dtls_srtp: dtls-connection closed (%m)\n", err);

	comp->tls_conn = mem_deref(comp->tls_conn);

	if (!comp->negotiated) {

		if (comp->ds->sess->errorh)
			comp->ds->sess->errorh(err, comp->ds->sess->arg);
	}
}


static void dtls_conn_handler(const struct sa *peer, void *arg)
{
	struct comp *comp = arg;
	int err;

	MDT_INFO("dtls_srtp: %s: incoming DTLS connect from %J\n",
	     sdp_media_name(comp->ds->sdpm), peer);

	if (comp->ds->active) {
		MDT_WARN("dtls_srtp: conn_handler: role is active\n");
		return;
	}

	if (comp->tls_conn) {
		MDT_WARN("dtls_srtp: '%s' dtls already accepted (peer = %J)\n",
			sdp_media_name(comp->ds->sdpm),
			dtls_peer(comp->tls_conn));

		if (comp->ds->sess->errorh)
			comp->ds->sess->errorh(EPROTO, comp->ds->sess->arg);

		return;
	}

	err = dtls_accept(&comp->tls_conn, tls, comp->dtls_sock,
			  dtls_estab_handler, NULL, dtls_close_handler, comp);
	if (err) {
		MDT_WARN("dtls_srtp: dtls_accept failed (%m)\n", err);
		return;
	}
}


static int component_start(struct comp *comp, const struct sa *raddr)
{
	int err = 0;

	MDT_DBG("dtls_srtp: component start: %s [raddr=%J]\n",
	      comp->is_rtp ? "RTP" : "RTCP", raddr);

	if (!comp->app_sock || comp->negotiated || comp->dtls_sock)
		return 0;

	err = dtls_listen(&comp->dtls_sock, NULL,
			  comp->app_sock, 2, LAYER_DTLS,
			  dtls_conn_handler, comp);
	if (err) {
		MDT_WARN("dtls_srtp: dtls_listen failed (%m)\n", err);
		return err;
	}

	/* maximum one DTLS connection */
	dtls_set_single(comp->dtls_sock, true);

	if (sa_isset(raddr, SA_ALL)) {

		if (comp->ds->active && !comp->tls_conn) {

			MDT_INFO("dtls_srtp: '%s,%s' dtls connect to %J\n",
			     sdp_media_name(comp->ds->sdpm),
			     comp->is_rtp ? "RTP" : "RTCP",
			     raddr);

			err = dtls_connect(&comp->tls_conn, tls,
					   comp->dtls_sock, raddr,
					   dtls_estab_handler, NULL,
					   dtls_close_handler, comp);
			if (err) {
				MDT_WARN("dtls_srtp: dtls_connect()"
					" failed (%m)\n", err);
				return err;
			}
		}
	}

	return err;
}


/* drop an association so it can be started again.
 *
 * Everything the handshake owns goes: the srtp streams and the udp helper are
 * keyed to the master secret being abandoned, and component_start() returns
 * early while `negotiated` or `dtls_sock` is still set.  app_sock stays — the
 * stream owns it, and the replacement association re-listens on it. */
static void medut_media_reset(struct dtls_srtp *st)
{
	size_t i;

	for (i=0; i<2; i++) {
		struct comp *c = &st->compv[i];

		c->uh_srtp    = mem_deref(c->uh_srtp);
		c->tls_conn   = mem_deref(c->tls_conn);
		c->dtls_sock  = mem_deref(c->dtls_sock);
		c->tx         = mem_deref(c->tx);
		c->rx         = mem_deref(c->rx);
		c->negotiated = false;
	}

	st->started = false;
}


static void medut_handshake_deadline(void *arg);
static uint32_t medut_handshake_ms(unsigned restarts);


static int media_start(struct dtls_srtp *st, struct sdp_media *sdpm,
		       const struct sa *raddr_rtp,
		       const struct sa *raddr_rtcp)
{
	int err = 0;

	if (st->started)
		return 0;

	MDT_INFO("dtls_srtp: media=%s -- start DTLS %s\n",
	     sdp_media_name(sdpm), st->active ? "client" : "server");

	if (!mdt_sdp_media_has_media(sdpm))
		return 0;

	err = component_start(&st->compv[0], raddr_rtp);

	if (!st->mux)
		err |= component_start(&st->compv[1], raddr_rtcp);

	if (err)
		return err;

	st->started = true;

	/* remember where this attempt was aimed, so a restart
	 * can be aimed at the same place without another offer/answer. */
	if (raddr_rtp)
		sa_cpy(&st->raddr_rtp, raddr_rtp);
	if (raddr_rtcp)
		sa_cpy(&st->raddr_rtcp, raddr_rtcp);

	/* nothing else ever revisits a handshake that does not
	 * complete.  Without this the call stays up, silent, forever. */
	tmr_start(&st->tmr_hs, medut_handshake_ms(st->hs_restarts),
		  medut_handshake_deadline, st);

	return 0;
}


/* how long to wait on the attempt after `restarts` restarts. */
static uint32_t medut_handshake_ms(unsigned restarts)
{
	return restarts ? MEDUT_DTLS_HANDSHAKE_LATER_MS
			: MEDUT_DTLS_HANDSHAKE_MS;
}


/* the handshake did not finish in time — restart it.
 *
 * Bounded: a peer that never answers must not be retried for the life of the
 * call.  After the last attempt the call is left as it was, which is exactly
 * what the module did after the first attempt before this patch. */
static void medut_handshake_deadline(void *arg)
{
	struct dtls_srtp *st = arg;
	uint32_t waited;

	if (st->compv[0].negotiated)
		return;

	if (st->hs_restarts >= MEDUT_DTLS_MAX_RESTARTS) {
		MDT_WARN("dtls_srtp: handshake did not complete after %u"
			" attempts — giving up (this call has no audio)\n",
			st->hs_restarts + 1);
		return;
	}

	/* The deadline that just expired is the one media_start() armed for the
	 * attempt now ending, so read it before the counter moves. */
	waited = medut_handshake_ms(st->hs_restarts);

	++st->hs_restarts;

	/* As the server, a
	 * handshake that is under way is the one thing a restart must not
	 * touch.  The peer's ClientHello has arrived and dtls_accept() holds
	 * the association; its next flight is on the peer's own retransmit
	 * schedule (1 s, 2 s, 4 s), and dropping the listener now throws that
	 * away and leaves the peer mid-handshake against a socket that no
	 * longer knows it.  Seen on-device (2026-10-14, inbound from FreePBX
	 * over WSS): the ClientHello landed 180 ms before the first deadline,
	 * the reset followed, and the call never got audio.  Spend the attempt
	 * waiting instead; only a peer that stays silent for the whole budget
	 * reaches the client-role fallback. */
	if (!st->active && st->compv[0].tls_conn &&
	    st->hs_restarts < MEDUT_DTLS_MAX_RESTARTS) {
		MDT_INFO("dtls_srtp: media=%s -- handshake in progress after %u ms,"
		     " waiting (attempt %u)\n",
		     sdp_media_name(st->sdpm), waited, st->hs_restarts + 1);
		tmr_start(&st->tmr_hs, medut_handshake_ms(st->hs_restarts),
			  medut_handshake_deadline, st);
		return;
	}

	/* on the last attempt, take the client role whatever
	 * was negotiated.
	 *
	 * As the answerer we advertise `passive` (session_alloc) precisely so
	 * the peer is the side that connects, which is what makes an inbound
	 * call as reliable as an outbound one.  It costs one thing: a peer that
	 * negotiates `active` and then never sends leaves both sides listening,
	 * and no amount of restarting a listener fixes that.
	 *
	 * By here every attempt has ended the same way — nothing arrived — so a
	 * ClientHello on the way out cannot make it worse.  A peer that really
	 * is the client ignores it, exactly as it has ignored our silence; a
	 * peer that was waiting for one gets its call back. */
	if (st->hs_restarts >= MEDUT_DTLS_MAX_RESTARTS && !st->active) {
		MDT_INFO("dtls_srtp: media=%s -- last attempt: taking the client"
		     " role, the peer never connected\n",
		     sdp_media_name(st->sdpm));
		st->active = true;
	}

	MDT_WARN("dtls_srtp: handshake still not complete after %u ms —"
		" restarting the association as %s (attempt %u)\n",
		waited, st->active ? "client" : "server", st->hs_restarts + 1);

	medut_media_reset(st);
	(void)media_start(st, st->sdpm, &st->raddr_rtp, &st->raddr_rtcp);
}


static int media_alloc(struct mdt_menc_media **mp, struct mdt_menc_sess *sess,
		       struct rtp_sock *rtp,
		       struct udp_sock *rtpsock, struct udp_sock *rtcpsock,
		       const struct sa *raddr_rtp,
		       const struct sa *raddr_rtcp,
		       struct sdp_media *sdpm, const struct mdt_stream *strm)
{
	struct dtls_srtp *st;
	const char *setup, *fingerprint;
	int err = 0;
	unsigned i;
	(void)rtp;

	if (!mp || !sess)
		return EINVAL;

	st = (struct dtls_srtp *)*mp;
	if (st)
		goto setup;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->sess = sess;
	st->sdpm = mem_ref(sdpm);
	st->strm = strm;
	st->compv[0].app_sock = mem_ref(rtpsock);
	st->compv[1].app_sock = mem_ref(rtcpsock);

	for (i=0; i<2; i++)
		st->compv[i].ds = st;

	st->compv[0].is_rtp = true;
	st->compv[1].is_rtp = false;

	err = sdp_media_set_alt_protos(st->sdpm, 4,
				       "RTP/SAVP",
				       "RTP/SAVPF",
				       "UDP/TLS/RTP/SAVP",
				       "UDP/TLS/RTP/SAVPF");
	if (err)
		goto out;

 out:
	if (err) {
		mem_deref(st);
		return err;
	}
	else
		*mp = (struct mdt_menc_media *)st;

 setup:
	st->mux = (rtpsock == rtcpsock) || (rtcpsock == NULL);

	setup = sdp_media_session_rattr(st->sdpm, st->sess->sdp, "setup");
	if (setup) {
		enum setup rsetup = setup_decode(setup);
		bool active;

		/* keep the role we take and the role we
		 * advertise in step, now that the answerer advertises `passive`
		 * (session_alloc).
		 *
		 * As the offerer we advertised `actpass` and the answer decides,
		 * which is upstream's rule.  As the answerer the decision is ours
		 * and it is `passive`, so only an offer that says `passive` can
		 * push us into the client role — and when it does, the answer has
		 * to say `active` or the peer listens for a handshake that is
		 * never coming.  This runs before the answer is encoded on the
		 * path where the offer is already decoded; where it is not, the
		 * attribute session_alloc() wrote stands and this corrects the
		 * next offer/answer instead. */
		if (st->sess->offerer)
			active = rsetup != SETUP_ACTIVE;
		else {
			active = (rsetup == SETUP_PASSIVE);

			err = sdp_session_set_lattr(st->sess->sdp, true,
						    "setup",
						    active ? "active"
							   : "passive");
			if (err)
				return err;
		}

		/* a re-negotiation that reverses the DTLS role
		 * has to restart the association.  media_start() latches
		 * `started` and never clears it, so without this the new role
		 * is recorded and then ignored: we go on waiting in the old
		 * role while the peer waits in the other one, and the handshake
		 * can never complete. */
		if (st->started && active != st->active) {
			MDT_INFO("dtls_srtp: media=%s -- role reversed by"
			     " re-negotiation (%s -> %s), restarting DTLS\n",
			     sdp_media_name(st->sdpm),
			     st->active ? "client" : "server",
			     active ? "client" : "server");
			medut_media_reset(st);
		}

		st->active = active;

		err = media_start(st, st->sdpm, raddr_rtp, raddr_rtcp);
		if (err)
			return err;
	}

	/* SDP offer/answer on fingerprint attribute */
	fingerprint = sdp_media_session_rattr(st->sdpm, st->sess->sdp,
					      "fingerprint");
	if (fingerprint) {

		struct pl hash;

		err = sdp_fingerprint_decode(fingerprint, &hash, NULL, NULL);
		if (err)
			return err;

		if (0 == pl_strcasecmp(&hash, "SHA-256")) {
			err = sdp_media_set_lattr(st->sdpm, true,
						  "fingerprint", "SHA-256 %H",
						 mdt_dtls_print_sha256_fingerprint,
						  tls);
		}
		else {
			MDT_INFO("dtls_srtp: unsupported fingerprint hash `%r'\n",
			     &hash);
			return EPROTO;
		}
	}

	return err;
}


static struct mdt_menc dtls_srtp = {
	.id          = "dtls_srtp",
	.sdp_proto   = "UDP/TLS/RTP/SAVPF",
	.wait_secure = true,
	.sessh       = session_alloc,
	.mediah      = media_alloc
};


/* The DTLS context every call shares: a self-signed EC certificate (the
 * peer checks its fingerprint from the SDP, not a CA) and the SRTP
 * profiles to negotiate. */
int mdt_dtls_srtp_init(void)
{
	const char *ec = "prime256v1";
	const char *cn = "dtls@medut";
	int err;

	err = tls_alloc(&tls, TLS_METHOD_DTLS, NULL, NULL);
	if (err) {
		MDT_WARN("dtls_srtp: failed to create DTLS context (%m)\n",
			err);
		return err;
	}

	err = tls_set_selfsigned_ec(tls, cn, ec);
	if (err) {
		MDT_WARN("dtls_srtp: failed to self-sign "
			"ec-certificate (%m)\n", err);
		tls = mem_deref(tls);
		return err;
	}

	tls_set_verify_client_trust_all(tls);

	err = tls_set_srtp(tls, srtp_profiles);
	if (err) {
		MDT_WARN("dtls_srtp: failed to enable SRTP profile (%m)\n",
			err);
		tls = mem_deref(tls);
		return err;
	}

	mdt_menc_register(mdt_mencl(), &dtls_srtp);

	MDT_DBG("DTLS-SRTP ready with profiles %s\n", srtp_profiles);

	return 0;
}


void mdt_dtls_srtp_close(void)
{
	mdt_menc_unregister(&dtls_srtp);
	tls = mem_deref(tls);
}
