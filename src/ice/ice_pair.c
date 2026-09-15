/**
 * @file ice_pair.c  echo-ice: candidate pairs and the check list
 *
 * RFC 8445 §6.1.2: pair every local candidate with every remote candidate
 * of the same component and address family, compute pair priorities from
 * the controlling/controlled split, sort descending, prune duplicates
 * (a server-reflexive local candidate checks from its base, so a pair
 * through it duplicates the host pair), then unfreeze the best pair of
 * each foundation.
 *
 * Unlike libre's check list this one never terminates: pairs stay in the
 * list for the generation's whole life so the scheduler (ice_check.c) can
 * keep pinging them.  FAILED is reserved for teardown and pruning; a pair
 * that stops answering is tracked through its write state instead.
 */

#include <string.h>
#include "ice_internal.h"


static void pair_destructor(void *arg)
{
	struct icea_pair *p = arg;

	list_unlink(&p->le);
}


static bool pair_prio_sort(struct le *le1, struct le *le2, void *arg)
{
	const struct icea_pair *p1 = le1->data, *p2 = le2->data;
	(void)arg;

	return p1->prio >= p2->prio;
}


uint64_t icea_pair_calc_prio(enum ice_role role, uint32_t lprio,
			     uint32_t rprio)
{
	/* g = controlling agent's candidate priority, d = controlled's.
	 * pair priority = 2^32*MIN(G,D) + 2*MAX(G,D) + (G>D?1:0) */
	const uint32_t g = (role == ICE_ROLE_CONTROLLING) ? lprio : rprio;
	const uint32_t d = (role == ICE_ROLE_CONTROLLING) ? rprio : lprio;
	const uint64_t m = g < d ? g : d;
	const uint64_t x = g < d ? d : g;

	return (m << 32) + 2 * x + (g > d ? 1 : 0);
}


struct icea_pair *icea_pair_find(const struct icea_gen *gen,
				 const struct icea_cand *lc,
				 const struct icea_cand *rc)
{
	struct le *le;

	if (!gen)
		return NULL;

	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;
		if (p->lc == lc && p->rc == rc)
			return p;
	}
	return NULL;
}


struct icea_pair *icea_pair_by_src(const struct icea_gen *gen, unsigned compid,
				   const struct icea_port *port,
				   const struct sa *src)
{
	struct le *le;

	if (!gen || !src)
		return NULL;

	/* Prefer an exact (port, remote) match; a packet that arrives on a
	 * port from a remote we know pairs with any local candidate on that
	 * port — host, srflx and prflx all share the socket. */
	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;

		if (p->lc->compid != compid)
			continue;
		if (port && p->lc->port != port)
			continue;
		if (!sa_cmp(&p->rc->addr, src, SA_ALL))
			continue;

		return p;
	}
	return NULL;
}


struct icea_pair *icea_pair_create(struct icea_gen *gen, struct icea_cand *lc,
				   struct icea_cand *rc)
{
	struct icea_pair *p;

	if (!gen || !lc || !rc)
		return NULL;

	p = icea_pair_find(gen, lc, rc);
	if (p)
		return p;

	p = mem_zalloc(sizeof(*p), pair_destructor);
	if (!p)
		return NULL;

	p->gen     = gen;
	p->lc      = lc;
	p->rc      = rc;
	p->prio    = icea_pair_calc_prio(gen->role, lc->prio, rc->prio);
	p->state   = ICE_CANDPAIR_FROZEN;
	p->created = icea_now();
	p->ws      = ICEA_WS_INIT;

	list_insert_sorted(&gen->pairl, pair_prio_sort, NULL, &p->le, p);

	return p;
}


int icea_pair_form(struct icea_gen *gen)
{
	struct le *lle, *rle;
	unsigned n = 0;

	if (!gen)
		return EINVAL;
	if (list_isempty(&gen->lcandl) || list_isempty(&gen->rcandl))
		return ENOENT;

	LIST_FOREACH(&gen->lcandl, lle) {
		struct icea_cand *lc = lle->data;

		LIST_FOREACH(&gen->rcandl, rle) {
			struct icea_cand *rc = rle->data;

			if (lc->compid != rc->compid)
				continue;
			if (sa_af(&lc->addr) != sa_af(&rc->addr))
				continue;
			if (icea_pair_find(gen, lc, rc))
				continue;

			/* §6.1.2.4 applied at formation: a server-reflexive
			 * local candidate checks from its base, so its pair
			 * with `rc` is the host pair under another name.
			 * Locals are sorted host-first, so the host pair is
			 * already there when we get here — and formation can
			 * be re-run later (new remote candidates) without
			 * resurrecting what icea_pair_prune() removed. */
			if (lc->type == ICE_CAND_TYPE_SRFLX && lc->base &&
			    lc->base != lc &&
			    icea_pair_find(gen, lc->base, rc))
				continue;

			if (!icea_pair_create(gen, lc, rc))
				return ENOMEM;
			n++;
		}
	}

	ICEA_DEBUG(gen, "ice[gen %u]: formed %u pairs (%u total)\n",
		   gen->seq, n, list_count(&gen->pairl));

	return 0;
}


/* The address a check from this local candidate really leaves from. */
static const struct sa *check_base(const struct icea_cand *c)
{
	return (c->type == ICE_CAND_TYPE_SRFLX && c->base) ? &c->base->addr
							   : &c->addr;
}


void icea_pair_prune(struct icea_gen *gen)
{
	struct le *le1, *tmp;
	unsigned n = 0;

	if (!gen)
		return;

	/* §6.1.2.4: replace a server-reflexive local candidate by its base
	 * and drop any pair whose (base, remote) equals a pair further up
	 * the list.  Sorted descending, so the survivor is always the one
	 * seen first.  Pairs that already carry state are never pruned:
	 * this is only meaningful before checks start on the generation. */
	LIST_FOREACH_SAFE(&gen->pairl, le1, tmp) {
		struct icea_pair *p1 = le1->data;
		struct le *le2;

		for (le2 = gen->pairl.head; le2 && le2 != le1; le2 = le2->next) {
			const struct icea_pair *p0 = le2->data;

			if (p0->lc->compid != p1->lc->compid)
				continue;
			if (!sa_cmp(check_base(p0->lc), check_base(p1->lc),
				    SA_ALL))
				continue;
			if (!sa_cmp(&p0->rc->addr, &p1->rc->addr, SA_ALL))
				continue;

			if (p1->state == ICE_CANDPAIR_FROZEN && !p1->valid) {
				mem_deref(p1);
				n++;
			}
			break;
		}
	}

	if (n)
		ICEA_DEBUG(gen, "ice[gen %u]: pruned %u pairs\n", gen->seq, n);
}


static bool same_foundation(const struct icea_pair *a, const struct icea_pair *b)
{
	return 0 == str_cmp(a->lc->foundation, b->lc->foundation) &&
	       0 == str_cmp(a->rc->foundation, b->rc->foundation);
}


void icea_pair_set_states(struct icea_gen *gen)
{
	struct le *le;

	if (!gen)
		return;

	/* §6.1.2.6: for every foundation, the pair with the lowest component
	 * ID — the highest priority among those — goes to Waiting; the rest
	 * stay Frozen until a pair of their foundation succeeds. */
	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data, *best = p;
		struct le *le2;

		if (p->state != ICE_CANDPAIR_FROZEN)
			continue;

		LIST_FOREACH(&gen->pairl, le2) {
			struct icea_pair *q = le2->data;

			if (q == p || !same_foundation(p, q))
				continue;
			if (q->lc->compid < best->lc->compid ||
			    (q->lc->compid == best->lc->compid &&
			     q->prio > best->prio))
				best = q;
		}

		if (best->state == ICE_CANDPAIR_FROZEN)
			best->state = ICE_CANDPAIR_WAITING;
	}
}


void icea_pair_reprio(struct icea_gen *gen)
{
	struct le *le;

	if (!gen)
		return;

	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;
		p->prio = icea_pair_calc_prio(gen->role, p->lc->prio, p->rc->prio);
	}

	list_sort(&gen->pairl, pair_prio_sort, NULL);
}
