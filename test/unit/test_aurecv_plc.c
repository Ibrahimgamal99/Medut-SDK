/**
 * @file test_aurecv_plc.c  The playout-concealment patch on baresip's audio
 *                          receiver (cmake/patch-baresip-sources.cmake, job 6)
 *
 * Builds the patched third_party/baresip/src/aureceiver.c itself, against
 * libre, with a codec whose every frame says what it is: a real frame holds
 * its packet number, a frame rebuilt from the next packet's FEC the number of
 * the slot it rebuilds, a plain concealment frame a constant.  A device reads
 * the receiver every P ms the way an auplay thread does, and the test reads
 * the played stream back as 20 ms slots.  What it pins:
 *
 *   - every slot plays exactly once and in order: a lost packet is concealed,
 *     never skipped, and never concealed twice;
 *   - a packet whose slot concealment has already played is dropped, not
 *     spliced into the concealment;
 *   - the delay after either returns to what it was before;
 *   - playout concealment stops after 1 s at every ptime;
 *   - a codec change leaves nothing for the new decoder to conceal from;
 *   - an RTP timestamp wrap during concealment is not a late packet.
 *
 * The jitter buffer in front of the receiver is modelled, not linked: it
 * hands packets over in sequence order, reports the gap before each as lost,
 * and drops one older than a packet it has already handed over (baresip
 * jbuf.c does all three).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "core.h"

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                       \
	if (cond) { g_pass++; }                                         \
	else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__);  \
	       printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

/* ── Stubs: logging, the player registry, the codec printer ─────────────── */

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

int aucodec_print(struct re_printf *pf, const struct aucodec *ac)
{
	(void)pf; (void)ac;
	return 0;
}

static struct auplay g_auplay = { .name = "test" };
static auplay_write_h *g_wh;
static void *g_wh_arg;

const struct auplay *auplay_find(const struct list *auplayl, const char *name)
{
	(void)auplayl; (void)name;
	return &g_auplay;
}

int auplay_alloc(struct auplay_st **stp, struct list *auplayl,
		 const char *name, struct auplay_prm *prm, const char *device,
		 auplay_write_h *wh, void *arg)
{
	(void)auplayl; (void)name; (void)prm; (void)device;
	g_wh     = wh;
	g_wh_arg = arg;
	*stp = mem_zalloc(1, NULL);
	return *stp ? 0 : ENOMEM;
}

/* ── A codec whose frames say what they are ─────────────────────────────── */

#define SRATE      48000u
#define VAL_REAL   1000      /* + packet number */
#define VAL_FEC    4000      /* + number of the slot it rebuilds */
#define VAL_PLC    9000

struct audec_state {
	size_t last_sampc;   /* the last decoded frame's size; 0 before any */
};

static unsigned g_plc_err;   /* concealment asked of a decoder with no frame */
static size_t   g_frame;     /* samples per packet */

static void dec_destructor(void *arg) { (void)arg; }

static int dec_update(struct audec_state **adsp, const struct aucodec *ac,
		      const char *fmtp)
{
	(void)ac; (void)fmtp;
	if (*adsp)
		return 0;
	*adsp = mem_zalloc(sizeof(**adsp), dec_destructor);
	return *adsp ? 0 : ENOMEM;
}

static void fill(int16_t *p, size_t n, int v)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (int16_t)v;
}

static int dec_frame(struct audec_state *ads, int fmt, void *sampv,
		     size_t *sampc, bool marker, const uint8_t *buf, size_t len)
{
	(void)fmt; (void)marker;
	if (len < 2 || *sampc < g_frame)
		return EINVAL;
	fill(sampv, g_frame, VAL_REAL + (buf[0] | buf[1] << 8));
	*sampc = g_frame;
	ads->last_sampc = g_frame;
	return 0;
}

/* Like opus_decode_pkloss(): the last packet's duration, an error before
 * there was one; rebuilt from the next packet when it is handed over. */
static int dec_plc(struct audec_state *ads, int fmt, void *sampv,
		   size_t *sampc, const uint8_t *buf, size_t len)
{
	(void)fmt;
	if (!ads->last_sampc) {
		++g_plc_err;
		return EPROTO;
	}
	if (buf && len >= 2)
		fill(sampv, ads->last_sampc,
		     VAL_FEC + (buf[0] | buf[1] << 8) - 1);
	else
		fill(sampv, ads->last_sampc, VAL_PLC);
	*sampc = ads->last_sampc;
	return 0;
}

static struct aucodec g_codec_a = {
	.name = "test-a", .srate = SRATE, .crate = SRATE, .ch = 1, .pch = 1,
	.decupdh = dec_update, .dech = dec_frame, .plch = dec_plc,
};
static struct aucodec g_codec_b = {
	.name = "test-b", .srate = SRATE, .crate = SRATE, .ch = 1, .pch = 1,
	.decupdh = dec_update, .dech = dec_frame, .plch = dec_plc,
};

#define PT_A 96
#define PT_B 97

/* ── The receiver, its network and its device ───────────────────────────── */

#define MAXPKT  400
#define MAXOUT  (SRATE * 12)

static int16_t  g_out[MAXOUT];
static size_t   g_outn;
static int64_t  g_at[MAXPKT];      /* delivery time in µs; -1: lost */
static unsigned g_npkt;
static uint32_t g_ts0;             /* RTP timestamp of packet 0 */

static struct config_audio g_cfg;
static struct audio_recv  *g_ar;

static void recv_open(const struct aucodec *ac, int pt, unsigned ptime)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.dec_fmt    = AUFMT_S16LE;
	g_cfg.play_fmt   = AUFMT_S16LE;
	g_cfg.buffer.min = 20;      /* baresip's default audio_buffer 20-160 */
	g_cfg.buffer.max = 160;

	g_outn    = 0;
	g_plc_err = 0;
	g_frame   = SRATE / 1000 * ptime;

	int err = aurecv_alloc(&g_ar, &g_cfg, SRATE / 1000 * 120, ptime);
	err |= aurecv_decoder_set(g_ar, ac, pt, NULL);
	err |= aurecv_start_player(g_ar, NULL);
	if (err || !g_wh) {
		printf("receiver would not start (%d)\n", err);
		exit(2);
	}
}

static void recv_close(void)
{
	aurecv_stop(g_ar);
	g_ar = mem_deref(g_ar);
	g_wh = NULL;
}

static void deliver(unsigned k, unsigned lostc, int pt)
{
	struct rtp_header hdr;
	struct mbuf *mb = mbuf_alloc(2);
	uint8_t b[2] = { (uint8_t)k, (uint8_t)(k >> 8) };

	memset(&hdr, 0, sizeof(hdr));
	hdr.pt  = (uint8_t)pt;
	hdr.seq = (uint16_t)k;
	hdr.ts  = g_ts0 + (uint32_t)(k * g_frame);
	mbuf_write_mem(mb, b, 2);
	mb->pos = 0;
	aurecv_receive(g_ar, &hdr, NULL, 0, mb, lostc);
	mem_deref(mb);
}

static void device_read(unsigned period_ms)
{
	struct auframe af;
	size_t n = SRATE / 1000 * period_ms;

	if (g_outn + n > MAXOUT)
		return;
	auframe_init(&af, AUFMT_S16LE, g_out + g_outn, n, SRATE, 1);
	g_wh(&af, g_wh_arg);
	g_outn += n;
}

/* Runs the packets in g_at[] against a device that reads every period_ms,
 * first at phase_ms (+0.5 ms, so a read never ties with an arrival). */
static void run(unsigned period_ms, unsigned phase_ms, unsigned dur_ms, int pt)
{
	unsigned order[MAXPKT], n = 0;
	int last = -1;

	for (unsigned k = 0; k < g_npkt; k++)
		if (g_at[k] >= 0)
			order[n++] = k;
	for (unsigned i = 1; i < n; i++)          /* stable, by delivery time */
		for (unsigned j = i; j > 0 &&
		     g_at[order[j - 1]] > g_at[order[j]]; j--) {
			unsigned t = order[j];
			order[j] = order[j - 1];
			order[j - 1] = t;
		}

	int64_t t_dev = (int64_t)phase_ms * 1000 + 500;
	unsigned i = 0;
	while (t_dev <= (int64_t)dur_ms * 1000) {
		if (i < n && g_at[order[i]] <= t_dev) {
			unsigned k = order[i++];
			if ((int)k <= last)
				continue;         /* jbuf: older than one handed over */
			deliver(k, (unsigned)((int)k - last - 1), pt);
			last = (int)k;
		}
		else {
			device_read(period_ms);
			t_dev += (int64_t)period_ms * 1000;
		}
	}
}

/* Every packet on time, every ptime_ms from 0. */
static void sched_nominal(unsigned npkt, unsigned ptime_ms)
{
	g_npkt = npkt;
	for (unsigned k = 0; k < npkt; k++)
		g_at[k] = (int64_t)k * ptime_ms * 1000;
}

/* ── Reading the played stream back ─────────────────────────────────────── */

struct played {
	unsigned skips;      /* slots that never played */
	unsigned repeats;    /* a slot played again, or out of order */
	unsigned ragged;     /* a run that is not whole frames */
	unsigned gaps;       /* silence after playout started */
	unsigned plc_after;  /* samples of concealment after the last real one */
	int      slot_of[MAXPKT];   /* how slot k played: VAL_REAL/FEC/PLC, 0 */
	double   delay_ms[MAXPKT];  /* real frame k: play position − send time */
};

/* Start-up is the upstream buffer's own business (its first reads fill and
 * drop), so reading starts at the first real frame from this slot on. */
#define STEADY 10

static void analyse(struct played *p, unsigned ptime_ms)
{
	int prev = -1;
	size_t i = 0;
	size_t last_real_end = 0;

	memset(p, 0, sizeof(*p));
	for (unsigned k = 0; k < MAXPKT; k++)
		p->delay_ms[k] = -1;

	while (i < g_outn && !(g_out[i] >= VAL_REAL + STEADY &&
	                       g_out[i] < VAL_FEC))
		i++;

	while (i < g_outn) {
		int v = g_out[i];
		size_t j = i;
		while (j < g_outn && g_out[j] == v)
			j++;
		size_t len = j - i;

		if (v == 0) {
			if (prev >= 0)
				p->gaps++;
		}
		else {
			int first, nslots, kind;

			if (len % g_frame && j < g_outn)
				p->ragged++;
			nslots = (int)((len + g_frame - 1) / g_frame);

			if (v >= VAL_PLC) {
				kind  = VAL_PLC;
				first = prev + 1;
			}
			else if (v >= VAL_FEC) {
				kind  = VAL_FEC;
				first = v - VAL_FEC;
			}
			else {
				kind  = VAL_REAL;
				first = v - VAL_REAL;
				p->delay_ms[first] = (double)i * 1000 / SRATE -
				                     (double)first * ptime_ms;
				last_real_end = j;
			}

			if (prev >= 0 && first > prev + 1)
				p->skips += (unsigned)(first - prev - 1);
			if (prev >= 0 && first <= prev) {
				p->repeats++;
				if (getenv("VERBOSE"))
					printf("    repeat at %.1f ms: value %d after slot %d\n",
					       (double)i * 1000 / SRATE, v, prev);
			}
			for (int s = 0; s < nslots; s++)
				if (first + s >= 0 && first + s < MAXPKT)
					p->slot_of[first + s] = kind;
			if (first + nslots - 1 > prev)
				prev = first + nslots - 1;
		}
		i = j;
	}

	for (size_t s = last_real_end; s < g_outn; s++)
		if (g_out[s] == VAL_PLC)
			p->plc_after++;
}

/* Largest |delay − delay of `base`| over real frames [from, to). */
static double delay_drift(const struct played *p, unsigned base,
			  unsigned from, unsigned to)
{
	double worst = 0;
	for (unsigned k = from; k < to; k++) {
		if (p->delay_ms[k] < 0)
			continue;
		double d = p->delay_ms[k] - p->delay_ms[base];
		if (d < 0) d = -d;
		if (d > worst) worst = d;
	}
	return worst;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

#define LOST 50

/* One packet lost; the next one 0-10 ms early, at device periods of 10, 20
 * and 40 ms and every phase.  Its FEC rebuilds the lost one.  Rounding the
 * buffer's shortfall down to whole frames used to drop that frame instead,
 * whenever the shortfall came out a fraction short of two. */
static void test_isolated_loss(void)
{
	static const unsigned periods[] = { 10, 20, 40 };
	unsigned runs = 0, concealed = 0, fec = 0;

	for (size_t pi = 0; pi < RE_ARRAY_SIZE(periods); pi++)
	for (unsigned phase = 0; phase < periods[pi]; phase++)
	for (unsigned early = 0; early <= 10; early++) {
		unsigned P = periods[pi];
		struct played p;

		g_ts0 = 123456;
		sched_nominal(120, 20);
		g_at[LOST] = -1;
		g_at[LOST + 1] -= (int64_t)early * 1000;

		recv_open(&g_codec_a, PT_A, 20);
		run(P, phase, 120 * 20 + 200, PT_A);
		recv_close();
		analyse(&p, 20);

		runs++;
		if (p.slot_of[LOST] == VAL_FEC || p.slot_of[LOST] == VAL_PLC)
			concealed++;
		if (p.slot_of[LOST] == VAL_FEC)
			fec++;

		double drift = delay_drift(&p, LOST - 10, LOST + 1, 115);
		if (p.skips || p.repeats || p.ragged || p.gaps || drift > 20.0 ||
		    !(p.slot_of[LOST] == VAL_FEC || p.slot_of[LOST] == VAL_PLC))
			CHECK(false, "P=%u phase=%u early=%u: slot %d played as %d, "
			      "skips=%u repeats=%u ragged=%u gaps=%u drift=%.1f ms",
			      P, phase, early, LOST, p.slot_of[LOST], p.skips,
			      p.repeats, p.ragged, p.gaps, drift);
		else
			g_pass++;
	}
	printf("  isolated loss: %u/%u concealed, %u from FEC\n",
	       concealed, runs, fec);
}

/* One packet 30 ms late but not lost, at device periods of 5-10 ms: playout
 * concealment plays its slot, then the packet arrives.  It used to go into
 * the buffer in timestamp order — spliced into the concealment, the next
 * frames played late by its length for good. */
static void test_late_packet(void)
{
	static const unsigned periods[] = { 5, 8, 10 };

	for (size_t pi = 0; pi < RE_ARRAY_SIZE(periods); pi++)
	for (unsigned phase = 0; phase < periods[pi]; phase++) {
		unsigned P = periods[pi];
		struct played p;

		g_ts0 = 123456;
		sched_nominal(120, 20);
		g_at[LOST] += 30000;
		/* The jitter buffer hands over in order: whatever arrived
		 * behind the late one waits for it. */
		for (unsigned k = LOST + 1; g_at[k] < g_at[LOST]; k++)
			g_at[k] = g_at[LOST];

		recv_open(&g_codec_a, PT_A, 20);
		run(P, phase, 120 * 20 + 200, PT_A);
		recv_close();
		analyse(&p, 20);

		double drift = delay_drift(&p, LOST - 10, LOST + 3, 115);
		if (p.skips || p.repeats || p.ragged || p.gaps || drift > 1.0)
			CHECK(false, "P=%u phase=%u: skips=%u repeats=%u "
			      "ragged=%u gaps=%u, delay %.1f ms off baseline",
			      P, phase, p.skips, p.repeats, p.ragged, p.gaps,
			      drift);
		else
			g_pass++;
	}
}

/* The stream stops.  Playout concealment fills in for 1 s and then lets it
 * go silent — 1 s at 60 ms ptime too, where a count of 50 frames was 3 s. */
static void test_plc_cap(void)
{
	static const unsigned ptimes[] = { 20, 60 };

	for (size_t i = 0; i < RE_ARRAY_SIZE(ptimes); i++) {
		unsigned pt_ms = ptimes[i];
		struct played p;

		g_ts0 = 123456;
		sched_nominal(40, pt_ms);
		recv_open(&g_codec_a, PT_A, pt_ms);
		run(10, 3, 40 * pt_ms + 5000, PT_A);
		recv_close();
		analyse(&p, pt_ms);

		double ms = (double)p.plc_after * 1000 / SRATE;
		CHECK(ms >= 1000.0 - 1 && ms <= 1000.0 + pt_ms,
		      "ptime %u: %.0f ms of concealment after the stream stopped",
		      pt_ms, ms);
	}
}

/* A mid-call codec change, then the device reads before the new codec's
 * first packet.  Concealment carried over from the old codec ran the new
 * decoder on no packet at all, once per device callback. */
static void test_codec_change(void)
{
	g_ts0 = 123456;
	sched_nominal(50, 20);
	recv_open(&g_codec_a, PT_A, 20);
	run(10, 3, 50 * 20, PT_A);

	g_plc_err = 0;
	aurecv_decoder_set(g_ar, &g_codec_b, PT_B, NULL);
	for (int i = 0; i < 30; i++)
		device_read(10);
	CHECK(g_plc_err == 0,
	      "%u concealment calls on the new decoder before its first packet",
	      g_plc_err);

	/* ...and the new codec's packets play. */
	size_t from = g_outn;
	g_ts0 = 900000;
	for (unsigned k = 0; k < 10; k++) {
		deliver(k, 0, PT_B);
		device_read(10);
		device_read(10);
	}
	bool played = false;
	for (size_t s = from; s < g_outn; s++)
		if (g_out[s] >= VAL_REAL && g_out[s] < VAL_FEC)
			played = true;
	CHECK(played, "new codec's packets never played");
	recv_close();
}

/* The RTP timestamp wraps while playout concealment is running.  The packet
 * after the wrap reads as far older than the concealment; it is not late. */
static void test_ts_wrap(void)
{
	struct played p;

	/* Packet 60 is the first past 2^32. */
	g_ts0 = (uint32_t)(0x100000000ull - 60ull * 960);
	sched_nominal(120, 20);
	g_at[58] = -1;
	g_at[59] = -1;
	for (unsigned k = 60; k < 120; k++)
		g_at[k] += 40000;              /* a 40 ms hole: concealed at playout */

	recv_open(&g_codec_a, PT_A, 20);
	run(10, 3, 120 * 20 + 300, PT_A);
	recv_close();
	analyse(&p, 20);

	unsigned after = 0;
	for (unsigned k = 60; k < 120; k++)
		if (p.slot_of[k] == VAL_REAL)
			after++;
	CHECK(after >= 55, "only %u of 60 packets past the wrap played", after);
}

int main(void)
{
	libre_init();

	test_isolated_loss();
	test_late_packet();
	test_plc_cap();
	test_codec_change();
	test_ts_wrap();

	printf("%d passed, %d failed\n", g_pass, g_fail);
	libre_close();
	return g_fail ? 1 : 0;
}
