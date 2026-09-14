/**
 * @file reinit_test.c  Gate test: echosdk_shutdown() → echosdk_init() in one
 *                      process must come back with a working stack
 *
 * What went wrong: libre_close() never unloads the module registry (re's
 * mod.c `modl` survives it) while baresip_close() frees the codec /
 * media-encryption / media-NAT lists those modules registered into.  On the
 * next echosdk_init(), module_load() found every module via mod_find(),
 * answered EALREADY, and never re-ran their init() — so the second stack had
 * zero codecs, no dtls_srtp and no ice, and echosdk_account_create() still
 * returned 0.  Hit in practice by two Flutter engines each treating the SDK
 * as their own: one engine's shutdown crippled the other's next init.
 *
 * echosdk_shutdown() now calls mod_close() (core.c), and
 * echosdk_account_create() refuses an account whose media encryption / ICE /
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
 *       -Wl,--whole-archive dist/linux/x86_64/echosdk.a -Wl,--no-whole-archive \
 *       -lssl -lcrypto -lz -lpthread -lm -lresolv -ldl -lstdc++ -lpulse \
 *       -o test/reinit_test && ./test/reinit_test
 */

#include <stdio.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "echosdk.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } \
	else { g_fail++; printf("FAIL(line %d): ", __LINE__); \
	       printf(__VA_ARGS__); fflush(stdout); } \
} while (0)

static void on_event(const echosdk_event_t *ev, void *ud)
{
	(void)ev; (void)ud;
}

static int stack_up(void)
{
	echosdk_config_t cfg;
	echosdk_config_init(&cfg);
	cfg.log_level = 0;
	cfg.event_cb  = on_event;
	return echosdk_init(&cfg);
}

/* One init → probe → create → shutdown cycle. */
static void cycle(int n)
{
	echosdk_account_config_t ac;
	echosdk_account_handle_t h = NULL;
	int err;

	err = stack_up();
	CHECK(err == 0, "cycle %d: echosdk_init returned %d\n", n, err);
	if (err)
		return;

	/* The registries the modules fill at init(). */
	CHECK(list_count(baresip_aucodecl()) >= 2,
	      "cycle %d: only %u audio codecs registered\n",
	      n, list_count(baresip_aucodecl()));
	CHECK(aucodec_find(baresip_aucodecl(), "opus", 48000, 2) != NULL,
	      "cycle %d: opus missing\n", n);
	CHECK(menc_find(baresip_mencl(), "dtls_srtp") != NULL,
	      "cycle %d: dtls_srtp missing\n", n);
	CHECK(mnat_find(baresip_mnatl(), "ice") != NULL,
	      "cycle %d: ice missing\n", n);

	/* An account that needs all three must be creatable. */
	memset(&ac, 0, sizeof(ac));
	ac.uri         = "100@192.0.2.1";
	ac.password    = "secret";
	ac.transport   = ECHOSDK_TRANSPORT_UDP;
	ac.media_enc   = ECHOSDK_MEDIA_ENC_DTLS_SRTP;
	ac.ice_enabled = true;
	err = echosdk_account_create(&ac, &h);
	CHECK(err == 0 && h != NULL,
	      "cycle %d: account_create with dtls_srtp+ice returned %d\n",
	      n, err);
	if (h)
		echosdk_account_destroy(h);

	echosdk_shutdown();
}

/* With the registries deliberately emptied the way the bug left them,
 * account_create must refuse rather than return 0. */
static void test_create_refuses_on_empty_registry(void)
{
	echosdk_account_config_t ac;
	echosdk_account_handle_t h = NULL;
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
	ac.transport = ECHOSDK_TRANSPORT_UDP;
	ac.media_enc = ECHOSDK_MEDIA_ENC_DTLS_SRTP;
	err = echosdk_account_create(&ac, &h);
	CHECK(err == ECHOSDK_ERR_STATE && h == NULL,
	      "account_create with an unregistered mediaenc returned %d "
	      "(expected ECHOSDK_ERR_STATE)\n", err);
	if (h)
		echosdk_account_destroy(h);

	/* Put it back so shutdown's module close finds what it expects. */
	if (dtls)
		list_append(baresip_mencl(), &dtls->le, dtls);

	echosdk_shutdown();
}

int main(void)
{
	printf("reinit_test: three init/shutdown cycles in one process\n");
	cycle(1);
	cycle(2);
	cycle(3);

	printf("reinit_test: account_create refuses a missing mediaenc\n");
	test_create_refuses_on_empty_registry();

	printf("reinit_test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
