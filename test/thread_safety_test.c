/**
 * @file thread_safety_test.c  N-thread concurrent API hammer test
 *
 * Spawns NTHREADS threads. Each thread repeatedly calls:
 *   - medut_call_invite() + medut_call_hangup()
 *   - medut_audio_mute() toggle
 *   - medut_call_get_stats()
 *
 * Build with: -fsanitize=thread
 * Pass: no crash, no TSan output, exit 0.
 * The test is intentionally short (1 second) so it fits in CI.
 *
 * Note: calls will fail (no server) but the point is thread-safety,
 * not call establishment.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <threads.h>
#include <time.h>
#include "../include/medut.h"

#define NTHREADS  8
#define RUN_MS    1000  /* 1 second per thread */

static medut_account_handle_t g_acct = NULL;
static atomic_int g_stop = 0;

static void event_handler(const medut_event_t *ev, void *ud)
{
	(void)ev; (void)ud;
}

static int worker(void *arg)
{
	(void)arg;

	struct timespec ts_end;
	timespec_get(&ts_end, TIME_UTC);
	ts_end.tv_sec += RUN_MS / 1000;

	while (!atomic_load(&g_stop)) {
		struct timespec now;
		timespec_get(&now, TIME_UTC);
		if (now.tv_sec > ts_end.tv_sec ||
		    (now.tv_sec == ts_end.tv_sec &&
		     now.tv_nsec >= ts_end.tv_nsec))
			break;

		/* Invite (will fail — server not present, that's fine) */
		medut_call_handle_t call = NULL;
		medut_call_invite(g_acct, "sip:test@127.0.0.1", &call);

		if (call) {
			/* Toggle mute */
			medut_audio_mute(call, true);
			medut_audio_mute(call, false);

			/* Get stats (will return error — call not established) */
			medut_ev_media_stats_t stats;
			medut_call_get_stats(call, &stats);

			medut_call_hangup(call);
		}

		/* Tiny sleep to avoid spinning */
		struct timespec sl = {0, 1000000}; /* 1 ms */
		thrd_sleep(&sl, NULL);
	}

	return 0;
}

int main(void)
{
	medut_config_t cfg;
	medut_config_init(&cfg);
	cfg.event_cb  = event_handler;
	cfg.log_level = -1; /* suppress all */

	int err = medut_init(&cfg);
	if (err) {
		fprintf(stderr, "medut_init failed: %d\n", err);
		return 1;
	}

	medut_account_config_t acfg = {
		.uri      = "sip:test@127.0.0.1",
		.password = "test",
	};
	err = medut_account_create(&acfg, &g_acct);
	if (err) {
		fprintf(stderr, "account_create failed: %d\n", err);
		medut_shutdown();
		return 1;
	}

	/* Spawn threads */
	thrd_t threads[NTHREADS];
	for (int i = 0; i < NTHREADS; i++)
		thrd_create(&threads[i], worker, NULL);

	/* Wait */
	for (int i = 0; i < NTHREADS; i++)
		thrd_join(threads[i], NULL);

	medut_shutdown();
	printf("Thread safety test passed\n");
	return 0;
}
