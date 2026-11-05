/**
 * @file test_sipreg_takeover.c  libre's registration client with patch Y
 *                               (cmake/patch-re-sources.cmake)
 *
 * The SDK replaces an account's registration client on every handover,
 * retry and recovery.  The new client takes over the old one's binding
 * (sipreg_takeover()): the old client sends nothing, and when a handover
 * moved the Contact, the new REGISTER removes the old one (expires=0) in
 * the same request, until a 2xx.  A replacement whose REGISTER failed or is
 * still out passes what it had to remove on to the next one.
 *
 * A fake registrar on 127.0.0.1 records each REGISTER's Contacts and answers
 * with a set status.  libre only.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <re.h>

/* The SDK's libre build leaves the public sip_dialog_route() to the SDK
 * (src/api/ws_path.c); without it, libre's own. */
const struct uri *__real_sip_dialog_route(const struct sip_dialog *dlg);
const struct uri *sip_dialog_route(const struct sip_dialog *dlg)
{
	return __real_sip_dialog_route(dlg);
}

/* Likewise websock_connect(); no WebSocket here. */
int websock_connect(struct websock_conn **connp, struct websock *sock,
		    struct http_cli *cli, const char *uri, unsigned kaint,
		    websock_estab_h *estabh, websock_recv_h *recvh,
		    websock_close_h *closeh, void *arg, const char *fmt, ...)
{
	(void)connp; (void)sock; (void)cli; (void)uri; (void)kaint;
	(void)estabh; (void)recvh; (void)closeh; (void)arg; (void)fmt;
	return ENOTSUP;
}

/* ── fake registrar ─────────────────────────────────────────────────────── */

enum { MAX_REQ = 32, MAX_CT = 4 };

struct req {
	int  nct;
	char ct[MAX_CT][256];
	char callid[128];
};

static struct udp_sock *g_us;
static struct sa g_reg_addr;
static struct req g_reqv[MAX_REQ];
static int g_nreq;
static uint16_t g_scode = 200;
static bool g_silent;      /* record, but do not answer */

static void fake_recv(const struct sa *src, struct mbuf *mb, void *arg)
{
	struct mbuf *r;
	struct req *rq;
	char *txt, *line, *save = NULL;
	(void)arg;

	if (g_nreq >= MAX_REQ)
		return;

	txt = mem_zalloc(mbuf_get_left(mb) + 1, NULL);
	r = mbuf_alloc(1024);
	if (!txt || !r)
		goto out;
	memcpy(txt, mbuf_buf(mb), mbuf_get_left(mb));

	if (strncmp(txt, "REGISTER ", 9))
		goto out;

	rq = &g_reqv[g_nreq++];
	(void)mbuf_printf(r, "SIP/2.0 %u %s\r\n", g_scode,
			  g_scode == 200 ? "OK" : "Refused");

	for (line = txt; line && *line; line = save) {

		save = strstr(line, "\r\n");
		if (save) {
			*save = '\0';
			save += 2;
		}

		if (!strncmp(line, "Via:", 4) || !strncmp(line, "From:", 5) ||
		    !strncmp(line, "Call-ID:", 8) ||
		    !strncmp(line, "CSeq:", 5))
			(void)mbuf_printf(r, "%s\r\n", line);
		else if (!strncmp(line, "To:", 3))
			(void)mbuf_printf(r, "%s;tag=fake\r\n", line);
		else if (!strncmp(line, "Contact:", 8) && rq->nct < MAX_CT)
			str_ncpy(rq->ct[rq->nct++], line,
				 sizeof(rq->ct[0]));

		if (!strncmp(line, "Call-ID:", 8))
			str_ncpy(rq->callid, line + 8, sizeof(rq->callid));
	}
	(void)mbuf_printf(r, "Content-Length: 0\r\n\r\n");

	if (g_silent)
		goto out;

	r->pos = 0;
	(void)udp_send(g_us, src, r);

 out:
	mem_deref(r);
	mem_deref(txt);
}

/* ── driving the loop ───────────────────────────────────────────────────── */

static struct tmr g_guard;
static int g_resp;      /* last final status, -err on error */

static void guard_h(void *arg)
{
	(void)arg;
	re_cancel();
}

static void resp_h(int err, const struct sip_msg *msg, void *arg)
{
	(void)arg;
	g_resp = err ? -err : (int)msg->scode;
	re_cancel();
}

/* Run the loop until a response (or `ms`), then `settle` ms more so a
 * stray request — an un-REGISTER from a replaced client — is seen too. */
static void run(uint32_t ms, uint32_t settle)
{
	g_resp = 0;
	tmr_start(&g_guard, ms, guard_h, NULL);
	(void)re_main(NULL);
	if (settle) {
		tmr_start(&g_guard, settle, guard_h, NULL);
		(void)re_main(NULL);
	}
	tmr_cancel(&g_guard);
}

static int newreg(struct sipreg **regp, struct sip *sip)
{
	char uri[64];

	(void)re_snprintf(uri, sizeof(uri), "sip:%J", &g_reg_addr);
	return sipreg_alloc(regp, sip, uri, "sip:u@test", NULL, "sip:u@test",
			    600, "u", NULL, 0, 0, NULL, NULL, false,
			    resp_h, NULL, NULL, NULL);
}

static int transp(struct sip *sip)
{
	struct sa local;

	sa_set_str(&local, "127.0.0.1", 0);
	return sip_transp_add(sip, SIP_TRANSP_UDP, &local);
}

/* ── checks ─────────────────────────────────────────────────────────────── */

static int g_fail;

static void check(bool ok, const char *what)
{
	printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		g_fail++;
}

static uint16_t ct_port(const char *ct)
{
	const char *p = strstr(ct, "@127.0.0.1:");

	return p ? (uint16_t)atoi(p + 11) : 0;
}

static bool ct_expires(const char *ct, unsigned exp)
{
	char want[32];

	(void)re_snprintf(want, sizeof(want), ";expires=%u", exp);
	return strstr(ct, want) != NULL;
}

/* The requests since `from` other than Call-ID `skip` (a replaced client's
 * retransmissions): exactly one, with `nct` Contacts. */
static const struct req *only_except(int from, int nct, const char *skip)
{
	const struct req *found = NULL;
	int i, k = 0;

	for (i = from; i < g_nreq; i++) {
		if (skip && !strcmp(g_reqv[i].callid, skip))
			continue;
		found = &g_reqv[i];
		k++;
	}
	if (k != 1 || found->nct != nct) {
		printf("    got %d request(s) since #%d (not %s):\n", k, from,
		       skip ? skip : "-");
		for (i = from; i < g_nreq; i++)
			for (int j = 0; j < g_reqv[i].nct; j++)
				printf("      #%d %s\n", i, g_reqv[i].ct[j]);
		return NULL;
	}
	return found;
}

/* Contact on `port` with ;expires=`exp` among rq's. */
static bool has_ct(const struct req *rq, uint16_t port, unsigned exp)
{
	for (int j = 0; rq && j < rq->nct; j++)
		if (ct_port(rq->ct[j]) == port && ct_expires(rq->ct[j], exp))
			return true;
	return false;
}

/* The requests since `from`: exactly one, with `nct` Contacts. */
static const struct req *only(int from, int nct)
{
	if (g_nreq - from != 1 || g_reqv[from].nct != nct) {
		int i;

		printf("    got %d request(s) since #%d:\n", g_nreq - from,
		       from);
		for (i = from; i < g_nreq; i++) {
			int j;

			for (j = 0; j < g_reqv[i].nct; j++)
				printf("      #%d %s\n", i, g_reqv[i].ct[j]);
		}
		return NULL;
	}
	return &g_reqv[from];
}

int main(void)
{
	struct sipreg *a = NULL, *b = NULL, *c = NULL, *d = NULL;
	struct sipreg *e = NULL, *f = NULL, *g = NULL, *h = NULL;
	char e_callid[128] = "";
	uint16_t pd, pe, pf;
	struct sip *sip = NULL;
	const struct req *rq;
	uint16_t pa, pb;
	struct sa local;
	int n;

	if (libre_init())
		return 1;

	sa_set_str(&local, "127.0.0.1", 0);
	if (udp_listen(&g_us, &local, fake_recv, NULL) ||
	    udp_local_get(g_us, &g_reg_addr) ||
	    sip_alloc(&sip, NULL, 16, 16, 16, "test", NULL, NULL) ||
	    transp(sip)) {
		printf("setup failed\n");
		return 1;
	}

	/* A registers. */
	if (newreg(&a, sip) || sipreg_send(a)) {
		printf("register failed\n");
		return 1;
	}
	run(2000, 0);
	check(g_resp == 200 && g_nreq == 1 && g_reqv[0].nct == 1,
	      "first REGISTER: one Contact, 200");
	pa = ct_port(g_reqv[0].ct[0]);

	/* Handover: the sockets are re-created on a new port, and B takes
	 * over from A.  One REGISTER: B's Contact, then A's with expires=0.
	 * Nothing from A. */
	sip_transp_flush(sip);
	(void)transp(sip);
	n = g_nreq;
	if (newreg(&b, sip))
		return 1;
	sipreg_takeover(b, a);
	a = sipreg_release(a);
	(void)sipreg_send(b);
	run(2000, 300);
	rq = only(n, 2);
	check(rq && g_resp == 200, "handover: one REGISTER, two Contacts");
	pb = rq ? ct_port(rq->ct[0]) : 0;
	check(rq && pb && pb != pa && ct_expires(rq->ct[0], 600),
	      "  first: the new Contact, expires=600");
	check(rq && ct_port(rq->ct[1]) == pa && ct_expires(rq->ct[1], 0),
	      "  second: the old Contact, expires=0");

	/* After the 2xx the old Contact is gone: a refresh names B's only. */
	n = g_nreq;
	(void)sipreg_refresh(b);
	run(2000, 0);
	rq = only(n, 1);
	check(rq && ct_port(rq->ct[0]) == pb && ct_expires(rq->ct[0], 600),
	      "refresh after the 2xx: the new Contact only");

	/* A retry without a handover: same Contact, so C sends only its own,
	 * and B sends no un-REGISTER for it. */
	n = g_nreq;
	if (newreg(&c, sip))
		return 1;
	sipreg_takeover(c, b);
	b = sipreg_release(b);
	(void)sipreg_send(c);
	run(2000, 300);
	rq = only(n, 1);
	check(rq && ct_port(rq->ct[0]) == pb && ct_expires(rq->ct[0], 600),
	      "same Contact: one REGISTER, no expires=0 anywhere");

	/* A registrar that refuses the two-Contact REGISTER: the next try
	 * goes without the old Contact. */
	sip_transp_flush(sip);
	(void)transp(sip);
	n = g_nreq;
	g_scode = 403;
	if (newreg(&d, sip))
		return 1;
	sipreg_takeover(d, c);
	c = sipreg_release(c);
	(void)sipreg_send(d);
	run(2000, 300);
	rq = only(n, 2);
	check(rq && g_resp == 403, "refused: the REGISTER carried two Contacts");
	g_scode = 200;
	n = g_nreq;
	(void)sipreg_send(d);
	run(2000, 0);
	rq = only(n, 1);
	check(rq && g_resp == 200 && ct_expires(rq->ct[0], 600),
	      "  the retry: its own Contact only, 200");
	pd = rq ? ct_port(rq->ct[0]) : 0;

	/* A chain: E takes over D after a handover, and its REGISTER is never
	 * answered.  F takes over E after another handover and must remove
	 * both D's binding and the one E's REGISTER may have made. */
	sip_transp_flush(sip);
	(void)transp(sip);
	n = g_nreq;
	g_silent = true;
	if (newreg(&e, sip))
		return 1;
	sipreg_takeover(e, d);
	d = sipreg_release(d);
	(void)sipreg_send(e);
	run(300, 0);
	g_silent = false;
	rq = n < g_nreq ? &g_reqv[n] : NULL;
	check(rq && rq->nct == 2 && has_ct(rq, pd, 0),
	      "chain: E's REGISTER (unanswered) removes D's binding");
	pe = rq ? ct_port(rq->ct[0]) : 0;
	if (rq)
		str_ncpy(e_callid, rq->callid, sizeof(e_callid));

	sip_transp_flush(sip);
	(void)transp(sip);
	n = g_nreq;
	if (newreg(&f, sip))
		return 1;
	sipreg_takeover(f, e);
	e = sipreg_release(e);
	(void)sipreg_send(f);
	run(2000, 300);
	rq = only_except(n, 3, e_callid);
	pf = rq ? ct_port(rq->ct[0]) : 0;
	check(rq && g_resp == 200 && pf && pf != pe &&
	      ct_expires(rq->ct[0], 600) && has_ct(rq, pd, 0) &&
	      has_ct(rq, pe, 0),
	      "  F: its own Contact, D's and E's with expires=0");

	/* A server error is not a refusal: G gets 503, and H, retrying from
	 * the same sockets, still removes F's binding. */
	sip_transp_flush(sip);
	(void)transp(sip);
	n = g_nreq;
	g_scode = 503;
	if (newreg(&g, sip))
		return 1;
	sipreg_takeover(g, f);
	f = sipreg_release(f);
	(void)sipreg_send(g);
	run(2000, 300);
	check(g_resp == 503, "503: G's REGISTER fails");
	g_scode = 200;
	n = g_nreq;
	if (newreg(&h, sip))
		return 1;
	sipreg_takeover(h, g);
	g = sipreg_release(g);
	(void)sipreg_send(h);
	run(2000, 300);
	rq = only_except(n, 2, e_callid);
	check(rq && g_resp == 200 && ct_expires(rq->ct[0], 600) &&
	      has_ct(rq, pf, 0),
	      "  H, the retry: its own Contact and F's with expires=0");

	/* An explicit unregister is unchanged: its own Contact, expires=0. */
	n = g_nreq;
	sipreg_unregister(h);
	run(2000, 0);
	rq = only_except(n, 1, e_callid);
	check(rq && ct_expires(rq->ct[0], 0),
	      "unregister: its own Contact, expires=0");

	mem_deref(h);
	sip_close(sip, true);
	mem_deref(sip);
	mem_deref(g_us);
	libre_close();

	printf("%d failed\n", g_fail);
	return g_fail ? 1 : 0;
}
