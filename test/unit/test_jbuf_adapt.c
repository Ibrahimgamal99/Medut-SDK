/**
 * @file test_jbuf_adapt.c  Adaptive sizing patch on baresip's jitter buffer
 *                          (cmake/patch-baresip-sources.cmake, job 8)
 *
 * Builds the patched third_party/baresip/src/jbuf.c itself, against libre,
 * and drives it the way rtprecv.c does: jbuf_put() as each packet arrives,
 * and a decode timer that drains with jbuf_get() and re-arms from
 * jbuf_next_play() (10 ms when that is negative).  The clock is simulated
 * (tmr_jiffies is wrapped), the streams are generated, so every run is the
 * same.  What it pins:
 *
 *   - a clean link stays at the minimum delay and loses nothing;
 *   - a link whose delay comes in spikes, with reordering — the shape
 *     measured on office Wi-Fi through an Asterisk echo — loses under 2% to
 *     lateness (upstream's 2x-mean-jitter sizing: see the job's header);
 *   - after the spikes stop the buffer comes back down, in steps of at most
 *     10 ms;
 *   - a timestamp jump with the sequence running on (a PBX prompt handing
 *     over to its echo) is not jitter: the buffer stays where it was;
 *   - packets that arrived too late to use still grow the buffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <re.h>
#include <baresip.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                       \
	if (cond) { g_pass++; }                                         \
	else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__);  \
	       printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

/* ── Stubs and the simulated clock ───────────────────────────────────────── */

static void log_va(const char *fmt, va_list ap)
{
	if (getenv("VERBOSE"))
		re_vfprintf(stderr, fmt, ap);
}

void _info(bool safe, const char *fmt, ...)
{
	va_list ap; (void)safe; va_start(ap, fmt); log_va(fmt, ap); va_end(ap);
}

void _warning(bool safe, const char *fmt, ...)
{
	va_list ap; (void)safe; va_start(ap, fmt); log_va(fmt, ap); va_end(ap);
}

void _debug(bool safe, const char *fmt, ...)
{
	va_list ap; (void)safe; va_start(ap, fmt); log_va(fmt, ap); va_end(ap);
}

static uint64_t g_now;   /* ms */

uint64_t __wrap_tmr_jiffies(void);
uint64_t __wrap_tmr_jiffies(void)
{
	return g_now;
}

/* ── Streams ─────────────────────────────────────────────────────────────── */

enum { SRATE = 48000, PTIME = 20, FRAME = SRATE / 1000 * PTIME,
       MIND = 40, MAXD = 400, MAXPKT = 8000 };

struct pkt {
	uint64_t arrive;   /* ms */
	uint16_t seq;
	uint32_t ts;
};

static struct pkt g_pkt[MAXPKT];
static size_t     g_n;

static uint32_t g_rng;

static uint32_t rnd(uint32_t lo, uint32_t hi)   /* [lo, hi] */
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return lo + (g_rng >> 8) % (hi - lo + 1);
}

static int by_arrival(const void *a, const void *b)
{
	const struct pkt *x = a, *y = b;
	if (x->arrive != y->arrive)
		return x->arrive < y->arrive ? -1 : 1;
	return (int)x->seq - (int)y->seq;
}

/* Start a stream.  The clock starts where transit (arrival in timestamp
 * units, less the timestamp) is half the 32-bit range from 0: jbuf reads 0
 * as "not set yet", which a real call (ms since boot) all but never hits. */
static uint64_t g_t0;
static uint32_t g_ts0;
static uint16_t g_seq0;

static void stream_start(uint32_t seed)
{
	g_rng  = seed;
	g_n    = 0;
	g_ts0  = 0x10000000u;
	g_seq0 = 1000;
	g_t0   = ((uint64_t)g_ts0 + 0x80000000ull) / (SRATE / 1000);
}

/* Packet i of the stream, sent at g_t0 + i * PTIME. */
static void add(uint32_t i, uint64_t delay_ms, int32_t ts_jump)
{
	if (g_n >= MAXPKT)
		return;
	g_pkt[g_n].arrive = g_t0 + (uint64_t)i * PTIME + delay_ms;
	g_pkt[g_n].seq    = (uint16_t)(g_seq0 + i);
	g_pkt[g_n].ts     = g_ts0 + i * FRAME + (uint32_t)ts_jump;
	g_n++;
}

/* Packets [from, to): base delay plus up to `noise` ms. */
static void gen_clean(uint32_t from, uint32_t to, uint32_t noise,
		      int32_t ts_jump)
{
	for (uint32_t i = from; i < to; i++)
		add(i, 30 + rnd(0, noise), ts_jump);
}

/* Packets [from, to) over the link measured on 2026-10-07 (office Wi-Fi,
 * Asterisk echo).  Its delay drifts: a queue builds and drains towards a
 * level re-drawn every 0.4-1.6 s from 0-160 ms, so consecutive packets
 * differ little while the spread is wide — which is what a mean of
 * inter-arrival steps underestimates.  On top, a little noise, a fifth of
 * the packets 10-60 ms behind their neighbours (reordered), and 1% lost. */
static void gen_spiky(uint32_t from, uint32_t to)
{
	int32_t level = 0, target = (int32_t)rnd(0, 160);
	uint32_t next = from + rnd(20, 80);

	for (uint32_t i = from; i < to; i++) {
		if (i >= next) {
			target = (int32_t)rnd(0, 160);
			next = i + rnd(20, 80);
		}
		level += level < target ? 4 : level > target ? -8 : 0;
		if (level < 0)
			level = 0;

		uint64_t delay = 30 + (uint64_t)level + rnd(0, 15);
		if (rnd(0, 4) == 0)
			delay += rnd(10, 60);
		if (rnd(0, 99) == 0)
			continue;

		add(i, delay, 0);
	}
}

/* ── Driver ──────────────────────────────────────────────────────────────── */

struct result {
	uint32_t expected, played, late_lost;
	uint32_t depth_max, depth_last;
	uint32_t moves, shrink_step_max;
	double   depth_avg;
};

/* Feed g_pkt[] (in arrival order) through a fresh jbuf.  Depth samples after
 * `measure_from` ms of stream count towards depth_avg. */
static struct result run(uint64_t measure_from)
{
	struct result r;
	struct jbuf *jb;
	uint64_t next_dec = 0;
	int64_t prev = -1;
	double sum = 0;
	uint32_t nsum = 0;
	size_t i = 0;
	uint16_t seq_hi = g_pkt[0].seq;

	memset(&r, 0, sizeof(r));
	qsort(g_pkt, g_n, sizeof(g_pkt[0]), by_arrival);

	if (jbuf_alloc(&jb, MIND, MAXD, 50))
		return r;
	jbuf_set_type(jb, JBUF_ADAPTIVE);
	jbuf_set_srate(jb, SRATE);

	while (i < g_n || next_dec) {
		if (i < g_n && (!next_dec || g_pkt[i].arrive < next_dec)) {
			struct rtp_header h;
			void *mb = mem_zalloc(4, NULL);

			g_now = g_pkt[i].arrive;
			memset(&h, 0, sizeof(h));
			h.seq = g_pkt[i].seq;
			h.ts = g_pkt[i].ts;
			h.ssrc = 0x1234;
			h.ts_arrive = g_now * (SRATE / 1000);
			(void)jbuf_put(jb, &h, mb);
			mem_deref(mb);

			if ((int16_t)(h.seq - seq_hi) > 0)
				seq_hi = h.seq;
			if (!next_dec)
				next_dec = g_now;
			i++;
			continue;
		}

		g_now = next_dec;
		uint32_t k = 1;
		do {
			struct rtp_header h;
			void *mb = NULL;
			int err = jbuf_get(jb, &h, &mb);
			if (err == EAGAIN)
				++k;
			else if (err)
				break;
			r.played++;
			mem_deref(mb);
		} while (--k);

		struct jbuf_stat st;
		jbuf_stats(jb, &st);
		if (prev >= 0 && st.c_delay != (uint32_t)prev) {
			r.moves++;
			if (st.c_delay < (uint32_t)prev &&
			    (uint32_t)prev - st.c_delay > r.shrink_step_max)
				r.shrink_step_max = (uint32_t)prev - st.c_delay;
		}
		prev = st.c_delay;
		if (st.c_delay > r.depth_max)
			r.depth_max = st.c_delay;
		if (g_now - g_t0 >= measure_from) {
			sum += st.c_delay;
			nsum++;
		}

		int32_t d = jbuf_next_play(jb);
		if (d < 0)
			d = 10;
		next_dec = (i < g_n || jbuf_packets(jb)) ? g_now + (uint64_t)d
							 : 0;
	}

	struct jbuf_stat st;
	jbuf_stats(jb, &st);
	r.late_lost  = st.n_late_lost;
	r.depth_last = (uint32_t)prev;
	r.depth_avg  = nsum ? sum / nsum : 0;
	r.expected   = (uint16_t)(seq_hi - g_seq0) + 1;
	mem_deref(jb);

	if (getenv("VERBOSE"))
		printf("  expected=%u played=%u late_lost=%u depth max=%u "
		       "avg=%.0f last=%u moves=%u shrink_step_max=%u\n",
		       r.expected, r.played, r.late_lost, r.depth_max,
		       r.depth_avg, r.depth_last, r.moves, r.shrink_step_max);
	return r;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_clean_link(void)
{
	stream_start(1);
	gen_clean(0, 1500, 4, 0);                     /* 30 s */
	struct result r = run(0);

	CHECK(r.late_lost == 0, "clean link lost %u packets late", r.late_lost);
	CHECK(r.depth_max == MIND, "clean link grew to %u ms", r.depth_max);
}

static void test_spiky_link(void)
{
	for (uint32_t seed = 1; seed <= 4; seed++) {
		stream_start(seed);
		gen_spiky(0, 3000);                   /* 60 s */
		struct result r = run(10000);
		double pct = 100.0 * r.late_lost / r.expected;

		CHECK(pct < 2.0, "seed %u: %.1f%% too late (%u)", seed, pct,
		      r.late_lost);
		CHECK(r.depth_avg < 260, "seed %u: average depth %.0f ms",
		      seed, r.depth_avg);
		CHECK(r.moves < 100, "seed %u: depth moved %u times", seed,
		      r.moves);
	}
}

static void test_shrinks_back(void)
{
	stream_start(7);
	gen_spiky(0, 1500);                           /* 30 s of spikes */
	gen_clean(1500, 4500, 4, 0);                  /* then 60 s clean */
	struct result r = run(0);

	CHECK(r.depth_max >= 120, "spikes only reached %u ms", r.depth_max);
	CHECK(r.depth_last == MIND, "still %u ms after 60 s clean",
	      r.depth_last);
	CHECK(r.shrink_step_max <= 10, "shrank %u ms in one step",
	      r.shrink_step_max);
}

static void test_ts_jump(void)
{
	static const int32_t jumps[] = { 1059 * (SRATE / 1000),
					 -1059 * (SRATE / 1000) };

	for (size_t j = 0; j < RE_ARRAY_SIZE(jumps); j++) {
		stream_start(3);
		gen_clean(0, 500, 4, 0);
		gen_clean(500, 1500, 4, jumps[j]);
		struct result r = run(0);

		CHECK(r.depth_max == MIND, "jump %d: depth went to %u ms",
		      jumps[j] / (SRATE / 1000), r.depth_max);
		CHECK(r.late_lost <= 1, "jump %d: %u too late",
		      jumps[j] / (SRATE / 1000), r.late_lost);
	}
}

/* Every 20th packet arrives 100 ms late, behind its next five: only
 * reordering, which only late-by-sequence drops reveal at first. */
static void test_late_packets_grow(void)
{
	stream_start(5);
	for (uint32_t i = 0; i < 1500; i++)
		add(i, 30 + (i % 20 == 19 ? 100 : rnd(0, 4)), 0);
	struct result r = run(0);

	CHECK(r.late_lost <= 5, "%u of 75 delayed packets lost", r.late_lost);
	CHECK(r.depth_max >= 110, "grew only to %u ms", r.depth_max);
}

int main(void)
{
	libre_init();

	test_clean_link();
	test_spiky_link();
	test_shrinks_back();
	test_ts_jump();
	test_late_packets_grow();

	printf("%d passed, %d failed\n", g_pass, g_fail);
	libre_close();
	return g_fail ? 1 : 0;
}
