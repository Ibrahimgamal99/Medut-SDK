/**
 * @file test_fmtp_bitrate.c  Unit tests for the adaptive-bitrate fmtp rewriter
 *
 * bsdk_adapt_fmtp_set_bitrate() is what makes adaptive bitrate safe.  The
 * obvious alternative — baresip's audio_set_bitrate() — re-runs the encoder
 * update with a NULL fmtp, so Opus re-derives useinbandfec, usedtx, cbr and
 * stereo from defaults and quietly drops whatever was negotiated.  Dropping
 * FEC at the moment the link needs it is the exact opposite of the intent, so
 * instead the negotiated fmtp is reused with only maxaveragebitrate rewritten.
 *
 * That rewriting is string surgery on a value that goes straight into the
 * encoder, and it runs repeatedly over a call — every adaptation step feeds it
 * its own previous output.  A parameter dropped, duplicated or corrupted here
 * silently changes how audio is encoded, which is why it gets its own test.
 *
 * Links src/adapt.c against libre only.  The baresip and EchoSDK symbols the
 * rest of that file references are stubbed below: pulling in libbaresip would
 * drag every audio backend the sysroot happens to have been built with.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "../../src/echosdk_internal.h"

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                       \
	if (cond) { g_pass++; }                                         \
	else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__);  \
	       printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

/* ── Stubs ───────────────────────────────────────────────────────────────────
 * adapt.c's other functions reference these; none is reachable from the
 * rewriter under test. */

struct bsdk_ctx g_bsdk;
int32_t opus_packet_loss;   /* modules/opus/opus.c */

void bsdk_post_quality_alert(struct echosdk_call *lc,
                             echosdk_quality_issue_t issue,
                             float value, float threshold, bool recovering)
{
	(void)lc; (void)issue; (void)value; (void)threshold; (void)recovering;
}

int bsdk_dispatch_sync(void (*fn)(void *), void *arg)
{
	(void)fn; (void)arg;
	return 0;
}

struct audio *call_audio(const struct call *call) { (void)call; return NULL; }
struct stream *audio_strm(const struct audio *au) { (void)au; return NULL; }
bool call_is_onhold(const struct call *call) { (void)call; return false; }
void call_enable_rtp_timeout(struct call *call, uint32_t ms)
{
	(void)call; (void)ms;
}
void stream_enable_rtp_timeout(struct stream *strm, uint32_t ms)
{
	(void)strm; (void)ms;
}
struct sdp_media *stream_sdpmedia(const struct stream *strm)
{
	(void)strm; return NULL;
}
int audio_encoder_set(struct audio *a, const struct aucodec *ac, int pt,
                      const char *params)
{
	(void)a; (void)ac; (void)pt; (void)params;
	return 0;
}

/* The stall check reads whether media setup is still under way: no streams,
 * nothing encrypting, no ICE, and the netmon seam it would call. */
static struct list g_streaml = LIST_INIT;
struct list *call_streaml(const struct call *call)
{
	(void)call; return &g_streaml;
}
bool stream_is_secure(const struct stream *strm) { (void)strm; return true; }
bool bsdk_ice_setup_pending(void *call) { (void)call; return false; }
void bsdk_netmon_call_stalled(struct echosdk_call *lc) { (void)lc; }

/* baresip's info() macro expands to this; its implementation lives in
 * libbaresip's log.c, which the test deliberately does not link. */
void _info(bool safe, const char *fmt, ...)
{
	(void)safe; (void)fmt;
}

/* ── Helper ──────────────────────────────────────────────────────────────── */

static void expect(const char *label, const char *src, uint32_t br,
                    const char *want)
{
	char buf[512];
	int err = bsdk_adapt_fmtp_set_bitrate(buf, sizeof(buf), src, br);

	CHECK(err == 0, "%s: unexpected error %d", label, err);
	if (err)
		return;
	CHECK(!strcmp(buf, want), "%s: got \"%s\", want \"%s\"",
	      label, buf, want);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* No negotiated fmtp at all — the common case for a peer that offered plain
 * opus/48000/2 with no parameters. */
static void test_no_source(void)
{
	expect("null src", NULL, 16000, "maxaveragebitrate=16000");
	expect("empty src", "", 16000, "maxaveragebitrate=16000");
	expect("null src, no bitrate", NULL, 0, "");
}

/* The flags this whole approach exists to preserve must survive untouched. */
static void test_preserves_negotiated_flags(void)
{
	expect("flags preserved",
	       "useinbandfec=1;usedtx=1;stereo=0",
	       12000,
	       "useinbandfec=1;usedtx=1;stereo=0;maxaveragebitrate=12000");
}

/* Replacing an existing value, wherever it sits in the string. */
static void test_replaces_existing(void)
{
	expect("key alone", "maxaveragebitrate=24000", 8000,
	       "maxaveragebitrate=8000");
	expect("key first", "maxaveragebitrate=24000;stereo=0", 8000,
	       "stereo=0;maxaveragebitrate=8000");
	expect("key middle", "stereo=0;maxaveragebitrate=24000;usedtx=1", 8000,
	       "stereo=0;usedtx=1;maxaveragebitrate=8000");
	expect("key last", "stereo=0;maxaveragebitrate=24000", 8000,
	       "stereo=0;maxaveragebitrate=8000");
	/* SDP parameter names are case-insensitive; a peer spelling it in camel
	 * case must not leave two competing values behind. */
	expect("key mixed case", "MaxAverageBitrate=24000;stereo=0", 8000,
	       "stereo=0;maxaveragebitrate=8000");
	/* A bare key with no value is still the parameter we are replacing. */
	expect("key without value", "maxaveragebitrate;stereo=0", 8000,
	       "stereo=0;maxaveragebitrate=8000");
}

/* Feeding our own output back in is what happens on every adaptation step
 * after the first — the string must reach a fixed point, not grow. */
static void test_idempotent_under_repeat(void)
{
	char a[512], b[512], c[512];

	CHECK(bsdk_adapt_fmtp_set_bitrate(a, sizeof(a),
	          "useinbandfec=1;stereo=0", 24000) == 0, "step 1 failed");
	CHECK(bsdk_adapt_fmtp_set_bitrate(b, sizeof(b), a, 12000) == 0,
	      "step 2 failed");
	CHECK(bsdk_adapt_fmtp_set_bitrate(c, sizeof(c), b, 12000) == 0,
	      "step 3 failed");

	CHECK(!strcmp(b, "useinbandfec=1;stereo=0;maxaveragebitrate=12000"),
	      "repeat: got \"%s\"", b);
	CHECK(!strcmp(b, c), "repeat not stable: \"%s\" vs \"%s\"", b, c);
	CHECK(strlen(c) == strlen(b), "repeat grew the string");
}

/* A parameter that merely starts with the key is a different parameter. */
static void test_prefix_not_confused(void)
{
	expect("prefix kept", "maxaveragebitratex=1;stereo=0", 8000,
	       "maxaveragebitratex=1;stereo=0;maxaveragebitrate=8000");
	expect("maxplaybackrate kept", "maxplaybackrate=48000", 8000,
	       "maxplaybackrate=48000;maxaveragebitrate=8000");
}

/* libre hands out fmtp strings with leading and doubled separators; emitting
 * them back would make the string grow on every adaptation step. */
static void test_separators_normalised(void)
{
	expect("leading semicolon", ";stereo=0", 8000,
	       "stereo=0;maxaveragebitrate=8000");
	expect("doubled semicolon", "stereo=0;;usedtx=1", 8000,
	       "stereo=0;usedtx=1;maxaveragebitrate=8000");
	expect("trailing semicolon", "stereo=0;", 8000,
	       "stereo=0;maxaveragebitrate=8000");
	expect("only separators", ";;;", 8000, "maxaveragebitrate=8000");
}

/* bitrate 0 means "restore the negotiated rate": strip our override and
 * advertise nothing in its place. */
static void test_strip_only(void)
{
	expect("strip", "stereo=0;maxaveragebitrate=24000;usedtx=1", 0,
	       "stereo=0;usedtx=1");
	expect("strip when absent", "stereo=0", 0, "stereo=0");
}

/* A source too long to copy must fail rather than emit a truncated parameter
 * list, which the encoder would happily accept as authoritative. */
static void test_overflow_rejected(void)
{
	char  buf[32];
	char  src[128];
	int   err;

	memset(src, 'a', sizeof(src) - 1);
	src[sizeof(src) - 1] = '\0';

	err = bsdk_adapt_fmtp_set_bitrate(buf, sizeof(buf), src, 8000);
	CHECK(err == EINVAL, "overflow: expected EINVAL, got %d", err);

	/* Zero-size destination must be rejected, not written to. */
	err = bsdk_adapt_fmtp_set_bitrate(buf, 0, "stereo=0", 8000);
	CHECK(err == EINVAL, "zero size: expected EINVAL, got %d", err);

	err = bsdk_adapt_fmtp_set_bitrate(NULL, sizeof(buf), "stereo=0", 8000);
	CHECK(err == EINVAL, "null buf: expected EINVAL, got %d", err);
}

/* ── FEC level (bsdk_adapt_fec_level) ────────────────────────────────────── */

static void expect_level(float loss, uint32_t cur, uint32_t want)
{
	uint32_t got = bsdk_adapt_fec_level(loss, cur);
	CHECK(got == want, "fec level(loss %.2f, cur %u): got %u, want %u",
	      loss, cur, got, want);
}

/* The bands: under 4 → 5, 4–10 → 10, 10–15 → 15, 15–20 → 20, above → 25. */
static void test_fec_bands(void)
{
	expect_level(2.f,  0, 5);
	expect_level(3.9f, 0, 5);
	expect_level(4.f,  0, 10);
	expect_level(9.9f, 0, 10);
	expect_level(10.f, 0, 15);
	expect_level(15.f, 0, 20);
	expect_level(20.f, 0, 25);
	expect_level(60.f, 0, 25);
}

/* Up at once, down only a point below the current band. */
static void test_fec_band_hysteresis(void)
{
	expect_level(12.f, 10, 15);   /* worse: straight up */
	expect_level(3.5f, 10, 10);   /* within a point of 4: hold */
	expect_level(2.9f, 10, 5);
	expect_level(9.5f, 15, 15);
	expect_level(8.9f, 15, 10);
	expect_level(0.5f, 25, 5);    /* far below: down in one step */
}

/* ── FEC on/off (bsdk_adapt_fec_update) ──────────────────────────────────── */

/* One receiver report every 5 s, 250 packets sent in between, and
 * `lost_pct` of them reported lost — the cadence measured against Asterisk. */
struct feed {
	struct bsdk_fec f;
	uint32_t sent, lost, ehsn;
	uint64_t now;
	float    rtt;
};

static bool report(struct feed *fd, float lost_pct)
{
	fd->sent += 250;
	fd->lost += (uint32_t)(250.f * lost_pct / 100.f + 0.5f);
	fd->ehsn += 250;              /* the peer's highest seq keeps up */
	fd->now  += 5000;
	fd->rtt  += 0.25f;            /* every report carries a fresh RTT */
	return bsdk_adapt_fec_update(&fd->f, fd->sent, fd->lost, fd->ehsn,
	                             fd->rtt, fd->now);
}

/* A stats tick between two reports: time and packets move, the report does
 * not.  It must change nothing. */
static bool quiet_tick(struct feed *fd)
{
	fd->sent += 100;
	fd->now  += 2000;
	return bsdk_adapt_fec_update(&fd->f, fd->sent, fd->lost, fd->ehsn,
	                             fd->rtt, fd->now);
}

static void feed_init(struct feed *fd)
{
	memset(fd, 0, sizeof(*fd));
	fd->rtt = 6.f;
	/* The first report is the baseline. */
	(void)bsdk_adapt_fec_update(&fd->f, 0, 0, 0, fd->rtt, 0);
}

static void test_fec_off_on_clean_and_light_loss(void)
{
	struct feed fd;
	feed_init(&fd);

	CHECK(!report(&fd, 0.f) && !fd.f.active, "clean link: FEC stays off");
	CHECK(!report(&fd, 1.6f) && !fd.f.active, "1.6%%: below the on line");
	CHECK(!report(&fd, 0.f) && fd.f.level == 0, "level 0 while off");
}

/* 2–4% needs a second report to confirm it. */
static void test_fec_borderline_needs_two_reports(void)
{
	struct feed fd;
	feed_init(&fd);

	CHECK(!report(&fd, 2.4f) && !fd.f.active, "one 2.4%% report: still off");
	CHECK(report(&fd, 3.2f), "second report >= 2%%: reported as a change");
	CHECK(fd.f.active && fd.f.level == 5, "2.4%% then 3.2%%: on at 5 (got %d/%u)",
	      fd.f.active, fd.f.level);
	CHECK(report(&fd, 8.f) && fd.f.level == 10, "8%%: up to 10 (got %u)",
	      fd.f.level);
}

/* A spike on a 1–2% link is not two reports in a row. */
static void test_fec_isolated_spikes_stay_off(void)
{
	struct feed fd;
	feed_init(&fd);

	for (int i = 0; i < 12; i++)   /* a minute: spike, calm, spike, calm... */
		(void)report(&fd, i & 1 ? 1.4f : 2.8f);
	CHECK(!fd.f.active, "alternating 2.8%% / 1.4%%: FEC stays off");
}

/* 4% and up is plainly a lossy link: no waiting. */
static void test_fec_clear_loss_turns_on_at_once(void)
{
	struct feed fd;
	feed_init(&fd);

	CHECK(report(&fd, 6.f) && fd.f.active && fd.f.level == 10,
	      "6%%: on at once, at 10 (got %d/%u)", fd.f.active, fd.f.level);
}

static void test_fec_ticks_between_reports_carry_no_news(void)
{
	struct feed fd;
	feed_init(&fd);

	(void)report(&fd, 6.f);
	CHECK(fd.f.active, "on after 6%%");
	/* Twenty ticks, 40 s, with no new report: no change, and no "off". */
	for (int i = 0; i < 20; i++)
		CHECK(!quiet_tick(&fd), "quiet tick %d changed the state", i);
	CHECK(fd.f.active && fd.f.level == 10,
	      "still on at 10 with no reports (got %d/%u)",
	      fd.f.active, fd.f.level);
}

static void test_fec_off_after_ten_seconds_under_one_percent(void)
{
	struct feed fd;
	feed_init(&fd);

	(void)report(&fd, 5.f);                        /* t=5: on (>= 4%) */
	(void)report(&fd, 0.4f);                       /* t=10: 5 s clean */
	CHECK(fd.f.active, "one clean report is not enough to turn it off");
	CHECK(report(&fd, 0.f) && !fd.f.active && fd.f.level == 0,
	      "off 10 s after the last lossy report (got %d/%u)",
	      fd.f.active, fd.f.level);
}

static void test_fec_hysteresis_band_keeps_it_on(void)
{
	struct feed fd;
	feed_init(&fd);

	(void)report(&fd, 3.f);
	(void)report(&fd, 3.f);              /* confirmed: on */
	for (int i = 0; i < 6; i++)          /* 30 s hovering at 1.2–1.8% */
		(void)report(&fd, i & 1 ? 1.2f : 1.8f);
	CHECK(fd.f.active && fd.f.level == 5,
	      "1–2%% keeps FEC on at 5 (got %d/%u)", fd.f.active, fd.f.level);
}

static void test_fec_no_rtt_no_decision(void)
{
	struct bsdk_fec f;
	memset(&f, 0, sizeof(f));
	CHECK(!bsdk_adapt_fec_update(&f, 500, 100, 400, 0.f, 5000) && !f.active,
	      "no receiver report yet (rtt 0): nothing decided");
}

/* A report whose highest sequence has not moved while we kept sending:
 * nothing arrived.  The peer's own loss count says 0 then. */
static bool dead_report(struct feed *fd, uint32_t sent)
{
	fd->sent += sent;             /* lost and ehsn stay where they were */
	fd->now  += 5000;
	fd->rtt  += 0.25f;
	return bsdk_adapt_fec_update(&fd->f, fd->sent, fd->lost, fd->ehsn,
	                             fd->rtt, fd->now);
}

static void test_fec_dead_uplink(void)
{
	struct feed fd;
	feed_init(&fd);
	(void)report(&fd, 0.f);

	CHECK(dead_report(&fd, 250) && fd.f.dead, "dead uplink detected");
	CHECK(fd.f.active && fd.f.level == 25,
	      "dead uplink reads as 100%%: FEC on at 25 (got %d/%u)",
	      fd.f.active, fd.f.level);

	/* Back: the peer's count catches up with the whole outage at once. */
	fd.sent += 250; fd.lost += 500; fd.ehsn += 750;
	fd.now  += 5000; fd.rtt += 0.25f;
	CHECK(!bsdk_adapt_fec_update(&fd.f, fd.sent, fd.lost, fd.ehsn, fd.rtt,
	                             fd.now),
	      "catch-up report after the outage is not a new loss reading");
	CHECK(!fd.f.dead && fd.f.recovered, "recovered, and flagged for bitrate");
}

static void test_fec_not_sending_is_not_dead(void)
{
	struct feed fd;
	feed_init(&fd);
	(void)report(&fd, 0.f);

	/* On hold: a report with no progress, but we sent next to nothing. */
	(void)dead_report(&fd, 10);
	CHECK(!fd.f.dead && !fd.f.active, "10 packets sent is not a dead uplink");
}

/* Off strips useinbandfec from the fmtp our encoder is handed. */
static void test_fmtp_strip_inbandfec(void)
{
	char buf[256];
	int err = bsdk_adapt_fmtp_set(buf, sizeof(buf),
	                              "stereo=1;useinbandfec=1;usedtx=0",
	                              "useinbandfec", 0);
	CHECK(err == 0 && !strcmp(buf, "stereo=1;usedtx=0"),
	      "strip useinbandfec: got \"%s\" (err %d)", buf, err);
}

int main(void)
{
	printf("fmtp bitrate rewriting tests\n");

	test_no_source();
	test_preserves_negotiated_flags();
	test_replaces_existing();
	test_idempotent_under_repeat();
	test_prefix_not_confused();
	test_separators_normalised();
	test_strip_only();
	test_overflow_rejected();
	test_fec_bands();
	test_fec_band_hysteresis();
	test_fec_off_on_clean_and_light_loss();
	test_fec_borderline_needs_two_reports();
	test_fec_isolated_spikes_stay_off();
	test_fec_clear_loss_turns_on_at_once();
	test_fec_ticks_between_reports_carry_no_news();
	test_fec_off_after_ten_seconds_under_one_percent();
	test_fec_hysteresis_band_keeps_it_on();
	test_fec_no_rtt_no_decision();
	test_fmtp_strip_inbandfec();
	test_fec_dead_uplink();
	test_fec_not_sending_is_not_dead();

	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
