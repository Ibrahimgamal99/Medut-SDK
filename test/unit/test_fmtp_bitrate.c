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

	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
