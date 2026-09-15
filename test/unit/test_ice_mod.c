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
void _warning(bool safe, const char *fmt, ...)
{ va_list ap; (void)safe; va_start(ap, fmt); vlog_stub("WRN", fmt, ap); va_end(ap); }

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
void bsdk_netmon_call_ice_failed(struct call *bc, int err)
{
	(void)bc; (void)err;
	g_ice_failed_calls++;
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

/* The full baresip choreography for an A→B call on loopback. */
static void connect_call(struct side *a, struct side *b)
{
	struct side *ab[2] = { a, b };

	CHECK(0 == side_attach(a, true));
	CHECK(run_until(estab_once, a, 2000));       /* A's offer released */
	CHECK(a->estab_err == 0);
	CHECK(0 == sdp_transfer(a, b, true));

	CHECK(0 == side_attach(b, false));
	CHECK(run_until(estab_once, b, 2000));       /* B gathered */
	CHECK(0 == vt()->updateh(b->sess));          /* B answers */
	CHECK(0 == sdp_transfer(b, a, false));
	CHECK(0 == vt()->updateh(a->sess));          /* A applies the answer */

	CHECK(run_until(both_completed, ab, 4000));
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


static void test_incoming_holds_connh_until_answer(void)
{
	/* B is the callee and still ringing when its checks conclude:
	 * media must not start before its answer is out. */
	struct side a, b;
	struct side *ab[2] = { &a, &b };

	CHECK(0 == side_init(&a, CALL_STATE_OUTGOING));
	CHECK(0 == side_init(&b, CALL_STATE_INCOMING));

	CHECK(0 == side_attach(&a, true));
	CHECK(run_until(estab_once, &a, 1000));
	CHECK(0 == sdp_transfer(&a, &b, true));
	CHECK(0 == side_attach(&b, false));
	CHECK(run_until(estab_once, &b, 1000));      /* B gathered → primed */
	/* B's checks run while ringing (prime), but A has no answer yet so
	 * only B → A checks happen; give it a moment */
	run_for(300);
	CHECK(b.connh_calls == 0);

	/* B answers */
	b.call.st = CALL_STATE_ESTABLISHED;
	CHECK(0 == vt()->updateh(b.sess));
	CHECK(0 == sdp_transfer(&b, &a, false));
	CHECK(0 == vt()->updateh(a.sess));
	CHECK(run_until(both_completed, ab, 4000));
	CHECK(run_until(connh_fired, &b, 1000));
	CHECK(b.connh_calls >= 1);

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
	test_teardown_either_order();
	test_module_close();

	tmr_debug();
	mem_debug();
	libre_close();

	printf("test_ice_mod: %d passed, %d failed (ice_failed events: %d)\n",
	       g_pass, g_fail, g_ice_failed_calls);
	return g_fail ? 1 : 0;
}
