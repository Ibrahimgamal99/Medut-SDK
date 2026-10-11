/**
 * @file ice_gen.c  echo-ice: generation lifecycle
 *
 * A generation is one ICE credential set — the initial one, or one restart.
 * It owns its ports, candidates, pairs and the ping table, and exactly one
 * timer (the scheduler tick).  Destruction order matters: the tick closes
 * over the generation, pings borrow pairs, pairs borrow candidates,
 * candidates borrow ports, and libre's stun destructor fires the completion
 * handler of every outstanding gathering transaction — so those are torn
 * down first (their owning ports go before `stun`).
 */

#include <stdarg.h>
#include <string.h>
#include "ice_internal.h"


static void gen_destructor(void *arg)
{
	struct icea_gen *gen = arg;

	tmr_cancel(&gen->tmr_tick);

	list_flush(&gen->pingl);
	list_flush(&gen->pairl);
	list_flush(&gen->rcandl);
	list_flush(&gen->lcandl);
	list_flush(&gen->portl);   /* port destructors drop ct_gath first */
	mem_deref(gen->stun);
	mem_deref(gen->tls);

	mem_deref(gen->rufrag);
	mem_deref(gen->rpwd);
}


int icea_gen_alloc(struct icea_gen **genp, const struct icea_conf *conf,
		   uint32_t seq, enum ice_role role, int af, unsigned ncomp,
		   const struct icea_gen_cb *cb)
{
	struct icea_gen *gen;

	if (!genp || !conf || ncomp < 1 || ncomp > ICEA_MAX_COMP)
		return EINVAL;

	gen = mem_zalloc(sizeof(*gen), gen_destructor);
	if (!gen)
		return ENOMEM;

	gen->conf  = conf;
	gen->seq   = seq;
	gen->role  = role;
	gen->af    = af;
	gen->ncomp = ncomp;
	gen->state = ICEA_STATE_NEW;
	if (cb)
		gen->cb = *cb;

	/* RFC 8445 §5.3: ufrag ≥ 4 chars (24 bits), pwd ≥ 22 chars (128 bits).
	 * Same sizes baresip's ice module used, so nothing downstream sees a
	 * change of shape. */
	rand_str(gen->lufrag, sizeof(gen->lufrag));
	rand_str(gen->lpwd,   sizeof(gen->lpwd));
	gen->tiebrk = rand_u64();

	list_init(&gen->portl);
	list_init(&gen->lcandl);
	list_init(&gen->rcandl);
	list_init(&gen->pairl);
	list_init(&gen->pingl);
	tmr_init(&gen->tmr_tick);

	*genp = gen;
	return 0;
}


void icea_gen_set_state(struct icea_gen *gen, enum icea_state st)
{
	enum icea_state prev;

	if (!gen || gen->state == st)
		return;

	prev = gen->state;
	gen->state = st;

	ICEA_DEBUG(gen, "ice[gen %u]: %s -> %s\n", gen->seq,
		   icea_state_name(prev), icea_state_name(st));

	if (gen->cb.state)
		gen->cb.state(gen, prev, st, gen->cb.arg);
}


const char *icea_state_name(enum icea_state st)
{
	switch (st) {
	case ICEA_STATE_NEW:          return "new";
	case ICEA_STATE_GATHERING:    return "gathering";
	case ICEA_STATE_CHECKING:     return "checking";
	case ICEA_STATE_CONNECTED:    return "connected";
	case ICEA_STATE_COMPLETED:    return "completed";
	case ICEA_STATE_DISCONNECTED: return "disconnected";
	case ICEA_STATE_FAILED:       return "failed";
	case ICEA_STATE_CLOSED:       return "closed";
	default:                      return "?";
	}
}


uint64_t icea_now(void)
{
	return tmr_jiffies();
}


void icea_debugf(const struct icea_conf *conf, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	(void)re_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (conf && conf->debug_h)
		conf->debug_h(buf);
	else
		re_printf("%s", buf);
}


int icea_gen_debug(struct re_printf *pf, const struct icea_gen *gen)
{
	struct le *le;
	int err;

	if (!gen)
		return 0;

	err = re_hprintf(pf, "--- ICE gen %u: role=%s state=%s ufrag=%s"
			 " remote=%s%s ---\n",
			 gen->seq, ice_role2name(gen->role),
			 icea_state_name(gen->state), gen->lufrag,
			 gen->rufrag ? gen->rufrag : "(none)",
			 gen->rlite ? " lite" : "");

	err |= re_hprintf(pf, " local candidates (%u):\n",
			  list_count(&gen->lcandl));
	LIST_FOREACH(&gen->lcandl, le) {
		const struct icea_cand *c = le->data;
		err |= re_hprintf(pf, "  {%u} fnd=%s prio=%08x %s:%J\n",
				  c->compid, c->foundation, c->prio,
				  ice_cand_type2name(c->type), &c->addr);
	}
	err |= re_hprintf(pf, " remote candidates (%u):\n",
			  list_count(&gen->rcandl));
	LIST_FOREACH(&gen->rcandl, le) {
		const struct icea_cand *c = le->data;
		err |= re_hprintf(pf, "  {%u} fnd=%s prio=%08x %s:%J\n",
				  c->compid, c->foundation, c->prio,
				  ice_cand_type2name(c->type), &c->addr);
	}
	err |= re_hprintf(pf, " pairs (%u):\n", list_count(&gen->pairl));
	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *p = le->data;
		err |= re_hprintf(pf, "  %s%s%s {%u} %s %J <-> %s %J"
				  " prio=%llx unans=%u rtt=%u\n",
				  gen->selected[p->lc->compid - 1] == p ? "*" : " ",
				  p->valid ? "V" : " ",
				  p->nominated || p->remote_nominated ? "N" : " ",
				  p->lc->compid,
				  ice_cand_type2name(p->lc->type), &p->lc->addr,
				  ice_cand_type2name(p->rc->type), &p->rc->addr,
				  (unsigned long long)p->prio, p->unanswered,
				  p->rtt_ms);
	}

	return err;
}
