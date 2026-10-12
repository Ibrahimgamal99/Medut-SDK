/**
 * @file dns.c  RFC 3263 SIP server location
 *
 * Implements the NAPTR→SRV lookup chain from RFC 3263 §4 using libre's
 * async DNS client obtained from baresip's network layer.
 *
 * The resolution stops at SRV level and returns (transport, hostname, port)
 * tuples. Final A/AAAA resolution is left to baresip's transport layer,
 * which does it internally when opening the TCP/TLS/WS connection. This
 * matches the layering described in RFC 3263 §4.2.
 *
 * All callbacks and the done_h handler fire on the re_main thread.
 *
 * Caller contract:
 *   bsdk_dns_init()   — call after baresip_init() in core.c
 *   bsdk_dns_resolve() — call from re_main thread only
 *   bsdk_dns_close()  — call before baresip_close() in core.c
 */

#include <string.h>
#include <stdlib.h>
#include "medut_internal.h"

/* ── DNS client ────────────────────────────────────────────────────────── */

static struct dnsc *g_dnsc;   /* borrowed from baresip network — not owned */

/* ── Limits ────────────────────────────────────────────────────────────── */

#define MAX_TARGETS  16   /* max (transport, host, port) results returned */
#define MAX_SRV       8   /* max SRV names to chase from NAPTR records    */

/* ── Internal types ────────────────────────────────────────────────────── */

struct bsdk_dns_target {
	medut_transport_t transport;
	char                host[256];
	uint16_t            port;
	uint16_t            priority;
	uint16_t            weight;
};

struct bsdk_dns_result {
	struct bsdk_dns_target targets[MAX_TARGETS];
	size_t                  count;
	int                     err;
};

struct srv_slot {
	char                name[256];   /* SRV qname, e.g. _sip._udp.example.com */
	medut_transport_t transport;
};

struct dns_lookup {
	char                domain[256];
	medut_transport_t transport_hint;
	uint16_t            port_hint;   /* 0 = no explicit port in URI */

	int                 pending;     /* in-flight DNS queries */
	bool                done;

	struct bsdk_dns_result result;

	struct srv_slot     srv[MAX_SRV];
	int                 srv_count;

	bsdk_dns_done_h    *done_h;
	void               *done_arg;
};

/* ── Helpers ───────────────────────────────────────────────────────────── */

static medut_transport_t transport_from_naptr_service(const char *svc)
{
	if (!svc)
		return (medut_transport_t)-1;
	/* RFC 3263 §4.1 service field values */
	if (!str_casecmp(svc, "SIPS+D2T")) return MEDUT_TRANSPORT_TLS;
	if (!str_casecmp(svc, "SIP+D2T"))  return MEDUT_TRANSPORT_TCP;
	if (!str_casecmp(svc, "SIP+D2U"))  return MEDUT_TRANSPORT_UDP;
	if (!str_casecmp(svc, "SIPS+D2W")) return MEDUT_TRANSPORT_WSS;
	if (!str_casecmp(svc, "SIP+D2W"))  return MEDUT_TRANSPORT_WS;
	return (medut_transport_t)-1;
}

static uint16_t default_port_for_transport(medut_transport_t t)
{
	switch (t) {
	case MEDUT_TRANSPORT_TLS: return 5061;
	/* WebSocket defaults, as in bsdk_parse_server_url. */
	case MEDUT_TRANSPORT_WS:  return 80;
	case MEDUT_TRANSPORT_WSS: return 443;
	default:                    return 5060;
	}
}

static const char *srv_prefix_for_transport(medut_transport_t t)
{
	switch (t) {
	case MEDUT_TRANSPORT_TLS: return "_sips._tcp.";
	case MEDUT_TRANSPORT_TCP: return "_sip._tcp.";
	case MEDUT_TRANSPORT_WS:  return "_sip._tcp.";
	case MEDUT_TRANSPORT_WSS: return "_sips._tcp.";
	default:                    return "_sip._udp.";
	}
}

static void add_result(struct dns_lookup *lk, medut_transport_t t,
                        const char *host, uint16_t port,
                        uint16_t pri, uint16_t weight)
{
	if (lk->result.count >= MAX_TARGETS)
		return;
	struct bsdk_dns_target *tgt = &lk->result.targets[lk->result.count++];
	tgt->transport = t;
	str_ncpy(tgt->host, host, sizeof(tgt->host));
	tgt->port     = port;
	tgt->priority = pri;
	tgt->weight   = weight;
}

/**
 * Order the targets the way a client is required to try them: lowest
 * priority value first, and within one priority, highest weight first
 * (RFC 2782 §3 — weight is a share of load, and taking the heaviest first is
 * the deterministic approximation of the random selection the RFC describes).
 *
 * The handlers append results in whatever order the DNS answers arrive, so
 * without this the "first" target is an accident of packet timing — which
 * makes a failover walk through the list meaningless.  Insertion sort: the
 * list is at most MAX_TARGETS entries and this runs once per lookup.
 */
static void sort_targets(struct bsdk_dns_result *res)
{
	for (size_t i = 1; i < res->count; i++) {
		struct bsdk_dns_target tmp = res->targets[i];
		size_t j = i;

		while (j > 0 &&
		       (res->targets[j-1].priority > tmp.priority ||
		        (res->targets[j-1].priority == tmp.priority &&
		         res->targets[j-1].weight   < tmp.weight))) {
			res->targets[j] = res->targets[j-1];
			j--;
		}
		res->targets[j] = tmp;
	}
}

static void lookup_finish(struct dns_lookup *lk)
{
	if (lk->done)
		return;
	lk->done = true;
	if (lk->result.count == 0)
		lk->result.err = ENOENT;
	else
		sort_targets(&lk->result);
	lk->done_h(&lk->result, lk->done_arg);
	mem_deref(lk);
}

/* ── SRV query handler ─────────────────────────────────────────────────── */

struct srv_ctx {
	struct dns_lookup  *lk;
	medut_transport_t transport;
};

static void srv_handler(int err, const struct dnshdr *hdr,
                        struct list *ansl, struct list *authl,
                        struct list *addl, void *arg)
{
	struct srv_ctx    *sc = arg;
	struct dns_lookup *lk = sc->lk;
	medut_transport_t t = sc->transport;
	bool found = false;

	(void)hdr; (void)authl; (void)addl;

	if (!err && ansl) {
		struct le *le;
		LIST_FOREACH(ansl, le) {
			struct dnsrr *rr = le->data;
			if (rr->type != DNS_TYPE_SRV)
				continue;
			/* RFC 2782: "." target means no service at this transport */
			if (!str_cmp(rr->rdata.srv.target, "."))
				continue;
			found = true;
			add_result(lk, t, rr->rdata.srv.target,
			           rr->rdata.srv.port,
			           rr->rdata.srv.pri,
			           rr->rdata.srv.weight);
		}
	}

	if (!found) {
		/* SRV absent or all "." targets — use domain:default_port directly */
		add_result(lk, t, lk->domain,
		           default_port_for_transport(t), 0, 0);
	}

	mem_deref(sc);

	if (--lk->pending == 0)
		lookup_finish(lk);
}

/* ── NAPTR query handler ───────────────────────────────────────────────── */

static void start_srv_queries(struct dns_lookup *lk);

static void naptr_handler(int err, const struct dnshdr *hdr,
                          struct list *ansl, struct list *authl,
                          struct list *addl, void *arg)
{
	struct dns_lookup *lk = arg;
	bool found = false;

	(void)hdr; (void)authl; (void)addl;

	if (!err && ansl) {
		struct le *le;
		LIST_FOREACH(ansl, le) {
			struct dnsrr *rr = le->data;
			if (rr->type != DNS_TYPE_NAPTR)
				continue;
			/* RFC 3263 §4.1: only "S" flag records point to SRV */
			if (!rr->rdata.naptr.flags ||
			    str_casecmp(rr->rdata.naptr.flags, "S") != 0)
				continue;

			medut_transport_t t =
				transport_from_naptr_service(rr->rdata.naptr.services);
			if ((int)t == -1)
				continue;
			if (lk->srv_count >= MAX_SRV)
				break;

			found = true;
			struct srv_slot *sl = &lk->srv[lk->srv_count++];
			str_ncpy(sl->name, rr->rdata.naptr.replace, sizeof(sl->name));
			sl->transport = t;
		}
	}

	if (!found) {
		/* No usable NAPTR — fall back to well-known SRV name */
		if (lk->srv_count < MAX_SRV) {
			struct srv_slot *sl = &lk->srv[lk->srv_count++];
			re_snprintf(sl->name, sizeof(sl->name), "%s%s",
			            srv_prefix_for_transport(lk->transport_hint),
			            lk->domain);
			sl->transport = lk->transport_hint;
		}
	}

	/* Consume the NAPTR query's pending slot before starting SRV queries,
	 * which will add their own pending slots. */
	--lk->pending;

	start_srv_queries(lk);
}

/* ── SRV query launcher ────────────────────────────────────────────────── */

static void start_srv_queries(struct dns_lookup *lk)
{
	int launched = 0;

	for (int i = 0; i < lk->srv_count; i++) {
		struct srv_ctx *sc = mem_zalloc(sizeof(*sc), NULL);
		if (!sc)
			continue;
		sc->lk        = lk;
		sc->transport = lk->srv[i].transport;

		struct dns_query *q = NULL;
		++lk->pending;
		int e = dnsc_query(&q, g_dnsc, lk->srv[i].name,
		                   DNS_TYPE_SRV, DNS_CLASS_IN,
		                   true, srv_handler, sc);
		mem_deref(q);   /* DNS client holds its own ref; ours is release */

		if (e) {
			--lk->pending;
			mem_deref(sc);
			continue;
		}
		++launched;
	}

	if (launched == 0) {
		/* All SRV queries failed to launch — add a bare A fallback entry */
		add_result(lk, lk->transport_hint, lk->domain,
		           lk->port_hint ? lk->port_hint
		                        : default_port_for_transport(lk->transport_hint),
		           0, 0);
		if (lk->pending == 0)
			lookup_finish(lk);
	}
}

/* ── Public API ────────────────────────────────────────────────────────── */

int bsdk_dns_init(void)
{
	/* Reuse the DNS client that baresip configured from the system resolver.
	 * We borrow this pointer — it is owned and freed by baresip. */
	g_dnsc = net_dnsc(baresip_network());
	if (!g_dnsc) {
		warning("Medut/dns: no DNS client available from baresip network\n");
		return ENOENT;
	}
	return 0;
}

void bsdk_dns_close(void)
{
	g_dnsc = NULL;
}

/**
 * Resolve a SIP domain using RFC 3263 NAPTR→SRV chain.
 *
 * Runs on re_main thread (must be called from re_main or via bsdk_dispatch).
 *
 * @param domain          Hostname to resolve (must not be a numeric IP)
 * @param transport_hint  Preferred transport, used when NAPTR is absent
 * @param port_hint       Explicit port from URI, 0 if none
 *                        When > 0, NAPTR/SRV are skipped and the result is
 *                        a single (transport_hint, domain, port_hint) entry
 *                        (RFC 3263 §4 step 1).
 * @param done_h          Completion callback — fires on re_main thread
 * @param arg             Passed through to done_h
 *
 * @return 0 on success (done_h will fire), or errno on immediate failure
 */
int bsdk_dns_resolve(const char *domain,
                     medut_transport_t transport_hint,
                     uint16_t port_hint,
                     bsdk_dns_done_h *done_h, void *arg)
{
	if (!domain || !done_h)
		return EINVAL;
	if (!g_dnsc)
		return ENOENT;

	struct dns_lookup *lk = mem_zalloc(sizeof(*lk), NULL);
	if (!lk)
		return ENOMEM;

	str_ncpy(lk->domain, domain, sizeof(lk->domain));
	lk->transport_hint = transport_hint;
	lk->port_hint      = port_hint;
	lk->done_h         = done_h;
	lk->done_arg       = arg;

	if (port_hint > 0) {
		/* Explicit port in URI: RFC 3263 says skip NAPTR+SRV, A/AAAA only.
		 * We skip A/AAAA too (baresip resolves that on connect) and return
		 * the domain directly with the given port. */
		add_result(lk, transport_hint, domain, port_hint, 0, 0);
		lk->result.err = 0;
		done_h(&lk->result, arg);
		mem_deref(lk);
		return 0;
	}

	/* Start NAPTR query */
	struct dns_query *q = NULL;
	++lk->pending;
	int err = dnsc_query(&q, g_dnsc, domain, DNS_TYPE_NAPTR,
	                     DNS_CLASS_IN, true, naptr_handler, lk);
	mem_deref(q);

	if (err) {
		--lk->pending;
		mem_deref(lk);
		return err;
	}

	return 0;
}


/* ── Result accessors ────────────────────────────────────────────────────────
 *
 * struct bsdk_dns_result is opaque outside this file so the target array stays
 * an implementation detail.  account.c needs to walk it for SRV failover, so
 * expose exactly the three things a walk needs.
 */

size_t bsdk_dns_result_count(const struct bsdk_dns_result *res)
{
	return res ? res->count : 0;
}

int bsdk_dns_result_err(const struct bsdk_dns_result *res)
{
	return res ? res->err : EINVAL;
}

int bsdk_dns_result_get(const struct bsdk_dns_result *res, size_t idx,
                        medut_transport_t *transport,
                        char *host, size_t host_sz, uint16_t *port)
{
	const struct bsdk_dns_target *t;

	if (!res || idx >= res->count)
		return EINVAL;

	t = &res->targets[idx];

	if (transport)
		*transport = t->transport;
	if (host && host_sz)
		str_ncpy(host, t->host, host_sz);
	if (port)
		*port = t->port;

	return 0;
}
