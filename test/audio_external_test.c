/**
 * @file audio_external_test.c  Gate test for the app-owned audio device
 *
 * Covers the parts of medut_audio_use_external() that only exist once the
 * whole stack is up, and that a unit test on audio_external.c alone cannot
 * see:
 *
 *   1. The switch actually re-points baresip at the "external" driver, and
 *      switching back restores the platform device rather than leaving the
 *      stack pointed at a module that is not there.
 *   2. The driver survives a shutdown/init cycle.  This is the regression that
 *      motivated the test: baresip_close()/baresip_init() re-initialise the
 *      device lists, so a registration made before a restart is discarded.
 *      Nothing called bsdk_audio_external_close(), so the driver believed it
 *      was still registered, skipped re-registering, and use_external(true)
 *      went on returning 0 while every call came up with no audio at all.
 *   3. Calling it before init() is refused rather than silently ignored — an
 *      app that misses that would start its own capture while the SDK still
 *      held the microphone.
 *
 * No SIP server needed; nothing here places a call.
 *
 * Build (from repo root, after scripts/build-linux.sh):
 *   gcc -fsanitize=address -g -O1 -std=gnu11 \
 *       -Iinclude -Idist/linux/x86_64/include -Idist/linux/x86_64/include/re \
 *       test/audio_external_test.c \
 *       -Wl,--whole-archive dist/linux/x86_64/medut.a -Wl,--no-whole-archive \
 *       -lssl -lcrypto -lz -lpthread -lm -lresolv -ldl -lstdc++ -lpulse \
 *       -o test/audio_external_test && ./test/audio_external_test
 *
 * Exit 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/medut.h"

/* White-box on purpose: the alternative to reading back what the SDK told
 * baresip is a live peer to negotiate a codec with. */
#include <re.h>
#include <rem.h>
#include <baresip.h>

static int g_pass = 0;
static int g_fail = 0;

/* stdout, not stderr: bsdk_log_init() takes stderr over for SIP logging. */
#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("FAIL(line %d): ", __LINE__); \
	       printf(__VA_ARGS__); fflush(stdout); } \
} while (0)

/* Borrowed delivery (the default): nothing to release, and nothing here cares
 * what the events say — the handler exists only so init() has one. */
static void ev_handler(const medut_event_t *ev, void *ud)
{
	(void)ev; (void)ud;
}

static int start_sdk(void)
{
	medut_config_t cfg;
	medut_config_init(&cfg);
	cfg.log_level = 3;
	cfg.event_cb  = ev_handler;
	return medut_init(&cfg);
}

static const char *src_mod(void)
{
	return conf_config()->audio.src_mod;
}

static const char *play_mod(void)
{
	return conf_config()->audio.play_mod;
}

int main(void)
{
	int err;

	/* ── 1. Refused before init ──────────────────────────────────────── */

	err = medut_audio_use_external(true);
	CHECK(err == MEDUT_ERR_STATE,
	      "use_external before init: expected MEDUT_ERR_STATE (%d), got %d\n",
	      MEDUT_ERR_STATE, err);

	/* ── 2. The switch reaches baresip both ways ─────────────────────── */

	err = start_sdk();
	if (err) {
		printf("FATAL: medut_init: %s (%d)\n", medut_strerror(err), err);
		return 1;
	}

	/* Whatever modules_init() settled on — pulse here, sles_vc/audiounit on
	 * mobile. Remember it so we can prove it comes back. */
	char platform_mod[64];
	str_ncpy(platform_mod, src_mod(), sizeof(platform_mod));
	CHECK(strcmp(platform_mod, "external") != 0,
	      "the SDK-owned device must be the default, got '%s'\n", platform_mod);

	err = medut_audio_use_external(true);
	CHECK(err == 0, "use_external(true): %d\n", err);
	CHECK(strcmp(src_mod(), "external") == 0,
	      "capture module: expected 'external', got '%s'\n", src_mod());
	CHECK(strcmp(play_mod(), "external") == 0,
	      "playback module: expected 'external', got '%s'\n", play_mod());
	CHECK(ausrc_find(baresip_ausrcl(), "external") != NULL,
	      "the 'external' capture driver must be registered\n");
	CHECK(auplay_find(baresip_auplayl(), "external") != NULL,
	      "the 'external' playback driver must be registered\n");

	/* No call is up, so there is no device open behind it yet. */
	CHECK(!medut_audio_external_is_active(),
	      "is_active must be false with no call\n");
	CHECK(medut_audio_external_format(NULL, NULL, NULL) == ENODEV,
	      "format must report ENODEV before a call has media\n");

	err = medut_audio_use_external(false);
	CHECK(err == 0, "use_external(false): %d\n", err);
	CHECK(strcmp(src_mod(), platform_mod) == 0,
	      "capture module: expected '%s' back, got '%s'\n",
	      platform_mod, src_mod());

	/* ── 3. It survives a restart ────────────────────────────────────── */

	medut_shutdown();

	err = start_sdk();
	if (err) {
		printf("FATAL: medut_init after shutdown: %s (%d)\n",
		       medut_strerror(err), err);
		return 1;
	}

	CHECK(ausrc_find(baresip_ausrcl(), "external") != NULL,
	      "the capture driver must be re-registered after a restart\n");
	CHECK(auplay_find(baresip_auplayl(), "external") != NULL,
	      "the playback driver must be re-registered after a restart\n");

	err = medut_audio_use_external(true);
	CHECK(err == 0, "use_external(true) after restart: %d\n", err);
	CHECK(strcmp(src_mod(), "external") == 0,
	      "capture module after restart: got '%s'\n", src_mod());

	/* The real point of the restart case: use_external() returning 0 is not
	 * worth much on its own, because it only writes a module name into the
	 * config. What matters is that the name still resolves to a driver. */
	struct ausrc_prm prm = {
		.srate = 8000, .ch = 1, .ptime = 20, .fmt = AUFMT_S16LE,
	};
	struct ausrc_st *st = NULL;
	err = ausrc_alloc(&st, baresip_ausrcl(), src_mod(), &prm, NULL,
	                  NULL, NULL, NULL);
	CHECK(err == EINVAL || err == 0,
	      "the configured module must resolve to a real driver after a "
	      "restart, got %d (ENOENT means it was never re-registered)\n", err);
	mem_deref(st);

	medut_shutdown();

	printf("audio_external_test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
