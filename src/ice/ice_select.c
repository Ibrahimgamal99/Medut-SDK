/**
 * @file ice_select.c  echo-ice: pair states, selection, nomination, liveness
 *
 * Runs from the scheduler tick.  Pair write/receive states follow
 * libwebrtc's Connection: writable while responses keep coming,
 * unreliable after 5 unanswered pings spanning 5 s, timed out at 15 s,
 * receiving while any STUN or data arrived within 2.5 s, dead once timed
 * out and silent for 30 s.  RFC 7675 consent is the time since the last
 * integrity-checked response; a pair without it for 30 s must not be sent
 * on.
 *
 * Selection is libwebrtc's comparison chain; nomination is RFC 8445's
 * regular mode — once, on the selected pair — unless the peer negotiated
 * a=ice-options:renomination.  The generation's state mirrors
 * RTCIceConnectionState and is derived from the newest generation only
 * (the owner keeps a previous generation's pair alive through a restart,
 * but it never blocks `failed`).
 */

#include <string.h>
#include "ice_internal.h"


/* ── Per-pair state math ─────────────────────────────────────────────────── */

static uint64_t oldest_unanswered(const struct icea_gen *gen,
				  const struct icea_pair *p)
{
	struct le *le;

	LIST_FOREACH(&gen->pingl, le) {      /* oldest first */
		const struct icea_ping *ping = le->data;
		if (ping->pair == p && !ping->early)
			return ping->sent;
	}
	return 0;
}


static void update_pair(struct icea_gen *gen, struct icea_pair *p,
			uint64_t now)
{
	const struct icea_conf *c = gen->conf;
	uint64_t oldest = p->unanswered ? oldest_unanswered(gen, p) : 0;
	uint64_t last = icea_pair_last_received(p);

	if (p->ws == ICEA_WS_WRITABLE &&
	    p->unanswered >= c->write_connect_failures && oldest &&
	    now - oldest >= c->write_connect_timeout_ms)
		p->ws = ICEA_WS_UNRELIABLE;

	if ((p->ws == ICEA_WS_UNRELIABLE || p->ws == ICEA_WS_INIT) &&
	    p->unanswered && oldest && now - oldest >= c->write_timeout_ms)
		p->ws = ICEA_WS_TIMEOUT;

	p->receiving = last && now - last <= c->receiving_timeout_ms;
}


bool icea_pair_dead(const struct icea_pair *p, uint64_t now)
{
	const struct icea_conf *c = p->gen->conf;
	uint64_t last;

	if (p->err_count >= c->err_failed_count)
		return true;
	if (p->ws != ICEA_WS_TIMEOUT)
		return false;

	last = icea_pair_last_received(p);
	return last ? now - last > c->dead_ms : now - p->created > c->dead_ms;
}


bool icea_pair_consent_ok(const struct icea_pair *p, uint64_t now)
{
	return p->last_consent_ok &&
	       now - p->last_consent_ok < p->gen->conf->consent_expiry_ms;
}


/* ── Selection ───────────────────────────────────────────────────────────── */

static bool ready(const struct icea_pair *p)
{
	return p->valid && p->state == ICE_CANDPAIR_SUCCEEDED &&
	       p->ws != ICEA_WS_TIMEOUT;
}


static bool nominated_for_us(const struct icea_gen *gen,
			     const struct icea_pair *p)
{
	return gen->role == ICE_ROLE_CONTROLLING ? p->nominated
						 : p->remote_nominated;
}


static int rx_state_rank(const struct icea_pair *p)
{
	if (p->ws == ICEA_WS_WRITABLE && p->receiving) return 3;
	if (p->ws == ICEA_WS_WRITABLE)                 return 2;
	if (p->receiving)                              return 1;
	return 0;
}


/* True when `a` should replace `b` as the selected pair. */
static bool pair_better(const struct icea_gen *gen, const struct icea_pair *a,
			const struct icea_pair *b)
{
	int ra, rb;

	if (!b)
		return ready(a);
	if (ready(a) != ready(b))
		return ready(a);

	/* A nominated pair is the one both sides agreed on. */
	if (nominated_for_us(gen, a) != nominated_for_us(gen, b))
		return nominated_for_us(gen, a);

	ra = rx_state_rank(a);
	rb = rx_state_rank(b);
	if (ra != rb)
		return ra > rb;

	/* Controlled and both receiving: follow where the peer sends. */
	if (gen->role == ICE_ROLE_CONTROLLED && a->receiving && b->receiving &&
	    a->last_data_recv != b->last_data_recv)
		return a->last_data_recv > b->last_data_recv;

	if (a->lc->port->cost != b->lc->port->cost)
		return a->lc->port->cost < b->lc->port->cost;

	if (a->prio != b->prio)
		return a->prio > b->prio;

	if (a->rtt_samples && b->rtt_samples &&
	    a->rtt_ms + gen->conf->rtt_switch_margin_ms < b->rtt_ms)
		return true;

	return false;
}


static void reselect(struct icea_gen *gen, unsigned compid, uint64_t now)
{
	struct icea_pair *cur = gen->selected[compid - 1];
	struct icea_pair *best = cur;
	struct le *le;
	bool was_writable = gen->sel_writable[compid - 1];

	LIST_FOREACH(&gen->pairl, le) {
		struct icea_pair *p = le->data;

		if (p->lc->compid != compid || p == best)
			continue;
		if (!ready(p) && !(p->valid && p->receiving))
			continue;
		if (pair_better(gen, p, best))
			best = p;
	}

	/* Leaving a pair that merely stopped receiving waits a moment
	 * (libwebrtc receiving_switching_delay), unless the newcomer is the
	 * nominated one or the current pair is no longer writable. */
	if (best != cur && cur && !cur->receiving &&
	    cur->ws == ICEA_WS_WRITABLE && !nominated_for_us(gen, best) &&
	    now - icea_pair_last_received(cur) < gen->conf->switching_delay_ms)
		best = cur;

	gen->sel_writable[compid - 1] = best && best->ws == ICEA_WS_WRITABLE;

	/* The same pair turning writable is news to the owner too: media
	 * starts on a writable pair, and a pair can be selected before it is
	 * one.  A peer answering a backlog of checks credits the oldest
	 * first, so the pair is unreliable at the first answer and writable
	 * a few ms later with nothing else changed — every inbound call that
	 * rang for more than 5 s came up silent (2026-09-29). */
	if (best == cur) {
		if (cur && gen->sel_writable[compid - 1] && !was_writable) {
			ICEA_DEBUG(gen, "ice[gen %u]: selected {%u} is writable\n",
				   gen->seq, compid);
			if (gen->cb.selected)
				gen->cb.selected(gen, compid, gen->cb.arg);
		}
		return;
	}

	gen->selected[compid - 1] = best;
	gen->selected_since[compid - 1] = now;

	ICEA_DEBUG(gen, "ice[gen %u]: selected {%u} %s %J <-> %s %J%s\n",
		   gen->seq, compid, ice_cand_type2name(best->lc->type),
		   &best->lc->addr, ice_cand_type2name(best->rc->type),
		   &best->rc->addr, nominated_for_us(gen, best) ? " (nominated)"
								: "");

	/* Regular nomination: the selected pair gets USE-CANDIDATE on its
	 * very next check, not at the next cadence slot. */
	if (icea_nominate_decision(gen, best))
		best->triggered = true;

	if (gen->cb.selected)
		gen->cb.selected(gen, compid, gen->cb.arg);
}


bool icea_nominate_decision(struct icea_gen *gen, const struct icea_pair *p)
{
	const struct icea_pair *sel;
	struct le *le;

	if (!gen || !p || gen->role != ICE_ROLE_CONTROLLING)
		return false;
	if (!p->valid || p->ws != ICEA_WS_WRITABLE)
		return false;

	sel = gen->selected[p->lc->compid - 1];

	if (gen->renomination || gen->conf->nomination == ICEA_NOMINATE_RENOMINATION) {
		/* libwebrtc SEMI_AGGRESSIVE: nominate whatever we would
		 * rather send on, as often as it changes. */
		return p == sel || !sel || sel->ws != ICEA_WS_WRITABLE ||
		       p->prio > sel->prio;
	}

	/* RFC 8445 §8.1.1 regular nomination: exactly one pair per component
	 * for the life of the generation, and it is the selected one.  An
	 * ICE-lite peer additionally needs it to be the pair we will keep. */
	if (p != sel)
		return false;
	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *q = le->data;
		if (q->lc->compid == p->lc->compid && q->nominated)
			return false;
	}

	/* RFC 8863 (ICE PAC): the first pair to validate is not necessarily
	 * the best.  Against this PBX its private-LAN candidate answers first
	 * from the office network and the public one a few ms later; holding
	 * the nomination briefly while a higher-priority pair is still under
	 * check lets the better path win (measured 2026-09-15: the private
	 * path showed 2–5 % loss blips the public one never did). */
	if (gen->checks_started &&
	    icea_now() - gen->checks_started < gen->conf->nomination_delay_ms) {
		LIST_FOREACH(&gen->pairl, le) {
			const struct icea_pair *q = le->data;

			if (q->lc->compid != p->lc->compid || q == p)
				continue;
			if (q->prio <= p->prio)
				continue;
			if (q->state == ICE_CANDPAIR_FROZEN ||
			    q->state == ICE_CANDPAIR_WAITING ||
			    q->state == ICE_CANDPAIR_INPROGRESS)
				return false;      /* give it its chance */
		}
	}
	return true;
}


/* ── Generation state ────────────────────────────────────────────────────── */

bool icea_gen_weak(const struct icea_gen *gen)
{
	for (unsigned i = 0; i < gen->ncomp; i++) {
		const struct icea_pair *s = gen->selected[i];
		if (!s || s->ws != ICEA_WS_WRITABLE || !s->receiving)
			return true;
	}
	return false;
}


static int state_rank(enum icea_state st)
{
	switch (st) {
	case ICEA_STATE_FAILED:       return 0;
	case ICEA_STATE_DISCONNECTED: return 1;
	case ICEA_STATE_NEW:
	case ICEA_STATE_GATHERING:
	case ICEA_STATE_CHECKING:     return 2;
	case ICEA_STATE_CONNECTED:    return 3;
	case ICEA_STATE_COMPLETED:    return 4;
	default:                      return 2;
	}
}


static enum icea_state comp_state(struct icea_gen *gen, unsigned compid,
				  uint64_t now)
{
	struct icea_pair *sel = gen->selected[compid - 1];
	struct le *le;
	bool any_pair = false, any_alive = false, any_writable = false;

	LIST_FOREACH(&gen->pairl, le) {
		const struct icea_pair *p = le->data;

		if (p->lc->compid != compid)
			continue;
		any_pair = true;
		if (!icea_pair_dead(p, now))
			any_alive = true;
		if (p->valid && p->ws == ICEA_WS_WRITABLE)
			any_writable = true;
	}

	if (!gen->remote_ready || !any_pair)
		return gen->gathered ? ICEA_STATE_CHECKING : ICEA_STATE_GATHERING;

	if (sel && sel->ws == ICEA_WS_WRITABLE && sel->receiving) {
		gen->was_connected[compid - 1] = true;
		return nominated_for_us(gen, sel) ? ICEA_STATE_COMPLETED
						  : ICEA_STATE_CONNECTED;
	}

	if (!any_alive && gen->gather_pending == 0)
		return ICEA_STATE_FAILED;

	if (gen->was_connected[compid - 1])
		return ICEA_STATE_DISCONNECTED;

	if (any_writable)
		return ICEA_STATE_CONNECTED;

	return ICEA_STATE_CHECKING;
}


void icea_select_tick(struct icea_gen *gen, uint64_t now)
{
	enum icea_state worst = ICEA_STATE_COMPLETED;
	struct le *le;

	if (!gen)
		return;

	LIST_FOREACH(&gen->pairl, le)
		update_pair(gen, le->data, now);

	for (unsigned c = 1; c <= gen->ncomp; c++) {
		enum icea_state st;
		struct icea_pair *sel;

		reselect(gen, c, now);

		/* A nomination held back by the RFC 8863 delay must not then
		 * wait for the selected pair's regular cadence as well: the
		 * moment the hold lapses (or the better pair gave up), ping it
		 * with USE-CANDIDATE. */
		sel = gen->selected[c - 1];
		if (sel && !sel->triggered && gen->role == ICE_ROLE_CONTROLLING &&
		    !nominated_for_us(gen, sel) && icea_nominate_decision(gen, sel))
			sel->triggered = true;

		st = comp_state(gen, c, now);
		if (state_rank(st) < state_rank(worst))
			worst = st;
	}

	if (gen->state == ICEA_STATE_CLOSED)
		return;
	if (worst == ICEA_STATE_FAILED && gen->failed_reported)
		return;
	if (worst == ICEA_STATE_FAILED)
		gen->failed_reported = true;

	icea_gen_set_state(gen, worst);
}
