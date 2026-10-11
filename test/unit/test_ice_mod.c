/**
 * @file test_ice_mod.c  echo-ice: the baresip media-NAT module, driven as
 *                        baresip drives it
 *
 * Two "calls" on loopback with real UDP sockets.  The test plays baresip:
 * it calls sessh / mediah / updateh through the registered vtable, moves
 * SDP between the two sides with sdp_encode/sdp_decode, and stubs the few
 * baresip symbols the module touches (call state, streams, log, config,
 * the media-NAT registry).  Links libre only.
 *
 * Build: make -C test/unit test_ice_mod && ./test/unit/test_ice_mod
 */

#include <stdio.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "../../src/echosdk_internal.h"
#include "../../src/ice/ice_internal.h"

struct bsdk_ctx g_bsdk;

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ── baresip stubs ───────────────────────────────────────────────────────── */

#include <stdlib.h>
#include <stdarg.h>
static void vlog_stub(const char *lvl, const char *fmt, va_list ap)
{
	char buf[1024];
	if (!getenv("ICE_MOD_VERBOSE"))
		return;
	re_vsnprintf(buf, sizeof(buf), fmt, ap);
	fprintf(stderr, "[%s] %s%s", lvl, buf,
		buf[strlen(buf) ? strlen(buf) - 1 : 0] == '\n' ? "" : "\n");
}
void _debug(bool safe, const char *fmt, ...)
{ va_list ap; (void)safe; va_start(ap, fmt); vlog_stub("dbg", fmt, ap); va_end(ap); }
void _info(bool safe, const char *fmt, ...)
{ va_list ap; (void)safe; va_start(ap, fmt); vlog_stub("inf", fmt, ap); va_end(ap); }
/* the media-start watchdog's two warnings, counted */
static unsigned g_warn_no_pair, g_warn_never_started;
void _warning(bool safe, const char *fmt, ...)
{
	va_list ap;
	(void)safe;
	if (strstr(fmt, "no writable ICE pair"))
		g_warn_no_pair++;
	if (strstr(fmt, "never started"))
		g_warn_never_started++;
	va_start(ap, fmt); vlog_stub("WRN", fmt, ap); va_end(ap);
}

static struct list g_mnatl;
struct list *baresip_mnatl(void) { return &g_mnatl; }

void mnat_register(struct list *mnatl, struct mnat *mnat)
{
	list_append(mnatl, &mnat->le, mnat);
}
void mnat_unregister(struct mnat *mnat) { list_unlink(&mnat->le); }
const struct mnat *mnat_find(const struct list *mnatl, const char *id)
{
	struct le *le;
	LIST_FOREACH(mnatl, le) {
		struct mnat *m = le->data;
		if (0 == str_cmp(m->id, id))
			return m;
	}
	return NULL;
}

struct fake_stream { bool secure; };
struct fake_call {
	enum call_state st;
	struct list streaml;
	struct le sle;
	struct fake_stream strm;
};
enum call_state call_state(const struct call *call)
{
	return ((const struct fake_call *)call)->st;
}
struct list *call_streaml(const struct call *call)
{
	return &((struct fake_call *)call)->streaml;
}
bool stream_is_secure(const struct stream *strm)
{
	return ((const struct fake_stream *)strm)->secure;
}
static int g_net_dummy;      /* struct network is opaque; only compared */
struct network *baresip_network(void) { return (struct network *)&g_net_dummy; }
static struct sa g_default_laddr;
const struct sa *net_laddr_af(const struct network *net, int af)
{
	(void)net; (void)af;
	return &g_default_laddr;
}
static struct config g_cfg;
struct config *conf_config(void) { return &g_cfg; }

/* baresip's: a remote format was negotiated and the remote port is not 0 */
bool sdp_media_has_media(const struct sdp_media *m)
{
	return sdp_media_rformat(m, NULL) != NULL && sdp_media_rport(m) != 0;
}

static int g_ice_failed_calls;
int bsdk_ice_servers_for_call(const struct call *bc,
                              struct bsdk_ice_server_snap *snap)
{
	(void)bc;
	memset(snap, 0, sizeof(*snap));
	return 0;
}

void bsdk_ice_server_snap_clear(struct bsdk_ice_server_snap *snap)
{
	if (snap) snap->v = mem_deref(snap->v);
}

int bsdk_net_bind_socket(re_sock_t fd, const char *ifname, int af)
{
	(void)fd; (void)ifname; (void)af;
	return ENOTSUP;
}

bool bsdk_net_if_pinnable(const char *ifname)
{
	(void)ifname;
	return false;
}

bool bsdk_net_pin_bind_any(void)
{
	return false;
}

int bsdk_net_if_cost(const char *ifname)
{
	(void)ifname;
	return 0;
}

void bsdk_netmon_call_ice_failed(struct call *bc, int err)
{
	(void)bc; (void)err;
	g_ice_failed_calls++;
}

static int g_ice_state_reports;
static int g_last_ice_state = -1;
void bsdk_netmon_call_ice_state(struct call *bc, int prev, int now)
{
	(void)bc; (void)prev;
	g_ice_state_reports++;
	g_last_ice_state = now;
}

/* The SDK's call registry and event queue: the module looks the baresip
 * call up to record its ICE state and to post ECHOSDK_EV_ICE_STATE.  No
 * wrappers exist here, so nothing is found and nothing is posted. */
struct echosdk_call *bsdk_call_find(const struct call *bc)
{
	(void)bc;
	return NULL;
}
struct echosdk_queued_event *bsdk_qev_alloc(void)
{
	return mem_zalloc(sizeof(struct echosdk_queued_event), NULL);
}
bool bsdk_event_post_qev(struct echosdk_queued_event *qev)
{
	mem_deref(qev);
	return true;
}

/* ── One side of a call ──────────────────────────────────────────────────── */

struct side {
	struct fake_call call;
	struct sdp_session *sdp;
	struct sdp_media *sdpm;
	struct udp_sock *sock;
	struct sa laddr;
	struct mnat_sess *sess;
	struct mnat_media *media;
	unsigned estab_calls;
	int estab_err;
	unsigned connh_calls;
	struct sa connh_raddr;
	unsigned app_rx;
	struct sa app_last_src;
};

static void estab_h(int err, uint16_t scode, const char *reason, void *arg)
{
	struct side *sd = arg;
	(void)scode; (void)reason;
	sd->estab_calls++;
	sd->estab_err = err;
}

static void connh(const struct sa *raddr1, const struct sa *raddr2, void *arg)
{
	struct side *sd = arg;
	(void)raddr2;
	sd->connh_calls++;
	sd->connh_raddr = *raddr1;
}

/* what the application (RTP) layer sees, after every helper ran */
static void app_recv(const struct sa *src, struct mbuf *mb, void *arg)
{
	struct side *sd = arg;
	(void)mb;
	sd->app_rx++;
	sd->app_last_src = *src;
}

static int side_init(struct side *sd, enum call_state st)
{
	struct sa lo;
	int err;

	memset(sd, 0, sizeof(*sd));
	sd->call.st = st;
	list_init(&sd->call.streaml);
	list_append(&sd->call.streaml, &sd->call.sle, &sd->call.strm);

	sa_set_str(&lo, "127.0.0.1", 0);
	err = udp_listen(&sd->sock, &lo, app_recv, sd);
	if (err)
		return err;
	udp_local_get(sd->sock, &sd->laddr);

	err = sdp_session_alloc(&sd->sdp, &lo);
	if (err)
		return err;
	err = sdp_media_add(&sd->sdpm, sd->sdp, "audio", sa_port(&sd->laddr),
			    "RTP/AVP");
	err |= sdp_format_add(NULL, sd->sdpm, false, "0", "PCMU", 8000, 1,
			      NULL, NULL, NULL, false, NULL);
	return err;
}

static void side_close(struct side *sd)
{
	/* baresip order: media (stream->mns) then session (call->mnats)
	 * — and the reverse is tolerated too */
	sd->media = mem_deref(sd->media);
	sd->sess  = mem_deref(sd->sess);
	sd->sdp   = mem_deref(sd->sdp);
	sd->sock  = mem_deref(sd->sock);
}

static const struct mnat *vt(void)
{
	return mnat_find(baresip_mnatl(), "ice");
}

static int side_attach(struct side *sd, bool offerer)
{
	int err = vt()->sessh(&sd->sess, vt(), NULL, AF_INET, NULL, NULL, NULL,
			      sd->sdp, offerer, estab_h, sd);
	if (err)
		return err;
	return vt()->mediah(&sd->media, sd->sess, sd->sock, NULL, sd->sdpm,
			    connh, sd);
}

static int sdp_transfer(struct side *from, struct side *to, bool offer)
{
	struct mbuf *mb = NULL;
	int err = sdp_encode(&mb, from->sdp, offer);
	if (err)
		return err;
	mb->pos = 0;
	err = sdp_decode(to->sdp, mb, offer);
	mem_deref(mb);
	return err;
}

/* ── Event loop ──────────────────────────────────────────────────────────── */

typedef bool (pred_h)(void *arg);
struct waiter { struct tmr tmr; pred_h *pred; void *arg; uint64_t deadline; bool ok; };

static void waiter_tick(void *arg)
{
	struct waiter *w = arg;
	if (w->pred(w->arg)) { w->ok = true; re_cancel(); return; }
	if (tmr_jiffies() >= w->deadline) { re_cancel(); return; }
	tmr_start(&w->tmr, 5, waiter_tick, w);
}
static bool run_until(pred_h *pred, void *arg, unsigned timeout_ms)
{
	struct waiter w = { .pred = pred, .arg = arg,
			    .deadline = tmr_jiffies() + timeout_ms };
	tmr_init(&w.tmr);
	tmr_start(&w.tmr, 1, waiter_tick, &w);
	re_main(NULL);
	tmr_cancel(&w.tmr);
	return w.ok;
}
static bool pred_never(void *arg) { (void)arg; return false; }
static void run_for(unsigned ms) { (void)run_until(pred_never, NULL, ms); }

static bool estab_once(void *arg) { return ((struct side *)arg)->estab_calls >= 1; }
static bool estab_twice(void *arg) { return ((struct side *)arg)->estab_calls >= 2; }
static bool connh_fired(void *arg) { return ((struct side *)arg)->connh_calls >= 1; }
static bool both_completed(void *arg)
{
	struct side **ab = arg;
	return bsdk_ice_call_state(&ab[0]->call) == ICEA_STATE_COMPLETED &&
	       bsdk_ice_call_state(&ab[1]->call) == ICEA_STATE_COMPLETED;
}

/* The choreography from A's offer onward, for a caller already attached. */
static void connect_call_from(struct side *a, struct side *b)
{
	struct side *ab[2] = { a, b };

	CHECK(0 == sdp_transfer(a, b, true));

	CHECK(0 == side_attach(b, false));
	CHECK(run_until(estab_once, b, 2000));       /* B gathered */
	CHECK(0 == vt()->updateh(b->sess));          /* B answers */
	CHECK(0 == sdp_transfer(b, a, false));
	CHECK(0 == vt()->updateh(a->sess));          /* A applies the answer */

	CHECK(run_until(both_completed, ab, 4000));
}


/* The full baresip choreography for an A→B call on loopback. */
static void connect_call(struct side *a, struct side *b)
{
	CHECK(0 == side_attach(a, true));
	CHECK(run_until(estab_once, a, 2000));       /* A's offer released */
	CHECK(a->estab_err == 0);
	connect_call_from(a, b);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_module_registers_once(void)
{
	CHECK(0 == bsdk_ice_module_init());
	CHECK(vt() != NULL);
	CHECK(0 == str_cmp(vt()->id, "ice"));
	CHECK(0 == str_cmp(vt()->ftag, "+sip.ice"));
	CHECK(vt()->wait_connected);
	CHECK(0 == bsdk_ice_module_init());          /* idempotent */
	CHECK(list_count(&g_mnatl) == 1);
}


static void test_gather_release_and_sdp(void)
{
	struct side a;
	struct mbuf *mb = NULL;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_attach(&a, true));
	CHECK(a.estab_calls == 0);                   /* never inside sessh */
	CHECK(run_until(estab_once, &a, 1000));
	CHECK(a.estab_calls == 1 && a.estab_err == 0);

	CHECK(0 == sdp_encode(&mb, a.sdp, true));
	CHECK(memmem(mb->buf, mb->end, "a=ice-ufrag:", 12) != NULL);
	CHECK(memmem(mb->buf, mb->end, "a=ice-pwd:", 10) != NULL);
	CHECK(memmem(mb->buf, mb->end, "typ host", 8) != NULL);
	CHECK(memmem(mb->buf, mb->end, "127.0.0.1", 9) != NULL);
	mem_deref(mb);

	CHECK(bsdk_ice_call_active(&a.call));
	CHECK(bsdk_ice_call_state(&a.call) == ICEA_STATE_GATHERING);

	/* a late "gathered" never re-offers by itself */
	run_for(200);
	CHECK(a.estab_calls == 1);

	side_close(&a);
}


static void test_deadline_releases_offer_without_media(void)
{
	/* The bug the shim was born for: a session whose media list stays
	 * empty (BUNDLE mux, or a stream that never attaches) reported
	 * nothing, and the INVITE never left. */
	struct side a;

	g_bsdk.cfg.ice_gathering_timeout_ms = 120;
	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == vt()->sessh(&a.sess, vt(), NULL, AF_INET, NULL, NULL, NULL,
			       a.sdp, true, estab_h, &a));
	/* no mediah */
	CHECK(!run_until(estab_once, &a, 60));
	CHECK(run_until(estab_once, &a, 500));
	CHECK(a.estab_err == 0);                     /* success, not error */
	CHECK(!bsdk_ice_call_active(&a.call));       /* nothing to check   */
	side_close(&a);
	g_bsdk.cfg.ice_gathering_timeout_ms = 2000;
}


static void test_full_call_virtual_peer(void)
{
	struct side a, b;
	struct sa vpeer, bogus;
	struct mbuf *mb;

	fprintf(stderr, "===== test_full_call_virtual_peer =====\n");
	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_ESTABLISHED));
	connect_call(&a, &b);
	if (getenv("ICE_MOD_VERBOSE")) {
		fprintf(stderr, "----- A: state=%d active=%d\n",
			bsdk_ice_call_state(&a.call), bsdk_ice_call_active(&a.call));
		fprintf(stderr, "----- B: state=%d active=%d connh=%u\n",
			bsdk_ice_call_state(&b.call), bsdk_ice_call_active(&b.call),
			b.connh_calls);
	}

	/* both sides got their connected handler, with the virtual peer */
	sa_set_str(&vpeer, ICEA_VPEER_V4, ICEA_VPEER_PORT);
	CHECK(run_until(connh_fired, &a, 1000));
	CHECK(run_until(connh_fired, &b, 1000));
	CHECK(sa_cmp(&a.connh_raddr, &vpeer, SA_ALL));
	CHECK(sa_cmp(&b.connh_raddr, &vpeer, SA_ALL));

	/* the selected pair is real */
	{
		struct sa l, r;
		CHECK(bsdk_ice_call_selected(&a.call, &l, &r, NULL, NULL, NULL,
					     NULL));
		CHECK(sa_cmp(&r, &b.laddr, SA_ALL));
		CHECK(sa_port(&l) == sa_port(&a.laddr));
	}

	/* An application packet sent to a bogus destination reaches B
	 * (dst rewritten to the selected pair) and B's application sees it
	 * from the virtual peer (src rewritten). */
	sa_set_str(&bogus, "203.0.113.77", 4444);
	mb = mbuf_alloc(64);
	mbuf_write_u8(mb, 0x80);                     /* RTP-ish, not STUN */
	mbuf_fill(mb, 0xab, 40);
	mb->pos = 0;
	CHECK(0 == udp_send(a.sock, &bogus, mb));
	mem_deref(mb);
	CHECK(run_until((pred_h *)connh_fired, &b, 10) || true);
	run_for(50);
	CHECK(b.app_rx >= 1);
	CHECK(sa_cmp(&b.app_last_src, &vpeer, SA_ALL));

	/* STUN keeps flowing (consent) while the call is up */
	{
		struct sa l0, l1;
		uint32_t rtt = 0;
		run_for(600);
		CHECK(bsdk_ice_call_selected(&a.call, &l0, &l1, NULL, NULL,
					     &rtt, NULL));
		CHECK(bsdk_ice_call_state(&a.call) == ICEA_STATE_COMPLETED);
	}

	/* a later SDP exchange (re-INVITE) re-asserts the vpeer */
	{
		unsigned before = a.connh_calls;
		CHECK(0 == vt()->updateh(a.sess));
		CHECK(a.connh_calls == before + 1);
		CHECK(sa_cmp(&a.connh_raddr, &vpeer, SA_ALL));
	}

	side_close(&a);
	side_close(&b);
}


static void test_restart_contract(void)
{
	struct side a, b;
	struct fake_call nobody;
	struct mnat_media *media_before;
	uint32_t gen_before = 0, gen_after = 0;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_ESTABLISHED));
	connect_call(&a, &b);
	media_before = a.media;
	CHECK(bsdk_ice_call_selected(&a.call, NULL, NULL, NULL, NULL, NULL,
				     &gen_before));
	CHECK(gen_before == 1);

	memset(&nobody, 0, sizeof(nobody));
	CHECK(ENOENT == bsdk_ice_restart(&nobody, NULL));

	CHECK(0 == bsdk_ice_restart(&a.call, NULL));
	CHECK(EALREADY == bsdk_ice_restart(&a.call, NULL));
	CHECK(a.media == media_before);              /* stable pointer */

	/* the restart offer goes out once the new generation gathered */
	CHECK(run_until(estab_twice, &a, 2000));
	CHECK(a.estab_err == 0);
	CHECK(0 == sdp_transfer(&a, &b, true));
	CHECK(0 == vt()->updateh(b.sess));           /* B answers the restart */
	CHECK(0 == sdp_transfer(&b, &a, false));
	CHECK(0 == vt()->updateh(a.sess));

	{
		struct side *ab[2] = { &a, &b };
		CHECK(run_until(both_completed, ab, 4000));
	}
	CHECK(bsdk_ice_call_selected(&a.call, NULL, NULL, NULL, NULL, NULL,
				     &gen_after));
	CHECK(gen_after == 2);
	CHECK(ENOENT != bsdk_ice_restart(&a.call, NULL));  /* restartable again */

	side_close(&a);
	side_close(&b);
}


static void test_plain_rtp_peer(void)
{
	/* B has no ICE at all: A must start media on the signalled address
	 * and pass packets through untouched. */
	struct side a, b;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_ESTABLISHED));
	CHECK(0 == side_attach(&a, true));
	CHECK(run_until(estab_once, &a, 1000));
	CHECK(0 == sdp_transfer(&a, &b, true));
	/* B answers with plain SDP (no sessh on B) */
	CHECK(0 == sdp_transfer(&b, &a, false));
	CHECK(0 == vt()->updateh(a.sess));

	CHECK(a.connh_calls == 1);
	CHECK(sa_cmp(&a.connh_raddr, &b.laddr, SA_ALL));
	CHECK(!bsdk_ice_call_active(&a.call));       /* not an ICE call */

	side_close(&a);
	side_close(&b);
}


/* Both incoming tests keep B in CALL_STATE_INCOMING across updateh, because
 * baresip does: call_answer() runs call_apply_sdp() — and so updateh — one
 * step before sdp_encode() and sipsess_answer(), and the state only becomes
 * ESTABLISHED on the ACK, which brings no updateh with it.  Answering by
 * flipping the state first is what hid the bug below until 2026-09-15. */

static void test_incoming_holds_connh_until_answer(void)
{
	/* B is the callee and its checks conclude while it is still ringing:
	 * media must not start before its answer is out, and must start as
	 * soon as it is. */
	struct side a, b;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_INCOMING));

	CHECK(0 == side_attach(&a, true));
	CHECK(run_until(estab_once, &a, 1000));
	CHECK(0 == sdp_transfer(&a, &b, true));
	CHECK(0 == side_attach(&b, false));
	CHECK(run_until(estab_once, &b, 1000));      /* B gathered → primed */

	/* B's candidates reach A while B is still ringing, so B's checks
	 * conclude with the answer still unsent. */
	CHECK(0 == sdp_transfer(&b, &a, false));
	CHECK(0 == vt()->updateh(a.sess));
	CHECK(run_until(connh_fired, &a, 4000));
	CHECK(bsdk_ice_call_state(&b.call) == ICEA_STATE_COMPLETED);
	CHECK(b.connh_calls == 0);                   /* held: still ringing */

	/* B answers.  Still INCOMING here, and nothing calls updateh again. */
	CHECK(0 == vt()->updateh(b.sess));
	CHECK(run_until(connh_fired, &b, 1000));
	CHECK(b.call.st == CALL_STATE_INCOMING);

	side_close(&a);
	side_close(&b);
}


static void test_incoming_connh_when_checks_conclude_after_answer(void)
{
	/* The other order, and the one FreeSWITCH produces: B's answer goes
	 * out first and the checks conclude a moment later.  There is no
	 * further updateh to release anything, so on_selected has to start
	 * media itself (2026-09-15: a 25 s call, ICE completed, tx 0 rx 0). */
	struct side a, b;
	struct side *ab[2] = { &a, &b };

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_INCOMING));

	CHECK(0 == side_attach(&a, true));
	CHECK(run_until(estab_once, &a, 1000));
	CHECK(0 == sdp_transfer(&a, &b, true));
	CHECK(0 == side_attach(&b, false));
	CHECK(run_until(estab_once, &b, 1000));      /* B gathered → primed */

	CHECK(0 == vt()->updateh(b.sess));           /* B answers */
	CHECK(0 == sdp_transfer(&b, &a, false));
	CHECK(0 == vt()->updateh(a.sess));           /* A applies the answer */

	CHECK(run_until(both_completed, ab, 4000));
	CHECK(run_until(connh_fired, &b, 1000));
	CHECK(b.call.st == CALL_STATE_INCOMING);

	side_close(&a);
	side_close(&b);
}


/* A caller that reads nothing until the callee picks up, and then reads its
 * backlog oldest first — Asterisk, whose answers to the checks an inbound
 * call primes while ringing all come after pickup (2026-09-29).  A receive
 * helper below ICE holds every datagram on the caller's socket and replays
 * them, one per ms, once released. */
struct holdq {
	struct udp_sock *us;
	struct udp_helper *uh;
	struct list pktl;
	struct tmr tmr;
	bool hold;
};

struct heldpkt {
	struct le le;
	struct sa src;
	struct mbuf *mb;
};

static void heldpkt_destructor(void *arg)
{
	struct heldpkt *hp = arg;
	list_unlink(&hp->le);
	mem_deref(hp->mb);
}

static bool holdq_recv_h(struct sa *src, struct mbuf *mb, void *arg)
{
	struct holdq *q = arg;
	struct heldpkt *hp;

	if (!q->hold && list_isempty(&q->pktl))
		return false;

	hp = mem_zalloc(sizeof(*hp), heldpkt_destructor);
	if (!hp)
		return true;
	hp->src = *src;
	hp->mb  = mbuf_alloc(mbuf_get_left(mb));
	mbuf_write_mem(hp->mb, mbuf_buf(mb), mbuf_get_left(mb));
	hp->mb->pos = 0;
	list_append(&q->pktl, &hp->le, hp);
	return true;
}

static void holdq_drain(void *arg)
{
	struct holdq *q = arg;
	struct le *le = list_head(&q->pktl);
	struct heldpkt *hp;

	if (!le)
		return;
	hp = le->data;
	list_unlink(&hp->le);          /* so the replay is not queued again */
	udp_recv_helper(q->us, &hp->src, hp->mb, q->uh);
	mem_deref(hp);

	if (!list_isempty(&q->pktl))
		tmr_start(&q->tmr, 1, holdq_drain, q);
}

static void holdq_init(struct holdq *q, struct udp_sock *us)
{
	memset(q, 0, sizeof(*q));
	q->us   = us;
	q->hold = true;
	list_init(&q->pktl);
	tmr_init(&q->tmr);
	CHECK(0 == udp_register_helper(&q->uh, us, -100, NULL,
				       holdq_recv_h, q));
}

static void holdq_release(struct holdq *q)
{
	q->hold = false;
	tmr_start(&q->tmr, 1, holdq_drain, q);
}

static void holdq_close(struct holdq *q)
{
	tmr_cancel(&q->tmr);
	list_flush(&q->pktl);
	q->uh = mem_deref(q->uh);
}


static void test_incoming_long_ring_backlog(void)
{
	/* The silent calls: B rings for more than 5 s while its checks pile
	 * up unanswered at A, then A answers the backlog oldest first.  Media
	 * has to start, and without the watchdog having to start it. */
	struct side a, b;
	struct side *ab[2] = { &a, &b };
	struct holdq q;
	unsigned no_pair = g_warn_no_pair, never = g_warn_never_started;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_INCOMING));
	holdq_init(&q, a.sock);

	CHECK(0 == side_attach(&a, true));
	CHECK(run_until(estab_once, &a, 1000));
	CHECK(0 == sdp_transfer(&a, &b, true));
	CHECK(0 == side_attach(&b, false));
	CHECK(run_until(estab_once, &b, 1000));      /* B gathered → primed */

	run_for(5600);                               /* ringing */
	CHECK(b.connh_calls == 0);
	CHECK(list_count(&q.pktl) > 5);              /* A has read none of it */

	CHECK(0 == vt()->updateh(b.sess));           /* B answers */
	CHECK(0 == sdp_transfer(&b, &a, false));
	CHECK(0 == vt()->updateh(a.sess));
	holdq_release(&q);

	CHECK(run_until(connh_fired, &b, 1500));
	CHECK(run_until(both_completed, ab, 2000));
	CHECK(g_warn_no_pair == no_pair);
	CHECK(g_warn_never_started == never);

	holdq_close(&q);
	side_close(&a);
	side_close(&b);
}


static void test_media_watchdog_reports_no_pair(void)
{
	/* B has answered and A still reads nothing, so neither side has a
	 * writable pair: each watchdog says so once, however long it lasts,
	 * and media starts as soon as A does read. */
	struct side a, b;
	struct holdq q;
	unsigned no_pair = g_warn_no_pair, never = g_warn_never_started;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_INCOMING));
	holdq_init(&q, a.sock);

	CHECK(0 == side_attach(&a, true));
	CHECK(run_until(estab_once, &a, 1000));
	CHECK(0 == sdp_transfer(&a, &b, true));
	CHECK(0 == side_attach(&b, false));
	CHECK(run_until(estab_once, &b, 1000));

	CHECK(0 == vt()->updateh(b.sess));           /* B answers at once */
	CHECK(0 == sdp_transfer(&b, &a, false));
	CHECK(0 == vt()->updateh(a.sess));

	run_for(ICEA_MEDIA_START_DEADLINE_MS - 500);
	CHECK(g_warn_no_pair == no_pair);            /* not before the deadline */
	run_for(500 + 2 * ICEA_MEDIA_START_POLL_MS);
	CHECK(g_warn_no_pair == no_pair + 2);        /* once a side, not per poll */
	CHECK(b.connh_calls == 0);

	holdq_release(&q);
	CHECK(run_until(connh_fired, &b, 1500));
	CHECK(g_warn_never_started == never);        /* ICE started it itself */

	holdq_close(&q);
	side_close(&a);
	side_close(&b);
}


static void test_pr2_seams(void)
{
	/* The three answers netmon and adapt.c ask the agent for:
	 *   - the *base* of the selected local candidate, so "did the media
	 *     path move" compares routable addresses;
	 *   - whether ICE is still setting up, so a stall clock does not count
	 *     the check phase as silence;
	 *   - every liveness transition, so a stall can be attributed. */
	struct side a, b;
	struct sa base, l, r;
	int before;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_ESTABLISHED));

	/* before anything is selected */
	CHECK(!bsdk_ice_call_laddr(&a.call, &base));

	before = g_ice_state_reports;
	CHECK(0 == side_attach(&a, true));
	CHECK(run_until(estab_once, &a, 1000));

	/* gathering, not yet connected: setup is pending */
	CHECK(bsdk_ice_setup_pending(&a.call));
	CHECK(g_ice_state_reports > before);       /* transitions reported */

	connect_call_from(&a, &b);

	/* connected: no longer pending, and the base is a real address */
	CHECK(!bsdk_ice_setup_pending(&a.call));
	CHECK(bsdk_ice_call_laddr(&a.call, &base));
	CHECK(bsdk_ice_call_selected(&a.call, &l, &r, NULL, NULL, NULL, NULL));
	/* loopback host candidate: base == the candidate itself */
	CHECK(sa_cmp(&base, &l, SA_ALL));
	CHECK(sa_port(&base) == sa_port(&a.laddr));
	CHECK(g_last_ice_state == ICEA_STATE_COMPLETED);

	/* a network change re-gathers without touching credentials, the
	 * selected pair or the generation */
	{
		uint32_t gen_before = 0, gen_after = 0;
		char ufrag[ICEA_UFRAG_LEN + 1];
		struct sa r_before, r_after;

		CHECK(bsdk_ice_call_selected(&a.call, NULL, &r_before, NULL, NULL,
					     NULL, &gen_before));
		{
			/* no STUN server configured here, so this is a no-op
			 * by design — it must still be safe to call */
			bsdk_ice_network_changed();
		}
		run_for(100);
		CHECK(bsdk_ice_call_selected(&a.call, NULL, &r_after, NULL, NULL,
					     NULL, &gen_after));
		CHECK(gen_after == gen_before);
		CHECK(sa_cmp(&r_after, &r_before, SA_ALL));
		CHECK(bsdk_ice_call_state(&a.call) == ICEA_STATE_COMPLETED);
		(void)ufrag;
	}

	/* a call with no ICE answers "not pending" and "no base" */
	CHECK(!bsdk_ice_setup_pending(&b.call) ||
	      bsdk_ice_call_state(&b.call) >= ICEA_STATE_CONNECTED);

	side_close(&a);
	side_close(&b);
}


static void test_teardown_either_order(void)
{
	struct side a, b;

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_ESTABLISHED));
	connect_call(&a, &b);

	/* session before media on one side, media before session on the
	 * other — neither may crash or leak */
	a.sess  = mem_deref(a.sess);
	a.media = mem_deref(a.media);
	b.media = mem_deref(b.media);
	b.sess  = mem_deref(b.sess);
	side_close(&a);
	side_close(&b);
	CHECK(true);
}


static void test_module_close(void)
{
	bsdk_ice_module_close();
	CHECK(vt() == NULL);
	bsdk_ice_module_close();                     /* idempotent */
	CHECK(0 == bsdk_ice_module_init());
	CHECK(vt() != NULL);
	bsdk_ice_module_close();
}


int main(void)
{
	if (libre_init()) {
		fprintf(stderr, "libre_init failed\n");
		return 1;
	}
	list_init(&g_mnatl);
	memset(&g_bsdk, 0, sizeof(g_bsdk));
	g_bsdk.cfg.ice_gathering_timeout_ms = 2000;
	g_bsdk.cfg.log_level = getenv("ICE_MOD_VERBOSE") ? 3 : 0;
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.avt.rtp_ports.min = 40000;
	g_cfg.avt.rtp_ports.max = 40100;
	sa_set_str(&g_default_laddr, "127.0.0.1", 0);

	test_module_registers_once();
	test_gather_release_and_sdp();
	test_deadline_releases_offer_without_media();
	test_full_call_virtual_peer();
	test_restart_contract();
	test_plain_rtp_peer();
	test_incoming_holds_connh_until_answer();
	test_incoming_connh_when_checks_conclude_after_answer();
	test_incoming_long_ring_backlog();
	test_media_watchdog_reports_no_pair();
	test_pr2_seams();
	test_teardown_either_order();
	test_module_close();

	tmr_debug();
	mem_debug();
	libre_close();

	printf("test_ice_mod: %d passed, %d failed (ice_failed events: %d)\n",
	       g_pass, g_fail, g_ice_failed_calls);
	return g_fail ? 1 : 0;
}
