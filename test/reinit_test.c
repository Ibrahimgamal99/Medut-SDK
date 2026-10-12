/**
 * @file reinit_test.c  Gate test: medut_shutdown() → medut_init() in one
 *                      process must come back with a working stack
 *
 * What went wrong: libre_close() never unloads the module registry (re's
 * mod.c `modl` survives it) while baresip_close() frees the codec /
 * media-encryption / media-NAT lists those modules registered into.  On the
 * next medut_init(), module_load() found every module via mod_find(),
 * answered EALREADY, and never re-ran their init() — so the second stack had
 * zero codecs, no dtls_srtp and no ice, and medut_account_create() still
 * returned 0.  Hit in practice by two Flutter engines each treating the SDK
 * as their own: one engine's shutdown crippled the other's next init.
 *
 * medut_shutdown() now calls mod_close() (core.c), and
 * medut_account_create() refuses an account whose media encryption / ICE /
 * codecs are not registered (account.c) instead of handing back one that
 * would offer plain RTP.
 *
 * White-box: reads baresip's registries directly, the way reattach_test.c
 * reads the account codec list — the alternative is a live peer.
 *
 * Build (from repo root, after scripts/build-linux.sh):
 *   gcc -fsanitize=address -g -O1 -std=gnu11 \
 *       -Iinclude -Ibuild/linux-x86_64/sysroot/include \
 *       -Ibuild/linux-x86_64/sysroot/include/re test/reinit_test.c \
 *       -Wl,--whole-archive dist/linux/x86_64/medut.a -Wl,--no-whole-archive \
 *       -lssl -lcrypto -lz -lpthread -lm -lresolv -ldl -lstdc++ -lpulse \
 *       -o test/reinit_test && ./test/reinit_test
 */

#include <stdio.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "medut.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("FAIL(line %d): ", __LINE__); \
	       printf(__VA_ARGS__); fflush(stdout); } \
} while (0)

static void on_event(const medut_event_t *ev, void *ud)
{
	(void)ev; (void)ud;
}

static int stack_up_with(medut_config_t *cfg)
{
	cfg->log_level = 0;
	cfg->event_cb  = on_event;
	return medut_init(cfg);
}

static int stack_up(void)
{
	medut_config_t cfg;
	medut_config_init(&cfg);
	return stack_up_with(&cfg);
}

/* Whether the account for AOR resolved Opus from its codec list. */
static bool account_has_opus(const char *aor)
{
	struct ua *ua = uag_find_aor(aor);
	struct account *acc = ua ? ua_account(ua) : NULL;
	struct list *l = acc ? account_aucodecl(acc) : NULL;

	for (struct le *le = list_head(l); le; le = le->next) {
		const struct aucodec *ac = le->data;
		if (ac && ac->name && !str_casecmp(ac->name, "opus"))
			return true;
	}
	return false;
}

/* One init → probe → create → shutdown cycle. */
static void cycle(int n)
{
	medut_account_config_t ac;
	medut_account_handle_t h = NULL;
	int err;

	err = stack_up();
	CHECK(err == 0, "cycle %d: medut_init returned %d\n", n, err);
	if (err)
		return;

	/* The registries the modules fill at init(). */
	CHECK(list_count(baresip_aucodecl()) >= 2,
	      "cycle %d: only %u audio codecs registered\n",
	      n, list_count(baresip_aucodecl()));
	/* Any channel count: mono unless cfg.opus.stereo. */
	CHECK(aucodec_find(baresip_aucodecl(), "opus", 48000, 0) != NULL,
	      "cycle %d: opus missing\n", n);
	CHECK(menc_find(baresip_mencl(), "dtls_srtp") != NULL,
	      "cycle %d: dtls_srtp missing\n", n);
	CHECK(mnat_find(baresip_mnatl(), "ice") != NULL,
	      "cycle %d: ice missing\n", n);

	/* An account that needs all three must be creatable. */
	memset(&ac, 0, sizeof(ac));
	ac.uri         = "100@192.0.2.1";
	ac.password    = "secret";
	ac.transport   = MEDUT_TRANSPORT_UDP;
	ac.media_enc   = MEDUT_MEDIA_ENC_DTLS_SRTP;
	ac.ice_enabled = true;
	err = medut_account_create(&ac, &h);
	CHECK(err == 0 && h != NULL,
	      "cycle %d: account_create with dtls_srtp+ice returned %d\n",
	      n, err);
	/* The default codec list must resolve Opus against the module as it
	 * registered: spelled opus/48000/2 against a mono module, the account
	 * silently had none and every call went out G.711. */
	CHECK(account_has_opus("sip:100@192.0.2.1"),
	      "cycle %d: the account resolved no Opus\n", n);
	if (h)
		medut_account_destroy(h);

	medut_shutdown();
}

/* The opus module reads its settings into globals once per init, and used to
 * write only the ones a key was present for: a later init that left a key
 * out kept the earlier value, and one mono init made every later one mono.
 * (cmake/patch-baresip-sources.cmake, job 7.) */
extern uint32_t opus_complexity;     /* modules/opus/opus.c */
extern int32_t  opus_packet_loss;

static void test_opus_settings_per_init(void)
{
	static const struct {
		bool     stereo;
		int      complexity;      /* -1: left at the SDK default */
		uint32_t pinned_loss;
		uint8_t  want_ch;
		uint32_t want_complexity;
		int32_t  want_loss;
	} inits[] = {
		{ true,  0,  20, 2, 0,  20 },
		{ false, -1, 0,  1, 10, 0  },
		{ true,  -1, 0,  2, 10, 0  },
	};

	for (size_t i = 0; i < sizeof(inits) / sizeof(inits[0]); i++) {
		medut_config_t cfg;
		const struct aucodec *opus;

		medut_config_init(&cfg);
		cfg.opus.stereo             = inits[i].stereo;
		cfg.opus.complexity         = inits[i].complexity;
		cfg.opus_expected_loss_pct  = inits[i].pinned_loss;
		if (stack_up_with(&cfg)) {
			CHECK(false, "opus init %zu: medut_init failed\n", i);
			continue;
		}
		opus = aucodec_find(baresip_aucodecl(), "opus", 48000, 0);
		CHECK(opus && opus->ch == inits[i].want_ch,
		      "opus init %zu: %u channels, want %u\n", i,
		      opus ? opus->ch : 0, inits[i].want_ch);
		CHECK(opus_complexity == inits[i].want_complexity,
		      "opus init %zu: complexity %u, want %u\n", i,
		      opus_complexity, inits[i].want_complexity);
		CHECK(opus_packet_loss == inits[i].want_loss,
		      "opus init %zu: expected loss %d, want %d\n", i,
		      opus_packet_loss, inits[i].want_loss);
		medut_shutdown();
	}
}

/* With the registries deliberately emptied the way the bug left them,
 * account_create must refuse rather than return 0. */
static void test_create_refuses_on_empty_registry(void)
{
	medut_account_config_t ac;
	medut_account_handle_t h = NULL;
	struct menc *dtls;
	int err;

	if (stack_up())
		return;

	/* Take dtls_srtp out of the registry (what an un-reinitialised
	 * module set looked like), then ask for it. */
	dtls = (struct menc *)menc_find(baresip_mencl(), "dtls_srtp");
	CHECK(dtls != NULL, "dtls_srtp not registered before the test\n");
	if (dtls)
		list_unlink(&dtls->le);

	memset(&ac, 0, sizeof(ac));
	ac.uri       = "100@192.0.2.1";
	ac.password  = "secret";
	ac.transport = MEDUT_TRANSPORT_UDP;
	ac.media_enc = MEDUT_MEDIA_ENC_DTLS_SRTP;
	err = medut_account_create(&ac, &h);
	CHECK(err == MEDUT_ERR_STATE && h == NULL,
	      "account_create with an unregistered mediaenc returned %d "
	      "(expected MEDUT_ERR_STATE)\n", err);
	if (h)
		medut_account_destroy(h);

	/* Put it back so shutdown's module close finds what it expects. */
	if (dtls)
		list_append(baresip_mencl(), &dtls->le, dtls);

	medut_shutdown();
}

int main(void)
{
	printf("reinit_test: three init/shutdown cycles in one process\n");
	cycle(1);
	cycle(2);
	cycle(3);

	printf("reinit_test: account_create refuses a missing mediaenc\n");
	test_create_refuses_on_empty_registry();

	printf("reinit_test: opus settings come from each init alone\n");
	test_opus_settings_per_init();

	printf("reinit_test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
