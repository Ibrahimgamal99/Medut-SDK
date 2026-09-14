/**
 * @file dispatch.c  Consumer thread → re_main bridge
 *
 * All state-mutating public API calls go through bsdk_dispatch() or
 * bsdk_dispatch_sync(). Both post the work to a libre mqueue whose handler
 * runs on the re_main event loop, so SIP state is only ever mutated from
 * re_main.
 *
 * Why an mqueue and not re_thread_async_main(): the latter looks like a
 * "run this on re_main" primitive, and with work=NULL it is one — but the
 * item still travels through libre's async WORKER POOL to get there (see
 * re's src/async/async.c: re_async() appends to `workl`, a worker_thread
 * dequeues it, and only then mqueue_push()es it to the loop).  That pool is
 * RE_THREAD_WORKERS = 4 threads, and it is the same pool libre uses for
 * blocking getaddrinfo() (dns/client.c, forced on for Android by core.c) and
 * for blocking mDNS ".local" ICE candidate resolution (ice/icesdp.c, ~5 s
 * each).  One SIP hostname costs two slots (A + AAAA in flight together), so
 * two accounts registering at once filled the pool, and every API call
 * behind them — register(), answer(), hangup(), the lot — queued until a
 * resolve returned.  Measured: 4 busy workers held echosdk_account_register()
 * off the wire for 3.0 s; 8 for 6.0 s.  The sync variants block the caller,
 * which for the Flutter binding is the UI isolate.
 *
 * mqueue_push() is a single pipe write of sizeof(struct msg) — well under
 * PIPE_BUF, so it is atomic and safe from any thread — and the handler runs
 * on re_main with no worker in between.  It is the primitive re_loop.c
 * already relies on to wake the loop for shutdown.
 *
 * bsdk_dispatch()      — fire-and-forget; returns immediately.
 * bsdk_dispatch_sync() — posts and blocks until work completes; runs the
 *                        function inline when already on the re thread
 *                        (blocking there would deadlock the loop).
 *
 * Lifetime: bsdk_dispatch_init() runs before the re thread starts and
 * bsdk_dispatch_close() after it has joined (both from re_loop.c).  A call
 * that arrives once the queue is gone gets ECHOSDK_ERR_STATE instead of a
 * push into a pipe nobody drains — which for the sync variant would have
 * been a cnd_wait() that never returns.
 */

#include "echosdk_internal.h"

/* mqueue message ids: which struct `data` points at. */
enum {
	DISPATCH_ASYNC = 1,
	DISPATCH_SYNC  = 2,
};

/* ── Async (fire-and-forget) ─────────────────────────────────────────────── */

typedef struct {
	bsdk_main_fn fn;
	void        *arg;
} dispatch_ctx_t;

/* ── Synchronous ─────────────────────────────────────────────────────────── */

typedef struct {
	bsdk_main_fn fn;
	void        *arg;
	mtx_t        lock;
	cnd_t        done;
	bool         finished;
} sync_ctx_t;

/* mqueue_h — runs on the re_main thread */
static void dispatch_mq_handler(int id, void *data, void *arg)
{
	(void)arg;

	switch (id) {

	case DISPATCH_ASYNC: {
		dispatch_ctx_t *ctx = data;
		ctx->fn(ctx->arg);
		mem_deref(ctx);
		break;
	}

	case DISPATCH_SYNC: {
		sync_ctx_t *ctx = data;
		ctx->fn(ctx->arg);
		mtx_lock(&ctx->lock);
		ctx->finished = true;
		cnd_signal(&ctx->done);
		mtx_unlock(&ctx->lock);
		/* do NOT mem_deref — caller owns ctx on its stack */
		break;
	}

	default:
		warning("EchoSDK/dispatch: unknown mqueue id %d\n", id);
		break;
	}
}

int bsdk_dispatch_init(void)
{
	if (g_bsdk.dispatch_mq)
		return 0;
	return mqueue_alloc(&g_bsdk.dispatch_mq, dispatch_mq_handler, NULL);
}

void bsdk_dispatch_close(void)
{
	g_bsdk.dispatch_mq = mem_deref(g_bsdk.dispatch_mq);
}

int bsdk_dispatch(bsdk_main_fn fn, void *arg)
{
	dispatch_ctx_t *ctx;
	int err;

	if (!g_bsdk.initialized || !g_bsdk.dispatch_mq)
		return ECHOSDK_ERR_STATE;

	ctx = mem_alloc(sizeof(*ctx), NULL);
	if (!ctx)
		return ECHOSDK_ERR_NOMEM;

	ctx->fn  = fn;
	ctx->arg = arg;

	err = mqueue_push(g_bsdk.dispatch_mq, DISPATCH_ASYNC, ctx);
	if (err)
		mem_deref(ctx);
	return err;
}

int bsdk_dispatch_sync(bsdk_main_fn fn, void *arg)
{
	sync_ctx_t ctx;
	int err;

	if (!g_bsdk.initialized)
		return ECHOSDK_ERR_STATE;

	/* Already on the re-loop thread (e.g. called from a timer or netmon
	 * handler): run inline — blocking here would deadlock the loop that
	 * must execute the callback.  Compare against our own spawned thread:
	 * re_thread_check() is NOT usable here because re->tid is stamped by
	 * libre_init(), which runs on the app thread in echosdk_init(). */
	if (g_bsdk.re_thread_running &&
	    thrd_equal(g_bsdk.re_thread, thrd_current())) {
		fn(arg);
		return 0;
	}

	if (!g_bsdk.dispatch_mq)
		return ECHOSDK_ERR_STATE;

	ctx.fn       = fn;
	ctx.arg      = arg;
	ctx.finished = false;
	mtx_init(&ctx.lock, mtx_plain);
	cnd_init(&ctx.done);

	err = mqueue_push(g_bsdk.dispatch_mq, DISPATCH_SYNC, &ctx);
	if (err)
		goto out;

	mtx_lock(&ctx.lock);
	while (!ctx.finished)
		cnd_wait(&ctx.done, &ctx.lock);
	mtx_unlock(&ctx.lock);

out:
	cnd_destroy(&ctx.done);
	mtx_destroy(&ctx.lock);
	return err;
}
