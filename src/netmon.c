/**
 * @file netmon.c  Network handover (Wi-Fi ↔ 4G/5G roaming)
 *
 * When the device moves between networks the local IP changes underneath a
 * live SIP stack.  Three things break at once and all three are repaired here:
 *
 *   1. The listening SIP transports are bound to an address that no longer
 *      exists, and any TCP/TLS/WSS connection over it is a dead socket that
 *      will not fail fast — it stalls until Timer B (32 s).
 *   2. The registrar's binding points at the old Contact, so inbound calls
 *      are routed into the void until the next REGISTER refresh (up to
 *      reg_expires seconds away).
 *   3. Active calls advertise the old address in the SDP `c=` line, so the
 *      peer keeps sending RTP to an address we can no longer receive on.
 *
 * The repair sequence, all on the re_main thread:
 *
 *      detect → settle → [transports] → [REGISTER] → [re-INVITE] → verify
 *
 * Detection is either pushed by the app (echosdk_network_changed(), driven
 * from ConnectivityManager / NWPathMonitor) or pulled by a poll timer.  Both
 * feed the same debounce: an interface coming up produces a burst of address
 * changes, and acting on the first one wastes a full REGISTER round-trip.
 * We wait until the address set has been stable for cfg.net_settle_ms.
 *
 * Media is verified rather than assumed.  A re-INVITE that gets a 200 OK can
 * still leave audio dead (the peer's ACK crosses a NAT that has not rebound,
 * SDP was answered with the wrong direction, the new path is blocked).  After
 * each re-INVITE we sample the stream's RX packet counter and check it has
 * advanced cfg.net_verify_ms later; if it has not, we re-offer, and after
 * cfg.net_max_attempts we report failure (and optionally hang up).
 *
 * RTP sockets themselves are never re-created: baresip binds them to the
 * wildcard address (stream_sock_alloc → rtp_listen with an unset sa), so they
 * follow the new default route automatically.  Only the address advertised in
 * the SDP has to change, which is exactly what call_reset_transp() does.
 *
 * That is the whole story for a direct-RTP call, and only half of it for an ICE
 * call.  ice.c writes the selected local candidate into the *media* address, and
 * a media-level `c=` line overrides the session-level one call_reset_transp()
 * rewrites (RFC 4566 §5.7); nothing re-gathers either, so the offer would carry
 * the address and the candidate list of the network the call just left.  Those
 * calls are migrated with an RFC 8445 §9 ICE restart instead —
 * bsdk_ice_restart(), whose re-INVITE carries new credentials, freshly gathered
 * candidates and the new address at both levels.  See src/ice/ice_mod.c.
 *
 * A restart is only attempted when the local address actually moved.  A
 * WebSocket call whose path is unchanged is re-INVITEd for a different reason
 * (to re-bind the dialog to the new connection — see call_signals_over_ws), and
 * putting working media through an ICE restart for that would cost audio.
 *
 * ECHOSDK_NET_CALL_ICE_STALE is still emitted, but now only for an ICE call the
 * restart could not be performed for; cfg.net_ice_handover then decides how long
 * to keep trying, as before: BEST_EFFORT runs the normal verify/retry budget,
 * FAIL_FAST gives up after a single attempt so the app gets a prompt
 * CALL_MIGRATION_FAILED it can answer by re-placing the call, instead of the
 * user holding a silent handset for net_verify_ms × net_max_attempts.
 */

#include "echosdk_internal.h"

/* Upper bound on accounts/calls processed per handover. Both lists are
 * snapshotted under their lock and walked afterwards, because migrating a
 * call re-enters baresip's event handler (which takes the same locks). */
#define BSDK_NET_MAX_SNAP    64

/* Bound on address add/remove passes per scan — a guard against spinning on
 * an address the kernel reports but baresip refuses to accept. */
#define BSDK_NET_SCAN_MAX    32

#define BSDK_NET_SETTLE_MS   1500
#define BSDK_NET_VERIFY_MS   4000
#define BSDK_NET_MAX_ATTEMPT 6

/* ── Stall repair ─────────────────────────────────────────────────────────
 *
 * A handover is announced: an address appears or disappears and the repair
 * starts from a known cause.  A stall is the opposite — media stops with the
 * network looking exactly as it did a second ago.  Neither the peer's NAT
 * rebinding nor a multi-WAN router moving our flow to its other egress
 * address shows up as a local address change, and both leave the call up,
 * signalling healthy, with dead audio.  adapt.c already measures it; these
 * bounds decide what to do about it.
 *
 * The clock is tighter than a handover's.  By the time we are called the call
 * has already been silent for cfg.media_stall_ms (4 s by default), so a
 * 4 s verify on top would put the first verdict 8 s in — past the point where
 * the user has given up on the call.
 *
 * The budget is smaller for the same reason, and because the attempts are no
 * longer interchangeable: each one offers from a different local address
 * (next_laddr), so three attempts is the whole rotation on a phone plus one.
 *
 * The cooldown is what keeps a genuinely dead call from being re-INVITEd for
 * ever: stall detection re-arms once a repair leaves the SENT state, so
 * without it a call whose media never returns would enter a new repair every
 * media_stall_ms.  ROUNDS is the harder stop — after that many episodes the
 * problem is not one we can offer our way out of, and the MEDIA_STALL alert
 * is left to speak for itself. */
#define BSDK_NET_STALL_VERIFY_MS   1500
#define BSDK_NET_STALL_COOLDOWN_MS 10000
#define BSDK_NET_STALL_ATTEMPTS        3
#define BSDK_NET_STALL_ROUNDS          3

/* How long a migration offer waits for a media-encryption handshake.
 *
 * The length of the handshake ladder itself: dtls_srtp waits 1200 ms on the
 * first attempt and 2500 ms on each of four restarts, so a handshake still
 * running at 12 s has been given up on and there is nothing left to protect.
 * See the hold in send_migration(). */
#define BSDK_NET_MIG_SECURE_HOLD_MS 12000

/* How long a reported network change waits for the path check (see
 * start_path_check()) before treating the silence as a dead connection.
 *
 * The probe is one OPTIONS over a connection that is already up, so a live
 * path answers in two round trips — the proxy's digest challenge and the
 * retry — which is well under a second even on a congested cellular link.
 * Erring short is safe: a check that runs out of time does exactly what every
 * report did before the check existed. */
#define BSDK_NET_CHECK_MS 3000

struct bsdk_netmon {
	struct tmr tmr_poll;    /* periodic interface scan                     */
	struct tmr tmr_settle;  /* debounce / retry of the handover itself     */
	struct tmr tmr_verify;  /* per-call migration progress tick            */
	struct tmr tmr_check;   /* path check deadline                         */

	bool     started;
	bool     reset_pending; /* addresses changed, handover still owed      */
	bool     force;         /* app asserted a change; run even if addrs eq */
	bool     announced;     /* CHANGE_DETECTED already emitted this round  */
	bool     down;          /* no usable local address                     */
	bool     checking;      /* path check in flight                        */
	uint32_t check_gen;     /* stamps this round's probes; bumped to end it */
	uint32_t check_left;    /* probes of this round not yet answered       */

	uint32_t gen;           /* handover generation; stamps per-call state  */
	uint32_t attempt;       /* consecutive failed transport resets         */
	uint32_t wait_ticks;    /* consecutive no-address polls (backoff only)  */
	uint64_t handover_start;/* tmr_jiffies() when this round was detected  */

	uint32_t poll_s;
	uint32_t settle_ms;
	uint32_t verify_ms;
	uint32_t max_attempts;
	bool     reinvite_calls;
	bool     hangup_on_fail;
	echosdk_ice_handover_t ice_handover;

	char     cur_laddr[64];
};

static struct bsdk_netmon g_nm;

static void settle_handler(void *arg);
static void verify_handler(void *arg);
static void send_migration(struct echosdk_call *lc);
static uint32_t max_attempts_for(const struct echosdk_call *lc);

/* ── Events ──────────────────────────────────────────────────────────────── */

static echosdk_error_t map_err(int err)
{
	switch (err) {
	case 0:        return ECHOSDK_OK;
	case ENOMEM:   return ECHOSDK_ERR_NOMEM;
	case EINVAL:   return ECHOSDK_ERR_INVAL;
	case ETIMEDOUT: return ECHOSDK_ERR_TIMEOUT;
	default:       return ECHOSDK_ERR_TRANSPORT;
	}
}

/* True when the call signals over WebSocket.
 *
 * It matters for handover because a WebSocket client has no listening port: its
 * Contact is a placeholder ("sip:user@10.0.0.5:9;transport=wss", RFC 7118 §5.2)
 * and the server reaches it by remembering which WebSocket the dialog's requests
 * arrived on.  A transport reset always builds a *new* WebSocket, so that
 * association is stale afterwards even when the local IP never moved: media
 * keeps flowing, our own BYE still gets out (it is routed, not received), but an
 * inbound BYE has nowhere to go and the call hangs in ESTABLISHED for the rest
 * of the session.  Sending any in-dialog request over the new connection re-binds
 * it, which is what the handover re-INVITE is for here.
 *
 * Address-routed transports do not have this problem: if the path is unchanged
 * their Contact still resolves. */
static bool call_signals_over_ws(const struct echosdk_call *lc)
{
	if (!lc || !lc->acct)
		return false;

	return lc->acct->parsed_transport == ECHOSDK_TRANSPORT_WS ||
	       lc->acct->parsed_transport == ECHOSDK_TRANSPORT_WSS;
}

/* Does this call have ICE?
 *
 * Asked of the media-NAT itself while the call is alive, because the config only
 * says what was requested: an account with ICE enabled still ends up with a
 * plain-RTP call when the peer offers no candidates, and that call migrates like
 * any other.  Falls back to the config for a call that has already gone, which
 * is only ever a reporting path. */
static bool call_uses_ice(const struct echosdk_call *lc)
{
	if (!lc || !lc->acct)
		return g_bsdk.cfg.ice_enabled;

	if (lc->bc)
		return bsdk_ice_call_active(lc->bc);

	return lc->acct->cfg.ice_enabled || g_bsdk.cfg.ice_enabled;
}

static void netmon_emit(echosdk_net_event_t what,
                         struct echosdk_call *lc,
                         struct echosdk_account *acct,
                         int err, uint32_t attempt)
{
	struct echosdk_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type                   = ECHOSDK_EV_NETWORK;
	qev->ev.u.network.event        = what;
	qev->ev.u.network.call         = lc;
	qev->ev.u.network.account      = acct;
	qev->ev.u.network.attempt      = attempt;
	/* Report the offer budget this call is actually held to, so a FAIL_FAST
	 * stale-ICE call renders as "1/1" rather than promising retries that will
	 * never be attempted.  WAIT_ADDR and DEFERRED ticks share net_mig_tries
	 * and keep the full budget, so `attempt` can exceed this on a call still
	 * waiting for a route — a progress counter, not an invariant. */
	qev->ev.u.network.max_attempts = lc ? max_attempts_for(lc)
	                                    : g_nm.max_attempts;
	qev->ev.u.network.error        = map_err(err);

	if (lc) {
		qev->ev.u.network.ice = call_uses_ice(lc);
		if (lc->net_mig_start)
			qev->ev.u.network.elapsed_ms =
				(uint32_t)(tmr_jiffies() - lc->net_mig_start);
	}
	else if (g_nm.handover_start) {
		qev->ev.u.network.elapsed_ms =
			(uint32_t)(tmr_jiffies() - g_nm.handover_start);
	}

	str_ncpy(qev->buf, g_nm.cur_laddr, sizeof(qev->buf));
	qev->ev.u.network.local_addr = qev->buf;

	bsdk_event_post_qev(qev);   /* warns and frees qev when the queue is full */
}

/* ── Address-set scanning ────────────────────────────────────────────────── */

struct scan_ctx {
	struct sa sa;
	bool      found;
};

/* net_laddr_apply predicate: does baresip already know this address? */
static bool laddr_eq_h(const char *ifname, const struct sa *sa, void *arg)
{
	(void)ifname;
	return sa_cmp((const struct sa *)arg, sa, SA_ADDR);
}

/* net_if_apply predicate: an OS address baresip has not been told about. */
static bool if_missing_h(const char *ifname, const struct sa *sa, void *arg)
{
	struct scan_ctx *c = arg;
	struct network  *net = baresip_network();

	/* Honours cfg.net.ifname, use_linklocal and the AF enable flags, and
	 * drops loopback — the same filter baresip applies at startup. */
	if (!net_ifaddr_filter(net, ifname, sa))
		return false;

	if (net_laddr_apply(net, laddr_eq_h, (void *)sa))
		return false;

	sa_cpy(&c->sa, sa);
	c->found = true;
	return true;
}

/* net_laddr_apply predicate: an address baresip knows that is gone from the
 * box.  net_if_getname() answers ENODEV when no interface carries it. */
static bool laddr_gone_h(const char *ifname, const struct sa *sa, void *arg)
{
	struct scan_ctx *c = arg;
	char ifn[2] = "?";
	(void)ifname;

	if (net_if_getname(ifn, sizeof(ifn), sa_af(sa), sa) != ENODEV)
		return false;

	sa_cpy(&c->sa, sa);
	c->found = true;
	return true;
}

/* What kind of local addresses the box currently has.
 *
 * The two are not interchangeable: an interface that is administratively up
 * but has lost its lease still carries an IPv6 link-local address, which can
 * bind a socket but cannot reach an off-link registrar.  Binding decisions
 * use `linklocal || routable`; connectivity reporting uses `routable`. */
struct usable_ctx {
	bool routable;
	bool linklocal;
};

static bool if_usable_h(const char *ifname, const struct sa *sa, void *arg)
{
	struct usable_ctx *c = arg;

	if (!net_ifaddr_filter(baresip_network(), ifname, sa))
		return false;

	if (sa_is_linklocal(sa))
		c->linklocal = true;
	else
		c->routable = true;

	return c->routable;   /* nothing better to find — stop */
}

static void scan_usable(struct usable_ctx *c)
{
	memset(c, 0, sizeof(*c));
	(void)net_if_apply(if_usable_h, c);
}

/* True when a SIP server outside the local link is reachable at all. */
static bool have_routable_addr(void)
{
	struct usable_ctx c;
	scan_usable(&c);
	return c.routable;
}

/**
 * Reconcile baresip's local address list with the kernel's.
 * @return number of addresses added or removed.
 */
static unsigned scan_addresses(void)
{
	struct network *net = baresip_network();
	unsigned changed = 0;
	int i;

	for (i = 0; i < BSDK_NET_SCAN_MAX; i++) {
		struct scan_ctx c;
		memset(&c, 0, sizeof(c));
		(void)net_if_apply(if_missing_h, &c);
		if (!c.found)
			break;
		if (net_add_address(net, &c.sa))
			break;   /* would re-find the same address forever */
		changed++;
	}

	for (i = 0; i < BSDK_NET_SCAN_MAX; i++) {
		struct scan_ctx c;
		memset(&c, 0, sizeof(c));
		(void)net_laddr_apply(net, laddr_gone_h, &c);
		if (!c.found)
			break;
		if (net_rm_address(net, &c.sa))
			break;
		changed++;
	}

	return changed;
}

/* net_if_apply predicate: take the first address that passes baresip's filter. */
static bool if_first_h(const char *ifname, const struct sa *sa, void *arg)
{
	struct scan_ctx *c = arg;

	if (!net_ifaddr_filter(baresip_network(), ifname, sa))
		return false;

	sa_cpy(&c->sa, sa);
	c->found = true;
	return true;
}

static void update_cur_laddr(void)
{
	struct network  *net = baresip_network();
	const struct sa *sa;

	/* net_af() is AF_UNSPEC whenever both families are enabled, and
	 * net_laddr_af(AF_UNSPEC) matches nothing — try each family in turn. */
	sa = net_laddr_af(net, net_af(net));
	if (!sa || !sa_isset(sa, SA_ADDR))
		sa = net_laddr_af(net, g_bsdk.cfg.prefer_ipv6 ? AF_INET6 : AF_INET);
	if (!sa || !sa_isset(sa, SA_ADDR))
		sa = net_laddr_af(net, g_bsdk.cfg.prefer_ipv6 ? AF_INET : AF_INET6);

	if (sa && sa_isset(sa, SA_ADDR)) {
		(void)re_snprintf(g_nm.cur_laddr, sizeof(g_nm.cur_laddr), "%j", sa);
		return;
	}

	/* baresip's list can be empty before the first scan; fall back to the
	 * kernel's view so the reported address is never blank while we are up. */
	{
		struct scan_ctx c;
		memset(&c, 0, sizeof(c));
		(void)net_if_apply(if_first_h, &c);
		if (c.found)
			(void)re_snprintf(g_nm.cur_laddr, sizeof(g_nm.cur_laddr),
			                  "%j", &c.sa);
		else
			g_nm.cur_laddr[0] = '\0';
	}
}

/* ── Snapshots (see BSDK_NET_MAX_SNAP comment) ───────────────────────────── */

struct call_snap {
	struct echosdk_call **v;
	size_t                max;
	size_t                n;
};

static void call_snap_cb(struct echosdk_call *lc, void *arg)
{
	struct call_snap *s = arg;
	if (s->n < s->max)
		s->v[s->n++] = lc;
}

static size_t snapshot_calls(struct echosdk_call **v, size_t max)
{
	struct call_snap s = { .v = v, .max = max, .n = 0 };
	bsdk_call_foreach(call_snap_cb, &s);
	return s.n;
}

static size_t snapshot_accounts(struct echosdk_account **v, size_t max)
{
	struct le *le;
	size_t n = 0;

	mtx_lock(&g_bsdk.acct_lock);
	LIST_FOREACH(&g_bsdk.accounts, le) {
		struct echosdk_account *a = le->data;
		if (n >= max)
			break;
		if (a->destroyed || !a->ua || !a->reg_wanted)
			continue;
		v[n++] = a;
	}
	mtx_unlock(&g_bsdk.acct_lock);

	return n;
}

/* Tell every account the app wants registered that its registration is on the
 * way back, not merely fine.
 *
 * A handover invalidates the registrar's binding before anything is re-sent:
 * the transports are about to be flushed, the Contact points at an address the
 * device has left, and inbound calls land nowhere until the REGISTER on the new
 * path is answered.  Leaving the account on REGISTERED through all that reports
 * a registration that demonstrably does not work, so the app shows a green dot
 * over a dead path — and the NET_* events, which do describe it, are not what a
 * registration indicator is bound to.
 *
 * Called when the change is first detected and again if the link goes away
 * entirely, so a long no-address wait is covered too.  The state then holds
 * through the re-REGISTER (see acct->reconnecting) until it is answered.
 */
static void mark_accounts_reconnecting(void)
{
	struct echosdk_account *snap[BSDK_NET_MAX_SNAP];
	size_t n = snapshot_accounts(snap, RE_ARRAY_SIZE(snap));

	for (size_t i = 0; i < n; i++)
		bsdk_account_reg_reconnecting(snap[i]);
}

/* Give up driving the re-REGISTER from here and let each account's own retry
 * policy carry it, backoff and all.  Used when the handover itself has run out
 * of attempts: the recovery has to stay somebody's job.
 *
 * Only the accounts we put in RECONNECTING — those are the ones promised a
 * recovery.  One that was already terminally FAILED (bad credentials) is not
 * given a retry it never asked for. */
static void handover_accounts_to_retry(void)
{
	struct echosdk_account *snap[BSDK_NET_MAX_SNAP];
	size_t n = snapshot_accounts(snap, RE_ARRAY_SIZE(snap));

	for (size_t i = 0; i < n; i++) {
		if (snap[i]->reg_state == ECHOSDK_REG_RECONNECTING)
			bsdk_account_schedule_retry(snap[i]);
	}
}

/* ── Per-call migration ──────────────────────────────────────────────────── */

static uint32_t rx_packets(const struct echosdk_call *lc)
{
	struct audio  *au;
	struct stream *strm;

	if (!lc->bc)
		return 0;

	au   = call_audio(lc->bc);
	strm = au ? audio_strm(au) : NULL;

	return strm ? stream_metric_get_rx_n_packets(strm) : 0;
}

/**
 * Budget for *offers* on this call — how many times the re-INVITE itself may be
 * re-sent before the migration is declared failed.
 *
 * One for a stale-ICE call under FAIL_FAST: the retries that help a non-ICE
 * call (the peer's NAT rebinding, symmetric-RTP latching) cannot help when the
 * offered candidates are the wrong ones, so repeating the same doomed offer
 * only lengthens the silence before the app is told to redial.
 *
 * This is deliberately NOT applied to the WAIT_ADDR and DEFERRED retries.
 * Those wait on local readiness — a default route that has not appeared yet, a
 * dialog that cannot carry a second offer — and have nothing to do with ICE
 * staleness.  Capping them at one would fail a FAIL_FAST call on the first
 * verify tick without ever sending the single offer it is promised.
 */
static uint32_t max_attempts_for(const struct echosdk_call *lc)
{
	/* A stall repair rotates local addresses rather than waiting out the
	 * same one, so its budget is a rotation, not a patience setting.  Taken
	 * before the FAIL_FAST shortcut below: that one is about offering stale
	 * ICE candidates after a handover, which is not what happened here. */
	if (lc->net_mig_stall) {
		return g_nm.max_attempts < BSDK_NET_STALL_ATTEMPTS
		     ? g_nm.max_attempts : BSDK_NET_STALL_ATTEMPTS;
	}

	/* A call whose ICE was restarted is not offering the wrong candidates any
	 * more, so the shortcut does not apply to it: it deserves the same retry
	 * budget as a direct-RTP call, because what it is now waiting on is the
	 * peer's NAT rebinding, exactly like one. */
	if (g_nm.ice_handover == ECHOSDK_ICE_HANDOVER_FAIL_FAST &&
	    call_uses_ice(lc) && !lc->net_ice_restarted)
		return 1;

	return g_nm.max_attempts;
}

static uint32_t verify_tick_ms(void)
{
	uint32_t ms = g_nm.verify_ms ? g_nm.verify_ms : 2000;
	return ms < 500 ? 500 : ms;
}

/* The verify clock for one call.
 *
 * cfg.net_verify_ms == 0 means "do not check media, take the answered offer as
 * confirmation", and that decision belongs to the app for a stall repair
 * exactly as it does for a handover — so the stall clock only applies where
 * verification is on at all. */
static uint32_t verify_tick_for(const struct echosdk_call *lc)
{
	if (!g_nm.verify_ms || !lc || !lc->net_mig_stall)
		return verify_tick_ms();

	return BSDK_NET_STALL_VERIFY_MS < verify_tick_ms()
	     ? BSDK_NET_STALL_VERIFY_MS : verify_tick_ms();
}

/* ── Local-address rotation ───────────────────────────────────────────────
 *
 * start_migration() asks the routing table which source address reaches the
 * peer, which is the right first answer and the wrong second one: ask it again
 * after a failed attempt and it says the same thing, so the retry re-offers
 * the path that just failed to carry media.  These three keep a per-repair
 * record of what has been offered and hand out the next untried address.
 *
 * What this does NOT do is move the media path by itself.  The RTP sockets are
 * wildcard-bound and follow the kernel's default route, so offering the
 * cellular address while the default route is Wi-Fi changes the SDP and not
 * where the packets leave from.  It is worth doing anyway — the address the
 * peer accepts media *from* is exactly what fails in the captures — but a real
 * second path needs the socket bound to the network
 * (android_setsocknetwork / SO_BINDTODEVICE), which is not done here. */
static bool laddr_tried(const struct echosdk_call *lc, const struct sa *sa)
{
	for (unsigned i = 0; i < lc->net_mig_ntried; i++) {
		if (sa_cmp(&lc->net_mig_tried[i], sa, SA_ADDR))
			return true;
	}
	return false;
}

static void laddr_remember(struct echosdk_call *lc, const struct sa *sa)
{
	if (!sa_isset(sa, SA_ADDR) || laddr_tried(lc, sa))
		return;
	if (lc->net_mig_ntried >= BSDK_NET_MAX_CAND)
		return;

	sa_cpy(&lc->net_mig_tried[lc->net_mig_ntried++], sa);
}

struct laddr_pick {
	const struct echosdk_call *lc;
	struct sa                  pick;
	int                        af;
	bool                       found;
};

static bool laddr_pick_handler(const char *ifname, const struct sa *sa,
                               void *arg)
{
	struct laddr_pick *p = arg;
	(void)ifname;

	if (sa_af(sa) != p->af || sa_is_loopback(sa) || sa_is_linklocal(sa))
		return false;
	if (laddr_tried(p->lc, sa))
		return false;

	sa_cpy(&p->pick, sa);
	p->found = true;
	return true;   /* stop the walk */
}

/* Has the address in hand had its chances, or is it still the best answer?
 *
 * The routing table's answer is the right one while the route it describes is
 * real, so a handover gives it two attempts before looking elsewhere: the
 * ordinary reason media has not returned yet is the peer's NAT rebinding, and
 * moving addresses under that spends the attempt that would have worked.  A
 * stall repair rotates a step sooner — nothing about the route changed, so the
 * routing table's answer is precisely the one that has already failed. */
static bool rotate_laddr(const struct echosdk_call *lc)
{
	return lc->net_mig_tries >= (lc->net_mig_stall ? 1u : 2u);
}

/* The next address to offer from, or false when there is only ever one.
 *
 * Once every interface has had a turn the record is cleared and the rotation
 * starts again, so a two-interface phone alternates for as long as the budget
 * lasts instead of settling on whichever one it tried last. */
static bool next_laddr(struct echosdk_call *lc, int af, struct sa *out)
{
	struct laddr_pick p = { .lc = lc, .af = af, .found = false };

	sa_init(&p.pick, AF_UNSPEC);
	net_laddr_apply(baresip_network(), laddr_pick_handler, &p);

	if (!p.found) {
		lc->net_mig_ntried = 0;
		net_laddr_apply(baresip_network(), laddr_pick_handler, &p);
	}

	if (!p.found)
		return false;

	sa_cpy(out, &p.pick);
	return true;
}

/* How long an ICE restart may take to put its re-INVITE on the wire.
 *
 * Unlike call_reset_transp(), a restart does not offer from inside
 * send_migration(): the offer is built when the re-gather reports, or when the
 * gathering deadline releases it.  The verify tick has to allow for that, or it
 * would judge the call before its offer had even been sent — and re-offer,
 * burning the budget on a migration that was still on its way. */
static uint32_t restart_grace_ms(void)
{
	return g_bsdk.cfg.ice_gathering_timeout_ms
	     ? g_bsdk.cfg.ice_gathering_timeout_ms
	     : BSDK_ICE_RESTART_DEADLINE_MS;
}

static void fail_migration(struct echosdk_call *lc, int err)
{
	lc->net_mig_state = BSDK_MIG_FAILED;
	netmon_emit(ECHOSDK_NET_CALL_MIGRATION_FAILED, lc, lc->acct, err,
	            lc->net_mig_tries);

	/* Leaving a call up with dead audio is usually worse than ending it,
	 * but that is the app's call to make. */
	if (g_nm.hangup_on_fail && lc->bc && lc->acct && lc->acct->ua)
		ua_hangup(lc->acct->ua, lc->bc, 0, NULL);
}

/* Is any call still owed a verdict — and how soon does the soonest one need
 * one?  A stall repair is judged on a shorter clock than a handover, and the
 * verify timer is shared, so the interval is whatever the most impatient
 * pending call asks for. */
static bool migration_pending(uint32_t *tick_ms)
{
	struct echosdk_call *snap[BSDK_NET_MAX_SNAP];
	size_t n = snapshot_calls(snap, RE_ARRAY_SIZE(snap));
	uint32_t tick = verify_tick_ms();
	bool pending = false;

	for (size_t i = 0; i < n; i++) {
		struct echosdk_call *lc = snap[i];

		if (lc->net_mig_gen != g_nm.gen)
			continue;
		if (lc->net_mig_state != BSDK_MIG_STALLED &&
		    lc->net_mig_state != BSDK_MIG_WAIT_ADDR &&
		    lc->net_mig_state != BSDK_MIG_DEFERRED &&
		    lc->net_mig_state != BSDK_MIG_SENT)
			continue;

		pending = true;
		if (verify_tick_for(lc) < tick)
			tick = verify_tick_for(lc);
	}

	if (tick_ms)
		*tick_ms = tick;

	return pending;
}

static void arm_verify(void)
{
	uint32_t tick = verify_tick_ms();

	if (migration_pending(&tick))
		tmr_start(&g_nm.tmr_verify, tick, verify_handler, NULL);
}

/* Send (or re-send) the re-INVITE that moves this call's media. */
static void send_migration(struct echosdk_call *lc)
{
	int err;

	if (!lc->bc) {
		lc->net_mig_state = BSDK_MIG_IDLE;
		return;
	}

	/* An early dialog has no negotiated SDP to refresh, and a dialog with
	 * an offer already in flight may not send a second one (RFC 3261 §14.1).
	 * Both resolve themselves — wait and retry rather than tearing the call
	 * down, which is what baresip's own uag_reset_transp() would do here. */
	if (!call_refresh_allowed(lc->bc)) {
		if (lc->net_mig_state != BSDK_MIG_DEFERRED) {
			lc->net_mig_state = BSDK_MIG_DEFERRED;
			netmon_emit(ECHOSDK_NET_CALL_DEFERRED, lc, lc->acct, 0,
			            lc->net_mig_tries + 1u);
		}
		return;
	}

	/* Hold the offer while the media encryption is still handshaking.
	 *
	 * the ICE module holds its candidate re-offer for exactly this reason, and
	 * measured across 22 calls that a re-INVITE landing mid-DTLS is the
	 * single strongest predictor of a permanently silent call: the handshake
	 * is still retransmitting when the address changes under it, the retries
	 * go into the void, `menc_secure` is never set, and stream_is_ready()
	 * stays false for the life of the call.  A migration offer is the same
	 * re-INVITE arriving at the same moment — it was simply reaching the
	 * dialog by a different route, so it was never covered.
	 *
	 * DEFERRED is the right state for it: the verify tick already re-runs
	 * send_migration() for a deferred call, so the offer goes out on the
	 * first tick after the handshake completes, and the hold costs nothing
	 * on a call that is not handshaking at all.  Deferring here rather than
	 * below the counter also means the wait does not spend an attempt.
	 *
	 * Bounded, because a call configured for DTLS whose peer answered plain
	 * RTP never reads as secure and would otherwise never migrate.  The
	 * bound is the length of the handshake ladder itself: dtls_srtp waits
	 * 1200 ms on the first attempt and 2500 ms on each of four restarts
	 * (cmake/patches/dtls_srtp-state.new), so after 12 s the module has
	 * given up and there is no handshake left to protect.  Same figure as
	 * BSDK_ICE_REOFFER_SECURE_DEADLINE_MS and BSDK_MEDIA_SETUP_GRACE_MS, for
	 * the same reason.
	 *
	 * Unlike the ICE module's re-offer hold, the deadline sends rather than drops.  A candidate
	 * re-offer is an optimisation and can be abandoned; a migration offer is
	 * the recovery — it carries the address (and, for ICE, the freshly
	 * gathered candidates) that a handshake failing because the path moved
	 * needs in order to succeed.  Holding it forever guarantees the silence
	 * it was trying to prevent. */
	if (bsdk_media_setup_pending(lc)) {
		uint64_t now = tmr_jiffies();

		if (!lc->net_mig_hs_since)
			lc->net_mig_hs_since = now;

		if (now - lc->net_mig_hs_since < BSDK_NET_MIG_SECURE_HOLD_MS) {
			if (lc->net_mig_state != BSDK_MIG_DEFERRED) {
				lc->net_mig_state = BSDK_MIG_DEFERRED;
				debug("EchoSDK/netmon: media encryption is still"
				      " handshaking — holding the migration"
				      " re-INVITE that would break it\n");
				netmon_emit(ECHOSDK_NET_CALL_DEFERRED, lc,
				            lc->acct, 0,
				            lc->net_mig_tries + 1u);
			}
			return;
		}

		info("EchoSDK/netmon: media encryption still not secure after"
		     " %u ms — the handshake ladder has run out, so the"
		     " migration goes ahead\n",
		     (unsigned)BSDK_NET_MIG_SECURE_HOLD_MS);

		/* net_mig_hs_since is deliberately left standing.  This
		 * generation has already waited the ladder out once, and a
		 * later attempt within it must not wait it out again — six
		 * attempts each paying a fresh 12 s would put the call off the
		 * air for over a minute.  It is cleared when a generation
		 * starts, and below whenever the handshake is actually done. */
	}
	else {
		lc->net_mig_hs_since = 0;
	}

	lc->net_rx_at_mig = rx_packets(lc);
	lc->net_mig_tries++;

	/* This address has now had its turn, whether or not it works out — a
	 * retry moves on to the next one.  See next_laddr(). */
	laddr_remember(lc, &lc->net_mig_laddr);

	/* An ICE call cannot be migrated by rewriting the session address alone:
	 * ice.c owns the *media* address (it writes the selected candidate there,
	 * and a media-level `c=` line overrides the session one per RFC 4566
	 * §5.7), and nothing re-gathers, so the offer would re-advertise the
	 * network the call just left.  Restart ICE instead — new credentials, a
	 * fresh gather on the interface that now has the route, and the re-INVITE
	 * follows from the gather.  See src/ice/ice_mod.c.
	 *
	 * Only when the address actually moved: a WebSocket call whose path is
	 * unchanged is re-INVITEd purely to re-bind the dialog to the new
	 * connection (see call_signals_over_ws), and putting the media through an
	 * ICE restart for that would interrupt audio that is working. */
	if (lc->net_mig_path_moved) {
		err = bsdk_ice_restart(lc->bc, &lc->net_mig_laddr);

		if (!err || err == EALREADY) {
			/* EALREADY: a restart from an earlier attempt is still
			 * gathering.  Its offer is still coming and is bounded by
			 * the gathering deadline, so wait for it rather than
			 * sending a second, worse offer on top. */
			lc->net_ice_restarted = true;
			lc->net_mig_state     = BSDK_MIG_SENT;
			lc->net_mig_due       = tmr_jiffies() + restart_grace_ms()
			                      + verify_tick_for(lc);
			netmon_emit(ECHOSDK_NET_CALL_MIGRATING, lc, lc->acct, 0,
			            lc->net_mig_tries);
			return;
		}

		/* ENOENT is the ordinary answer for a call with no ICE at all —
		 * the plain re-offer below is exactly right for it.  Anything
		 * else means this call does have ICE that could not be
		 * restarted, so its candidates really are stale: say so once,
		 * before the app has to sit through the retries. */
		if (err != ENOENT && !lc->net_ice_stale_sent) {
			lc->net_ice_stale_sent = true;
			netmon_emit(ECHOSDK_NET_CALL_ICE_STALE, lc, lc->acct,
			            err, lc->net_mig_tries);
		}
	}

	/* Rewrites the SDP session address and sends the re-INVITE.  A 491
	 * glare or a 401/407 challenge is retried by libre's sipsess layer. */
	err = call_reset_transp(lc->bc, &lc->net_mig_laddr);
	if (err) {
		if (lc->net_mig_tries >= max_attempts_for(lc))
			fail_migration(lc, err);
		else
			lc->net_mig_state = BSDK_MIG_DEFERRED;
		return;
	}

	lc->net_mig_state = BSDK_MIG_SENT;
	lc->net_mig_due   = tmr_jiffies() + verify_tick_for(lc);
	netmon_emit(ECHOSDK_NET_CALL_MIGRATING, lc, lc->acct, 0,
	            lc->net_mig_tries);

	if (!g_nm.verify_ms) {
		lc->net_mig_state = BSDK_MIG_DONE;
		netmon_emit(ECHOSDK_NET_CALL_MIGRATED, lc, lc->acct, 0,
		            lc->net_mig_tries);
	}
}

/* Re-offer after an attempt that did not bring media back.
 *
 * The difference from send_migration() is where it offers from: the address
 * just used has been given its chance and did not carry RTP, so the next
 * attempt takes the next local address rather than the same one.  When there
 * is only one address to choose from this is exactly the old behaviour. */
static void retry_migration(struct echosdk_call *lc)
{
	struct sa next;

	laddr_remember(lc, &lc->net_mig_laddr);

	if (rotate_laddr(lc) && sa_isset(&lc->net_mig_laddr, SA_ADDR) &&
	    next_laddr(lc, sa_af(&lc->net_mig_laddr), &next) &&
	    !sa_cmp(&next, &lc->net_mig_laddr, SA_ADDR)) {

		info("EchoSDK/netmon: no media on %j after %u ms —"
		     " re-offering from %j\n",
		     &lc->net_mig_laddr, verify_tick_for(lc), &next);

		sa_cpy(&lc->net_mig_laddr, &next);
	}

	send_migration(lc);
}


/**
 * Park a call whose new source address cannot be determined yet.
 *
 * The routing table can lag the address change by a beat — very likely during
 * Wi-Fi→cellular, which is exactly when this runs.  The call MUST already be
 * stamped with the current generation, otherwise verify_handler() skips it and
 * the call is stranded on its old SDP address with nothing to notice.
 */
static void wait_for_addr(struct echosdk_call *lc)
{
	if (lc->net_mig_state == BSDK_MIG_WAIT_ADDR)
		return;

	lc->net_mig_state = BSDK_MIG_WAIT_ADDR;
	netmon_emit(ECHOSDK_NET_CALL_DEFERRED, lc, lc->acct, 0,
	            lc->net_mig_tries + 1u);
}

static void start_migration(struct echosdk_call *lc)
{
	struct stream   *strm;
	const struct sa *raddr, *old;
	struct sa        laddr, ice_laddr;

	if (!lc->bc) {
		lc->net_mig_state = BSDK_MIG_IDLE;
		return;
	}

	/* Claim the call for this handover generation BEFORE any bail-out below.
	 * apply_handover() has already cleared reset_pending and force by the
	 * time we run, so netmon_trigger() will not re-enter on its own; an
	 * unstamped call is invisible to verify_handler() and would be dropped
	 * silently.  Guarded so a retry from verify_handler() does not reset the
	 * attempt counter and loop forever. */
	if (lc->net_mig_gen != g_nm.gen) {
		lc->net_mig_gen   = g_nm.gen;
		lc->net_mig_tries = 0;
		lc->net_mig_due   = 0;
		lc->net_mig_hs_since   = 0;
		lc->net_ice_stale_sent = false;
		lc->net_ice_restarted  = false;
		lc->net_mig_path_moved = false;
		/* A network change supersedes any stall repair in progress: the
		 * cause is known now, and the handover's clock and budget are
		 * the right ones for it. */
		lc->net_mig_stall      = false;
		lc->net_mig_force_restart = false;
		lc->net_mig_ntried     = 0;
		/* Clock the audio outage from the moment the network changed, not
		 * from the re-INVITE — the gap the user hears starts earlier. */
		lc->net_mig_start = g_nm.handover_start ? g_nm.handover_start
		                                        : tmr_jiffies();
		sa_init(&lc->net_mig_laddr, AF_UNSPEC);
	}

	strm = call_audio(lc->bc) ? audio_strm(call_audio(lc->bc)) : NULL;
	if (!strm) {
		wait_for_addr(lc);
		return;
	}

	/* Prefer the address RTP is actually being sent to (learned from the
	 * peer) over the one in the SDP. */
	raddr = stream_raddr(strm);
	if (!raddr || !sa_isset(raddr, SA_ADDR))
		raddr = sdp_media_raddr(stream_sdpmedia(strm));
	if (!raddr || !sa_isset(raddr, SA_ADDR)) {
		wait_for_addr(lc);   /* no peer media address yet */
		return;
	}

	/* Ask the routing table which source address now reaches the peer. */
	if (net_dst_source_addr_get(raddr, &laddr) ||
	    !sa_isset(&laddr, SA_ADDR)) {
		wait_for_addr(lc);   /* route not installed yet */
		return;
	}

	/* A retry offers from somewhere else.  The routing table is asked first
	 * and answers the same thing every time, so without this the second
	 * attempt re-offers the address whose media just died. */
	if (rotate_laddr(lc) && laddr_tried(lc, &laddr)) {
		struct sa next;

		if (next_laddr(lc, sa_af(&laddr), &next) &&
		    !sa_cmp(&next, &laddr, SA_ADDR)) {
			info("EchoSDK/netmon: %j did not carry media —"
			     " offering from %j instead\n", &laddr, &next);
			sa_cpy(&laddr, &next);
		}
	}

	/* What address is this call's media actually leaving from?
	 *
	 * For an ICE call that is the *base* of the selected local candidate,
	 * not baresip's session address and not the candidate itself: a
	 * server- or peer-reflexive candidate carries the NAT's address, which
	 * never equals anything net_dst_source_addr_get() can return, so
	 * comparing against it reads as "the path moved" on every single
	 * check — and routes every stall through an ICE restart, which is the
	 * trade the comment below exists to avoid. */
	if (!lc->bc || !bsdk_ice_call_laddr(lc->bc, &ice_laddr))
		old = call_laddr(lc->bc);
	else
		old = &ice_laddr;
	if (old && sa_cmp(&laddr, old, SA_ADDR) && !call_signals_over_ws(lc) &&
	    !lc->net_mig_stall) {
		lc->net_mig_state = BSDK_MIG_IDLE;   /* same path — no re-INVITE */
		return;
	}

	/* Over WebSocket the re-INVITE is needed even on an unchanged path: it is
	 * what re-binds the dialog to the connection the transport reset just
	 * created.  See call_signals_over_ws().
	 *
	 * Whether the media path moved is a different question from whether a
	 * re-INVITE is owed, and send_migration() needs the first one: it decides
	 * between an ICE restart and a plain re-offer. */
	if (lc->net_mig_stall) {
		/* A stall repair restarts ICE only when it is actually moving the
		 * media somewhere — that is, when the rotation above picked a
		 * different local address than the one the call is using.
		 *
		 * It used to claim the path had moved unconditionally, purely to
		 * route every stall through the ICE restart in send_migration()
		 * on the reasoning that a plain re-offer under the old
		 * credentials is ignored by some peers.  Against a peer that
		 * answers a restart offer with `a=connection:new` — Asterisk /
		 * FreePBX does, on every one — that trade is a bad one: it tears
		 * down and rebuilds its RTP instance, our connectivity checks
		 * race the rebuild and are answered `401 Unauthorized`, the check
		 * list goes Failed with an empty valid list, and nothing ever
		 * nominates a pair again.  Five restarts across three calls
		 * on-device (2026-09-10), 401 every time, each call permanently
		 * silent afterwards — while a browser (sip.js) on the same
		 * network and the same PBX is fine, because nothing in a browser
		 * stack restarts ICE four seconds into a healthy call.
		 *
		 * So a stall on an address that has not moved gets the plain
		 * re-offer below, which refreshes the media path without asking
		 * the peer to throw its ICE session away.  A stall that *did*
		 * rotate to another interface still restarts: there the new
		 * candidates are the entire point, and the old ones really are
		 * wrong. */
		lc->net_mig_path_moved = lc->net_mig_force_restart ||
		                         (old && !sa_cmp(&laddr, old, SA_ADDR));

		if (lc->net_mig_force_restart) {
			info("EchoSDK/netmon: ICE reported the path failed —"
			     " restarting ICE from %j\n", &laddr);
		}
		else if (!lc->net_mig_path_moved) {
			debug("EchoSDK/netmon: media stalled on %j — re-offering"
			      " without an ICE restart (the address has not"
			      " moved)\n", &laddr);
		}
	}
	else if (old && sa_cmp(&laddr, old, SA_ADDR)) {
		debug("EchoSDK/netmon: same path but WS transport was reset —"
		      " re-INVITE to refresh the Contact\n");
	}
	else {
		lc->net_mig_path_moved = true;
	}

	sa_cpy(&lc->net_mig_laddr, &laddr);
	send_migration(lc);
}

/* Fail every call this handover still owed a migration to.
 *
 * Called when the transport reset has run out of attempts.  Those calls were
 * never re-INVITEd — apply_handover() bails before migrate_calls() — so their
 * SDP still advertises an address the device has left and their audio is gone.
 * Without this they sit in that state silently: the generation is never bumped,
 * so verify_handler() does not see them, and no timer is armed to pick them up.
 * The app is told, and can redial or show the user something truthful. */
static void fail_pending_migrations(int err)
{
	struct echosdk_call *snap[BSDK_NET_MAX_SNAP];
	size_t n = snapshot_calls(snap, RE_ARRAY_SIZE(snap));

	for (size_t i = 0; i < n; i++) {
		struct echosdk_call *lc = snap[i];

		/* Only calls that were live when this round started and have not
		 * already reached a terminal migration state. */
		if (!lc->bc || lc->net_mig_state == BSDK_MIG_DONE ||
		    lc->net_mig_state == BSDK_MIG_FAILED)
			continue;

		/* A call the handover never got as far as stamping still needs the
		 * clock, or the event reports a zero-length outage. */
		if (!lc->net_mig_start)
			lc->net_mig_start = g_nm.handover_start;

		fail_migration(lc, err);
	}
}

static void migrate_calls(void)
{
	struct echosdk_call *snap[BSDK_NET_MAX_SNAP];
	size_t n = snapshot_calls(snap, RE_ARRAY_SIZE(snap));

	for (size_t i = 0; i < n; i++)
		start_migration(snap[i]);

	arm_verify();
}

/**
 * Progress tick for calls that are mid-migration.
 *
 * DEFERRED — the dialog was not refreshable; retry until it is.
 * SENT     — a re-INVITE went out; confirm RTP resumed, else re-offer.
 */
static void verify_handler(void *arg)
{
	struct echosdk_call *snap[BSDK_NET_MAX_SNAP];
	size_t n;
	(void)arg;

	if (!g_nm.started)
		return;

	n = snapshot_calls(snap, RE_ARRAY_SIZE(snap));

	for (size_t i = 0; i < n; i++) {
		struct echosdk_call *lc = snap[i];

		if (lc->net_mig_gen != g_nm.gen)
			continue;

		if (!lc->bc) {
			lc->net_mig_state = BSDK_MIG_IDLE;
			continue;
		}

		switch (lc->net_mig_state) {

		case BSDK_MIG_STALLED:
			/* Asked for from the stats tick (bsdk_netmon_call_stalled).
			 * The work happens here so the offer is never built inside
			 * another handler's stack — the same reason
			 * bsdk_netmon_call_refreshable() bounces off this timer. */
			start_migration(lc);
			break;

		case BSDK_MIG_WAIT_ADDR:
			/* Re-run discovery, not send_migration(): net_mig_laddr was
			 * never resolved, so there is nothing valid to offer yet.
			 * Full budget — see max_attempts_for(). */
			if (++lc->net_mig_tries >= g_nm.max_attempts)
				fail_migration(lc, EHOSTUNREACH);
			else
				start_migration(lc);
			break;

		case BSDK_MIG_DEFERRED:
			if (call_refresh_allowed(lc->bc))
				send_migration(lc);
			else if (++lc->net_mig_tries >= g_nm.max_attempts)
				fail_migration(lc, ETIMEDOUT);
			break;

		case BSDK_MIG_SENT:
			/* Not yet judgeable: the offer is still in flight, or (for
			 * an ICE restart) still being gathered.  The 100 ms of
			 * slack is so a tick that fires a hair early — timer
			 * jitter, or another call's refreshable bounce — does not
			 * cost this call a whole verify period. */
			if (lc->net_mig_due &&
			    tmr_jiffies() + 100 < lc->net_mig_due)
				break;

			/* Media check disabled.  The plain path reports MIGRATED
			 * as soon as the offer is sent; an ICE restart has no
			 * offer to report until the re-gather produces one, so it
			 * lands here instead — and must be taken at its word the
			 * same way, without inspecting RTP or retrying. */
			if (!g_nm.verify_ms) {
				lc->net_mig_state = BSDK_MIG_DONE;
				netmon_emit(ECHOSDK_NET_CALL_MIGRATED, lc,
				            lc->acct, 0, lc->net_mig_tries);
				break;
			}

			/* A held call carries no RTP, so the counter can never
			 * advance — the answered re-INVITE is all the confirmation
			 * available. */
			if (lc->state == ECHOSDK_CALL_HELD ||
			    call_is_onhold(lc->bc) ||
			    rx_packets(lc) > lc->net_rx_at_mig) {
				lc->net_mig_state = BSDK_MIG_DONE;
				netmon_emit(ECHOSDK_NET_CALL_MIGRATED, lc, lc->acct,
				            0, lc->net_mig_tries);
			}
			else if (lc->net_mig_tries >= max_attempts_for(lc))
				fail_migration(lc, ETIMEDOUT);
			else
				retry_migration(lc);
			break;

		default:
			break;
		}
	}

	arm_verify();
}

/**
 * An ICE generation concluded with an empty valid list.
 *
 * baresip's ice module only warns about this ("connectivity check failed") and
 * carries on: `m->complete` stays false, no estab handler runs, and no event
 * reaches the app — so a call whose media path was never validated looks
 * identical, from the outside, to one that is working.  On-device (2026-09-10)
 * that meant three calls sat fully established, SIP-healthy and silent, with
 * nothing in the API to distinguish them from a good call.
 *
 * Report it and stop there.  ice_shim.c holds the stream on the last pair that
 * *was* validated, so audio often survives a failed generation, and a call the
 * SDK cannot fix is not automatically a call the SDK should end.
 */
static bool stall_claim(struct echosdk_call *lc, bool force_restart,
                        const char *why);

void bsdk_netmon_call_ice_failed(struct call *bc, int err)
{
	struct echosdk_call *lc = bc ? bsdk_call_find(bc) : NULL;

	if (!lc)
		return;

	netmon_emit(ECHOSDK_NET_CALL_ICE_FAILED, lc, lc->acct, err,
	            lc->net_mig_tries);

	/* What a browser app does on iceConnectionState "failed": restart
	 * ICE — new credentials, a fresh gather, a re-INVITE — a bounded
	 * number of times.  The stall machinery already has the budget
	 * (BSDK_NET_STALL_ROUNDS rounds, BSDK_NET_STALL_COOLDOWN_MS apart)
	 * and the verify clock; a generation that fails while a migration is
	 * in flight is left to that migration. */
	if (g_nm.reinvite_calls)
		(void)stall_claim(lc, true, "ICE reported the path failed");
}

/**
 * Inbound RTP has stopped on an established call.
 *
 * adapt.c raises ECHOSDK_QUALITY_MEDIA_STALL when the receive counter has not
 * moved for cfg.media_stall_ms — and not while the media encryption is still
 * handshaking (BSDK_MEDIA_SETUP_GRACE_MS there, because the re-INVITE this
 * repair ends in would break the handshake it is waiting for).  This is the
 * repair that used to be missing behind that alert.  Everything the handover
 * path does applies — ask the routing table which source address reaches the
 * peer, restart ICE (new credentials, fresh gather) or re-offer, then check
 * whether RTP came back — with two differences that matter:
 *
 *   - The address has usually not moved.  A handover stops right there
 *     ("same path — no re-INVITE"); a stall repair must not, because the path
 *     is what broke.  start_migration() reads net_mig_stall for both that and
 *     the decision to take the ICE restart anyway.
 *   - Nothing announced it, so nothing will announce the next one either.
 *     The cooldown and the round cap are what stop a call whose media is
 *     never coming back from being re-INVITEd for the rest of its life.
 *
 * Called from the stats tick, on re_main.  The offer is not built here: the
 * call is marked and the shared verify timer picks it up, so nothing
 * re-enters baresip from inside another handler.
 */
void bsdk_netmon_call_stalled(struct echosdk_call *lc)
{
	if (!g_nm.started || !lc || !lc->bc)
		return;

	/* A repair (or a handover) is already running for this call. */
	if (lc->net_mig_gen == g_nm.gen &&
	    (lc->net_mig_state == BSDK_MIG_STALLED ||
	     lc->net_mig_state == BSDK_MIG_WAIT_ADDR ||
	     lc->net_mig_state == BSDK_MIG_DEFERRED ||
	     lc->net_mig_state == BSDK_MIG_SENT))
		return;

	/* A held call carries no RTP by design.  adapt.c does not report a
	 * stall for one, but hold can be entered between the two. */
	if (lc->state == ECHOSDK_CALL_HELD || lc->local_hold ||
	    call_is_onhold(lc->bc))
		return;

	/* Silence is not proof that the path is broken.
	 *
	 * The RTP counter answers "is audio arriving", never "whose fault is
	 * it".  An ICE agent that is still exchanging connectivity checks on
	 * the selected pair has just told us the path is alive in both
	 * directions — every check it sends is answered — so the silence is
	 * the far end's: a peer that muted, stopped sending without
	 * signalling hold, or is itself waiting on something.  Re-offering
	 * then repairs nothing and costs real audio: against this PBX a
	 * re-offer draws `a=connection:new`, it rebuilds its RTP instance,
	 * and the call that was merely quiet comes back silent for good
	 * (2026-09-10, five restarts across three calls).
	 *
	 * This is what a browser does by default and why sip.js survives the
	 * same PBX: nothing in a browser stack re-offers four seconds into a
	 * call whose ICE is connected.  The SDK could not make this
	 * distinction before — libre's ICE sent Binding *Indications* and
	 * never learned anything from them — and had to treat every stall as
	 * its own to fix.
	 *
	 * DISCONNECTED or FAILED is the opposite verdict and falls through to
	 * the repair; so does a call without ICE, where there is nothing to
	 * ask. */
	if (lc->bc && bsdk_ice_call_active(lc->bc)) {
		int ice = bsdk_ice_call_state(lc->bc);

		if (ice == ECHOSDK_ICE_CONNECTED || ice == ECHOSDK_ICE_COMPLETED) {
			info("EchoSDK/netmon: media stalled but ICE is still"
			     " connected — the far end has gone quiet, not our"
			     " path; not re-offering\n");
			return;
		}
	}

	(void)stall_claim(lc, false, "media stalled on an established call");
}

/* Claim the call for a repair round: the cooldown and the round cap, then
 * the same stamps a handover generation gets so start_migration() finds
 * them set.  `force_restart` routes the repair through an ICE restart
 * whatever the routing table says about the address. */
static bool stall_claim(struct echosdk_call *lc, bool force_restart,
                        const char *why)
{
	uint64_t now;

	if (!g_nm.started || !lc || !lc->bc)
		return false;

	/* A repair (or a handover) is already running for this call. */
	if (lc->net_mig_gen == g_nm.gen &&
	    (lc->net_mig_state == BSDK_MIG_STALLED ||
	     lc->net_mig_state == BSDK_MIG_WAIT_ADDR ||
	     lc->net_mig_state == BSDK_MIG_DEFERRED ||
	     lc->net_mig_state == BSDK_MIG_SENT))
		return false;

	if (lc->state == ECHOSDK_CALL_HELD || lc->local_hold ||
	    call_is_onhold(lc->bc))
		return false;

	now = tmr_jiffies();

	if (lc->net_stall_repair_at &&
	    now - lc->net_stall_repair_at < BSDK_NET_STALL_COOLDOWN_MS)
		return false;

	if (lc->net_stall_rounds >= BSDK_NET_STALL_ROUNDS) {
		debug("EchoSDK/netmon: %s again after %u repairs —"
		      " leaving it to the app\n", why, lc->net_stall_rounds);
		return false;
	}

	lc->net_stall_repair_at = now;
	lc->net_stall_rounds++;

	/* Claim the call for a repair round of its own.  Same fields a handover
	 * generation stamps, so start_migration() finds them already set and
	 * does not reset the counters underneath us. */
	lc->net_mig_gen        = g_nm.gen;
	lc->net_mig_tries      = 0;
	lc->net_mig_due        = 0;
	lc->net_mig_hs_since   = 0;
	lc->net_ice_stale_sent = false;
	lc->net_ice_restarted  = false;
	lc->net_mig_path_moved = false;
	lc->net_mig_ntried     = 0;
	lc->net_mig_stall      = true;
	lc->net_mig_force_restart = force_restart;
	lc->net_mig_start      = now;
	lc->net_mig_state      = BSDK_MIG_STALLED;
	sa_init(&lc->net_mig_laddr, AF_UNSPEC);

	info("EchoSDK/netmon: %s — repairing (round %u/%u%s)\n", why,
	     lc->net_stall_rounds, (unsigned)BSDK_NET_STALL_ROUNDS,
	     force_restart ? ", ICE restart" : "");

	tmr_start(&g_nm.tmr_verify, 1, verify_handler, NULL);
	return true;
}


/**
 * The ICE agent's liveness verdict for one call changed.
 *
 * Recorded, not acted on: the repair decisions this feeds — whether a stall
 * is ours to fix, and whether it warrants an ICE restart — belong to
 * bsdk_netmon_call_stalled() and start_migration(), which run on their own
 * clocks and are rate-limited.  Acting here would put a re-INVITE on the
 * wire from inside a STUN response handler.
 */
void bsdk_netmon_call_ice_state(struct call *bc, int prev, int now)
{
	struct echosdk_call *lc = bsdk_call_find(bc);

	if (!lc)
		return;

	lc->net_ice_state = (uint8_t)now;

	debug("EchoSDK/netmon: ICE %d -> %d\n", prev, now);
}


void bsdk_netmon_call_refreshable(struct echosdk_call *lc)
{
	if (!g_nm.started || !lc)
		return;
	if (lc->net_mig_state != BSDK_MIG_DEFERRED || lc->net_mig_gen != g_nm.gen)
		return;

	/* We are inside a baresip event emit right now; call_modify() would
	 * emit nested events from the same handler.  Bounce off the timer. */
	tmr_start(&g_nm.tmr_verify, 1, verify_handler, NULL);
}

void bsdk_netmon_call_sdp_answer(struct echosdk_call *lc)
{
	if (!g_nm.started || !lc)
		return;
	if (lc->net_mig_state != BSDK_MIG_SENT || lc->net_mig_gen != g_nm.gen)
		return;

	/* The peer took our new address.  Media has not necessarily resumed —
	 * that is what the verify tick is still watching for. */
	netmon_emit(ECHOSDK_NET_CALL_MIGRATE_ACCEPTED, lc, lc->acct, 0,
	            lc->net_mig_tries);
}

/* ── Registration refresh ────────────────────────────────────────────────── */

static void reregister_accounts(void)
{
	struct echosdk_account *snap[BSDK_NET_MAX_SNAP];
	size_t n = snapshot_accounts(snap, RE_ARRAY_SIZE(snap));

	for (size_t i = 0; i < n; i++) {
		struct echosdk_account *a = snap[i];

		/* Credentials the registrar rejected are still wrong on the new
		 * network, so this account is not part of the recovery.
		 * mark_accounts_reconnecting() already declined to move it out of
		 * FAILED for the same reason ("a terminal FAILED is not turned back
		 * into hope by a new link"); re-REGISTERing it here anyway would
		 * contradict that, and would leave it reporting FAILED to the app
		 * while carrying reconnecting = true internally.  Every other
		 * failure — transport, timeout, 5xx — is exactly what a new network
		 * might fix, so those do get the fresh attempt below. */
		if (a->reg_state == ECHOSDK_REG_FAILED &&
		    a->reg_error == ECHOSDK_ERR_AUTH)
			continue;

		/* A new network deserves a fresh attempt: an account sitting in a
		 * five-minute backoff from the old network must not wait it out. */
		tmr_cancel(&a->retry_tmr);
		a->retry_attempt = 0;

		/* This REGISTER is the reconnect, so report it as one — including
		 * for an account that was mid-retry when the network moved, and one
		 * whose handover was forced without any address change (a WebSocket
		 * re-bind).  Cleared when it is answered. */
		a->reconnecting = true;

		netmon_emit(ECHOSDK_NET_REREGISTERING, NULL, a, 0, 0);
		(void)ua_register(a->ua);
		bsdk_account_watch_registration(a);
	}
}

/* ── Handover state machine ──────────────────────────────────────────────── */

/* 1, 2, 4, 8, 16, 32 s */
static uint32_t backoff_ms(uint32_t attempt)
{
	uint32_t shift = attempt > 5 ? 5 : attempt;
	return 1000u << shift;
}

static void apply_handover(void)
{
	struct network   *net = baresip_network();
	struct usable_ctx use;
	int err;

	scan_usable(&use);

	/* Between "Wi-Fi is gone" and "cellular is up" there is no address to
	 * bind at all.  Flushing the transports here would strand the stack
	 * with nothing to re-bind onto, so hold the pending handover and keep
	 * looking with a backoff. */
	if (!use.routable && !use.linklocal) {
		if (!g_nm.down) {
			g_nm.down = true;
			g_nm.cur_laddr[0] = '\0';
			netmon_emit(ECHOSDK_NET_DOWN, NULL, NULL, 0, 0);
			mark_accounts_reconnecting();
		}
		/* Backoff for the no-address wait uses its OWN counter.  Sharing
		 * g_nm.attempt would let a long outage exhaust the reset-failure
		 * budget, so the first uag_reset_transp() failure afterwards would
		 * fail the attempt < max_attempts guard below and schedule no retry
		 * at all — leaving reset_pending true with no timer running. */
		tmr_start(&g_nm.tmr_settle, backoff_ms(g_nm.wait_ticks++),
		          settle_handler, NULL);
		return;
	}

	g_nm.wait_ticks = 0;

	/* Link-local only: enough to re-bind (some LANs really do run SIP that
	 * way) but not enough to call the network up. */
	if (!use.routable) {
		if (!g_nm.down) {
			g_nm.down = true;
			netmon_emit(ECHOSDK_NET_DOWN, NULL, NULL, 0, 0);
			mark_accounts_reconnecting();
		}
	}
	else if (g_nm.down) {
		g_nm.down = false;
		update_cur_laddr();
		netmon_emit(ECHOSDK_NET_UP, NULL, NULL, 0, 0);
	}

	/* Cellular and Wi-Fi hand out different resolvers; the old ones are
	 * unreachable and every SIP lookup would stall on them.  Skipped when
	 * the app pinned its own nameservers. */
	if (!conf_config()->net.nsc)
		net_dns_refresh(net);

	/* There is a routable address again, and the migrations below are about
	 * to re-offer each call's candidates.  The NAT mapping behind our RTP
	 * sockets almost certainly died with the old network, so ask the ICE
	 * agent to re-run its reflexive gathering first — otherwise the offer
	 * carries the mapping that has just gone away.  One STUN request per
	 * port, no new credentials, no new generation and nothing signalled on
	 * its own; whether a call needs a full ICE restart is still decided per
	 * call in start_migration().
	 *
	 * Deliberately below the no-address bail above: gathering into a dead
	 * network only leaves the generation marked un-gathered while the
	 * handover waits for an interface to appear. */
	bsdk_ice_network_changed();

	/* Flush every SIP transport and re-bind on the current addresses.  This
	 * also drops the connection hash, which is what kills the half-open
	 * TCP/TLS/WSS sockets from the old network.  reg/reinvite are passed
	 * false: we drive both ourselves so that accounts the app never asked
	 * to register stay unregistered, and so calls are re-INVITEd under our
	 * retry policy instead of being hung up. */
	err = uag_reset_transp(false, false);
	if (err) {
		g_nm.attempt++;
		netmon_emit(ECHOSDK_NET_HANDOVER_FAILED, NULL, NULL, err,
		            g_nm.attempt);
		if (g_nm.attempt < g_nm.max_attempts) {
			tmr_start(&g_nm.tmr_settle, backoff_ms(g_nm.attempt),
			          settle_handler, NULL);
		}
		else {
			/* Out of attempts.  reset_pending stays true so the next real
			 * address change or echosdk_network_changed() picks it up, but
			 * reset the counter — otherwise that next round would inherit an
			 * exhausted budget and give up immediately.  The app sees
			 * attempt == max_attempts on the final HANDOVER_FAILED. */
			g_nm.attempt = 0;

			/* Hand the accounts to the registration retry policy.  They were
			 * moved to RECONNECTING when the change was detected and no
			 * re-REGISTER is coming from here any more, so without this they
			 * would sit in a recovery nothing is driving until the next
			 * network change — however long that is. */
			handover_accounts_to_retry();

			/* Calls have no such policy to fall back on: nothing else
			 * re-INVITEs them and reset_pending alone wakes nothing when
			 * the app drives handover from the OS callback
			 * (net_monitor_interval_s == 0, the documented mobile
			 * setting).  Report them rather than leaving the app holding
			 * a call it believes is up.
			 *
			 * Before handover_start is cleared below, because that is what
			 * the reported outage is measured from. */
			fail_pending_migrations(err);

			/* Let the next round announce itself.  Leaving `announced`
			 * set suppresses its CHANGE_DETECTED, and a stale
			 * handover_start makes every later elapsed_ms cumulative —
			 * so a handover that eventually succeeds reports an outage
			 * measured from the failed round before it. */
			g_nm.announced      = false;
			g_nm.handover_start = 0;
		}
		return;
	}

	g_nm.attempt       = 0;
	g_nm.reset_pending = false;
	g_nm.force         = false;
	g_nm.announced     = false;
	g_nm.gen++;

	update_cur_laddr();
	netmon_emit(ECHOSDK_NET_TRANSPORT_RESET, NULL, NULL, 0, 0);

	reregister_accounts();

	if (g_nm.reinvite_calls)
		migrate_calls();
}

static void settle_handler(void *arg)
{
	(void)arg;

	if (!g_nm.started)
		return;

	if (scan_addresses()) {
		/* Still moving — an interface is mid-configuration.  Restart the
		 * debounce so the handover runs once, on the final address set. */
		g_nm.reset_pending = true;
		tmr_start(&g_nm.tmr_settle, g_nm.settle_ms, settle_handler, NULL);
		return;
	}

	if (!g_nm.reset_pending && !g_nm.force)
		return;

	apply_handover();
}

/* End a path check without a verdict: answers still on the wire are ignored
 * from here on (their generation no longer matches). */
static void check_abandon(void)
{
	if (!g_nm.checking)
		return;

	tmr_cancel(&g_nm.tmr_check);
	g_nm.checking = false;
	g_nm.check_gen++;
}

static void netmon_trigger(bool forced)
{
	if (!g_nm.started)
		return;

	if (scan_addresses())
		g_nm.reset_pending = true;
	if (forced)
		g_nm.force = true;

	if (!g_nm.reset_pending && !g_nm.force)
		return;

	if (!g_nm.announced) {
		/* The handover about to run answers whatever a pending check
		 * was asking. */
		check_abandon();

		g_nm.announced      = true;
		g_nm.handover_start = tmr_jiffies();
		update_cur_laddr();
		netmon_emit(ECHOSDK_NET_CHANGE_DETECTED, NULL, NULL, 0, 0);
		mark_accounts_reconnecting();
	}

	tmr_start(&g_nm.tmr_settle, g_nm.settle_ms, settle_handler, NULL);
}

/* ── Path check ──────────────────────────────────────────────────────────────
 *
 * echosdk_network_changed() says the OS saw *something* move.  On a phone most
 * of those reports are not a move of ours: the platform monitor replays the
 * current network every time it is registered (every engine attach of a
 * Flutter app, a push-woken headless engine included), iOS reports a VPN or a
 * second radio appearing next to an unchanged primary, and hosts call it on
 * resume or after a push as a precaution.  Each one used to be a full
 * handover — RECONNECTING, a transport flush, a re-REGISTER, a re-INVITE per
 * call — on a path that never broke, which is the "always reconnecting" the
 * user sees.
 *
 * A browser does none of that.  It keeps its WebSocket until the WebSocket
 * itself fails, and ICE carries the media across the network underneath.
 *
 * So when the address set is exactly the one the transports are bound to, ask
 * the path instead of assuming it died: one OPTIONS per registered account,
 * over the connection it already has.  An answer from every account means the
 * connection the registrar routes us by is alive, and nothing is done.  Any
 * failure, or silence for BSDK_NET_CHECK_MS, and the report is handled as the
 * handover it was sent as.  A dead socket is precisely what the check finds:
 * the request goes into a connection with nobody on the far end and nothing
 * comes back.
 *
 * Only asked where an answer proves enough:
 *   - connection-oriented transports.  A registrar reaches a WS/TCP/TLS
 *     client down the connection it registered on, so a live connection is a
 *     live binding.  Over UDP the answer comes back through whatever NAT
 *     mapping exists now, while the registrar may still hold the old one.
 *     "The" connection matters: when the one a binding lives on closes, the
 *     next request quietly opens a new one, which would answer this probe
 *     for a binding that is gone.  libre's sipreg watches its connection and
 *     fails the registration the moment it closes (patch-re-sources.cmake,
 *     hunk P), so such an account is already RECONNECTING by the time a
 *     check could run, and the check below declines it.
 *   - every account already REGISTERED.  One that is recovering is owed the
 *     fresh attempt a handover gives it (reregister_accounts()).
 *   - every live call on ICE.  The agent keeps checking its own pair and
 *     reports the path through ECHOSDK_EV_ICE_STATE; a plain-RTP call only
 *     moves when it is re-INVITEd, so it keeps the unconditional handover.
 */

static void check_done(bool ok)
{
	check_abandon();

	if (ok) {
		info("EchoSDK/netmon: network change reported, but every "
		     "connection to the registrar still answers — nothing "
		     "to hand over\n");
		return;
	}

	info("EchoSDK/netmon: network change reported and the path "
	     "check failed — handing over\n");
	netmon_trigger(true);
}

static void check_resp_handler(int err, const struct sip_msg *msg, void *arg)
{
	uint32_t gen = (uint32_t)(uintptr_t)arg;

	if (!g_nm.started || !g_nm.checking || gen != g_nm.check_gen)
		return;

	/* Any response is an answer, as for the keepalive probe: a proxy that
	 * refuses OPTIONS still had to receive it over this connection. */
	if (err || !msg) {
		check_done(false);
		return;
	}

	if (g_nm.check_left && --g_nm.check_left == 0)
		check_done(true);
}

static void check_timeout(void *arg)
{
	(void)arg;

	if (!g_nm.started || !g_nm.checking)
		return;

	check_done(false);
}

/* @return true when a check is running and will answer this report. */
static bool start_path_check(void)
{
	struct echosdk_account *acc[BSDK_NET_MAX_SNAP];
	struct echosdk_call    *calls[BSDK_NET_MAX_SNAP];
	size_t n, nc, i;
	uint32_t gen;

	/* A report landing while one is out is answered by that one. */
	if (g_nm.checking)
		return true;

	n = snapshot_accounts(acc, RE_ARRAY_SIZE(acc));
	if (!n)
		return false;

	for (i = 0; i < n; i++) {
		if (acc[i]->reg_state != ECHOSDK_REG_REGISTERED)
			return false;
		if (acc[i]->parsed_transport == ECHOSDK_TRANSPORT_UDP)
			return false;
	}

	nc = snapshot_calls(calls, RE_ARRAY_SIZE(calls));
	for (i = 0; i < nc; i++) {
		if (!call_uses_ice(calls[i]))
			return false;
	}

	g_nm.checking   = true;
	g_nm.check_left = (uint32_t)n;
	gen = ++g_nm.check_gen;
	tmr_start(&g_nm.tmr_check, BSDK_NET_CHECK_MS, check_timeout, NULL);

	for (i = 0; i < n; i++) {
		int err = bsdk_account_probe_send(acc[i], check_resp_handler,
		                                  (void *)(uintptr_t)gen);

		/* Ended from inside the send — a synchronous failure already
		 * started the handover. */
		if (!g_nm.checking || gen != g_nm.check_gen)
			return true;

		if (err) {
			/* Cannot even ask: no better off than before the check. */
			check_abandon();
			return false;
		}
	}

	return true;
}

static void poll_handler(void *arg)
{
	(void)arg;

	if (!g_nm.started)
		return;

	netmon_trigger(false);

	if (g_nm.poll_s)
		tmr_start(&g_nm.tmr_poll, g_nm.poll_s * 1000, poll_handler, NULL);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bsdk_netmon_init(void)
{
	const echosdk_config_t *cfg = &g_bsdk.cfg;

	memset(&g_nm, 0, sizeof(g_nm));
	tmr_init(&g_nm.tmr_poll);
	tmr_init(&g_nm.tmr_settle);
	tmr_init(&g_nm.tmr_verify);
	tmr_init(&g_nm.tmr_check);

	g_nm.poll_s         = cfg->net_monitor_interval_s;
	g_nm.settle_ms      = cfg->net_settle_ms ? cfg->net_settle_ms
	                                         : BSDK_NET_SETTLE_MS;
	g_nm.verify_ms      = cfg->net_verify_ms;   /* 0 = media check disabled */
	g_nm.max_attempts   = cfg->net_max_attempts ? cfg->net_max_attempts
	                                            : BSDK_NET_MAX_ATTEMPT;
	g_nm.reinvite_calls = cfg->net_reinvite_calls;
	g_nm.hangup_on_fail = cfg->net_hangup_on_migration_failure;
	g_nm.ice_handover   = cfg->net_ice_handover;
	g_nm.started        = true;

	update_cur_laddr();
	g_nm.down = !have_routable_addr();

	if (g_nm.poll_s)
		tmr_start(&g_nm.tmr_poll, g_nm.poll_s * 1000, poll_handler, NULL);

	return 0;
}

/* Two-step teardown.  bsdk_netmon_stop() is called from the app thread at the
 * top of echosdk_shutdown(), before UAs and calls are torn down: the handlers
 * bail on !started, so no timer that fires meanwhile touches a half-freed
 * account or call.  The timers themselves are cancelled from
 * bsdk_netmon_close() once the re loop has stopped and nothing can fire. */
void bsdk_netmon_stop(void)
{
	g_nm.started = false;
}

void bsdk_netmon_close(void)
{
	g_nm.started = false;
	tmr_cancel(&g_nm.tmr_poll);
	tmr_cancel(&g_nm.tmr_settle);
	tmr_cancel(&g_nm.tmr_verify);
	tmr_cancel(&g_nm.tmr_check);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

static void changed_fn(void *arg)
{
	(void)arg;

	if (!g_nm.started)
		return;

	/* Already handing over, or waiting for an address to appear: the report
	 * joins that round, as it always has. */
	if (g_nm.announced || g_nm.reset_pending || g_nm.force || g_nm.down) {
		netmon_trigger(true);
		return;
	}

	/* The addresses moved.  That is a handover whatever the connections
	 * would say — every transport is bound to the old set. */
	if (scan_addresses()) {
		g_nm.reset_pending = true;
		netmon_trigger(true);
		return;
	}

	if (start_path_check())
		return;

	netmon_trigger(true);
}

int echosdk_network_changed(void)
{
	return bsdk_dispatch(changed_fn, NULL);
}

typedef struct {
	uint32_t seconds;
} monitor_ctx_t;

static void set_monitor_fn(void *arg)
{
	monitor_ctx_t *ctx = arg;

	g_nm.poll_s = ctx->seconds;
	tmr_cancel(&g_nm.tmr_poll);
	if (g_nm.poll_s)
		tmr_start(&g_nm.tmr_poll, g_nm.poll_s * 1000, poll_handler, NULL);
}

int echosdk_network_set_monitor_interval(uint32_t seconds)
{
	monitor_ctx_t ctx = { .seconds = seconds };
	return bsdk_dispatch_sync(set_monitor_fn, &ctx);
}

typedef struct {
	bool reinvite;
	bool hangup;
} policy_ctx_t;

static void set_policy_fn(void *arg)
{
	policy_ctx_t *ctx = arg;

	g_nm.reinvite_calls = ctx->reinvite;
	g_nm.hangup_on_fail = ctx->hangup;
}

int echosdk_network_set_handover_policy(bool reinvite_calls,
                                         bool hangup_on_failure)
{
	policy_ctx_t ctx = { .reinvite = reinvite_calls,
	                     .hangup   = hangup_on_failure };
	return bsdk_dispatch_sync(set_policy_fn, &ctx);
}

typedef struct {
	char  *buf;
	size_t sz;
} laddr_ctx_t;

static void local_addr_fn(void *arg)
{
	laddr_ctx_t *ctx = arg;

	update_cur_laddr();
	str_ncpy(ctx->buf, g_nm.cur_laddr, ctx->sz);
}

int echosdk_network_local_addr(char *buf, size_t sz)
{
	laddr_ctx_t ctx = { .buf = buf, .sz = sz };

	if (!buf || sz == 0)
		return ECHOSDK_ERR_INVAL;

	buf[0] = '\0';
	return bsdk_dispatch_sync(local_addr_fn, &ctx);
}

static void is_up_fn(void *arg)
{
	bool *out = arg;
	*out = have_routable_addr();
}

bool echosdk_network_is_up(void)
{
	bool up = false;

	if (bsdk_dispatch_sync(is_up_fn, &up))
		return false;

	return up;
}
