/**
 * @file reattach_shutdown_test.c  The Flutter smoke test's native calls,
 *                                 without Dart, on Linux and Windows
 *
 * bindings/flutter/test/medut_smoke_test.dart runs its second and third
 * tests in one process.  On Windows x64 its process ends, intermittently and
 * without a crash dump, during the third test's shutdown — the second
 * init / shutdown cycle of the process, after a detach and a reattach.  This
 * program makes the same calls the Dart FFI backend makes, in the same
 * order, against the same black-hole registrar, but from C: when it never
 * fails where the Dart test does, the problem lies in what the Dart side
 * does around those calls (owned events released late, a callback used
 * after it was closed); when it fails too, it lies in the stack, and the
 * last "step:" line it printed says where.
 *
 *   cycle 1 (smoke test 2)  init with owned events, an account on UDP to
 *                           192.0.2.1 (TEST-NET-1, never answers) with a fast
 *                           retry policy, 100rel, register, 3 s of retries,
 *                           refresh, app-owned audio on and off, destroy the
 *                           account, shutdown;
 *   cycle 2 (smoke test 3)  init again with a global codec list, an account,
 *                           register, 1 s, detach (handler NULL) and reattach
 *                           (handler set again, accounts and calls walked as
 *                           an adopting instance does), unregister, 0.3 s,
 *                           register, 2 s, destroy the account, shutdown.
 *
 * Events are delivered owned, as the Dart backend asks for them, and every
 * one is released with medut_event_release() inside the callback.
 *
 * Uses the public API only (include/medut.h), so it links against the
 * shared library on every platform:
 *
 *   Linux (after scripts/build-linux.sh):
 *     gcc -std=gnu11 -Iinclude test/reattach_shutdown_test.c \
 *         dist/linux/x86_64/medut.so -Wl,-rpath,dist/linux/x86_64 \
 *         -o test/reattach_shutdown_test && ./test/reattach_shutdown_test
 *
 *   Windows (after scripts/build-windows.ps1, in a VS developer prompt):
 *     cl /nologo /W3 /Iinclude test\reattach_shutdown_test.c ^
 *        /link dist\windows\x64\medut.lib
 *     (with medut.dll next to the .exe)
 *
 * Exit 0 when both cycles complete.
 */

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "medut.h"

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000u);
#endif
}

/* Printed and flushed before each call, so that a process that ends inside
 * one leaves the name of that call as its last line of output. */
static void step(const char *what)
{
	printf("step: %s\n", what);
	fflush(stdout);
}

static volatile unsigned long n_events;

static void on_event(const medut_event_t *ev, void *ud)
{
	(void)ud;
	n_events++;
	medut_event_release(ev);   /* owned delivery, as the Dart backend asks */
}

static void count_account(medut_account_handle_t acct, void *arg)
{
	(void)acct;
	(*(int *)arg)++;
}

static void count_call(medut_call_handle_t call, void *arg)
{
	(void)call;
	(*(int *)arg)++;
}

static medut_account_handle_t black_hole_account(void)
{
	medut_account_config_t acfg;
	medut_account_handle_t acct = NULL;

	memset(&acfg, 0, sizeof(acfg));
	acfg.uri       = "alice@192.0.2.1:5060";
	acfg.password  = "secret";
	acfg.transport = MEDUT_TRANSPORT_UDP;
	if (medut_account_create(&acfg, &acct) != MEDUT_OK)
		return NULL;
	medut_account_set_retry_policy(acct, 200, 1000, 1.5f, 0);
	return acct;
}

static int cycle_one(void)
{
	medut_config_t cfg;
	medut_account_handle_t acct;
	int err;

	step("cycle 1: init");
	medut_config_init(&cfg);
	cfg.event_cb               = on_event;
	cfg.deliver_owned_events   = true;
	cfg.log_level              = 3;
	cfg.trace_sip              = true;
	cfg.net_monitor_interval_s = 0;
	err = medut_init(&cfg);
	if (err != MEDUT_OK) {
		printf("cycle 1: medut_init failed: %d\n", err);
		return 1;
	}

	step("cycle 1: account and register");
	acct = black_hole_account();
	if (!acct) {
		printf("cycle 1: medut_account_create failed\n");
		return 1;
	}
	medut_account_set_100rel(acct, MEDUT_100REL_ENABLED);
	medut_account_register(acct);
	sleep_ms(3000);

	step("cycle 1: refresh registration");
	medut_account_refresh_registration(acct);

	step("cycle 1: app-owned audio on and off");
	medut_audio_use_external(true);
	medut_audio_use_external(false);

	step("cycle 1: destroy account");
	medut_account_destroy(acct);

	step("cycle 1: shutdown");
	medut_shutdown();
	return 0;
}

static int cycle_two(void)
{
	medut_config_t cfg;
	medut_account_handle_t acct;
	int n_accounts = 0, n_calls = 0;
	int err;

	step("cycle 2: init");
	medut_config_init(&cfg);
	cfg.event_cb               = on_event;
	cfg.deliver_owned_events   = true;
	cfg.log_level              = 3;
	cfg.net_monitor_interval_s = 0;
	strcpy(cfg.audio_codec_names[0], "ulaw");
	strcpy(cfg.audio_codec_names[1], "opus");
	cfg.audio_codec_name_count = 2;
	err = medut_init(&cfg);
	if (err != MEDUT_OK) {
		printf("cycle 2: medut_init failed: %d\n", err);
		return 1;
	}

	step("cycle 2: account and register");
	acct = black_hole_account();
	if (!acct) {
		printf("cycle 2: medut_account_create failed\n");
		return 1;
	}
	medut_account_register(acct);
	sleep_ms(1000);

	step("cycle 2: detach (no event handler)");
	medut_set_event_handler(NULL, NULL, false);

	step("cycle 2: reattach (handler again, accounts and calls walked)");
	medut_set_event_handler(on_event, NULL, true);
	medut_account_foreach(count_account, &n_accounts);
	medut_call_foreach(count_call, &n_calls);
	if (n_accounts != 1 || n_calls != 0) {
		printf("cycle 2: reattach found %d accounts, %d calls\n",
		       n_accounts, n_calls);
		return 1;
	}

	step("cycle 2: unregister, then register again");
	medut_account_unregister(acct);
	sleep_ms(300);
	medut_account_register(acct);
	sleep_ms(2000);

	step("cycle 2: destroy account");
	medut_account_destroy(acct);

	step("cycle 2: shutdown");
	medut_shutdown();
	return 0;
}

int main(void)
{
	if (cycle_one() || cycle_two())
		return 1;

	printf("both cycles completed, %lu events delivered\n", n_events);
	return 0;
}
