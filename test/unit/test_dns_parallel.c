/**
 * @file test_dns_parallel.c  libre's DNS client with patch W
 *                            (cmake/patch-re-sources.cmake)
 *
 * A query goes to every nameserver at once.  The first positive answer
 * wins; a negative one counts in the first round only from the first
 * server, so a name only the local resolver knows (split-horizon DNS) is
 * not lost to a public resolver's quicker "no such name".  From the second
 * round any answer counts.
 *
 * Two fake nameservers on 127.0.0.1, each answering after a set delay with
 * an address, NXDOMAIN or SERVFAIL — or not at all.  libre only.
 */

#include <string.h>
#include <stdio.h>
#include <re.h>

enum mode { POS, NX, SERVFAIL, SILENT };

struct fake {
	struct udp_sock *us;
	struct sa addr;
	enum mode mode;
	uint32_t delay_ms;
	uint8_t ip;           /* answers 192.0.2.<ip> */
};

struct pending {
	struct le le;
	struct tmr tmr;
	struct fake *f;
	struct sa src;
	struct mbuf *mb;
};

static struct list g_pending;

static void pending_destructor(void *arg)
{
	struct pending *p = arg;

	tmr_cancel(&p->tmr);
	list_unlink(&p->le);
	mem_deref(p->mb);
}

/* The query echoed back up to its question, with the answer's header
 * fields and, for POS, one A record. */
static void reply_h(void *arg)
{
	struct pending *p = arg;
	struct mbuf *q = p->mb, *r = mbuf_alloc(512);
	size_t n = 12;
	uint16_t flags;

	if (!r)
		goto out;

	while (n < q->end && q->buf[n])          /* the question's name */
		n += 1 + q->buf[n];
	n += 1 + 4;                               /* root label, type, class */
	if (n > q->end)
		goto out;

	flags = 0x8180 | (p->f->mode == NX ? 3 : p->f->mode == SERVFAIL ? 2 : 0);
	(void)mbuf_write_mem(r, q->buf, 2);                    /* id      */
	(void)mbuf_write_u16(r, htons(flags));
	(void)mbuf_write_u16(r, htons(1));                     /* qdcount */
	(void)mbuf_write_u16(r, htons(p->f->mode == POS));     /* ancount */
	(void)mbuf_write_u16(r, 0);
	(void)mbuf_write_u16(r, 0);
	(void)mbuf_write_mem(r, q->buf + 12, n - 12);          /* question */
	if (p->f->mode == POS) {
		static const uint8_t rr[] = {0xc0, 0x0c, 0, 1, 0, 1,
					     0, 0, 0, 60, 0, 4, 192, 0, 2};
		(void)mbuf_write_mem(r, rr, sizeof(rr));
		(void)mbuf_write_u8(r, p->f->ip);
	}
	r->pos = 0;
	(void)udp_send(p->f->us, &p->src, r);

 out:
	mem_deref(r);
	mem_deref(p);
}

static void fake_recv(const struct sa *src, struct mbuf *mb, void *arg)
{
	struct fake *f = arg;
	struct pending *p;

	if (f->mode == SILENT)
		return;

	p = mem_zalloc(sizeof(*p), pending_destructor);
	if (!p)
		return;
	p->f   = f;
	p->src = *src;
	p->mb  = mbuf_alloc(mbuf_get_left(mb));
	(void)mbuf_write_mem(p->mb, mbuf_buf(mb), mbuf_get_left(mb));
	list_append(&g_pending, &p->le, p);
	tmr_start(&p->tmr, f->delay_ms, reply_h, p);
}

static int fake_start(struct fake *f)
{
	struct sa local;

	sa_set_str(&local, "127.0.0.1", 0);
	if (udp_listen(&f->us, &local, fake_recv, f))
		return 1;
	return udp_local_get(f->us, &f->addr);
}

/* ── one query ──────────────────────────────────────────────────────────── */

struct result {
	int err;
	int rcode;
	int ip;               /* last octet of the A answer, -1 none */
	uint64_t ms;
	bool done;
};

static uint64_t g_t0;

static void query_h(int err, const struct dnshdr *hdr, struct list *ansl,
		    struct list *authl, struct list *addl, void *arg)
{
	struct result *res = arg;
	struct dnsrr *rr = list_ledata(list_head(ansl));
	(void)authl; (void)addl;

	res->err   = err;
	res->rcode = hdr ? hdr->rcode : -1;
	res->ip    = (rr && rr->type == DNS_TYPE_A)
		   ? (int)(rr->rdata.a.addr & 0xff) : -1;
	res->ms    = tmr_jiffies() - g_t0;
	res->done  = true;
	re_cancel();
}

static void guard_h(void *arg)
{
	(void)arg;
	re_cancel();
}

static struct result run(struct fake *a, struct fake *b)
{
	struct dnsc_conf conf = {16, 2, 10000, 30000, 0, false}; /* no cache */
	struct result res = {0, -1, -1, 0, false};
	struct dnsc *dnsc = NULL;
	struct dns_query *q = NULL;
	struct sa srvv[2];
	struct tmr guard;
	uint32_t n = 0;

	srvv[n++] = a->addr;
	if (b)
		srvv[n++] = b->addr;

	if (dnsc_alloc(&dnsc, &conf, srvv, n))
		return res;

	tmr_init(&guard);
	tmr_start(&guard, 15000, guard_h, NULL);
	g_t0 = tmr_jiffies();
	if (0 == dnsc_query(&q, dnsc, "pbx.example.test", DNS_TYPE_A,
			    DNS_CLASS_IN, true, query_h, &res))
		(void)re_main(NULL);

	tmr_cancel(&guard);
	mem_deref(q);
	mem_deref(dnsc);
	list_flush(&g_pending);
	return res;
}

static int g_fail;

static void expect(const char *what, struct result r, int err, int rcode,
		   int ip, uint64_t max_ms)
{
	bool ok = r.done && r.err == err && (err || (r.rcode == rcode &&
					     r.ip == ip)) && r.ms <= max_ms;

	printf("  %-48s err=%d rcode=%d ip=%d %llu ms  %s\n", what, r.err,
	       r.rcode, r.ip, (unsigned long long)r.ms, ok ? "ok" : "FAIL");
	if (!ok)
		g_fail++;
}

int main(void)
{
	struct fake a = {0}, b = {0};

	if (libre_init() || fake_start(&a) || fake_start(&b)) {
		printf("setup failed\n");
		return 1;
	}

	/* The first server slow, the second quick: the quick answer is used,
	 * at once — not after the first server's 500 ms timeout. */
	a = (struct fake){a.us, a.addr, POS, 1500, 1};
	b = (struct fake){b.us, b.addr, POS, 20, 2};
	expect("slow first server, quick second", run(&a, &b), 0, 0, 2, 300);

	/* Split horizon: only the first server knows the name, the second
	 * says NXDOMAIN first.  The name still resolves. */
	a = (struct fake){a.us, a.addr, POS, 200, 1};
	b = (struct fake){b.us, b.addr, NX, 10, 2};
	expect("split horizon: second says no such name", run(&a, &b),
	       0, 0, 1, 450);

	/* The first server says NXDOMAIN: that settles it (it is the one the
	 * system listed first). */
	a = (struct fake){a.us, a.addr, NX, 10, 1};
	b = (struct fake){b.us, b.addr, POS, 150, 2};
	expect("first server says no such name", run(&a, &b), 0, 3, -1, 100);

	/* The first server fails (SERVFAIL): the second one's answer. */
	a = (struct fake){a.us, a.addr, SERVFAIL, 5, 1};
	b = (struct fake){b.us, b.addr, POS, 50, 2};
	expect("first server fails, second answers", run(&a, &b),
	       0, 0, 2, 300);

	/* The first server is dead and the name does not exist: the second
	 * one's NXDOMAIN counts from the second round (~500 ms). */
	a = (struct fake){a.us, a.addr, SILENT, 0, 1};
	b = (struct fake){b.us, b.addr, NX, 10, 2};
	expect("first server dead, no such name", run(&a, &b),
	       0, 3, -1, 900);

	/* One server: as before. */
	a = (struct fake){a.us, a.addr, POS, 20, 1};
	expect("one server", run(&a, NULL), 0, 0, 1, 300);

	/* Both dead: the query still ends, with a timeout. */
	a = (struct fake){a.us, a.addr, SILENT, 0, 1};
	b = (struct fake){b.us, b.addr, SILENT, 0, 2};
	expect("both servers dead", run(&a, &b), ETIMEDOUT, -1, -1, 12000);

	mem_deref(a.us);
	mem_deref(b.us);
	libre_close();

	printf("%d passed, %d failed\n", 7 - g_fail, g_fail);
	return g_fail ? 1 : 0;
}
