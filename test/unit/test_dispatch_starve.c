/**
 * @file test_dispatch_starve.c  bsdk_dispatch() must not queue behind libre's
 *                               async worker pool
 *
 * Regression test for the head-of-line block that made registration (and
 * every other API call) wait on unrelated blocking DNS work:
 * bsdk_dispatch()/bsdk_dispatch_sync() used re_thread_async_main(), and even
 * with work=NULL an item transits one of RE_THREAD_WORKERS (= 4) worker
 * threads before reaching re_main.  libre runs blocking getaddrinfo() and
 * mDNS ".local" ICE candidate resolution on the same pool, so four resolves
 * in flight — two accounts registering on Android is enough — held every
 * dispatch until one returned.  Measured against the SDK: 3.0 s with 4 busy
 * workers, 6.0 s with 8.
 *
 * The test occupies all four workers with sleeping jobs and then times a
 * dispatch.  With the mqueue-based dispatch.c the work lands on re_main in
 * milliseconds; with the old one it lands when the first sleeper finishes.
 *
 * Links src/dispatch.c and src/re_loop.c against libre only.  Both read
 * g_bsdk (defined here, as test_ice_deadline.c does) and nothing else from
 * the SDK.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <re.h>
#include <baresip.h>
#include "../../src/echosdk_internal.h"

struct bsdk_ctx g_bsdk;

/* dispatch.c's unknown-id branch calls baresip's warning(); core.c and the
 * log module are not linked, so provide the symbol. */
void _warning(bool safe, const char *fmt, ...)
{
	va_list ap;
	(void)safe;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

#define BLOCK_MS   1200   /* how long each worker is held */
#define WORKERS    4      /* RE_THREAD_WORKERS in re/src/main/main.c */
#define BUDGET_MS  300    /* a dispatch slower than this is queued behind them */

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); \
	} } while (0)

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* Stands in for async_getaddrinfo(): blocks a worker for BLOCK_MS. */
static int blocker_work(void *arg)
{
	(void)arg;
	sleep_ms(BLOCK_MS);
	return 0;
}

static void blocker_done(int err, void *arg)
{
	(void)err; (void)arg;
}

/* What the dispatch delivers to re_main. */
static void mark_fn(void *arg)
{
	*(volatile bool *)arg = true;
}

/* For the ordering test: records the order in which it ran. */
struct step { volatile int *slot; };
static volatile int step_counter;

static void step_fn(void *arg)
{
	struct step *st = arg;
	*st->slot = step_counter++;
}

/* Wait for a flag set on re_main; returns elapsed ms or -1 on timeout. */
static double wait_flag(volatile bool *flag, double t0, unsigned timeout_ms)
{
	while (!*flag) {
		if (now_ms() - t0 > timeout_ms)
			return -1;
		sleep_ms(2);
	}
	return now_ms() - t0;
}

static void occupy_workers(void)
{
	for (int i = 0; i < WORKERS; i++) {
		int err = re_thread_async_main(blocker_work, blocker_done, NULL);
		CHECK(err == 0, "re_thread_async_main: %d", err);
	}
	/* Let the pool pick them up so they are *running*, not queued. */
	sleep_ms(50);
}

static void test_async_dispatch_not_starved(void)
{
	volatile bool hit = false;
	double t0, dt;

	printf("async dispatch with all workers busy\n");

	occupy_workers();

	t0 = now_ms();
	CHECK(bsdk_dispatch(mark_fn, (void *)&hit) == 0, "bsdk_dispatch failed");
	dt = wait_flag(&hit, t0, BLOCK_MS + 500);

	CHECK(dt >= 0, "dispatch never reached re_main");
	CHECK(dt >= 0 && dt < BUDGET_MS,
	      "dispatch took %.0f ms — queued behind the worker pool", dt);
	if (dt >= 0)
		printf("  reached re_main in %.1f ms\n", dt);
}

static void test_sync_dispatch_not_starved(void)
{
	volatile bool hit = false;
	double t0, dt;

	printf("sync dispatch with all workers busy\n");

	/* The previous test's sleepers are still in flight (BLOCK_MS > the
	 * test's own duration), but top up in case timing was generous. */
	occupy_workers();

	t0 = now_ms();
	CHECK(bsdk_dispatch_sync(mark_fn, (void *)&hit) == 0,
	      "bsdk_dispatch_sync failed");
	dt = now_ms() - t0;

	CHECK(hit, "sync dispatch returned without running the function");
	CHECK(dt < BUDGET_MS,
	      "sync dispatch blocked the caller %.0f ms — queued behind the "
	      "worker pool", dt);
	printf("  returned in %.1f ms\n", dt);
}

static void test_fifo_order(void)
{
	/* Two async posts must run in order — the pipe is FIFO and the loop
	 * drains it in order; a regression to a pool would allow reordering. */
	static volatile int seq[2];
	struct step s0 = { &seq[0] }, s1 = { &seq[1] };
	volatile bool done = false;

	printf("dispatch preserves order\n");

	step_counter = 0;
	seq[0] = seq[1] = -1;

	CHECK(bsdk_dispatch(step_fn, &s0) == 0, "post 0");
	CHECK(bsdk_dispatch(step_fn, &s1) == 0, "post 1");
	CHECK(bsdk_dispatch(mark_fn, (void *)&done) == 0, "post done");
	CHECK(wait_flag(&done, now_ms(), 1000) >= 0, "order test timed out");

	CHECK(seq[0] == 0 && seq[1] == 1,
	      "ran out of order: seq = {%d, %d}", seq[0], seq[1]);
}

static void test_dispatch_after_stop_fails_fast(void)
{
	volatile bool hit = false;
	double t0, dt;

	printf("dispatch after the loop stopped fails instead of hanging\n");

	bsdk_re_loop_stop();

	t0 = now_ms();
	CHECK(bsdk_dispatch(mark_fn, (void *)&hit) == ECHOSDK_ERR_STATE,
	      "async dispatch after stop did not return ERR_STATE");
	CHECK(bsdk_dispatch_sync(mark_fn, (void *)&hit) == ECHOSDK_ERR_STATE,
	      "sync dispatch after stop did not return ERR_STATE");
	dt = now_ms() - t0;

	CHECK(!hit, "function ran with no loop to run it on");
	CHECK(dt < 100, "post-stop dispatch took %.0f ms", dt);
}

int main(void)
{
	int err;

	err = libre_init();
	if (err) {
		printf("libre_init: %d\n", err);
		return 1;
	}

	memset(&g_bsdk, 0, sizeof(g_bsdk));

	err = bsdk_re_loop_start();
	if (err) {
		printf("bsdk_re_loop_start: %d\n", err);
		return 1;
	}
	g_bsdk.initialized = true;

	test_async_dispatch_not_starved();
	test_sync_dispatch_not_starved();
	test_fifo_order();
	test_dispatch_after_stop_fails_fast();

	/* The sleepers are still running; libre_close() joins the pool. */
	libre_close();

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_dispatch_starve: all checks passed\n");
	return 0;
}
