/**
 * @file re_loop.c  re_main thread lifecycle
 *
 * Runs the re event loop on a dedicated thread. After libre_init() the global
 * re context (re_global) is set. The re_main thread uses it via the automatic
 * fallback in re_get() — no explicit re_thread_attach() needed.
 *
 * Stopping: re_cancel() alone cannot interrupt a blocking select() call.
 * We use an mqueue (whose pipe is registered with fd_listen) to wake the
 * select, then call re_cancel() so the loop exits on the next iteration.
 */

#include "echosdk_internal.h"

static void wakeup_handler(int id, void *data, void *arg)
{
	(void)id; (void)data; (void)arg;
	re_cancel();
}

static int re_main_thread_fn(void *arg)
{
	(void)arg;
	re_main(NULL);  /* blocks until re_cancel() */
	return 0;
}

int bsdk_re_loop_start(void)
{
	/* Allocate the wakeup mqueue BEFORE starting the thread.
	 * mqueue_alloc registers a pipe with fd_listen on re_global so that
	 * mqueue_push() can interrupt a blocking select() call. */
	int err = mqueue_alloc(&g_bsdk.re_wakeup_mq, wakeup_handler, NULL);
	if (err)
		return err;

	/* Same reasoning for the dispatch queue: its pipe has to be in
	 * re_global's fd table before the loop that polls it starts. */
	err = bsdk_dispatch_init();
	if (err) {
		g_bsdk.re_wakeup_mq = mem_deref(g_bsdk.re_wakeup_mq);
		return err;
	}

	int rc = thrd_create(&g_bsdk.re_thread, re_main_thread_fn, NULL);
	if (rc != thrd_success) {
		bsdk_dispatch_close();
		g_bsdk.re_wakeup_mq = mem_deref(g_bsdk.re_wakeup_mq);
		return ENOMEM;
	}
	g_bsdk.re_thread_running = true;
	return 0;
}

void bsdk_re_loop_stop(void)
{
	if (!g_bsdk.re_thread_running) {
		/* Never started (init unwound early) — nothing to join, but a
		 * queue allocated by a start that then failed must not leak. */
		bsdk_dispatch_close();
		return;
	}

	/* Push to the wakeup mqueue — this writes to the pipe, which wakes up
	 * the blocking select() in the re_main loop.  The wakeup_handler then
	 * calls re_cancel() from within the re thread so polling=false is seen
	 * on the very next iteration. */
	mqueue_push(g_bsdk.re_wakeup_mq, 0, NULL);

	thrd_join(g_bsdk.re_thread, NULL);
	/* After the join: nothing can be mid-handler on re_main any more, and
	 * a dispatch that races this teardown now fails with ERR_STATE rather
	 * than writing into a pipe with no reader. */
	bsdk_dispatch_close();
	g_bsdk.re_wakeup_mq = mem_deref(g_bsdk.re_wakeup_mq);
	g_bsdk.re_thread_running = false;
}
