/**
 * @file reconnect_state_test.c  Gate test for MEDUT_REG_RECONNECTING
 *
 * A registration the SDK is still recovering must be reported as RECONNECTING,
 * and only one it has given up on as FAILED.  Both halves are checked against a
 * black-hole registrar (TEST-NET-1), so no SIP server is needed:
 *
 *   REGISTERING → RECONNECTING (timeout, retry armed)
 *               → RECONNECTING (each retry, no flip back to REGISTERING)
 *               → FAILED       (retry budget exhausted — the app is owed this)
 *
 * The retry policy is tightened so the whole sequence runs in ~10 s: two
 * attempts, 1 s apart, against a 2 s registration watchdog.
 *
 * Build (from repo root, after scripts/build-linux.sh):
 *   gcc -g -O1 -std=gnu11 \
 *       -Iinclude test/reconnect_state_test.c \
 *       dist/linux/x86_64/medut.so \
 *       -Wl,-rpath,'$ORIGIN/../dist/linux/x86_64' \
 *       -o test/reconnect_state_test && ./test/reconnect_state_test
 *
 * Exit 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <threads.h>
#include <unistd.h>
#include "../include/medut.h"

#define MAX_SEQ 64

static mtx_t                g_lock;
static medut_reg_state_t  g_seq[MAX_SEQ];
static size_t               g_seq_n;
static atomic_int           g_failed_seen;

static const char *sname(medut_reg_state_t s)
{
	switch (s) {
	case MEDUT_REG_UNREGISTERED:  return "UNREGISTERED";
	case MEDUT_REG_REGISTERING:   return "REGISTERING";
	case MEDUT_REG_REGISTERED:    return "REGISTERED";
	case MEDUT_REG_FAILED:        return "FAILED";
	case MEDUT_REG_UNREGISTERING: return "UNREGISTERING";
	case MEDUT_REG_RECONNECTING:  return "RECONNECTING";
	}
	return "?";
}

static void event_handler(const medut_event_t *ev, void *ud)
{
	(void)ud;

	if (ev->type != MEDUT_EV_REG_STATE)
		return;

	printf("[REG] %-13s err=%d attempt=%u delay=%u str=%s\n",
	       sname(ev->u.reg.state), ev->u.reg.error,
	       ev->u.reg.retry_attempt, ev->u.reg.retry_delay_ms,
	       ev->u.reg.error_str ? ev->u.reg.error_str : "-");
	fflush(stdout);

	mtx_lock(&g_lock);
	if (g_seq_n < MAX_SEQ)
		g_seq[g_seq_n++] = ev->u.reg.state;
	mtx_unlock(&g_lock);

	if (ev->u.reg.state == MEDUT_REG_FAILED)
		atomic_store(&g_failed_seen, 1);
}

#define CHECK(cond, ...) do {                       \
	if (!(cond)) {                              \
		printf("FAIL: " __VA_ARGS__);       \
		return 1;                           \
	}                                           \
} while (0)

int main(void)
{
	medut_config_t cfg;
	medut_account_config_t acfg;
	medut_account_handle_t acct = NULL;
	size_t reconnecting = 0, registering = 0;
	medut_reg_state_t last = MEDUT_REG_UNREGISTERED;
	int err;

	mtx_init(&g_lock, mtx_plain);

	medut_config_init(&cfg);
	cfg.event_cb               = event_handler;
	cfg.log_level              = 0;
	cfg.sip_timer_f_ms         = 2000;   /* fail an unanswered REGISTER fast */
	cfg.reg_retry_initial_ms   = 1000;
	cfg.reg_retry_max_ms       = 1000;
	cfg.reg_retry_backoff      = 1.0f;
	cfg.reg_retry_jitter       = 0.f;    /* deterministic timing */
	cfg.reg_retry_max_attempts = 2;      /* two retries, then give up */
	cfg.keepalive_interval     = 0;      /* not what this test is about */
	cfg.net_monitor_interval_s = 0;

	err = medut_init(&cfg);
	CHECK(!err, "medut_init: %d\n", err);

	memset(&acfg, 0, sizeof(acfg));
	acfg.uri       = "alice@192.0.2.1";   /* TEST-NET-1: goes nowhere */
	acfg.password  = "secret";
	acfg.transport = MEDUT_TRANSPORT_UDP;

	err = medut_account_create(&acfg, &acct);
	CHECK(!err, "account_create: %d\n", err);

	medut_account_register(acct);

	/* Two 2 s timeouts plus two 1 s backoffs, with room to spare. */
	for (int i = 0; i < 15 && !atomic_load(&g_failed_seen); i++)
		sleep(1);

	mtx_lock(&g_lock);
	for (size_t i = 0; i < g_seq_n; i++) {
		if (g_seq[i] == MEDUT_REG_RECONNECTING)
			reconnecting++;
		/* The first REGISTER is a plain REGISTERING; every attempt after
		 * it belongs to the recovery and must not report REGISTERING
		 * again, or the app's status line flickers once per retry. */
		else if (g_seq[i] == MEDUT_REG_REGISTERING && i > 0)
			registering++;
	}
	if (g_seq_n)
		last = g_seq[g_seq_n - 1];
	mtx_unlock(&g_lock);

	CHECK(g_seq_n > 0, "no REG_STATE events at all\n");
	CHECK(g_seq[0] == MEDUT_REG_REGISTERING,
	      "first event was %s, want REGISTERING\n", sname(g_seq[0]));
	CHECK(reconnecting >= 2,
	      "only %zu RECONNECTING events; the retry loop should report one "
	      "per failure and one per armed retry\n", reconnecting);
	CHECK(registering == 0,
	      "%zu REGISTERING events inside the recovery; retries must report "
	      "RECONNECTING\n", registering);
	CHECK(atomic_load(&g_failed_seen),
	      "the exhausted retry budget never reported FAILED — an app would "
	      "render \"Reconnecting…\" for ever\n");
	CHECK(last == MEDUT_REG_FAILED,
	      "last event was %s, want FAILED\n", sname(last));
	CHECK(medut_account_get_reg_state(acct) == MEDUT_REG_FAILED,
	      "get_reg_state() = %s, want FAILED\n",
	      sname(medut_account_get_reg_state(acct)));

	medut_account_destroy(acct);
	medut_shutdown();
	mtx_destroy(&g_lock);

	printf("PASS\n");
	return 0;
}
