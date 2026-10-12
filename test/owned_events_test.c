/**
 * @file owned_events_test.c  Gate test for cfg.deliver_owned_events
 *
 * Proves the owned-event contract: events handed to the callback remain
 * valid (struct + strings) after the callback returns, until
 * medut_event_release() — even when released from a different thread.
 *
 * No SIP server needed: registration against a black-hole address produces
 * REG_STATE(RECONNECTING/FAILED) events with error_str strings, plus LOG
 * events.
 *
 * Build (from repo root, after scripts/build-linux.sh):
 *   gcc -fsanitize=address -g -O1 -std=gnu11 \
 *       -Iinclude test/owned_events_test.c \
 *       -Wl,--whole-archive dist/linux/x86_64/medut.a -Wl,--no-whole-archive \
 *       -lssl -lcrypto -lz -lpthread -lm -lresolv -ldl -lstdc++ -lpulse \
 *       -o test/owned_events_test && ./test/owned_events_test
 *
 * Exit 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include "../include/medut.h"

#define MAX_EVENTS 4096

/* Events stashed by the callback, released later by the consumer thread. */
static const medut_event_t *g_stash[MAX_EVENTS];
static atomic_int  g_head = 0;   /* next slot the callback writes   */
static atomic_int  g_tail = 0;   /* next slot the consumer releases */
static atomic_int  g_reg_failed_seen = 0;
static atomic_int  g_log_seen = 0;
static atomic_bool g_stop = false;

static bool g_owned_mode = true;   /* run with argv[1]=="borrowed" to compare */

static void event_cb(const medut_event_t *ev, void *ud)
{
	(void)ud;
	if (!g_owned_mode) {
		/* borrowed mode: touch strings inline only (legacy contract) */
		if (ev->type == MEDUT_EV_LOG && ev->u.log.message)
			(void)strlen(ev->u.log.message);
		atomic_fetch_add(&g_head, 1);
		atomic_fetch_add(&g_tail, 1);
		return;
	}
	int slot = atomic_fetch_add(&g_head, 1);
	if (slot >= MAX_EVENTS) {
		/* overflow — release inline so we don't leak */
		medut_event_release(ev);
		return;
	}
	g_stash[slot] = ev;   /* keep the pointer PAST callback return */
}

/* Consumer thread: lags behind the producer, validates strings that were
 * written into the (freed, in borrowed mode) original event, then releases. */
static void *consumer_fn(void *arg)
{
	(void)arg;
	for (;;) {
		int tail = atomic_load(&g_tail);
		int head = atomic_load(&g_head);
		if (head > MAX_EVENTS)
			head = MAX_EVENTS;
		if (tail >= head) {
			if (atomic_load(&g_stop))
				break;
			usleep(2000);
			continue;
		}
		/* Deliberate lag so the C side has long since "moved on". */
		usleep(5000);

		const medut_event_t *ev = g_stash[tail];
		switch (ev->type) {
		case MEDUT_EV_LOG:
			/* Touch every byte — ASAN catches use-after-free. */
			if (ev->u.log.message)
				(void)strlen(ev->u.log.message);
			atomic_store(&g_log_seen, 1);
			break;
		case MEDUT_EV_REG_STATE:
			/* A black-hole registrar times out, and a timeout with a
			 * retry armed is reported as RECONNECTING — the string this
			 * test is about rides on either one. */
			if (ev->u.reg.state == MEDUT_REG_FAILED ||
			    ev->u.reg.state == MEDUT_REG_RECONNECTING) {
				if (ev->u.reg.error_str)
					(void)strlen(ev->u.reg.error_str);
				atomic_store(&g_reg_failed_seen, 1);
			}
			break;
		case MEDUT_EV_SIP_TRACE:
			if (ev->u.sip_trace.raw_message)
				(void)strlen(ev->u.sip_trace.raw_message);
			if (ev->u.sip_trace.transport)
				(void)strlen(ev->u.sip_trace.transport);
			break;
		default:
			break;
		}
		medut_event_release(ev);
		atomic_store(&g_tail, tail + 1);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "borrowed") == 0)
		g_owned_mode = false;

	medut_config_t cfg;
	medut_config_init(&cfg);
	cfg.log_level            = 3;      /* max LOG event volume */
	cfg.trace_sip            = true;   /* SIP_TRACE events too */
	cfg.event_cb             = event_cb;
	cfg.deliver_owned_events = g_owned_mode;
	cfg.net_monitor_interval_s = 0;

	int err = medut_init(&cfg);
	if (err) {
		fprintf(stderr, "FAIL: medut_init: %d\n", err);
		return 1;
	}

	pthread_t consumer;
	pthread_create(&consumer, NULL, consumer_fn, NULL);

	/* Register against a black-hole (TEST-NET-1) — guaranteed failure path
	 * with short SIP timers so retries generate a stream of events. */
	medut_account_config_t acfg;
	memset(&acfg, 0, sizeof(acfg));
	acfg.uri       = "alice@192.0.2.1:5060";
	acfg.password  = "secret";
	acfg.transport = MEDUT_TRANSPORT_UDP;

	medut_account_handle_t acct = NULL;
	err = medut_account_create(&acfg, &acct);
	if (err) {
		fprintf(stderr, "FAIL: account_create: %d\n", err);
		return 1;
	}
	medut_account_set_retry_policy(acct, 200, 1000, 1.5f, 0);
	medut_account_register(acct);

	/* Let events flow for a while (registration timeout is timer-B bound;
	 * LOG/TRACE events arrive immediately). */
	sleep(8);

	medut_account_destroy(acct);
	medut_shutdown();

	/* Drain what's left, then stop the consumer. */
	atomic_store(&g_stop, true);
	pthread_join(consumer, NULL);

	int produced = atomic_load(&g_head);
	int released = atomic_load(&g_tail);
	if (produced > MAX_EVENTS)
		produced = MAX_EVENTS;

	fprintf(stderr, "events produced=%d released=%d log=%d reg_failed=%d\n",
	        produced, released,
	        atomic_load(&g_log_seen), atomic_load(&g_reg_failed_seen));

	if (produced == 0 || released != produced) {
		fprintf(stderr, "FAIL: produced/released mismatch\n");
		return 1;
	}
	if (!atomic_load(&g_log_seen)) {
		fprintf(stderr, "FAIL: no LOG event observed\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
