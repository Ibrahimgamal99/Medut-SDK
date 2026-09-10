/**
 * @file ice_shim.c  Deadline on ICE candidate gathering
 *
 * An outgoing call with a media-NAT does not send its INVITE when it is
 * placed.  baresip defers it (src/call.c, call_connect):
 *
 *     if (!call->acc->mnat)
 *             err = send_invite(call);
 *     else
 *             err = call_streams_alloc(call);   // INVITE deferred
 *
 * and sends it later from mnat_handler(), which runs when the media-NAT
 * reports that candidate gathering is done.  Nothing bounds how long that
 * takes, and there is one path where the report never comes at all: the ice
 * module's no-STUN/TURN case arms a 1 ms timer whose handler walks
 * `sess->medial` and calls the gather handler once per media entry, so an
 * empty list means the estab handler is never invoked.  The call then sits in
 * ECHOSDK_CALL_CALLING forever — no INVITE on the wire, no event, nothing for
 * the app to react to.  `ua_connect()` returned 0, so the SDK cannot see it
 * either.
 *
 * Every other SIP client bounds this wait and offers whatever it has when the
 * clock runs out.  JsSIP and SIP.js resolve their offer promise on either
 * `icegatheringstatechange == complete` *or* a timer; dart-sip-ua does the
 * same with `ice_gathering_timeout`, default 500 ms; pjsua has
 * PJSUA_ICE_TRANSPORT_INIT_TIMEOUT (30 s) whose own comment calls it "a safety
 * net so the calling thread cannot block indefinitely if the callback never
 * arrives".  This file is that bound for EchoSDK, as cfg.ice_gathering_timeout_ms.
 *
 * ── How it hooks in ─────────────────────────────────────────────────────────
 *
 * `send_invite()` is static inside baresip, so there is no seam to call it
 * from.  The seam that does exist is the media-NAT vtable: baresip finds it
 * with mnat_find(baresip_mnatl(), "ice") and calls through the function
 * pointers.  We overwrite three of those pointers on the registered struct and
 * keep the originals, which is the same interposition ws_path.c performs on
 * libre with --wrap, applied to a vtable instead of the linker.
 *
 * Replacing the struct rather than registering a second media-NAT keeps its
 * identity intact: baresip matches `mnat->id` against "ice" to advertise
 * `Supported: ice` (src/ua.c), and `ftag` puts "+sip.ice" in the Contact.  A
 * differently-named clone would silently drop both.
 *
 * Wrapping sessh alone is not enough — baresip hands whatever `*sessp` we
 * return back to mediah() and updateh(), so those two have to unwrap it.  For
 * the same reason mediah() returns a wrapper of its own and attrh() unwraps it:
 * see the ICE-restart section below for why the media object has to be ours.
 *
 * ── ICE restart on network handover ─────────────────────────────────────────
 *
 * The second job of this file is the one thing netmon.c cannot do from where it
 * stands.  When the device moves between Wi-Fi and cellular, netmon migrates a
 * call with call_reset_transp(), which is sdp_session_set_laddr() plus a
 * re-INVITE.  That is the whole fix for a direct-RTP call, and no fix at all for
 * an ICE call:
 *
 *   - ice.c writes the selected local candidate into the *media* address
 *     (refresh_comp_laddr → sdp_media_set_laddr), and libre emits a
 *     media-level `c=` line whenever that is set (re/src/sdp/msg.c).  Per
 *     RFC 4566 §5.7 it overrides the session-level line netmon just rewrote,
 *     so the offer re-advertises the address that just went away.
 *   - Nothing re-gathers.  The ice module's update handler runs ice_start()
 *     with `started` already true, which only calls icem_update() and
 *     re-encodes the candidate list it already has, under the same
 *     ice-ufrag/ice-pwd.
 *
 * So the peer keeps sending RTP to the old address, and drops what arrives from
 * the new one — with ICE, a source that is not in the candidate list is not
 * accepted (Asterisk: "Source not in ICE active candidate list").  The call
 * stays up with dead audio until netmon runs out of attempts.
 *
 * The repair is an RFC 8445 §9 ICE restart, which needs a fresh
 * ice-ufrag/ice-pwd pair and a fresh gather — neither of which the ice module
 * exposes.  What it does expose, to us, is that we own its session object: the
 * inner `struct mnat_sess` is ours to throw away and allocate again.  A new one
 * mints new credentials (rand_str in session_alloc), writes them into the SDP,
 * and re-gathers against the interface that now carries the default route; when
 * it reports, baresip's mnat_handler turns it into the re-INVITE, which
 * therefore carries the new candidates *and* the new `c=` at both levels.
 *
 * Two things make that safe to do under a live call:
 *
 *   - the media object must survive the swap, because baresip stores it as
 *     `stream->mns` and hands it back to attrh().  Hence the wrapper: the
 *     pointer baresip holds is ours and does not move, while the ice module's
 *     media underneath it is replaced.
 *   - a gather that fails must not be reported as a failure.  baresip answers
 *     an mnat error with CALL_EVENT_CLOSED, which would hang up a call that is
 *     up and merely needs a new offer, so a failed restart releases the offer
 *     with whatever candidates exist instead.
 *
 * The RTP sockets are reused as they are — they are wildcard-bound and follow
 * the new default route on their own, and re-creating them would break the
 * media encryption that is keyed to them.
 */

#include "echosdk_internal.h"

/* Originals, captured at install time. */
static mnat_sess_h   *real_sessh;
static mnat_media_h  *real_mediah;
static mnat_update_h *real_updateh;
static mnat_attr_h   *real_attrh;

/* The registered struct we mutated, so close() can put it back. */
static struct mnat *s_mnat;

/* Live sessions, so a restart can find the one belonging to a call.  baresip
 * keeps no accessor for `call->mnats`, and the call pointer is what netmon has
 * — it is the `arg` the ice session was allocated with. */
static struct list s_sessl = LIST_INIT;

/* Deadline for a restart gather when cfg.ice_gathering_timeout_ms is 0.
 *
 * Zero means "no deadline" for the initial INVITE, where waiting is baresip's
 * own behaviour and the call has not started yet.  A restart is different: it
 * happens under a live call whose audio is already gone, and a gather that
 * never reports would leave it that way with no offer ever sent.  So the
 * restart always has a bound, configured or not. */
#define BSDK_ICE_RESTART_DEADLINE_MS 3000

/* How long the candidate re-offer waits for a media-encryption handshake.
 *
 * The re-offer is a re-INVITE, and a re-INVITE mid-DTLS perturbs the handshake
 * it interrupts, so the offer waits for the handshake rather than racing it.
 *
 * It used to give up waiting after 300 ms and send anyway, on the reasoning
 * that a strict peer (Asterisk: "DTLS packet from <ip:port> dropped. Source
 * not in ICE active candidate list.") can never complete a handshake it is
 * dropping, so waiting on one is a deadlock and the offer is its only cure.
 * Both halves of that turned out to be wrong, measured across 22 calls
 * on-device (2026-08-31, inbound over WSS behind a NAT with two egress
 * addresses):
 *
 *   - The peer is not that strict.  It learns our address from the
 *     connectivity check itself (RFC 8445 s7.3.1.3, peer-reflexive), and the
 *     one call in the capture that reached `Secure` before this deadline
 *     expired went on to carry audio from an address that had never been
 *     signalled.  The re-offer was never the thing that made media flow.
 *
 *   - Sending it does not cure a strict peer either.  A re-offer under the
 *     same ice-ufrag/ice-pwd is not an ICE restart, so nothing on the peer
 *     re-runs its checks for the new candidate (RFC 8445 s9); what the peer
 *     did do was answer with `a=connection:new` and reset its own DTLS.  All
 *     9 calls that released the offer this way died — every subsequent
 *     handshake attempt drew no response at all — against 10 of 11 that never
 *     released one coming up with audio.
 *
 * So the deadline no longer releases the offer: it abandons it.  Holding
 * costs nothing (the poll releases the moment the handshake lands, which on a
 * healthy call is inside 50 ms), and the deadline exists only to stop polling
 * for a handshake that is not coming.  It spans dtls_srtp's own restart ladder
 * — 1200 ms plus four 2500 ms attempts, cmake/patches/dtls_srtp-state.new —
 * so a call that recovers late still gets its address across.
 *
 * A peer that genuinely filters on the signalled address needs an ICE restart
 * (fresh credentials, fresh gather — bsdk_ice_restart() already does exactly
 * that for handover), not a plain re-offer.  Nothing in the captures needs it:
 * every inbound call reached `Secure` or reached nothing, and the DTLS role
 * change (cmake/patches/dtls_srtp-setup.new) removes the address dependency
 * from the handshake in the first place. */
#define BSDK_ICE_REOFFER_SECURE_DEADLINE_MS 12000
#define BSDK_ICE_REOFFER_POLL_MS               50

/* How many peer-initiated ICE restarts to answer with one of our own.
 *
 * A restart re-offers, and a peer that restarts in response to every offer it
 * receives would otherwise trade re-INVITEs with us for the life of the call.
 * Two is enough to cover the real sequence (peer restarts once, in the answer
 * to our candidate re-offer) with one spare. */
#define BSDK_ICE_MAX_REMOTE_RESTARTS           2

/**
 * Our stand-in for the ice module's session object.
 *
 * baresip holds this as `call->mnats` and passes it to mediah()/updateh();
 * both unwrap to `inner` before delegating.  It owns `inner`, so dropping the
 * call frees the real session through our destructor.
 *
 * The fields below `medial` are the arguments the ice module was allocated
 * with, kept so an equivalent session can be built again on a restart.
 */
struct bsdk_ice_sess {
	struct le         le;       /* s_sessl */
	struct mnat_sess *inner;    /* the ice module's own session */
	mnat_estab_h     *estabh;   /* baresip's mnat_handler */
	void             *arg;      /* its `struct call *` */
	struct tmr        tmr;      /* gathering deadline */
	struct tmr        tmr_reoffer;/* re-offer held for the DTLS handshake */
	uint64_t          reoffer_due;/* jiffies at which to stop holding it */
	bool              released; /* the offer has been handed to baresip */
	bool              reoffered;/* a candidate re-offer has been requested */
	bool              restarting;/* a restart gather is outstanding */
	bool              in_estab; /* inside estabh — see ice_updateh() */
	bool              await_answer;/* a restart offer is on the wire */
	struct tmr        tmr_rrestart;/* deferred answer to a peer ICE restart */
	unsigned          rrestarts;/* peer restarts answered with our own */
	struct tmr        tmr_chkfail;/* watchdog: restart's checks concluded? */
	bool              checking;  /* a restart's checks are outstanding */
	bool              primed;   /* early connectivity checks kicked off */
	struct sdp_session *sdp;    /* borrowed; the call's SDP session */
	struct list       medial;   /* struct bsdk_ice_media */

	const struct mnat *mnat;
	struct dnsc       *dnsc;    /* borrowed; owned by the SIP stack */
	int                af;
	bool               have_srv;
	struct stun_uri    srv;     /* .host is ours (see sess_destructor) */
	char              *user;
	char              *pass;
};

/**
 * One media stream.
 *
 * This is what baresip holds as `stream->mns`, so its address must not change
 * for the life of the stream — `inner` is the ice module's media object and is
 * what gets replaced on a restart.
 *
 * It also remembers the address we signalled, so it can be compared with the
 * one ICE ends up using.  See the peer-reflexive note on ice_connected().
 */
struct bsdk_ice_media {
	struct le              le;
	struct bsdk_ice_sess  *sess;    /* NULL once the session is gone */
	struct mnat_media     *inner;   /* the ice module's own media */
	struct udp_sock       *sock1;   /* refs: re-creating `inner` needs them */
	struct udp_sock       *sock2;   /* NULL with rtcp-mux */
	struct sdp_media      *sdpm;    /* borrowed; owned by the call's SDP */
	mnat_connected_h      *connh;   /* baresip's stream handler */
	void                  *arg;
	struct sa              signalled; /* laddr at offer/answer time */
	struct sa              selected[2]; /* raddr ICE nominated: RTP, RTCP */
	struct sa              rsig;      /* peer's signalled raddr back then */
	char                  *rufrag;    /* peer's ice-ufrag, as last decoded */
	bool                   sel_set;   /* selected[] has been filled in */
	bool                   held;      /* connh deferred until the answer */
	bool                   reasserted;/* logged the first re-assert */
};

static void media_destructor(void *data)
{
	struct bsdk_ice_media *m = data;

	list_unlink(&m->le);
	mem_deref(m->inner);
	mem_deref(m->sock1);
	mem_deref(m->sock2);
	mem_deref(m->rufrag);
}

static void sess_destructor(void *data)
{
	struct bsdk_ice_sess *s = data;
	struct le *le, *tmp;

	/* Timers first: all four close over `s`. */
	tmr_cancel(&s->tmr);
	tmr_cancel(&s->tmr_reoffer);
	tmr_cancel(&s->tmr_rrestart);
	tmr_cancel(&s->tmr_chkfail);
	list_unlink(&s->le);

	/* The media wrappers belong to baresip's streams, which may outlive us:
	 * call_destructor() drops call->mnats after call->audio, but nothing
	 * guarantees that order on every teardown path.  Cut them loose — their
	 * `inner` is about to be freed with the inner session (its destructor
	 * list_flushes its own media list), and a wrapper left pointing at it
	 * would double-free from media_destructor. */
	LIST_FOREACH_SAFE(&s->medial, le, tmp) {
		struct bsdk_ice_media *m = le->data;
		m->inner = mem_deref(m->inner);
		m->sess  = NULL;
		list_unlink(&m->le);
	}

	mem_deref(s->inner);
	mem_deref(s->srv.host);
	mem_deref(s->user);
	mem_deref(s->pass);
}

static struct bsdk_ice_sess *sess_find(const void *call)
{
	struct le *le;

	LIST_FOREACH(&s_sessl, le) {
		struct bsdk_ice_sess *s = le->data;
		if (s->arg == call)
			return s;
	}

	return NULL;
}

/**
 * Re-baseline what each stream has signalled, at the moment baresip is about to
 * build the offer or answer from the SDP as it now stands.
 *
 * `signalled` is seeded in ice_mediah() from the *session* address, which is
 * all there is before gathering produces anything — on a phone behind NAT the
 * private host address.  The SDP that actually goes out carries the srflx the
 * gather just found, so comparing against that seed reports "never signalled"
 * for the srflx on every NAT'd call and re-offers a candidate the peer was
 * already told about.  Take the baseline from the addresses ICE has written
 * into the SDP by now instead.
 */
static void rebase_signalled(struct bsdk_ice_sess *s)
{
	struct le *le;

	LIST_FOREACH(&s->medial, le) {
		struct bsdk_ice_media *m = le->data;
		const struct sa *laddr = sdp_media_laddr(m->sdpm);

		if (laddr && sa_isset(laddr, SA_ADDR))
			m->signalled = *laddr;
	}
}


/* ── Peer ICE credentials ────────────────────────────────────────────────── */

/* Not a trigger: `a=connection:new`.
 *
 * RFC 4145 §4 reads like one — the peer is asking for a new connection for the
 * stream — and Asterisk does send it when it tears down and re-creates its RTP
 * instance, sometimes without changing its ice-ufrag, which is a restart a
 * credential comparison cannot see.  Tried on-device on 2026-08-31 and reverted
 * the same hour: Asterisk attaches it to essentially every DTLS answer it
 * writes, including the answers to our own restarts.  Restarting on it produced
 * a storm — two restarts inside 70 ms, each provoking another answer carrying
 * it again — and the call it was meant to rescue still came up silent.
 *
 * A credential change stays the only signal used here, because it is the only
 * one that distinguishes "the peer threw our validated pair away" from "the
 * peer is answering an offer".
 *
 * Track the peer's ICE credentials and report a restart.
 *
 * A new ufrag from the peer is an RFC 8445 §9 ICE restart: it has discarded the
 * credentials under which our nominated pair was validated, along with the
 * permission that pair represented to accept media from our address.  Nothing
 * in baresip or libre notices.  ice.c's update handler runs `ice_start()` with
 * `sess->started` already true, which refreshes local addresses and returns
 * without calling icem_conncheck_start(), while libre's ufrag_decode() simply
 * overwrites `icem->rufrag` — so the new credentials land under a check list
 * that is already Completed, and no further check is ever sent.
 *
 * Observed on-device (2026-08-31): Asterisk answered a candidate re-offer with
 * `a=connection:new`, a fresh fingerprint and a fresh ice-ufrag, having
 * restarted both its DTLS association and its ICE session.  Our side restarted
 * neither.  It therefore had no validated pair covering our source address,
 * kept dropping our DTLS, and the call ran 36 s with tx 0 / rx 0 before the
 * peer gave up and sent BYE.
 *
 * Reading it back from the SDP rather than tracking it through ice_attrh()
 * keeps this independent of which attributes baresip chooses to route through
 * the media-NAT: sdp_media_session_rattr() checks the media level and falls
 * back to the session level, covering a peer that puts one ufrag on the
 * session for every stream.
 */
static bool remote_ufrag_sync(struct bsdk_ice_sess *s)
{
	struct le *le;
	bool restarted = false;

	if (!s || !s->sdp)
		return false;

	LIST_FOREACH(&s->medial, le) {
		struct bsdk_ice_media *m = le->data;
		const char *ru;
		bool baselined;

		if (!m->sdpm)
			continue;

		ru = sdp_media_session_rattr(m->sdpm, s->sdp, ice_attr_ufrag);
		if (!str_isset(ru))
			continue;

		/* Nothing to compare against on the first SDP for this stream,
		 * so the first pass only takes the baseline. */
		baselined = str_isset(m->rufrag);

		if (baselined && str_cmp(m->rufrag, ru)) {
			info("EchoSDK/ice: peer restarted ICE on '%s'"
			     " (ice-ufrag %s -> %s)\n",
			     sdp_media_name(m->sdpm), m->rufrag, ru);
			restarted = true;
		}

		m->rufrag = mem_deref(m->rufrag);
		(void)str_dup(&m->rufrag, ru);
	}

	return restarted;
}


/* Answer the peer's restart with ours, off the SDP-handling call stack.
 *
 * Deferred through a timer because this runs from the media-NAT update handler,
 * which baresip calls from inside call_apply_sdp() while it is still applying
 * the SDP that carried the peer's new credentials.  bsdk_ice_restart() replaces
 * the ICE session under that call and arms an offer; doing it from underneath
 * the SDP the offer is built from is asking for trouble. */
static void rrestart_handler(void *arg)
{
	struct bsdk_ice_sess *s = arg;
	int err = bsdk_ice_restart(s->arg, NULL);

	if (err && err != EALREADY) {
		warning("EchoSDK/ice: could not answer the peer's ICE restart"
		        " (%m) — this call may have no audio\n", err);
	}
}


/* How long a restart's connectivity checks get before they are called failed.
 *
 * A check transaction is RTO-paced with Rc=7 retransmits, so the last one goes
 * out around 3.1 s after the first and a pair cannot be declared dead before
 * then; the pacing between pairs adds to that.  Six seconds clears the whole
 * ladder for the two-pair check lists these calls actually form, without
 * leaving a genuinely broken generation unreported for long. */
#define BSDK_ICE_CHECK_DEADLINE_MS 6000

/**
 * A restart's connectivity checks never concluded on a usable pair.
 *
 * baresip's ice module reports failure by warning and returning — no estab
 * handler, no connected handler, no event (modules/ice/ice.c, conncheck_handler)
 * — so without this the app cannot tell a call whose media path was never
 * validated from one that is working.  Three such calls on-device (2026-09-10)
 * ran fully established and silent to the end.
 *
 * Nothing is torn down here.  reassert_selected() has kept the stream on the
 * pair the previous generation validated, which is what lets audio survive a
 * failed restart, and whether to redial is the app's decision.
 */
static void chkfail_handler(void *arg)
{
	struct bsdk_ice_sess *s = arg;

	if (!s->checking)
		return;

	s->checking = false;

	warning("EchoSDK/ice: the restarted ICE generation did not nominate a"
	        " pair within %u ms — the peer rejected our checks or the path"
	        " stopped carrying UDP.  Holding the media on the pair the"
	        " previous generation validated\n",
	        (unsigned)BSDK_ICE_CHECK_DEADLINE_MS);

	bsdk_netmon_call_ice_failed((struct call *)s->arg, ETIMEDOUT);
}


/**
 * Start connectivity checks as soon as both candidate sets are known.
 *
 * On an inbound call every input ICE needs is present while the phone is still
 * ringing: the peer's candidates arrived in the offer, ours are what the
 * gather this handler reports just produced.  baresip still waits — the only
 * path to icem_conncheck_start() is the media-NAT update handler, and
 * call_apply_sdp() only calls that from call_answer().  So the checks, and the
 * DTLS handshake gated behind them, run *after* the user answers, and every
 * millisecond lands on the caller's ear.
 *
 * Measured on-device (2026-08-31, inbound over WSS on cellular): gathering
 * finished 89 ms after the INVITE, the phone then rang for 3.1 s with nothing
 * happening, and after the 200 OK the checks took 2.33 s and the handshake a
 * further 3.11 s — 5.9 s of dead air on an answered call.  On WiFi, where the
 * PBX's private candidate happened to be routable, the same sequence took
 * 83 ms; the cost is entirely the doomed pairs and the retransmits, which is
 * exactly the work that can be done while ringing.
 *
 * So run the update handler here.  `sess->started` is false, so ice.c takes
 * its not-yet-started branch and starts the checks.  The answer-time call then
 * takes the started branch, whose refresh_laddr() writes the address ICE
 * settled on into the answer — which is also why an inbound call that
 * concludes during ringing needs no candidate re-offer at all: see
 * ice_connected().
 *
 * Only for a call that is still ringing, and the call state is what says so.
 *
 * "The peer's ice-ufrag is known" used to stand in for that, on the reasoning
 * that only the answerer has remote candidates this early.  It is also true of
 * an *established* call whose ICE session has just been replaced: the peer's
 * credentials are still in the SDP from the last answer, so a restart gather
 * reported here would prime a second round of checks against the generation
 * the restart is in the middle of replacing — the very mistake ice_updateh()
 * now suppresses.  It also produced the nonsense line "running the
 * connectivity checks now, before the call is answered" on a call that had
 * been up for seconds (2026-09-02, outbound to *43 over WSS).
 *
 * As the offerer on a fresh call there are no remote candidates at all, and
 * the ice module would decline anyway (verify_peer_ice).
 */
static void prime_conncheck(struct bsdk_ice_sess *s)
{
	struct le *le;
	bool have_remote = false;

	if (!s || s->primed || !real_updateh || !s->inner || !s->sdp)
		return;

	if (!s->arg || CALL_STATE_INCOMING != call_state((struct call *)s->arg))
		return;

	LIST_FOREACH(&s->medial, le) {
		struct bsdk_ice_media *m = le->data;

		if (m->sdpm &&
		    str_isset(sdp_media_session_rattr(m->sdpm, s->sdp,
		                                      ice_attr_ufrag))) {
			have_remote = true;
			break;
		}
	}

	if (!have_remote)
		return;

	s->primed = true;

	info("EchoSDK/ice: peer candidates are already known — running the"
	     " connectivity checks now, before the call is answered\n");

	(void)real_updateh(s->inner);

	/* Baseline the credentials these checks are running against, so the
	 * answer-time update is not mistaken for a peer restart. */
	(void)remote_ufrag_sync(s);
}


/* ── Estab handler — runs on re_main ─────────────────────────────────────── */

/**
 * Hand the offer to baresip, with the offer-side media-NAT update suppressed.
 *
 * Every path into baresip's mnat_handler on an established call ends in
 * call_modify(), which sends the re-INVITE and *then* calls call_update_media()
 * — so the media-NAT update handler runs before the answer arrives (baresip
 * src/call.c, call_modify).  For a session that has just been replaced by a
 * restart that is the worst possible moment: ice.c's update handler takes its
 * not-yet-started branch, re-decodes the peer's credentials from the SDP as it
 * still stands (the *previous* answer), and calls icem_conncheck_start() —
 * pairing our new ice-ufrag/ice-pwd with the peer's old ones.  The peer answers
 * those checks `401 Unauthorized`, and because ice_start() sets `sess->started`
 * on the way out, the answer that carries the peer's new credentials can only
 * reach the refresh branch: no further check is ever sent and the whole ICE
 * generation is dead on arrival.
 *
 * Measured on-device (2026-09-02, outbound to *43 over WSS on cellular): three
 * restarts in a row, each one's checks answered 401, `concluded=0` on every
 * check list, tx 0 / rx 0 for the life of the call.
 *
 * So `in_estab` marks this window and ice_updateh() returns without delegating
 * inside it.  The next update handler baresip runs is the one from
 * call_apply_sdp() when the answer lands, and that one finds `started` still
 * false — the checks then start against the credentials the peer is actually
 * using.  Nothing is lost by skipping the earlier call: the candidates and
 * credentials for the offer were written into the SDP by the gather handler
 * before we got here, and the not-yet-started branch does not touch the SDP.
 */
static void estab_release(struct bsdk_ice_sess *s, int err, uint16_t scode,
                          const char *reason)
{
	/* A failure can end the call from inside estabh — baresip's mnat_handler
	 * answers one with CALL_EVENT_CLOSED — and take `s` with it, which is
	 * why ice_estab() only touches `s` again on the success path.  There is
	 * no offer on that path either, so leave it to unwind untouched. */
	if (err || scode) {
		s->estabh(err, scode, reason, s->arg);
		return;
	}

	s->in_estab = true;
	s->estabh(0, 0, NULL, s->arg);
	s->in_estab = false;
}

static void ice_estab(int err, uint16_t scode, const char *reason, void *arg)
{
	struct bsdk_ice_sess *s = arg;

	tmr_cancel(&s->tmr);

	if (s->released) {
		/* The offer for this session has already gone out.
		 *
		 * Two different things arrive here, and only one of them is a
		 * late gather.  The ice module calls its estab handler again
		 * from conncheck_handler() once the checks conclude and the
		 * selected candidate has changed (ice.c: "sending Re-INVITE
		 * with updated default candidates"), which on a NAT'd call is
		 * the ordinary outcome — the peer-reflexive candidate ICE
		 * settles on was not in the offer.  The other is the gather the
		 * deadline gave up waiting for, reporting at last.
		 *
		 * Both want the same thing: pass it on, and baresip turns a
		 * media-NAT established on a call that is no longer waiting for
		 * one into a re-INVITE carrying whatever the SDP now says.  So
		 * the handling does not distinguish them — but the log line
		 * must, because reading "gathering completed after the
		 * deadline" against a gather that finished in 144 ms sends you
		 * looking for a gathering problem that is not there.
		 *
		 * A failure is not actionable any more.  The offer is on the
		 * wire and the call may well be up; baresip's handler answers a
		 * failure with CALL_EVENT_CLOSED, which would tear down a
		 * working call because a STUN server we no longer need did not
		 * answer.  Log it and stop here. */
		s->restarting = false;

		if (err || scode) {
			warning("EchoSDK/ice: the media-NAT reported a failure "
			        "after the offer had already gone out "
			        "(%m, %u %s) — call left alone\n",
			        err, scode, reason ? reason : "");
			return;
		}

		info("EchoSDK/ice: the media-NAT reports again with the offer "
		     "already out (ICE concluded on a new candidate, or a "
		     "late gather) — re-offering what the SDP carries now\n");
	}

	/* A restart gathers under a call that is already up, so a failure here
	 * must not travel: baresip's mnat_handler answers one with
	 * CALL_EVENT_CLOSED and the call the restart was trying to rescue would
	 * be hung up instead.  Release the offer with whatever the ice module
	 * managed to put in the SDP — even a host-only candidate on the new
	 * interface is better than the old network's candidate list, which is
	 * what not offering at all would leave in place. */
	if (s->restarting && (err || scode)) {
		warning("EchoSDK/ice: restart gathering failed (%m, %u %s) —"
		        " offering the candidates gathered so far\n",
		        err, scode, reason ? reason : "");
		err    = 0;
		scode  = 0;
		reason = NULL;
	}
	s->restarting = false;

	if (!err && !scode)
		rebase_signalled(s);

	s->released = true;
	estab_release(s, err, scode, reason);

	/* After estabh, not before: for an inbound call that handler is what
	 * delivers CALL_EVENT_INCOMING, and the checks belong to the ringing it
	 * starts, not ahead of it. */
	if (!err && !scode)
		prime_conncheck(s);
}

/* ── Deadline — runs on re_main ──────────────────────────────────────────── */

static void gather_deadline(void *arg)
{
	struct bsdk_ice_sess *s = arg;

	if (s->released)
		return;

	/* Report success with no error: baresip's mnat_handler treats that as
	 * "media-nat established/gathered" and sends the INVITE with whatever
	 * the ice module has put into the SDP so far.  Reporting an error
	 * instead would close the call, which is the outcome the deadline
	 * exists to avoid. */
	warning("EchoSDK/ice: %s gathering did not complete in time; offering "
	        "the candidates gathered so far "
	        "(cfg.ice_gathering_timeout_ms=%u)\n",
	        s->restarting ? "restart" : "candidate",
	        g_bsdk.cfg.ice_gathering_timeout_ms);

	/* `released` is not cleared: a real gather that reports after this takes
	 * the `released` path in ice_estab(), which re-offers the complete set.
	 *
	 * `restarting` IS cleared, and must be.  It gates bsdk_ice_restart()
	 * against re-entry, and the gather this deadline just gave up on may
	 * never report at all — that is the failure the deadline exists for.
	 * Leaving the flag set would make every later restart on this call
	 * return EALREADY, so a second handover would silently skip the ICE
	 * restart and migrate with the candidates of a network that is gone. */
	s->restarting = false;
	s->released   = true;
	estab_release(s, 0, 0, NULL);
}

/* ── Wrapped vtable entries ──────────────────────────────────────────────── */

static int ice_sessh(struct mnat_sess **sessp, const struct mnat *mnat,
                     struct dnsc *dnsc, int af, const struct stun_uri *srv,
                     const char *user, const char *pass,
                     struct sdp_session *sdp, bool offerer,
                     mnat_estab_h *estabh, void *arg)
{
	struct bsdk_ice_sess *s;
	int err = 0;

	if (!sessp || !estabh)
		return EINVAL;

	s = mem_zalloc(sizeof(*s), sess_destructor);
	if (!s)
		return ENOMEM;

	tmr_init(&s->tmr);
	tmr_init(&s->tmr_reoffer);
	tmr_init(&s->tmr_rrestart);
	list_init(&s->medial);
	s->estabh = estabh;
	s->arg    = arg;
	s->sdp    = sdp;

	/* Everything a replacement session needs.  `srv` points into the
	 * account, which can be re-configured under a live call, so the host
	 * string is copied rather than referenced. */
	s->mnat = mnat;
	s->dnsc = dnsc;
	s->af   = af;
	if (srv) {
		s->have_srv   = true;
		s->srv.scheme = srv->scheme;
		s->srv.port   = srv->port;
		s->srv.proto  = srv->proto;
		if (srv->host)
			err = str_dup(&s->srv.host, srv->host);
	}
	if (!err && user)
		err = str_dup(&s->user, user);
	if (!err && pass)
		err = str_dup(&s->pass, pass);
	if (err) {
		mem_deref(s);
		return err;
	}

	err = real_sessh(&s->inner, mnat, dnsc, af, srv, user, pass,
	                 sdp, offerer, ice_estab, s);
	if (err) {
		mem_deref(s);
		return err;
	}

	/* Arm after the inner session exists.  `released` is checked because a
	 * media-NAT is free to report synchronously; the ice module does not,
	 * but a deadline armed after the fact would never be cancelled. */
	if (g_bsdk.cfg.ice_gathering_timeout_ms && !s->released) {
		tmr_start(&s->tmr, g_bsdk.cfg.ice_gathering_timeout_ms,
		          gather_deadline, s);
	}

	list_append(&s_sessl, &s->le, s);

	*sessp = (struct mnat_sess *)s;
	return 0;
}

/**
 * Is a media-encryption handshake still in flight on this call?
 *
 * Two questions, because `menc_secure` is only ever set by an encryption that
 * has something to secure: a plain-RTP call leaves it false for the life of
 * the stream, and waiting on it there would hold every re-offer to the
 * deadline for nothing.  So the fingerprint attribute decides whether there is
 * a handshake at all, and stream_is_secure() decides whether it has finished.
 *
 * sdp_media_session_rattr() reads the media level first and falls back to the
 * session level, which is where baresip's dtls_srtp puts our own — and where a
 * peer that offers one fingerprint for the whole session puts its.
 */
static bool sess_awaiting_secure(const struct bsdk_ice_sess *s)
{
	struct list *streaml;
	struct le *le;
	bool dtls = false;

	if (!s || !s->sdp || !s->arg)
		return false;

	LIST_FOREACH(&s->medial, le) {
		const struct bsdk_ice_media *m = le->data;

		if (m->sdpm &&
		    sdp_media_session_rattr(m->sdpm, s->sdp, "fingerprint")) {
			dtls = true;
			break;
		}
	}

	if (!dtls)
		return false;

	streaml = call_streaml(s->arg);
	if (!streaml)
		return false;

	LIST_FOREACH(streaml, le) {
		const struct stream *strm = le->data;

		if (!stream_is_secure(strm))
			return true;
	}

	return false;
}


/* Hand the re-offer to baresip.  mnat_wait is false by now, so it turns into
 * call_modify(), i.e. a re-INVITE.  It is guarded by sipsess_refresh_allowed(),
 * so a call whose negotiation is still in flight ignores it rather than
 * sending a malformed request. */
static void reoffer_release(struct bsdk_ice_sess *s)
{
	tmr_cancel(&s->tmr_reoffer);
	estab_release(s, 0, 0, NULL);
}


static void reoffer_deadline(void *arg)
{
	struct bsdk_ice_sess *s = arg;

	if (sess_awaiting_secure(s)) {
		if (tmr_jiffies() < s->reoffer_due) {
			tmr_start(&s->tmr_reoffer, BSDK_ICE_REOFFER_POLL_MS,
			          reoffer_deadline, s);
			return;
		}

		/* The handshake is not coming back, and this offer is not what
		 * would bring it back — see BSDK_ICE_REOFFER_SECURE_DEADLINE_MS
		 * for the 9 calls that died proving it.  Drop it: a re-INVITE
		 * now only resets what little state the peer has left. */
		tmr_cancel(&s->tmr_reoffer);
		warning("EchoSDK/ice: media encryption still not secure after "
		        "%u ms — dropping the candidate re-offer\n",
		        BSDK_ICE_REOFFER_SECURE_DEADLINE_MS);
		return;
	}

	reoffer_release(s);
}


/**
 * ICE finished its connectivity checks for one stream.
 *
 * This is where a peer-reflexive candidate has to be re-offered.  ICE learns
 * srflx candidates from STUN *before* the offer/answer, so those are signalled
 * and the peer accepts media from them.  A *peer-reflexive* candidate is
 * different: it is discovered from the connectivity checks themselves, after
 * the SDP has gone out, and RFC 8445 §5.1.3 does not signal it.  When ICE then
 * nominates that candidate — which is exactly what happens on a phone behind
 * NAT with no STUN server configured, where the host candidate is the only one
 * offered — we start sending media from an address the peer has never been
 * told about.
 *
 * A strict peer drops it.  Asterisk logs
 *
 *     res_rtp_asterisk.c: DTLS packet from <ip:port> dropped.
 *     Source not in ICE active candidate list.
 *
 * and since DTLS-SRTP gates RTP behind the handshake (`wait_secure`), the call
 * connects, signalling looks perfect, and neither side hears anything.  It hits
 * the answerer hardest: the answerer picks `a=setup:active` per RFC 5763 and so
 * is the side that sends first, into an address the peer will not accept.
 *
 * baresip already knows how to fix this — ice.c sets `send_reinvite` when the
 * selected local candidate changes — but only acts on it when its conncheck
 * handler is called with `update` set, which does not happen on this path
 * (`connectivity check is complete (update=0)`).  So do it here: if the address
 * ICE settled on is not the one we signalled, ask baresip to re-offer.  By the
 * time this runs, ice.c has already written the new candidates into the SDP,
 * so the re-INVITE carries them.
 *
 * Two things have since narrowed what this is for, and neither makes it free:
 * a peer learns our real address from the connectivity check itself, so it is
 * rarely waiting to be told; and a re-offer that goes out mid-handshake is the
 * single strongest predictor of a silent call in any capture we have.  So the
 * offer is held for the handshake and dropped if the handshake never lands
 * (BSDK_ICE_REOFFER_SECURE_DEADLINE_MS), and the disagreement it answers is
 * itself now much rarer: the SDK no longer gathers on interfaces the media
 * does not use (cmake/patches/ice-ifsel.new), and an inbound call no longer
 * depends on the peer accepting a handshake we started
 * (cmake/patches/dtls_srtp-setup.new).
 */
static void ice_connected(const struct sa *raddr1, const struct sa *raddr2,
                          void *arg)
{
	struct bsdk_ice_media *m = arg;
	struct bsdk_ice_sess *s = m->sess;
	const struct sa *laddr;

	if (!s) {
		/* Detached by sess_destructor — the call is going away. */
		if (m->connh)
			m->connh(raddr1, raddr2, m->arg);
		return;
	}

	/* This generation nominated a pair, so the watchdog armed for it in
	 * ice_updateh() has nothing left to report. */
	s->checking = false;
	tmr_cancel(&s->tmr_chkfail);

	/* Remember the pair ICE nominated.  call_apply_sdp() throws it away on
	 * every later SDP exchange — see ice_updateh(). */
	if (raddr1 && sa_isset(raddr1, SA_ALL)) {
		const struct sa *rsig = sdp_media_raddr(m->sdpm);

		m->selected[0] = *raddr1;
		sa_init(&m->selected[1], AF_UNSPEC);
		if (raddr2)
			sa_cpy(&m->selected[1], raddr2);
		sa_init(&m->rsig, AF_UNSPEC);
		if (rsig)
			sa_cpy(&m->rsig, rsig);
		m->sel_set = true;
	}

	/* Nothing below may run before the answer has gone out.
	 *
	 * With prime_conncheck() an inbound call's checks conclude while the
	 * phone is still ringing, and baresip's connected handler is what starts
	 * the media encryption: dtls_srtp would send its ClientHello seconds
	 * before the peer has our answer, and therefore before it has our
	 * fingerprint, our setup role or our ice-ufrag.  It would be dropped,
	 * and the handshake watchdog would spend its restart budget on a call
	 * nobody had answered yet — arriving at "giving up (this call has no
	 * audio)" before the first word.
	 *
	 * So hold the pair.  ice_updateh() delivers it through
	 * reassert_selected() when call_answer() applies the SDP, which is the
	 * same call stack that writes the 200 OK.  The re-offer below is held
	 * for the same reason and needs nothing further: the answer carries the
	 * address ICE settled on, because ice.c's update handler calls
	 * refresh_laddr() for a session that has already started. */
	if (s->arg && CALL_STATE_INCOMING == call_state((struct call *)s->arg)) {
		if (m->sel_set)
			m->held = true;
		info("EchoSDK/ice: '%s' concluded on %J while the call was"
		     " still ringing — holding media setup until the answer"
		     " goes out\n",
		     sdp_media_name(m->sdpm), raddr1);
		return;
	}

	/* baresip first: it starts the media encryption, and a re-offer must not
	 * jump ahead of that. */
	if (m->connh)
		m->connh(raddr1, raddr2, m->arg);

	/* SA_ADDR, not SA_ALL: the port belongs to the socket and never moves,
	 * while the session laddr this may be compared against carries a
	 * different port entirely.  The address is the thing the peer filters
	 * on. */
	laddr = sdp_media_laddr(m->sdpm);
	if (!laddr || !sa_isset(&m->signalled, SA_ADDR) ||
	    !sa_isset(laddr, SA_ADDR))
		return;

	if (sa_cmp(laddr, &m->signalled, SA_ADDR))
		return;   /* unchanged — the peer already knows this address */

	/* Once per session.  Each stream would otherwise ask separately, and a
	 * burst of re-INVITEs on one dialog is worse than the problem. */
	if (s->reoffered)
		return;
	s->reoffered = true;

	info("EchoSDK/ice: selected local candidate %J was never signalled "
	     "(offered %J) — re-offering so the peer accepts our media\n",
	     laddr, &m->signalled);

	/* Never while the media encryption is mid-handshake: the re-INVITE
	 * would perturb an association dtls_srtp can only start once, and the
	 * call would come up silent.  See BSDK_ICE_REOFFER_SECURE_DEADLINE_MS. */
	if (sess_awaiting_secure(s)) {
		info("EchoSDK/ice: holding the re-offer until the media "
		     "encryption handshake completes\n");
		s->reoffer_due = tmr_jiffies() +
		                 BSDK_ICE_REOFFER_SECURE_DEADLINE_MS;
		tmr_start(&s->tmr_reoffer, BSDK_ICE_REOFFER_POLL_MS,
		          reoffer_deadline, s);
		return;
	}

	reoffer_release(s);
}

static int ice_mediah(struct mnat_media **mp, struct mnat_sess *sess,
                      struct udp_sock *sock1, struct udp_sock *sock2,
                      struct sdp_media *sdpm,
                      mnat_connected_h *connh, void *arg)
{
	struct bsdk_ice_sess *s = (struct bsdk_ice_sess *)sess;
	struct bsdk_ice_media *m;
	const struct sa *laddr;
	int err;

	if (!s)
		return EINVAL;

	m = mem_zalloc(sizeof(*m), media_destructor);
	if (!m)
		return ENOMEM;

	m->sess  = s;
	m->sdpm  = sdpm;
	m->connh = connh;
	m->arg   = arg;
	/* Referenced, not borrowed: a restart hands the same sockets to the
	 * replacement media, so they have to outlive the ice module's own
	 * reference to them. */
	m->sock1 = mem_ref(sock1);
	m->sock2 = mem_ref(sock2);

	err = real_mediah(&m->inner, s->inner, sock1, sock2, sdpm,
	                  ice_connected, m);
	if (err) {
		mem_deref(m);
		return err;
	}

	/* Record the address this stream is about to be advertised on.
	 *
	 * A stream's own laddr is unset until ICE picks one — sdp_media_add()
	 * only fills in the port, so sdp_media_laddr() reads back as
	 * 0.0.0.0:<port> and the `c=` line the peer receives comes from the
	 * *session*.  Fall back to that, or the baseline is an address that was
	 * never advertised to anyone and every ICE call re-offers itself. */
	laddr = sdp_media_laddr(sdpm);
	if (laddr && sa_isset(laddr, SA_ADDR)) {
		m->signalled = *laddr;
	}
	else if (s->sdp) {
		const struct sa *slad = sdp_session_laddr(s->sdp);
		if (slad)
			m->signalled = *slad;
	}

	list_append(&s->medial, &m->le, m);

	/* Ours, not the ice module's: baresip keeps this as `stream->mns` for
	 * the life of the stream, and a restart replaces what is underneath it.
	 * ice_attrh() unwraps it again. */
	*mp = (struct mnat_media *)m;
	return 0;
}

/**
 * Remote ICE attributes for one stream.
 *
 * Pure unwrapping: baresip calls this with what mediah() returned, which is our
 * wrapper, while the ice module's handler expects its own object.  During the
 * window where a restart has dropped the old media and not yet built the
 * replacement there is nothing to decode into, and the attributes are re-read
 * from the SDP by the ice module's update handler anyway.
 */
static void ice_attrh(struct mnat_media *mm, const char *name,
                      const char *value)
{
	struct bsdk_ice_media *m = (struct bsdk_ice_media *)mm;

	if (!m || !m->inner || !real_attrh)
		return;

	real_attrh(m->inner, name, value);
}

/**
 * Re-assert the remote address ICE selected, after an SDP exchange discarded it.
 *
 * baresip keeps the address it sends media to in `stream->tx.raddr_rtp`, and
 * two different things write it:
 *
 *   - stream_mnat_connected(), from the media-NAT's connected handler, with the
 *     remote candidate ICE nominated;
 *   - stream_remote_set(), from stream_update(), with `sdp_media_raddr()` — the
 *     address in the peer's SDP, verbatim.
 *
 * The second one runs on *every* call to call_update_media(): our re-offer from
 * ice_connected(), ice.c's own re-INVITE, a re-INVITE the peer sends, a session
 * refresh, hold/unhold.  It overwrites the first, and nothing puts it back: the
 * ice module's update handler only refreshes *local* addresses
 * (ice_start() with sess->started already true), and no new connectivity check
 * runs, so no further connected handler fires.
 *
 * When the peer is behind NAT the two addresses are not the same.  A PBX that
 * signals `c=IN IP4 172.16.11.52` and is reached, per ICE, at
 * 82.129.158.253:19914 leaves the stream pointed at 172.16.11.52 from the first
 * re-INVITE onwards — an RFC 1918 address on the far side of the internet.  With
 * DTLS-SRTP the handshake is what dies first: it is still retransmitting when
 * the address changes under it, the retries go into the void, `menc_secure`
 * never gets set, and stream_is_ready() stays false forever.  So audio_update()
 * is never called, the mic is never opened, and the call sits there fully
 * established with tx 0 / rx 0 and no audio levels at all — in both directions,
 * on a call where signalling looks perfect.
 *
 * call_apply_sdp() calls the media-NAT's update handler as its last act, after
 * stream_update() has run for every stream, which makes this the seam that
 * covers all of those paths at once.
 */
static void reassert_selected(struct bsdk_ice_sess *s)
{
	struct le *le;

	LIST_FOREACH(&s->medial, le) {
		struct bsdk_ice_media *m = le->data;
		const struct sa *rsig;

		if (!m->sel_set || !m->connh)
			continue;

		/* A pair held by ice_connected() has never been delivered at
		 * all, so it is released unconditionally — this is the delivery
		 * baresip would have had during ringing, only deferred until the
		 * answer was out.  The re-assert guard below must not stand in
		 * front of it: refusing here would mean the connected handler is
		 * never called, and a call that started media nowhere is a worse
		 * outcome than one that started it at a stale address. */
		if (m->held) {
			m->held = false;
			info("EchoSDK/ice: answer is out — starting media on "
			     "the pair ICE nominated during ringing (%J)\n",
			     &m->selected[0]);
			m->connh(&m->selected[0],
			         sa_isset(&m->selected[1], SA_ALL)
			                 ? &m->selected[1] : NULL,
			         m->arg);
			continue;
		}

		/* Only when the peer is still asking for the same place.  A peer
		 * that signals a *different* address has genuinely moved its
		 * media — a transfer, a re-bridge onto another media server —
		 * and stream_update() is right to follow it.  Pinning the old
		 * pair there would break the very case this is meant to protect.
		 * The nominated pair belongs to an address the peer no longer
		 * uses, so forget it and let ICE nominate again. */
		rsig = sdp_media_raddr(m->sdpm);
		if (!rsig || !sa_cmp(rsig, &m->rsig, SA_ALL)) {
			m->sel_set = false;
			continue;
		}

		if (!m->reasserted) {
			m->reasserted = true;
			info("EchoSDK/ice: re-applying the remote address ICE "
			     "selected (%J) after the SDP exchange reset it to "
			     "the signalled %J\n",
			     &m->selected[0], &m->rsig);
		}

		m->connh(&m->selected[0],
		         sa_isset(&m->selected[1], SA_ALL) ? &m->selected[1]
		                                           : NULL,
		         m->arg);
	}
}


/* ── ICE restart (called from netmon.c on handover) ──────────────────────── */

/**
 * Take one stream's pre-restart ICE state out of the SDP.
 *
 * The re-gather rewrites all of this the moment it reports (set_media_attributes
 * deletes and re-adds the candidate list, gather_handler re-points the media
 * address at the new default candidate), so in the ordinary case this is
 * invisible.  It matters when the gather does *not* report — a STUN server that
 * is unreachable on the new network is exactly the case the deadline exists for.
 * The offer released then would otherwise pair new credentials with the old
 * network's candidates and the old network's media address: the peer would run
 * its checks against addresses that are gone and get nowhere.
 *
 * Cleared instead, the same offer says "no candidates" and carries `c=` on the
 * new interface, which the peer reads as ICE unsupported (ice.c's own
 * verify_peer_ice does the mirror image of this) and answers with plain RTP to
 * an address that works.
 *
 * The port is kept: it belongs to the RTP socket, which is not being re-created,
 * and is what the m= line is built from.  It is read back from the socket rather
 * than from the SDP, because the address that was there may have been a
 * server-reflexive candidate whose port is the NAT's, not ours.
 */
static void media_clear_stale(struct bsdk_ice_media *m, const struct sa *laddr)
{
	struct sa local;

	sdp_media_del_lattr(m->sdpm, ice_attr_cand);
	sdp_media_del_lattr(m->sdpm, ice_attr_remote_cand);

	if (!laddr || !sa_isset(laddr, SA_ADDR) || !m->sock1)
		return;

	if (udp_local_get(m->sock1, &local))
		return;

	{
		struct sa media = *laddr;
		sa_set_port(&media, sa_port(&local));
		sdp_media_set_laddr(m->sdpm, &media);
	}

	if (m->sock2 && !udp_local_get(m->sock2, &local)) {
		struct sa rtcp = *laddr;
		sa_set_port(&rtcp, sa_port(&local));
		sdp_media_set_laddr_rtcp(m->sdpm, &rtcp);
	}
}

/**
 * Restart ICE for one call and re-offer on `laddr`.
 *
 * @param call  baresip call the migration is for (netmon's lc->bc).
 * @param laddr New local address, or NULL to leave the session address alone.
 *
 * @return 0 when a restart is under way — the re-INVITE follows from the estab
 *         handler, within the gathering deadline.
 *         ENOENT when this call has no ICE media-NAT to restart (the caller's
 *         plain re-offer is the right thing then), EALREADY when a restart is
 *         already gathering, or an errorcode when the replacement session could
 *         not be allocated (the old one is left running).
 */
int bsdk_ice_restart(void *call, const struct sa *laddr)
{
	struct bsdk_ice_sess *s = sess_find(call);
	struct mnat_sess *inner;
	struct le *le;
	uint32_t deadline;
	int err;

	if (!s_mnat || !s)
		return ENOENT;

	/* No media means baresip disabled the media-NAT for every stream (BUNDLE
	 * mux does this), so there is no ICE state to restart and no candidates
	 * in the SDP to be stale. */
	if (list_isempty(&s->medial))
		return ENOENT;

	if (s->restarting)
		return EALREADY;

	/* Build the replacement before dismantling anything, so a failure leaves
	 * the call with the ICE state it already had.
	 *
	 * offerer=true: we are the side sending this offer, which per RFC 8445
	 * §7.3.1.1 makes us the controlling agent for the restarted session; the
	 * new tie-breaker sorts it out if the peer disagrees. */
	err = real_sessh(&inner, s->mnat, s->dnsc, s->af,
	                 s->have_srv ? &s->srv : NULL, s->user, s->pass,
	                 s->sdp, true, ice_estab, s);
	if (err) {
		warning("EchoSDK/ice: restart: replacement session failed"
		        " (%m) — keeping the current ICE state\n", err);
		return err;
	}

	/* The session-level address matters for the streams whose media address
	 * the gather has not written yet, and for a peer that ignores ICE. */
	if (laddr && sa_isset(laddr, SA_ADDR))
		sdp_session_set_laddr(s->sdp, laddr);

	/* Drop the old media objects first: each unlinks itself from the old
	 * session's list, so the deref below cannot free them a second time. */
	LIST_FOREACH(&s->medial, le) {
		struct bsdk_ice_media *m = le->data;
		m->inner = mem_deref(m->inner);
		media_clear_stale(m, laddr);
	}

	mem_deref(s->inner);
	s->inner        = inner;
	s->released     = false;
	s->restarting   = true;
	s->reoffered    = false;
	/* A verdict on the generation being replaced is no longer wanted — this
	 * one supersedes it, and will arm a watchdog of its own. */
	s->checking     = false;
	tmr_cancel(&s->tmr_chkfail);
	/* The peer will restart in its answer to this, per RFC 8445 §9.  Marks
	 * that credential change as ours to expect — see ice_updateh(). */
	s->await_answer = true;

	LIST_FOREACH(&s->medial, le) {
		struct bsdk_ice_media *m = le->data;
		int e;

		/* selected[]/sel_set deliberately survive the swap.
		 *
		 * They describe the pair the *previous* generation validated,
		 * and until this one nominates its own that is the only address
		 * known to carry media.  Clearing it here left reassert_selected()
		 * with nothing to re-apply, so every SDP exchange during the
		 * restart handed the stream back to the address the peer
		 * signals — for a PBX behind NAT, its private one — and a
		 * restart whose checks never concluded stranded the call there
		 * for good.  ice_connected() overwrites them the moment this
		 * generation nominates, and reassert_selected() only re-applies
		 * them while the peer is still signalling the same place, so a
		 * peer that genuinely moved its media is still followed. */
		m->held       = false;
		m->reasserted = false;

		e = real_mediah(&m->inner, s->inner, m->sock1, m->sock2,
		                m->sdpm, ice_connected, m);
		if (e) {
			warning("EchoSDK/ice: restart: media '%s' failed (%m)"
			        " — that stream keeps the candidates it had\n",
			        sdp_media_name(m->sdpm), e);
		}
	}

	deadline = g_bsdk.cfg.ice_gathering_timeout_ms
	         ? g_bsdk.cfg.ice_gathering_timeout_ms
	         : BSDK_ICE_RESTART_DEADLINE_MS;
	tmr_start(&s->tmr, deadline, gather_deadline, s);

	/* No laddr is not a missing one: rrestart_handler() answers a peer's ICE
	 * restart without moving addresses, and passes NULL to say so.  Printing
	 * an unset sa there produced a line that read like a bug ("restarting
	 * ICE on  —"), so say which case this is. */
	if (laddr && sa_isset(laddr, SA_ADDR)) {
		info("EchoSDK/ice: restarting ICE on %j — new credentials,"
		     " re-gathering, re-INVITE follows within %u ms\n",
		     laddr, deadline);
	}
	else {
		info("EchoSDK/ice: restarting ICE, keeping the current local"
		     " address — new credentials, re-gathering, re-INVITE"
		     " follows within %u ms\n", deadline);
	}

	return 0;
}

/**
 * Does this call have a live ICE media-NAT?
 *
 * Answers the question netmon actually has ("are this call's candidates mine to
 * migrate?") rather than the one the config answers ("was ICE asked for?").
 */
bool bsdk_ice_call_active(void *call)
{
	struct bsdk_ice_sess *s = sess_find(call);

	return s && !list_isempty(&s->medial);
}

static int ice_updateh(struct mnat_sess *sess)
{
	struct bsdk_ice_sess *s = (struct bsdk_ice_sess *)sess;
	bool changed;
	int err;

	if (!s)
		return EINVAL;

	/* A restart's offer-side update, from call_modify() before the answer is
	 * in.  Delegating here is what starts this generation's connectivity
	 * checks against the credentials of the one it replaces — see
	 * estab_release().  Nothing is lost by waiting: call_modify() encodes
	 * and sends the offer before it calls call_update_media(), so this
	 * update never contributed to the offer on the wire, and the answer's
	 * update does the same work a round trip later.
	 *
	 * Narrow to a restart on purpose.  On a session the ice module has
	 * already started, the same call is a harmless refresh (icem_update,
	 * refresh_laddr, set_media_attributes — SDP bookkeeping, no callbacks),
	 * and suppressing it would change the plain re-offer paths for nothing.
	 *
	 * reassert_selected() still has to run: call_apply_sdp() got here
	 * through stream_update(), which reset each stream to the address the
	 * peer signalled.  A restart has dropped its nominated pairs so this is
	 * a no-op today, but skipping it would silently break the day one is
	 * kept. */
	if (s->in_estab && s->await_answer) {
		reassert_selected(s);
		return 0;
	}

	err = real_updateh(s->inner);

	changed = remote_ufrag_sync(s);

	/* This is the answer to a restart *we* offered, so the peer's new
	 * credentials are the expected consequence of our own re-INVITE and not
	 * a restart it decided on.
	 *
	 * RFC 8445 §9 leaves it no choice: an answerer that receives a restart
	 * offer restarts too, and mints a new ice-ufrag/ice-pwd to say so.
	 * Reading that as a peer-initiated restart made every restart of ours
	 * provoke another one of ours — three re-INVITEs inside a second, each
	 * one's answer triggering the next until the budget ran out, then
	 * netmon's next stall round starting the sequence again (2026-09-02,
	 * outbound to *43 over WSS).  `s->restarting` does not cover it: it is
	 * cleared in ice_estab() before the offer is even handed to baresip, so
	 * by the time the answer arrives the guard below sees nothing.
	 *
	 * Cleared unconditionally, not only when the credentials changed: a peer
	 * that answers under the same ufrag must not leave the flag armed for a
	 * genuine restart later in the call to be swallowed by.  It is single-
	 * shot for the same reason — if call_modify() declined to send the offer
	 * at all (call_refresh_allowed() false under glare), the flag is spent on
	 * whichever exchange comes next instead, which costs one restart out of
	 * the budget of BSDK_ICE_MAX_REMOTE_RESTARTS and cannot wedge. */
	if (s->await_answer) {
		s->await_answer = false;

		/* The answer to our restart is in, so this generation's checks
		 * start now.  Nothing in baresip or libre reports a check list
		 * that ends with an empty valid list — ice.c's conncheck_handler
		 * warns and returns — so put a clock on it here.  Armed whether
		 * or not the peer changed credentials: a generation can fail
		 * against unchanged ones just as easily. */
		s->checking = true;
		tmr_start(&s->tmr_chkfail, BSDK_ICE_CHECK_DEADLINE_MS,
		          chkfail_handler, s);

		if (changed) {
			info("EchoSDK/ice: the peer's new credentials are the"
			     " answer to our own ICE restart — checks are"
			     " running against them, not restarting again\n");

			/* Keep the pair the previous generation validated until
			 * this one has actually nominated its own.
			 *
			 * Dropping it here is what made a failed restart fatal.
			 * The theory was that the nominated pair belonged to the
			 * generation being replaced and so had to go; what
			 * happens in practice is that stream_update() has just
			 * reset the stream to the address the peer *signals*,
			 * which for a PBX behind NAT is its private one
			 * (`c=IN IP4 172.16.11.52`).  With the pair forgotten,
			 * reassert_selected() has nothing to put back, and the
			 * only thing that would ever repoint the stream is
			 * ice_connected() from the new generation.  When that
			 * generation's checks are answered 401 — every restart
			 * against this Asterisk was — it never fires, and the
			 * stream stays aimed at an RFC 1918 address for the rest
			 * of the call: rx climbing, `spk -72.2 dBov`, SRTP
			 * replay failures, permanent silence (2026-09-10).
			 *
			 * Holding the old pair costs nothing when the restart
			 * succeeds: ice_connected() overwrites it as soon as the
			 * new generation nominates.  It is the difference
			 * between a restart that fails harmlessly and one that
			 * kills the call. */
			reassert_selected(s);

			return err;
		}
	}

	/* A peer that restarted ICE has thrown away the pair we nominated, so
	 * re-asserting that pair's remote address would pin the stream to a
	 * place the peer no longer accepts media from.  Drop it and restart ICE
	 * ourselves: fresh credentials, a fresh gather and a fresh offer, which
	 * is what gives the peer a candidate list it can validate us against.
	 *
	 * Only when the media encryption has not come up: a restart that leaves
	 * a secure, flowing stream alone is not worth a re-INVITE, and
	 * reassert_selected() is still the right thing for it.  A plain-RTP call
	 * reads as "not awaiting secure" and is likewise left alone — nothing
	 * gates its media on a handshake, and symmetric-RTP latching follows the
	 * peer without help from us. */
	if (changed && sess_awaiting_secure(s)) {
		struct le *le;

		LIST_FOREACH(&s->medial, le) {
			struct bsdk_ice_media *m = le->data;
			m->sel_set = false;
		}

		/* Not while one is already under way.  Two SDP exchanges can
		 * carry a credential change 70 ms apart, and bsdk_ice_restart()
		 * would answer the second with EALREADY.  Counting that as a
		 * restart spends the budget on a call that never happened, and
		 * the genuine restart later in the call then finds none left.
		 * (The commonest such pair — our own restart offer and its
		 * answer — is taken out above, before this runs.) */
		if (s->restarting || tmr_isrunning(&s->tmr_rrestart)) {
			info("EchoSDK/ice: an ICE restart is already under way"
			     " — not starting another\n");
		}
		else if (s->rrestarts < BSDK_ICE_MAX_REMOTE_RESTARTS) {
			++s->rrestarts;
			tmr_start(&s->tmr_rrestart, 0, rrestart_handler, s);
		}
		else {
			warning("EchoSDK/ice: peer has restarted ICE %u times"
			        " and the media encryption is still not up —"
			        " not restarting again\n", s->rrestarts);
		}

		return err;
	}

	reassert_selected(s);

	return err;
}

/* ── Install / restore ───────────────────────────────────────────────────── */

/**
 * Interpose the gathering deadline on the "ice" media-NAT.
 *
 * Call after modules_init(), which is what loads the ice module and registers
 * the struct we mutate.  A build without the ice module returns ENOENT and the
 * SDK runs unchanged — there is no media-NAT to defer an INVITE behind.
 */
int bsdk_ice_shim_init(void)
{
	/* mnat_find returns const because callers have no business editing the
	 * registry entry; the object itself is a mutable static owned by the
	 * ice module, and interposing on it is exactly what we are here for. */
	struct mnat *m = (struct mnat *)mnat_find(baresip_mnatl(), "ice");

	if (!m)
		return ENOENT;

	/* echosdk_init after echosdk_shutdown re-runs module_init(), which
	 * re-registers the same static struct.  Installing twice would capture
	 * our own wrappers as the originals and recurse forever. */
	if (m->sessh == ice_sessh)
		return 0;

	real_sessh   = m->sessh;
	real_mediah  = m->mediah;
	real_updateh = m->updateh;
	real_attrh   = m->attrh;

	m->sessh   = ice_sessh;
	m->mediah  = ice_mediah;
	m->updateh = ice_updateh;
	m->attrh   = ice_attrh;
	s_mnat     = m;

	return 0;
}

void bsdk_ice_shim_close(void)
{
	if (!s_mnat)
		return;

	s_mnat->sessh   = real_sessh;
	s_mnat->mediah  = real_mediah;
	s_mnat->updateh = real_updateh;
	s_mnat->attrh   = real_attrh;
	s_mnat = NULL;

	real_sessh   = NULL;
	real_mediah  = NULL;
	real_updateh = NULL;
	real_attrh   = NULL;
}
