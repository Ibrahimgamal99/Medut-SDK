/**
 * @file test_ice_candpair.c  A candidate pair that fails, then succeeds, must
 *                            end up Succeeded
 *
 * Regression test for the two libre hunks in cmake/patch-re-sources.cmake
 * (ice/candpair.c).  The symptom on-device was
 *
 *   icecomp: {audio.1} set_selected: invalid state 'Failed'
 *     [{comp=1} Failed { V } seth_lte0:prflx:… <---> host:…]
 *
 * — a pair that is valid, selected and carrying media, but whose state says
 * Failed because libre treats Failed as terminal and (a) icem_candpair_clone()
 * copied that state onto the peer-reflexive clone, (b) icem_candpair_make_valid()
 * could not move it.  concluding_ice() selects by state, so such a pair is
 * never nominated.
 *
 * Links against the libre in the sysroot.  Until that libre is rebuilt from
 * the patched tree the last two tests fail — that is the point of the test.
 * The fixture is the minimum the two functions touch: an icem with its lists,
 * one component, and two candidates (no sockets, no STUN, debug off so
 * icecomp_printf() is a no-op).
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
/* Not <re.h>: it pulls in re_trice.h, whose *own* struct ice_candpair clashes
 * with the internal one in ice.h.  Use the header set candpair.c itself
 * compiles against, plus re_main.h for libre_init/libre_close. */
#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_list.h>
#include <re_tmr.h>
#include <re_sa.h>
#include <re_stun.h>
#include <re_ice.h>
#include <re_main.h>
#include "../../third_party/re/src/ice/ice.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); \
	} } while (0)

struct fixture {
	struct icem      icem;
	struct icem_comp comp;
	struct ice_cand *lhost;   /* local host candidate         */
	struct ice_cand *lprflx;  /* local peer-reflexive, learned */
	struct ice_cand *rhost;   /* remote host candidate        */
};

static struct ice_cand *cand(enum ice_cand_type type, const char *ip,
			     uint16_t port)
{
	struct ice_cand *c = mem_zalloc(sizeof(*c), NULL);
	c->type   = type;
	c->compid = 1;
	c->prio   = 1;
	sa_set_str(&c->addr, ip, port);
	return c;
}

static void fixture_init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));

	list_init(&f->icem.lcandl);
	list_init(&f->icem.rcandl);
	list_init(&f->icem.checkl);
	list_init(&f->icem.validl);
	list_init(&f->icem.compl);
	f->icem.conf.debug = false;
	str_ncpy(f->icem.name, "audio", sizeof(f->icem.name));

	f->comp.icem = &f->icem;
	f->comp.id   = 1;
	list_append(&f->icem.compl, &f->comp.le, &f->comp);

	f->lhost  = cand(ICE_CAND_TYPE_HOST,  "10.0.0.2",       40000);
	f->lprflx = cand(ICE_CAND_TYPE_PRFLX, "196.156.38.66",  17028);
	f->rhost  = cand(ICE_CAND_TYPE_HOST,  "82.129.158.253", 14738);
}

static void fixture_close(struct fixture *f)
{
	list_flush(&f->icem.checkl);
	list_flush(&f->icem.validl);
	mem_deref(f->lhost);
	mem_deref(f->lprflx);
	mem_deref(f->rhost);
}

static bool on_list(const struct list *lst, const struct ice_candpair *cp)
{
	struct le *le;
	LIST_FOREACH(lst, le) {
		if (le->data == cp)
			return true;
	}
	return false;
}

/* Unchanged behaviour: a pair that never failed becomes Succeeded. */
static void test_make_valid_from_waiting(void)
{
	struct fixture f;
	struct ice_candpair *cp = NULL;

	printf("make_valid on a Waiting pair\n");
	fixture_init(&f);

	CHECK(icem_candpair_alloc(&cp, &f.icem, f.lhost, f.rhost) == 0, "alloc");
	icem_candpair_set_state(cp, ICE_CANDPAIR_WAITING);

	icem_candpair_make_valid(cp);

	CHECK(cp->state == ICE_CANDPAIR_SUCCEEDED, "state %s",
	      ice_candpair_state2name(cp->state));
	CHECK(cp->valid, "valid not set");
	CHECK(on_list(&f.icem.validl, cp), "not on the valid list");

	fixture_close(&f);
}

/* Hunk (b): a late success on a pair already marked Failed. */
static void test_make_valid_from_failed(void)
{
	struct fixture f;
	struct ice_candpair *cp = NULL;

	printf("make_valid on a Failed pair (late STUN success)\n");
	fixture_init(&f);

	CHECK(icem_candpair_alloc(&cp, &f.icem, f.lhost, f.rhost) == 0, "alloc");
	icem_candpair_failed(cp, ETIMEDOUT, 0);
	CHECK(cp->state == ICE_CANDPAIR_FAILED, "precondition: not Failed");

	icem_candpair_make_valid(cp);

	CHECK(cp->state == ICE_CANDPAIR_SUCCEEDED,
	      "state stayed %s — Failed is being treated as terminal",
	      ice_candpair_state2name(cp->state));
	CHECK(cp->valid, "valid not set");
	CHECK(cp->err == 0 && cp->scode == 0, "err/scode not cleared");
	CHECK(on_list(&f.icem.validl, cp), "not on the valid list");
	CHECK(!on_list(&f.icem.checkl, cp), "still on the check list");

	fixture_close(&f);
}

/* Hunk (a): the peer-reflexive clone of a Failed parent. */
static void test_clone_of_failed_parent(void)
{
	struct fixture f;
	struct ice_candpair *parent = NULL, *clone = NULL;

	printf("clone of a Failed pair for a prflx candidate\n");
	fixture_init(&f);

	CHECK(icem_candpair_alloc(&parent, &f.icem, f.lhost, f.rhost) == 0,
	      "alloc");
	icem_candpair_failed(parent, ETIMEDOUT, 0);

	/* What construct_valid_pair() does when the mapped address is new. */
	CHECK(icem_candpair_clone(&clone, parent, f.lprflx, f.rhost) == 0,
	      "clone");
	CHECK(clone != NULL, "no clone");
	if (!clone) { fixture_close(&f); return; }

	CHECK(clone->lcand == f.lprflx && clone->rcand == f.rhost,
	      "clone has the wrong candidates");
	CHECK(clone->state == ICE_CANDPAIR_FROZEN,
	      "clone born %s — inherited the parent's verdict",
	      ice_candpair_state2name(clone->state));
	CHECK(clone->err == 0 && clone->scode == 0,
	      "clone inherited err=%d scode=%u", clone->err, clone->scode);
	CHECK(!clone->valid, "clone born valid");

	/* …and the make_valid that follows in construct_valid_pair(). */
	icem_candpair_make_valid(clone);

	CHECK(clone->state == ICE_CANDPAIR_SUCCEEDED,
	      "clone ended %s", ice_candpair_state2name(clone->state));
	CHECK(on_list(&f.icem.validl, clone), "clone not on the valid list");

	/* This is the pair concluding_ice() must be able to find. */
	CHECK(icem_candpair_find_st(&f.icem.validl, 1,
				    ICE_CANDPAIR_SUCCEEDED) == clone,
	      "concluding_ice() would not find the clone by state");

	fixture_close(&f);
}

int main(void)
{
	int err = libre_init();
	if (err) {
		printf("libre_init: %d\n", err);
		return 1;
	}

	test_make_valid_from_waiting();
	test_make_valid_from_failed();
	test_clone_of_failed_parent();

	libre_close();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_ice_candpair: all checks passed\n");
	return 0;
}
