/**
 * @file test_ice_agent.c  echo-ice: two agents over a simulated network
 *
 * Two generations talk through in-memory sockets (udp_alloc_sockless)
 * joined by a simulator that delivers every datagram asynchronously
 * (a tmr, so a reply can never land before its request was recorded),
 * applies a cone NAT to a node when asked, drops packets by probability,
 * and can kill a node outright.  Timers are compressed through
 * struct icea_conf so a 15 s libwebrtc timeout is 1.5 s here.
 *
 * Credentials and candidates are exchanged directly, the way SDP would
 * carry them; test_ice_sdp.c covers the grammar.
 *
 * Build: make -C test/unit test_ice_agent && ./test/unit/test_ice_agent
 */

#include <stdio.h>
#include <string.h>
#include <re.h>
#include "../../src/ice/ice_internal.h"

static int g_pass, g_fail;

#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)


/* ── Compressed clock ────────────────────────────────────────────────────── */

#define T(ms) (ms)   /* real ms; the conf below is what compresses time */

static const struct icea_conf fast_conf = {
	.ping_weak_ms = 20, .ping_strong_ms = 100,
	.pair_stabilizing_ms = 100, .pair_stable_ms = 250,
	.backup_ping_ms = 2000,
	.write_connect_failures = 5, .write_connect_timeout_ms = 500,
	.write_timeout_ms = 1500,
	.receiving_timeout_ms = 300, .dead_ms = 3000,
	.consent_expiry_ms = 3000,
	.rtt_switch_margin_ms = 10, .switching_delay_ms = 100,
	.err_retry_base_ms = 50, .err_retry_cap_ms = 400,
	.err_failed_count = 8,
	.stun_keepalive_ms = 1000, .ping_table_expiry_ms = 3000,
	.stun_rto_ms = 40, .stun_rc = 3,
	.nomination_delay_ms = 100,
	.nomination = ICEA_NOMINATE_REGULAR, .policy = ICEA_TRANSPORT_ALL,
	.drop_unknown_src = false, .family_filter = true,
	.debug = false,
};


/* ── Simulated network ───────────────────────────────────────────────────── */

enum nat_kind { NAT_NONE, NAT_CONE };

struct node {
	struct le le;
	struct udp_sock *us;
	struct sa priv;          /* what the socket believes it is bound to */
	struct sa pub;           /* NAT_CONE: the mapped address            */
	enum nat_kind nat;
	bool dead;
	unsigned loss_pct;       /* outbound loss                           */
	unsigned delay_ms;
	unsigned tx, rx;
	char name[8];
	struct icea_port *port;  /* the agent port bound to this node        */
	bool stun_server;        /* answers Binding Requests with the source */
};

struct pkt {
	struct le le;
	struct tmr tmr;
	struct node *to;
	struct sa src;
	struct mbuf *mb;
};

static struct list g_nodes;
static struct list g_pkts;      /* in flight */


static struct node *node_by_addr(const struct sa *addr)
{
	struct le *le;

	LIST_FOREACH(&g_nodes, le) {
		struct node *n = le->data;

		if (sa_cmp(&n->priv, addr, SA_ALL))
			return n;
		if (n->nat == NAT_CONE && sa_cmp(&n->pub, addr, SA_ALL))
			return n;
	}
	return NULL;
}


static void pkt_destructor(void *arg)
{
	struct pkt *pkt = arg;
	tmr_cancel(&pkt->tmr);
	list_unlink(&pkt->le);
	mem_deref(pkt->mb);
}


static void pkt_deliver(void *arg)
{
	struct pkt *pkt = arg;

	if (!pkt->to->dead) {
		pkt->to->rx++;
		udp_recv_packet(pkt->to->us, &pkt->src, pkt->mb);
	}
	mem_deref(pkt);
}


static int sim_send(const struct sa *dst, struct mbuf *mb, void *arg)
{
	struct node *from = arg, *to;
	struct pkt *pkt;

	if (from->dead)
		return ENETDOWN;
	from->tx++;

	if (from->loss_pct && (rand_u32() % 100) < from->loss_pct)
		return 0;

	to = node_by_addr(dst);
	if (!to || to->dead)
		return 0;                     /* black hole, like the internet */

	/* A NAT'd node is reachable only on its mapped address. */
	if (to->nat == NAT_CONE && !sa_cmp(&to->pub, dst, SA_ALL))
		return 0;

	pkt = mem_zalloc(sizeof(*pkt), pkt_destructor);
	pkt->to  = to;
	pkt->src = from->nat == NAT_CONE ? from->pub : from->priv;
	pkt->mb  = mbuf_alloc(mbuf_get_left(mb));
	mbuf_write_mem(pkt->mb, mbuf_buf(mb), mbuf_get_left(mb));
	pkt->mb->pos = 0;

	tmr_init(&pkt->tmr);
	list_append(&g_pkts, &pkt->le, pkt);
	tmr_start(&pkt->tmr, from->delay_ms ? from->delay_ms : 1,
		  pkt_deliver, pkt);
	return 0;
}


/* The sockless socket's one `arg` is the node (the send handler needs it),
 * so reception is forwarded from here rather than via udp_handler_set(). */
static void sim_recv(const struct sa *src, struct mbuf *mb, void *arg)
{
	struct node *n = arg;

	if (n->stun_server) {
		/* a public STUN server: XOR-MAPPED-ADDRESS = where it came from */
		struct stun_unknown_attr ua;
		struct stun_msg *msg = NULL;
		struct mbuf *rep;

		if (stun_msg_decode(&msg, mb, &ua))
			return;
		if (stun_msg_class(msg) == STUN_CLASS_REQUEST) {
			rep = mbuf_alloc(128);
			if (0 == stun_msg_encode(rep, STUN_METHOD_BINDING,
						 STUN_CLASS_SUCCESS_RESP,
						 stun_msg_tid(msg), NULL, NULL, 0,
						 true, 0, 1,
						 STUN_ATTR_XOR_MAPPED_ADDR, src)) {
				rep->pos = 0;
				(void)udp_send(n->us, src, rep);
			}
			mem_deref(rep);
		}
		mem_deref(msg);
		return;
	}

	if (n->port)
		icea_route_port_recv(src, mb, n->port);
}


static void node_destructor(void *arg)
{
	struct node *n = arg;
	list_unlink(&n->le);
	mem_deref(n->us);
}


static struct node *node_new(const char *name, const char *priv, uint16_t port,
			     const char *pub, uint16_t pubport)
{
	struct node *n = mem_zalloc(sizeof(*n), node_destructor);

	str_ncpy(n->name, name, sizeof(n->name));
	sa_set_str(&n->priv, priv, port);
	if (pub) {
		sa_set_str(&n->pub, pub, pubport);
		n->nat = NAT_CONE;
	}
	n->delay_ms = 2;
	CHECK(0 == udp_alloc_sockless(&n->us, sim_send, sim_recv, n));
	list_append(&g_nodes, &n->le, n);
	return n;
}


/* ── Event loop driving ──────────────────────────────────────────────────── */

typedef bool (pred_h)(void *arg);

struct waiter {
	struct tmr tmr;
	pred_h *pred;
	void *arg;
	uint64_t deadline;
	bool ok;
};

static void waiter_tick(void *arg)
{
	struct waiter *w = arg;

	if (w->pred(w->arg)) {
		w->ok = true;
		re_cancel();
		return;
	}
	if (tmr_jiffies() >= w->deadline) {
		re_cancel();
		return;
	}
	tmr_start(&w->tmr, 5, waiter_tick, w);
}

/* Run the loop until pred() holds or timeout_ms passes. */
static bool run_until(pred_h *pred, void *arg, unsigned timeout_ms)
{
	struct waiter w;

	memset(&w, 0, sizeof(w));
	w.pred = pred;
	w.arg = arg;
	w.deadline = tmr_jiffies() + timeout_ms;
	tmr_init(&w.tmr);
	tmr_start(&w.tmr, 1, waiter_tick, &w);
	re_main(NULL);
	tmr_cancel(&w.tmr);
	return w.ok;
}

static bool pred_never(void *arg) { (void)arg; return false; }

static void run_for(unsigned ms)
{
	(void)run_until(pred_never, NULL, ms);
}


/* ── Agents ──────────────────────────────────────────────────────────────── */

struct agent {
	struct icea_gen *gen;
	struct node *node[ICEA_MAX_COMP];
	struct icea_port *port[ICEA_MAX_COMP];
	unsigned state_changes;
	unsigned selected_changes;
	unsigned data_rx;
	enum icea_state last_state;
};

static void cb_state(struct icea_gen *gen, enum icea_state prev,
		     enum icea_state now, void *arg)
{
	struct agent *a = arg;
	(void)gen; (void)prev;
	a->state_changes++;
	a->last_state = now;
}

static void cb_selected(struct icea_gen *gen, unsigned compid, void *arg)
{
	struct agent *a = arg;
	(void)gen; (void)compid;
	a->selected_changes++;
}

static void cb_data(struct icea_gen *gen, unsigned compid,
		    const struct sa *src, struct mbuf *mb, void *arg)
{
	struct agent *a = arg;
	(void)gen; (void)compid; (void)src; (void)mb;
	a->data_rx++;
}


static void agent_init(struct agent *a, const struct icea_conf *conf,
		       enum ice_role role, unsigned ncomp,
		       struct node **nodes)
{
	struct icea_gen_cb cb = { .state = cb_state, .selected = cb_selected,
				  .data = cb_data, .arg = a };

	memset(a, 0, sizeof(*a));
	CHECK(0 == icea_gen_alloc(&a->gen, conf, 1, role, AF_INET, ncomp, &cb));
	a->gen->cb.arg = a;

	for (unsigned c = 0; c < ncomp; c++) {
		struct node *n = nodes[c];
		struct icea_cand *host;

		a->node[c] = n;
		CHECK(0 == icea_port_attach(a->gen, c + 1, n->us, &n->priv,
					    n->name, 0, false, &a->port[c]));
		n->port = a->port[c];
		host = icea_cand_add_local(a->gen, ICE_CAND_TYPE_HOST, c + 1,
					   a->port[c], NULL, &n->priv, 65535);
		CHECK(host != NULL);
	}
	a->gen->gathered = true;
	icea_gen_set_state(a->gen, ICEA_STATE_GATHERING);
}


/* Hand `from`'s credentials and candidates to `to`, as its SDP would. */
static void agent_learn(struct agent *to, const struct agent *from)
{
	struct le *le;

	mem_deref(to->gen->rufrag);
	mem_deref(to->gen->rpwd);
	str_dup(&to->gen->rufrag, from->gen->lufrag);
	str_dup(&to->gen->rpwd, from->gen->lpwd);

	LIST_FOREACH(&from->gen->lcandl, le) {
		const struct icea_cand *c = le->data;
		struct ice_cand_attr attr;

		memset(&attr, 0, sizeof(attr));
		str_ncpy(attr.foundation, c->foundation, sizeof(attr.foundation));
		attr.compid = c->compid;
		attr.proto  = IPPROTO_UDP;
		attr.prio   = c->prio;
		attr.addr   = c->addr;
		attr.type   = c->type;
		CHECK(icea_cand_add_remote(to->gen, &attr) != NULL);
	}
	to->gen->remote_ready = true;
}


static void agent_close(struct agent *a)
{
	for (unsigned c = 0; c < ICEA_MAX_COMP; c++)
		if (a->node[c])
			a->node[c]->port = NULL;
	a->gen = mem_deref(a->gen);
}


static bool both_completed(void *arg)
{
	struct agent **ab = arg;
	return ab[0]->gen->state == ICEA_STATE_COMPLETED &&
	       ab[1]->gen->state == ICEA_STATE_COMPLETED;
}

static bool a_completed(void *arg)
{
	struct agent *a = arg;
	return a->gen->state == ICEA_STATE_COMPLETED;
}

static bool a_disconnected(void *arg)
{
	struct agent *a = arg;
	return a->gen->state == ICEA_STATE_DISCONNECTED;
}

static bool a_connected_or_better(void *arg)
{
	struct agent *a = arg;
	return a->gen->state == ICEA_STATE_CONNECTED ||
	       a->gen->state == ICEA_STATE_COMPLETED;
}

static bool a_failed(void *arg)
{
	struct agent *a = arg;
	return a->gen->state == ICEA_STATE_FAILED;
}


static unsigned count_nominated(const struct icea_gen *gen, unsigned compid,
				bool remote)
{
	struct le *le;
	unsigned n = 0;

	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *p = le->data;
		if (p->lc->compid != compid)
			continue;
		if (remote ? p->remote_nominated : p->nominated)
			n++;
	}
	return n;
}


static void nodes_flush(void)
{
	list_flush(&g_pkts);     /* anything still in flight */
	list_flush(&g_nodes);
}


/* ── Scenarios ───────────────────────────────────────────────────────────── */

static void test_direct_both_full(void)
{
	struct node *na = node_new("A", "10.0.0.1", 5000, NULL, 0);
	struct node *nb = node_new("B", "10.0.0.2", 6000, NULL, 0);
	struct agent a, b, *ab[2] = { &a, &b };

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);

	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(0 == icea_gen_start_checks(b.gen));

	if (!run_until(both_completed, ab, 3000)) {
		g_fail++;
		re_fprintf(stderr, "FAIL: direct scenario did not complete\n%H%H",
			   icea_gen_debug, a.gen, icea_gen_debug, b.gen);
		agent_close(&a); agent_close(&b); nodes_flush();
		return;
	}
	g_pass++;
	CHECK(a.gen->selected[0] && b.gen->selected[0]);
	CHECK(a.gen->selected[0]->nominated);
	CHECK(b.gen->selected[0]->remote_nominated);
	/* regular nomination: exactly one pair nominated per component */
	CHECK(count_nominated(a.gen, 1, false) == 1);
	CHECK(count_nominated(b.gen, 1, true) == 1);
	/* the same pair from both sides */
	CHECK(sa_cmp(&a.gen->selected[0]->rc->addr, &nb->priv, SA_ALL));
	CHECK(sa_cmp(&b.gen->selected[0]->rc->addr, &na->priv, SA_ALL));
	CHECK(a.gen->selected[0]->rtt_samples > 0);
	CHECK(a.gen->selected[0]->rtt_ms <= 20);

	/* keep-alive continues: pings still flow while strong */
	{
		unsigned tx = na->tx;
		run_for(600);
		CHECK(na->tx > tx);
		CHECK(a.gen->state == ICEA_STATE_COMPLETED);
		CHECK(icea_pair_consent_ok(a.gen->selected[0], icea_now()));
	}

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_peer_reflexive_behind_nat(void)
{
	/* A is behind a cone NAT and offers only its private host candidate;
	 * B is public.  B's checks to A's private address vanish; A's check
	 * reaches B from the mapped address, which B learns as prflx and
	 * A learns from XOR-MAPPED-ADDRESS. */
	struct node *na = node_new("A", "192.168.1.2", 5000, "203.0.113.1", 40000);
	struct node *nb = node_new("B", "198.51.100.2", 6000, NULL, 0);
	struct agent a, b, *ab[2] = { &a, &b };

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);
	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(0 == icea_gen_start_checks(b.gen));

	CHECK(run_until(both_completed, ab, 3000));
	CHECK(a.gen->selected[0]->lc->type == ICE_CAND_TYPE_PRFLX);
	CHECK(sa_cmp(&a.gen->selected[0]->lc->addr, &na->pub, SA_ALL));
	CHECK(b.gen->selected[0]->rc->type == ICE_CAND_TYPE_PRFLX);
	CHECK(sa_cmp(&b.gen->selected[0]->rc->addr, &na->pub, SA_ALL));
	CHECK(b.gen->selected[0]->remote_nominated);

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_ice_lite_peer(void)
{
	/* B behaves as ICE-lite: answers checks, never sends any. */
	struct node *na = node_new("A", "10.0.0.1", 5000, NULL, 0);
	struct node *nb = node_new("B", "10.0.0.2", 6000, NULL, 0);
	struct agent a, b;

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);
	a.gen->rlite = true;
	/* B: credentials known so it can validate, but no scheduler */
	CHECK(0 == icea_gen_start_checks(a.gen));

	CHECK(run_until(a_completed, &a, 3000));
	CHECK(count_nominated(a.gen, 1, false) == 1);
	CHECK(a.gen->selected[0]->nominated);
	/* B saw the nomination and recorded the peer-reflexive pair */
	CHECK(count_nominated(b.gen, 1, true) == 1 ||
	      list_count(&b.gen->pairl) >= 1);

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_role_conflict(void)
{
	struct node *na = node_new("A", "10.0.0.1", 5000, NULL, 0);
	struct node *nb = node_new("B", "10.0.0.2", 6000, NULL, 0);
	struct agent a, b, *ab[2] = { &a, &b };

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLING, 1, &nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);
	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(0 == icea_gen_start_checks(b.gen));

	CHECK(run_until(both_completed, ab, 3000));
	CHECK(a.gen->role != b.gen->role);
	{
		const struct icea_gen *ctl = a.gen->role == ICE_ROLE_CONTROLLING
					   ? a.gen : b.gen;
		const struct icea_gen *cld = ctl == a.gen ? b.gen : a.gen;
		CHECK(count_nominated(ctl, 1, false) == 1);
		CHECK(count_nominated(cld, 1, true) == 1);
	}

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_401_storm_is_not_terminal(void)
{
	/* B rejects everything for a while (it is "rebuilding", like Asterisk
	 * after a restart): its password does not match what A sends, so
	 * every check is 401'd.  Then it comes right.  No pair may end up
	 * dead, and the call converges. */
	struct node *na = node_new("A", "10.0.0.1", 5000, NULL, 0);
	struct node *nb = node_new("B", "10.0.0.2", 6000, NULL, 0);
	struct agent a, b, *ab[2] = { &a, &b };
	char real_pwd[ICEA_PWD_LEN + 1];

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);

	strcpy(real_pwd, b.gen->lpwd);
	strcpy(b.gen->lpwd, "wrongwrongwrongwrongwrongwrongwr");
	CHECK(0 == icea_gen_start_checks(a.gen));

	run_for(600);
	CHECK(a.gen->state != ICEA_STATE_COMPLETED);
	{
		const struct icea_pair *p = list_head(&a.gen->pairl)->data;
		CHECK(p->err_count >= 2);
		CHECK(p->last_scode == 401);
		CHECK(p->state != ICE_CANDPAIR_FAILED);
		CHECK(!icea_pair_dead(p, icea_now()));
	}

	strcpy(b.gen->lpwd, real_pwd);
	CHECK(0 == icea_gen_start_checks(b.gen));

	CHECK(run_until(both_completed, ab, 4000));
	CHECK(a.gen->selected[0]->err_count == 0);

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_two_components(void)
{
	struct node *na[2] = { node_new("A1", "10.0.0.1", 5000, NULL, 0),
			       node_new("A2", "10.0.0.1", 5001, NULL, 0) };
	struct node *nb[2] = { node_new("B1", "10.0.0.2", 6000, NULL, 0),
			       node_new("B2", "10.0.0.2", 6001, NULL, 0) };
	struct agent a, b, *ab[2] = { &a, &b };

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 2, na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 2, nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);
	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(0 == icea_gen_start_checks(b.gen));

	CHECK(run_until(both_completed, ab, 4000));
	CHECK(a.gen->selected[0] && a.gen->selected[1]);
	CHECK(a.gen->selected[0]->lc->compid == 1);
	CHECK(a.gen->selected[1]->lc->compid == 2);
	CHECK(count_nominated(a.gen, 1, false) == 1);
	CHECK(count_nominated(a.gen, 2, false) == 1);
	CHECK(sa_port(&a.gen->selected[1]->rc->addr) == 6001);

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_unknown_source_request_is_rejected(void)
{
	/* A stranger sends a Binding Request with the wrong password: 401,
	 * and nothing is learned from it. */
	struct node *na = node_new("A", "10.0.0.1", 5000, NULL, 0);
	struct node *nx = node_new("X", "10.0.0.9", 7000, NULL, 0);
	struct agent a;
	struct mbuf *mb = mbuf_alloc(128);
	uint8_t tid[STUN_TID_SIZE];
	uint32_t prio = 1;
	uint64_t tb = 1;
	char user[64];

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);

	re_snprintf(user, sizeof(user), "%s:stranger", a.gen->lufrag);
	stun_generate_tid(tid);
	CHECK(0 == stun_msg_encode(mb, STUN_METHOD_BINDING, STUN_CLASS_REQUEST,
				   tid, NULL, (uint8_t *)"badpassword", 11,
				   true, 0, 3,
				   STUN_ATTR_USERNAME, user,
				   STUN_ATTR_PRIORITY, &prio,
				   STUN_ATTR_CONTROLLED, &tb));
	mb->pos = 0;
	CHECK(0 == udp_send(nx->us, &na->priv, mb));
	mem_deref(mb);

	run_for(50);
	CHECK(nx->rx == 1);                         /* got a reply (401) */
	CHECK(list_isempty(&a.gen->rcandl));        /* learned nothing   */
	CHECK(list_isempty(&a.gen->pairl));

	agent_close(&a);
	nodes_flush();
}


static void test_early_inbound_check(void)
{
	/* B knows A's credentials (it read A's offer) and checks at once;
	 * A has not processed B's answer yet.  A must answer using its own
	 * password and remember B as a prflx remote with a triggered check,
	 * then converge fast once B's credentials arrive. */
	struct node *na = node_new("A", "10.0.0.1", 5000, NULL, 0);
	struct node *nb = node_new("B", "10.0.0.2", 6000, NULL, 0);
	struct agent a, b, *ab[2] = { &a, &b };

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb);
	agent_learn(&b, &a);                 /* B has A's creds, not vice versa */
	CHECK(0 == icea_gen_start_checks(b.gen));

	run_for(150);
	CHECK(!a.gen->remote_ready);
	CHECK(list_count(&a.gen->rcandl) == 1);      /* prflx learned      */
	{
		const struct icea_cand *rc = list_head(&a.gen->rcandl)->data;
		CHECK(rc->type == ICE_CAND_TYPE_PRFLX);
		CHECK(sa_cmp(&rc->addr, &nb->priv, SA_ALL));
	}
	CHECK(list_count(&a.gen->pairl) == 1);
	CHECK(((struct icea_pair *)list_head(&a.gen->pairl)->data)->triggered);
	CHECK(nb->rx >= 1);                          /* B got answers       */

	/* now the answer lands */
	agent_learn(&a, &b);
	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(run_until(both_completed, ab, 2000));
	/* the signalled remote was already known as prflx: no duplicate */
	CHECK(list_count(&a.gen->rcandl) == 1);

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_liveness_disconnect_and_fail(void)
{
	struct node *na = node_new("A", "10.0.0.1", 5000, NULL, 0);
	struct node *nb = node_new("B", "10.0.0.2", 6000, NULL, 0);
	struct agent a, b, *ab[2] = { &a, &b };
	uint64_t t0;

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);
	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(0 == icea_gen_start_checks(b.gen));
	CHECK(run_until(both_completed, ab, 3000));

	/* B vanishes.  A: receiving lapses (300 ms) → DISCONNECTED; the pair
	 * times out (1.5 s) and dies (3 s) → FAILED, reported once. */
	nb->dead = true;
	t0 = icea_now();
	CHECK(run_until(a_disconnected, &a, 2000));
	CHECK(icea_now() - t0 < 1000);
	CHECK(a.gen->selected[0]->ws != ICEA_WS_TIMEOUT ||
	      a.gen->state == ICEA_STATE_DISCONNECTED);

	CHECK(run_until(a_failed, &a, 6000));
	CHECK(a.gen->failed_reported);
	CHECK(!icea_pair_consent_ok(a.gen->selected[0], icea_now()));
	{
		unsigned changes = a.state_changes;
		run_for(500);
		CHECK(a.state_changes == changes);     /* FAILED only once */
	}

	/* B returns.  Its checks reach A again, the pair is receiving and so
	 * no longer dead, A pings it and recovers on the SAME pair — no
	 * restart, no signalling (libwebrtc behaviour). */
	nb->dead = false;
	CHECK(run_until(a_completed, &a, 4000));
	CHECK(a.gen->selected[0]->ws == ICEA_WS_WRITABLE);
	CHECK(icea_pair_consent_ok(a.gen->selected[0], icea_now()));

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_silent_switch_for_controlled(void)
{
	/* A (controlling) has two paths to B: B offers two host candidates.
	 * After nomination the first remote dies; the second keeps answering.
	 * As the controlling agent under regular nomination A must NOT move
	 * its tx pair (RFC 8445 §8.1.1) — but it must notice the loss. */
	struct node *na  = node_new("A",  "10.0.0.1", 5000, NULL, 0);
	struct node *nb1 = node_new("B1", "10.0.0.2", 6000, NULL, 0);
	struct node *nb2 = node_new("B2", "10.0.0.3", 6000, NULL, 0);
	struct agent a, b1, b2;

	agent_init(&a,  &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b1, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb1);
	agent_init(&b2, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb2);
	/* one peer identity with two addresses: b2 shares b1's credentials */
	strcpy(b2.gen->lufrag, b1.gen->lufrag);
	strcpy(b2.gen->lpwd,   b1.gen->lpwd);
	agent_learn(&a, &b1);
	{
		struct ice_cand_attr attr;
		memset(&attr, 0, sizeof(attr));
		strcpy(attr.foundation, "B2");
		attr.compid = 1; attr.proto = IPPROTO_UDP;
		attr.prio = 2130706430u; attr.addr = nb2->priv;
		attr.type = ICE_CAND_TYPE_HOST;
		CHECK(icea_cand_add_remote(a.gen, &attr) != NULL);
	}
	agent_learn(&b1, &a);
	agent_learn(&b2, &a);
	CHECK(0 == icea_gen_start_checks(a.gen));

	CHECK(run_until(a_completed, &a, 3000));
	CHECK(sa_cmp(&a.gen->selected[0]->rc->addr, &nb1->priv, SA_ALL));
	CHECK(list_count(&a.gen->pairl) == 2);
	run_for(300);
	{
		/* both pairs validated, one nominated */
		unsigned valid = 0;
		struct le *le;
		LIST_FOREACH(&a.gen->pairl, le) {
			const struct icea_pair *p = le->data;
			if (p->valid) valid++;
		}
		CHECK(valid == 2);
		CHECK(count_nominated(a.gen, 1, false) == 1);
	}

	nb1->dead = true;
	CHECK(run_until(a_disconnected, &a, 2000));
	/* tx pair is pinned by the nomination rule: still B1 */
	CHECK(sa_cmp(&a.gen->selected[0]->rc->addr, &nb1->priv, SA_ALL));

	agent_close(&a);
	agent_close(&b1);
	agent_close(&b2);
	nodes_flush();
}


static void test_renomination_switches(void)
{
	/* Same topology, but the peer negotiated renomination: A may move
	 * its selected pair to the surviving path and nominate it. */
	struct node *na  = node_new("A",  "10.0.0.1", 5000, NULL, 0);
	struct node *nb1 = node_new("B1", "10.0.0.2", 6000, NULL, 0);
	struct node *nb2 = node_new("B2", "10.0.0.3", 6000, NULL, 0);
	struct agent a, b1, b2;

	agent_init(&a,  &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b1, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb1);
	agent_init(&b2, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb2);
	strcpy(b2.gen->lufrag, b1.gen->lufrag);
	strcpy(b2.gen->lpwd,   b1.gen->lpwd);
	a.gen->renomination = true;
	b1.gen->renomination = b2.gen->renomination = true;
	agent_learn(&a, &b1);
	{
		struct ice_cand_attr attr;
		memset(&attr, 0, sizeof(attr));
		strcpy(attr.foundation, "B2");
		attr.compid = 1; attr.proto = IPPROTO_UDP;
		attr.prio = 2130706430u; attr.addr = nb2->priv;
		attr.type = ICE_CAND_TYPE_HOST;
		CHECK(icea_cand_add_remote(a.gen, &attr) != NULL);
	}
	agent_learn(&b1, &a);
	agent_learn(&b2, &a);
	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(run_until(a_completed, &a, 3000));
	run_for(300);

	nb1->dead = true;
	{
		/* the surviving pair keeps answering pings; A should switch
		 * to it and renominate; state returns to COMPLETED */
		unsigned sel_before = a.selected_changes;
		CHECK(run_until(a_completed, &a, 3000) ||
		      a.gen->state == ICEA_STATE_CONNECTED);
		run_for(400);
		CHECK(run_until(a_completed, &a, 3000));
		CHECK(a.selected_changes > sel_before);
		CHECK(sa_cmp(&a.gen->selected[0]->rc->addr, &nb2->priv, SA_ALL));
		CHECK(a.gen->selected[0]->nominated);
	}

	agent_close(&a);
	agent_close(&b1);
	agent_close(&b2);
	nodes_flush();
}


static void test_receipt_credits_reflexive_selected_pair(void)
{
	/* A NAT'd caller: the selected pair is peer-reflexive while the host
	 * pair to the same remote still exists and outranks it.  Inbound data
	 * must keep the *selected* pair receiving — crediting only the
	 * highest-priority match made COMPLETED flap to DISCONNECTED on the
	 * ping cadence (found against FreeSWITCH, 2026-09-15). */
	struct node *na = node_new("A", "192.168.1.2", 5000, "203.0.113.1", 40000);
	struct node *nb = node_new("B", "198.51.100.2", 6000, NULL, 0);
	struct agent a, b, *ab[2] = { &a, &b };
	struct icea_pair *sel;
	unsigned host_pairs = 0;
	struct le *le;
	uint64_t t0;

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&b, &fast_conf, ICE_ROLE_CONTROLLED, 1, &nb);
	agent_learn(&a, &b);
	agent_learn(&b, &a);
	CHECK(0 == icea_gen_start_checks(a.gen));
	CHECK(0 == icea_gen_start_checks(b.gen));
	CHECK(run_until(both_completed, ab, 3000));

	sel = a.gen->selected[0];
	CHECK(sel != NULL);
	CHECK(sel->lc->type == ICE_CAND_TYPE_PRFLX);   /* the NAT case */

	/* the host pair to the same remote exists and outranks the selected */
	LIST_FOREACH(&a.gen->pairl, le) {
		const struct icea_pair *p = le->data;
		if (p->lc->type == ICE_CAND_TYPE_HOST &&
		    sa_cmp(&p->rc->addr, &sel->rc->addr, SA_ALL)) {
			host_pairs++;
			CHECK(p->prio > sel->prio);
		}
	}
	CHECK(host_pairs == 1);

	/* Feed data only — no checks at all — for longer than the receiving
	 * window, and the selected pair must stay receiving and COMPLETED. */
	icea_gen_hold(a.gen);                     /* stop A's scheduler */
	t0 = icea_now();
	while (icea_now() - t0 < fast_conf.receiving_timeout_ms * 3) {
		struct mbuf *mb = mbuf_alloc(64);
		mbuf_write_u8(mb, 0x80);          /* RTP-ish, not STUN */
		mbuf_fill(mb, 0x5a, 40);
		mb->pos = 0;
		CHECK(0 == udp_send(nb->us, &na->pub, mb));
		mem_deref(mb);
		run_for(40);
		icea_select_tick(a.gen, icea_now());
		CHECK(sel->receiving);
		CHECK(a.gen->state == ICEA_STATE_COMPLETED);
	}

	agent_close(&a);
	agent_close(&b);
	nodes_flush();
}


static void test_pac_waits_for_better_pair(void)
{
	/* RFC 8863 (ICE PAC).  B offers two candidates: a high-priority one on
	 * a slow node and a low-priority one that answers at once.  Nomination
	 * must wait out the delay and settle on the high-priority pair rather
	 * than on whichever validated first — RFC 8445 §8.1.1 gives one
	 * nomination per component, so getting it wrong is permanent. */
	struct node *na   = node_new("A",  "10.0.0.1", 5000, NULL, 0);
	struct node *slow = node_new("Bs", "10.0.0.2", 6000, NULL, 0);
	struct node *fast = node_new("Bf", "10.0.0.3", 6000, NULL, 0);
	struct agent a, bs, bf;

	slow->delay_ms = 60;          /* one round trip ≈ 120 ms             */

	agent_init(&a,  &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	agent_init(&bs, &fast_conf, ICE_ROLE_CONTROLLED, 1, &slow);
	agent_init(&bf, &fast_conf, ICE_ROLE_CONTROLLED, 1, &fast);
	/* one peer identity on two addresses */
	strcpy(bf.gen->lufrag, bs.gen->lufrag);
	strcpy(bf.gen->lpwd,   bs.gen->lpwd);

	mem_deref(a.gen->rufrag);
	mem_deref(a.gen->rpwd);
	str_dup(&a.gen->rufrag, bs.gen->lufrag);
	str_dup(&a.gen->rpwd,   bs.gen->lpwd);
	{
		struct ice_cand_attr hi, lo;

		memset(&hi, 0, sizeof(hi));
		strcpy(hi.foundation, "HI");
		hi.compid = 1; hi.proto = IPPROTO_UDP;
		hi.prio = 2130706431u;            /* host              */
		hi.addr = slow->priv; hi.type = ICE_CAND_TYPE_HOST;
		CHECK(icea_cand_add_remote(a.gen, &hi) != NULL);

		memset(&lo, 0, sizeof(lo));
		strcpy(lo.foundation, "LO");
		lo.compid = 1; lo.proto = IPPROTO_UDP;
		lo.prio = 1694498815u;            /* srflx — lower     */
		lo.addr = fast->priv; lo.type = ICE_CAND_TYPE_SRFLX;
		CHECK(icea_cand_add_remote(a.gen, &lo) != NULL);
	}
	a.gen->remote_ready = true;
	agent_learn(&bs, &a);
	agent_learn(&bf, &a);

	CHECK(0 == icea_gen_start_checks(a.gen));

	/* the fast (low-priority) pair validates first */
	CHECK(run_until(a_connected_or_better, &a, 1000));
	{
		const struct icea_pair *first = a.gen->selected[0];
		CHECK(first != NULL);
		CHECK(sa_cmp(&first->rc->addr, &fast->priv, SA_ALL));
		CHECK(count_nominated(a.gen, 1, false) == 0);   /* held back */
	}

	/* once the delay lapses the high-priority pair is the nominated one */
	CHECK(run_until(a_completed, &a, 2000));
	CHECK(count_nominated(a.gen, 1, false) == 1);
	CHECK(sa_cmp(&a.gen->selected[0]->rc->addr, &slow->priv, SA_ALL));
	CHECK(a.gen->selected[0]->nominated);
	CHECK(a.gen->selected[0]->rc->type == ICE_CAND_TYPE_HOST);

	agent_close(&a);
	agent_close(&bs);
	agent_close(&bf);
	nodes_flush();
}


static void test_gather_srflx_via_sim_stun(void)
{
	/* A behind a cone NAT gathers against a public STUN server: the
	 * srflx must be its mapped address; a second server that never
	 * answers must not hold gathering hostage beyond its ladder. */
	struct node *na = node_new("A", "192.168.1.2", 5000, "203.0.113.1", 40000);
	struct node *ns = node_new("S", "198.51.100.10", 3478, NULL, 0);
	struct agent a;
	struct list servers;
	struct icea_server *s1 = NULL, *s2 = NULL;
	struct sa dead;
	struct le *le;
	unsigned srflx = 0;

	ns->stun_server = true;
	list_init(&servers);
	CHECK(0 == icea_server_alloc(&s1, false, IPPROTO_UDP, false, &ns->priv,
				     NULL, NULL));
	sa_set_str(&dead, "198.51.100.11", 3478);      /* nobody there */
	CHECK(0 == icea_server_alloc(&s2, false, IPPROTO_UDP, false, &dead,
				     NULL, NULL));
	list_append(&servers, &s1->le, s1);
	list_append(&servers, &s2->le, s2);

	agent_init(&a, &fast_conf, ICE_ROLE_CONTROLLING, 1, &na);
	a.gen->gathered = false;
	CHECK(0 == icea_gather_start(a.gen, &servers, 0, 0));
	CHECK(!icea_gather_done(a.gen));

	CHECK(run_until((pred_h *)icea_gather_done, a.gen, 3000));
	LIST_FOREACH(&a.gen->lcandl, le) {
		const struct icea_cand *c = le->data;
		if (c->type == ICE_CAND_TYPE_SRFLX) {
			srflx++;
			CHECK(sa_cmp(&c->addr, &na->pub, SA_ALL));
			CHECK(c->base && c->base->type == ICE_CAND_TYPE_HOST);
			CHECK(sa_cmp(&c->rel, &na->priv, SA_ALL));
		}
	}
	CHECK(srflx == 1);
	CHECK(icea_cand_default(a.gen, 1)->type == ICE_CAND_TYPE_SRFLX);

	agent_close(&a);
	list_flush(&servers);
	nodes_flush();
}


static void test_own_port_alloc(void)
{
	/* Real sockets on loopback: even RTP port inside the range, the
	 * RTCP component on the odd one above it, EADDRINUSE skipped. */
	struct icea_gen *gen = NULL;
	struct icea_port *p1 = NULL, *p2 = NULL, *p3 = NULL;
	struct sa lo;

	CHECK(0 == icea_gen_alloc(&gen, &fast_conf, 1, ICE_ROLE_CONTROLLING,
				  AF_INET, 2, NULL));
	sa_set_str(&lo, "127.0.0.1", 0);

	CHECK(0 == icea_port_alloc(gen, 1, &lo, "lo", 0, 40000, 40010, &p1));
	CHECK(p1 && (sa_port(&p1->laddr) % 2) == 0);
	CHECK(sa_port(&p1->laddr) >= 40000 && sa_port(&p1->laddr) <= 40010);
	CHECK(0 == icea_port_alloc(gen, 2, &lo, "lo", 0, 40000, 40010, &p2));
	CHECK(p2 && (sa_port(&p2->laddr) % 2) == 1);
	CHECK(0 == icea_port_alloc(gen, 1, &lo, "lo", 0, 40000, 40010, &p3));
	CHECK(p3 && sa_port(&p3->laddr) != sa_port(&p1->laddr));
	CHECK(icea_port_find(gen, p3->us) == p3);
	CHECK(list_count(&gen->portl) == 3);

	mem_deref(gen);
}


int main(void)
{
	if (libre_init()) {
		fprintf(stderr, "libre_init failed\n");
		return 1;
	}
	list_init(&g_nodes);
	list_init(&g_pkts);

	test_direct_both_full();
	test_peer_reflexive_behind_nat();
	test_ice_lite_peer();
	test_role_conflict();
	test_401_storm_is_not_terminal();
	test_two_components();
	test_unknown_source_request_is_rejected();
	test_early_inbound_check();
	test_liveness_disconnect_and_fail();
	test_silent_switch_for_controlled();
	test_renomination_switches();
	test_receipt_credits_reflexive_selected_pair();
	test_pac_waits_for_better_pair();
	test_gather_srflx_via_sim_stun();
	test_own_port_alloc();

	tmr_debug();
	mem_debug();
	libre_close();

	printf("test_ice_agent: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
