/**
 * @file ice_cand.c  echo-ice: candidates
 *
 * Priorities follow RFC 8445 §5.1.2 through libre's ice_cand_calc_prio():
 * type preference (host 126 › prflx 110 › srflx 100 › relay 0) in the top
 * byte, a 16-bit local preference derived from the interface cost, and the
 * component in the low byte.  Both candidate lists are kept sorted by
 * priority, descending — pair formation and the default-candidate scan
 * rely on it.
 *
 * Foundations (§5.1.1.3) hash the type and the *base* address, so the two
 * components of one interface share a foundation and unfreeze together,
 * while a server-reflexive candidate and its host do not.  libre hashed the
 * candidate's own address instead; the difference only affects our own
 * frozen/waiting grouping, never the wire.
 */

#include <string.h>
#include "ice_internal.h"


static void cand_destructor(void *arg)
{
	struct icea_cand *cand = arg;

	list_unlink(&cand->le);
}


/* Sort handler: higher priority first. */
static bool cand_prio_sort(struct le *le1, struct le *le2, void *arg)
{
	const struct icea_cand *c1 = le1->data, *c2 = le2->data;
	(void)arg;

	return c1->prio >= c2->prio;
}


static void compute_foundation(struct icea_cand *cand, uint32_t gen_seq)
{
	const struct sa *base = cand->base ? &cand->base->addr : &cand->addr;
	uint32_t v;

	v  = sa_hash(base, SA_ADDR);
	v ^= (uint32_t)cand->type << 28;
	if (cand->type != ICE_CAND_TYPE_HOST)
		v ^= sa_hash(&cand->addr, SA_ADDR) >> 3;

	/* A new ICE session gets new foundations (RFC 8445 §5.1.1.3 scopes
	 * them to one session).  It also matters to Asterisk: pjnath compares
	 * the candidates of a restart offer with the ones in use — foundation
	 * included — and preserves its old ICE session when they are equal,
	 * answering every check under the new credentials with 401 for good.
	 * A restart on an unchanged address (ICE failed, a port carried over
	 * with its mapping) would otherwise never validate (2026-10-11). */
	v ^= gen_seq * 0x9e3779b9u;

	(void)re_snprintf(cand->foundation, sizeof(cand->foundation),
			  "%08x", v);
}


uint16_t icea_cand_lpref(const struct icea_port *port)
{
	int32_t pref = 65535;

	if (!port)
		return 65535;

	/* Interface cost pushes a whole tier down: 0 = wired/Wi-Fi at the
	 * top, 1 = VPN, 2 = cellular.  Within a tier the scheduler's
	 * receive/RTT comparison, not the priority, tells ports apart. */
	pref -= port->cost * 4096;
	if (pref < 1024)
		pref = 1024;

	return (uint16_t)pref;
}


struct icea_cand *icea_cand_find(const struct list *lst, unsigned compid,
				 const struct sa *addr)
{
	struct le *le;

	if (!lst)
		return NULL;

	LIST_FOREACH(lst, le) {
		struct icea_cand *cand = le->data;

		if (compid && cand->compid != compid)
			continue;
		if (addr && !sa_cmp(&cand->addr, addr, SA_ALL))
			continue;

		return cand;
	}
	return NULL;
}


struct icea_cand *icea_cand_add_local(struct icea_gen *gen,
				      enum ice_cand_type type, unsigned compid,
				      struct icea_port *port,
				      struct icea_cand *base,
				      const struct sa *addr, uint16_t lpref)
{
	struct icea_cand *cand;

	if (!gen || !port || !addr || compid < 1 || compid > gen->ncomp)
		return NULL;

	/* Relay-only: nothing but relayed candidates is ever offered, so a
	 * peer cannot pick a direct path even when one exists. */
	if (gen->conf->policy == ICEA_TRANSPORT_RELAY &&
	    type != ICE_CAND_TYPE_RELAY)
		return NULL;

	cand = icea_cand_find(&gen->lcandl, compid, addr);
	if (cand)
		return cand;

	cand = mem_zalloc(sizeof(*cand), cand_destructor);
	if (!cand)
		return NULL;

	cand->local  = true;
	cand->type   = type;
	cand->compid = compid;
	cand->port   = port;
	cand->base   = base ? base : cand;
	cand->addr   = *addr;
	cand->prio   = ice_cand_calc_prio(type, lpref, compid);
	if (type != ICE_CAND_TYPE_HOST)
		cand->rel = cand->base->addr;

	compute_foundation(cand, gen->seq);

	list_insert_sorted(&gen->lcandl, cand_prio_sort, NULL, &cand->le, cand);

	ICEA_DEBUG(gen, "ice[gen %u]: local %s candidate {%u} %J prio=%08x\n",
		   gen->seq, ice_cand_type2name(type), compid, addr, cand->prio);

	return cand;
}


struct icea_cand *icea_cand_add_remote(struct icea_gen *gen,
				       const struct ice_cand_attr *attr)
{
	struct icea_cand *cand;

	if (!gen || !attr)
		return NULL;

	/* Only UDP candidates pair with ours (no ICE-TCP), only components
	 * we have, and never a link-local address: it is not a path from
	 * anywhere but the peer's own segment (RFC 8445 §5.1.1.1).  Loopback
	 * is not excluded — the peer may genuinely be on this host. */
	if (attr->proto != IPPROTO_UDP)
		return NULL;
	if (attr->compid < 1 || attr->compid > gen->ncomp)
		return NULL;
	if (sa_is_linklocal(&attr->addr))
		return NULL;

	cand = icea_cand_find(&gen->rcandl, attr->compid, &attr->addr);
	if (cand)
		return cand;

	cand = mem_zalloc(sizeof(*cand), cand_destructor);
	if (!cand)
		return NULL;

	cand->local  = false;
	cand->type   = attr->type;
	cand->compid = attr->compid;
	cand->prio   = attr->prio;
	cand->addr   = attr->addr;
	cand->rel    = attr->rel_addr;
	str_ncpy(cand->foundation, attr->foundation, sizeof(cand->foundation));

	list_insert_sorted(&gen->rcandl, cand_prio_sort, NULL, &cand->le, cand);

	ICEA_DEBUG(gen, "ice[gen %u]: remote %s candidate {%u} %J prio=%08x\n",
		   gen->seq, ice_cand_type2name(cand->type), cand->compid,
		   &cand->addr, cand->prio);

	return cand;
}


struct icea_cand *icea_cand_add_prflx_remote(struct icea_gen *gen,
					     unsigned compid, uint32_t prio,
					     const struct sa *addr)
{
	struct icea_cand *cand;

	if (!gen || !addr || compid < 1 || compid > gen->ncomp)
		return NULL;

	cand = icea_cand_find(&gen->rcandl, compid, addr);
	if (cand)
		return cand;

	cand = mem_zalloc(sizeof(*cand), cand_destructor);
	if (!cand)
		return NULL;

	cand->local  = false;
	cand->type   = ICE_CAND_TYPE_PRFLX;
	cand->compid = compid;
	cand->prio   = prio;
	cand->addr   = *addr;
	/* RFC 8445 §7.3.1.3: a random foundation the peer never told us. */
	(void)re_snprintf(cand->foundation, sizeof(cand->foundation),
			  "%08x", rand_u32());

	list_insert_sorted(&gen->rcandl, cand_prio_sort, NULL, &cand->le, cand);

	ICEA_DEBUG(gen, "ice[gen %u]: peer-reflexive remote {%u} %J prio=%u\n",
		   gen->seq, compid, addr, prio);

	return cand;
}


/* Is `cand` the base of some other local candidate? */
static bool is_a_base(const struct icea_gen *gen, const struct icea_cand *cand)
{
	struct le *le;

	LIST_FOREACH(&gen->lcandl, le) {
		const struct icea_cand *c = le->data;
		if (c != cand && c->base == cand)
			return true;
	}
	return false;
}


void icea_cand_redund_elim(struct icea_gen *gen)
{
	struct le *le1, *tmp;
	unsigned n = 0;

	if (!gen)
		return;

	/* RFC 8445 §5.1.3: two candidates with the same transport address
	 * and the same base are redundant; keep the higher priority.  A
	 * server-reflexive candidate that came back equal to its host is the
	 * usual case (no NAT). */
	LIST_FOREACH_SAFE(&gen->lcandl, le1, tmp) {
		struct icea_cand *c1 = le1->data;
		struct le *le2;

		for (le2 = le1->next; le2; le2 = le2->next) {
			struct icea_cand *c2 = le2->data;

			if (c1->compid != c2->compid)
				continue;
			if (c1->base != c2->base && c1 != c2->base &&
			    c2 != c1->base)
				continue;
			if (!sa_cmp(&c1->addr, &c2->addr, SA_ALL))
				continue;

			/* c1 sorts first, so it has the higher priority.
			 * Drop c2 unless something is based on it. */
			if (!is_a_base(gen, c2)) {
				mem_deref(c2);
				n++;
				break;
			}
		}
	}

	if (n)
		ICEA_DEBUG(gen, "ice[gen %u]: %u redundant candidates dropped\n",
			   gen->seq, n);
}


const struct icea_cand *icea_cand_default(const struct icea_gen *gen,
					  unsigned compid)
{
	const struct icea_cand *def = NULL;
	struct le *le;

	if (!gen)
		return NULL;

	/* RFC 8445 §5.1.4: relay if there is one, else server-reflexive,
	 * else host — the list is sorted so the first of each kind wins. */
	LIST_FOREACH(&gen->lcandl, le) {
		const struct icea_cand *cand = le->data;

		if (cand->compid != compid)
			continue;

		switch (cand->type) {

		case ICE_CAND_TYPE_RELAY:
			return cand;

		case ICE_CAND_TYPE_SRFLX:
			if (!def || def->type != ICE_CAND_TYPE_SRFLX)
				def = cand;
			break;

		case ICE_CAND_TYPE_HOST:
			if (!def)
				def = cand;
			break;

		default:
			break;
		}
	}

	return def;
}
