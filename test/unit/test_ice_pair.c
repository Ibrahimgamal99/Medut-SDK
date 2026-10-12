/**
 * @file test_ice_pair.c  medut-ice: pair formation, pruning, unfreezing
 *
 * Build: make -C test/unit test_ice_pair && ./test/unit/test_ice_pair
 */

#include <stdio.h>
#include <string.h>
#include <re.h>
#include "../../src/ice/ice_internal.h"

static int g_pass, g_fail;

#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int dummy_send(const struct sa *dst, struct mbuf *mb, void *arg)
{
	(void)dst; (void)mb; (void)arg;
	return 0;
}

static const struct icea_conf conf = ICEA_CONF_DEFAULT;


/* A generation with host+srflx locals on one port per component and the
 * PBX's two reachable host candidates as remotes. */
static struct icea_gen *make_gen(enum ice_role role, unsigned ncomp,
				 struct udp_sock **us)
{
	struct icea_gen *gen = NULL;
	struct sa a;

	CHECK(0 == icea_gen_alloc(&gen, &conf, 1, role, AF_INET, ncomp, NULL));
	CHECK(0 == udp_alloc_sockless(us, dummy_send, NULL, NULL));

	for (unsigned c = 1; c <= ncomp; c++) {
		struct icea_port *port = NULL;
		struct icea_cand *host;

		sa_set_str(&a, "10.0.0.5", 6000 + c - 1);
		CHECK(0 == icea_port_attach(gen, c, *us, &a, "eth0", 0, false,
					    &port));
		host = icea_cand_add_local(gen, ICE_CAND_TYPE_HOST, c, port,
					   NULL, &a, 65535);
		CHECK(host != NULL);
		sa_set_str(&a, "203.0.113.9", 6000 + c - 1);
		CHECK(icea_cand_add_local(gen, ICE_CAND_TYPE_SRFLX, c, port,
					  host, &a, 65535) != NULL);

		{
			struct ice_cand_attr r;
			memset(&r, 0, sizeof(r));
			r.proto = IPPROTO_UDP; r.compid = c;
			r.type = ICE_CAND_TYPE_HOST; r.prio = 2130706430u;
			strcpy(r.foundation, "H1");
			sa_set_str(&r.addr, "82.129.158.253", 17654 + c - 1);
			CHECK(icea_cand_add_remote(gen, &r) != NULL);
			strcpy(r.foundation, "H2");
			r.prio = 2130706429u;
			sa_set_str(&r.addr, "172.16.11.52", 17654 + c - 1);
			CHECK(icea_cand_add_remote(gen, &r) != NULL);
			/* a v6 remote never pairs with v4 locals */
			strcpy(r.foundation, "H3");
			sa_set_str(&r.addr, "2001:db8::5", 17654);
			CHECK(icea_cand_add_remote(gen, &r) != NULL);
		}
	}

	return gen;
}


static void test_prio(void)
{
	/* controlling: g = local, d = remote */
	uint64_t p1 = icea_pair_calc_prio(ICE_ROLE_CONTROLLING, 100, 200);
	uint64_t p2 = icea_pair_calc_prio(ICE_ROLE_CONTROLLED,  200, 100);
	uint64_t p3 = icea_pair_calc_prio(ICE_ROLE_CONTROLLING, 200, 100);

	CHECK(p1 == ((uint64_t)100 << 32) + 2 * 200 + 0);
	CHECK(p1 == p2);            /* mirror roles, mirror candidates */
	CHECK(p3 == p1 + 1);        /* g > d adds the tie bit */
}


static void test_form_prune_states_single(void)
{
	struct udp_sock *us = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us);
	struct le *le;
	unsigned waiting = 0, frozen = 0;

	CHECK(0 == icea_pair_form(gen));
	/* 2 locals × 2 v4 remotes, minus the srflx pairs that duplicate
	 * their host pair (formed from the same base) = 2; v6 excluded */
	CHECK(list_count(&gen->pairl) == 2);

	/* sorted descending */
	{
		uint64_t last = UINT64_MAX;
		LIST_FOREACH(&gen->pairl, le) {
			const struct icea_pair *p = le->data;
			CHECK(p->prio <= last);
			last = p->prio;
		}
	}

	/* prune is a no-op on an already-minimal list */
	icea_pair_prune(gen);
	CHECK(list_count(&gen->pairl) == 2);
	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *p = le->data;
		CHECK(p->lc->type == ICE_CAND_TYPE_HOST);
	}

	/* two remote foundations → two pairs unfrozen */
	icea_pair_set_states(gen);
	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *p = le->data;
		if (p->state == ICE_CANDPAIR_WAITING) waiting++;
		if (p->state == ICE_CANDPAIR_FROZEN)  frozen++;
	}
	CHECK(waiting == 2 && frozen == 0);

	/* forming again adds nothing */
	CHECK(0 == icea_pair_form(gen));
	CHECK(list_count(&gen->pairl) == 2);

	/* lookup by source */
	{
		struct sa src;
		struct icea_port *port = list_head(&gen->portl)->data;
		sa_set_str(&src, "172.16.11.52", 17654);
		CHECK(icea_pair_by_src(gen, 1, port, &src) != NULL);
		sa_set_str(&src, "172.16.11.52", 1);
		CHECK(icea_pair_by_src(gen, 1, port, &src) == NULL);
	}

	mem_deref(gen);
	mem_deref(us);
}


static void test_two_components(void)
{
	struct udp_sock *us = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLED, 2, &us);
	struct le *le;
	unsigned waiting1 = 0, waiting2 = 0;

	CHECK(0 == icea_pair_form(gen));
	CHECK(list_count(&gen->pairl) == 4);
	icea_pair_prune(gen);
	CHECK(list_count(&gen->pairl) == 4);

	/* per foundation only the lowest component is unfrozen */
	icea_pair_set_states(gen);
	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *p = le->data;
		if (p->state != ICE_CANDPAIR_WAITING)
			continue;
		if (p->lc->compid == 1) waiting1++; else waiting2++;
	}
	CHECK(waiting1 == 2 && waiting2 == 0);

	mem_deref(gen);
	mem_deref(us);
}


static void test_role_switch_reorders(void)
{
	struct udp_sock *us = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us);
	const struct icea_pair *first_before, *first_after;
	uint64_t prio_before;

	CHECK(0 == icea_pair_form(gen));
	icea_pair_prune(gen);
	first_before = list_head(&gen->pairl)->data;
	prio_before  = first_before->prio;

	/* Same candidates, other role: every pair's priority is recomputed
	 * (the tie bit flips where local ≠ remote) and the list re-sorted. */
	gen->role = ICE_ROLE_CONTROLLED;
	icea_pair_reprio(gen);
	first_after = list_head(&gen->pairl)->data;
	CHECK(first_after->prio != prio_before);
	{
		uint64_t last = UINT64_MAX;
		struct le *le;
		LIST_FOREACH(&gen->pairl, le) {
			const struct icea_pair *p = le->data;
			CHECK(p->prio <= last);
			last = p->prio;
		}
	}

	mem_deref(gen);
	mem_deref(us);
}


static void test_prune_keeps_stateful_pairs(void)
{
	struct udp_sock *us = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us);
	struct le *le;
	unsigned kept_srflx = 0;

	CHECK(0 == icea_pair_form(gen));
	/* Create the srflx pairs by hand — the path a validated check takes
	 * when the mapped address turns out to be the srflx — and mark them
	 * valid; prune must leave stateful pairs alone. */
	LIST_FOREACH(&gen->lcandl, le) {
		struct icea_cand *lc = le->data;
		struct le *rle;
		if (lc->type != ICE_CAND_TYPE_SRFLX)
			continue;
		LIST_FOREACH(&gen->rcandl, rle) {
			struct icea_cand *rc = rle->data;
			struct icea_pair *p;
			if (sa_af(&rc->addr) != AF_INET)
				continue;
			p = icea_pair_create(gen, lc, rc);
			CHECK(p != NULL);
			p->valid = true;
			p->state = ICE_CANDPAIR_SUCCEEDED;
		}
	}
	CHECK(list_count(&gen->pairl) == 4);
	icea_pair_prune(gen);
	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *p = le->data;
		if (p->lc->type == ICE_CAND_TYPE_SRFLX)
			kept_srflx++;
	}
	CHECK(kept_srflx == 2);
	CHECK(list_count(&gen->pairl) == 4);

	mem_deref(gen);
	mem_deref(us);
}


int main(void)
{
	if (libre_init()) {
		fprintf(stderr, "libre_init failed\n");
		return 1;
	}

	test_prio();
	test_form_prune_states_single();
	test_two_components();
	test_role_switch_reorders();
	test_prune_keeps_stateful_pairs();

	tmr_debug();
	mem_debug();
	libre_close();

	printf("test_ice_pair: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
