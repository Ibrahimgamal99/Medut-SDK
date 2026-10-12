/**
 * @file log.c  baresip log + re dbg → MEDUT_EV_LOG bridge
 */

#include "medut_internal.h"
#include <re_dbg.h>
#include <stdio.h>

/*
 * Baresip log levels:  LEVEL_DEBUG=0  LEVEL_INFO=1  LEVEL_WARN=2  LEVEL_ERROR=3
 * SDK    log_level:    3=debug        2=info        1=warn        0=err
 *
 * A baresip message at level L is shown when L >= (3 - sdk_log_level).
 * Drop condition: L < (3 - sdk_log_level).
 */
static void queue_log_event(const char *msg, size_t len)
{
	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	if (len > 0 && len < sizeof(qev->buf))
		memcpy(qev->buf, msg, len);
	else
		str_ncpy(qev->buf, msg, sizeof(qev->buf));
	qev->ev.type = MEDUT_EV_LOG;
	qev->ev.u.log.message = qev->buf;

	/* Deliberately not bsdk_event_post_qev(): that warns on a full queue,
	 * and a warning from inside the log handler re-enters this function.
	 * The bookkeeping below must therefore stay in sync with it by hand —
	 * ev_queue_len++ on the enqueue is not optional (see event.c). */
	mtx_lock(&g_bsdk.ev_lock);
	if (g_bsdk.ev_queue_len >= g_bsdk.ev_queue_max) {
		mtx_unlock(&g_bsdk.ev_lock);
		mem_deref(qev);
		return; /* silently drop — no warning to avoid re-entrant logging */
	}
	list_append(&g_bsdk.ev_queue, &qev->le, qev);
	g_bsdk.ev_queue_len++;
	cnd_signal(&g_bsdk.ev_cond);
	mtx_unlock(&g_bsdk.ev_lock);
}

static void log_handler(uint32_t level, const char *msg)
{
	if ((int)level < (3 - g_bsdk.cfg.log_level))
		return;

	queue_log_event(msg, 0);
}

/*
 * re library dbg levels: DBG_ERR=3, DBG_WARNING=4, DBG_NOTICE=5,
 *                        DBG_INFO=6, DBG_DEBUG=7
 * Map to baresip LEVEL_* for the same SDK filter:
 *   re <= 3 → LEVEL_ERROR (3)
 *   re == 4 → LEVEL_WARN  (2)
 *   re >= 5 → LEVEL_INFO  (1)  (DEBUG passes only at sdk log_level 3)
 */
static void re_dbg_handler(int re_level, const char *p, size_t len, void *arg)
{
	(void)arg;
	uint32_t bslevel;
	if (re_level <= 3)      bslevel = 3;
	else if (re_level == 4) bslevel = 2;
	else                    bslevel = 1;

	if ((int)bslevel < (3 - g_bsdk.cfg.log_level))
		return;

	queue_log_event(p, len);
}

static struct log s_logger = {
	.h = log_handler,
};

int bsdk_log_init(void)
{
	/* Route all baresip log output through our handler only.
	 * log_enable_stdout(false) stops vlog() from printing directly to
	 * stdout — without this, every warning() call bypasses the handler
	 * and prints to the terminal regardless of what the app does.
	 * log_level_set(LEVEL_DEBUG) lets all messages reach the handler;
	 * we apply the SDK log_level filter above.
	 *
	 * dbg_handler_set intercepts re library messages (websock:, tls:,
	 * sip:, ...) that use DEBUG_WARNING/re_printf.  Once a handler is
	 * registered, re's dbg_vprintf skips its own stderr write, so
	 * those messages no longer appear in the terminal. */
	log_enable_stdout(false);
	log_level_set(LEVEL_DEBUG);
	log_register_handler(&s_logger);
	dbg_handler_set(re_dbg_handler, NULL);

	/* Some re library paths (e.g. sip/transp.c) use re_fprintf(stderr,...)
	 * directly, bypassing dbg_handler.  Redirect stderr to null so
	 * these don't leak to the terminal; all real diagnostics go through
	 * the SDK event system. */
#ifdef _WIN32
	freopen("NUL", "w", stderr);
#else
	freopen("/dev/null", "w", stderr);
#endif
	return 0;
}

void bsdk_log_close(void)
{
	dbg_handler_set(NULL, NULL);
	log_unregister_handler(&s_logger);
	log_enable_stdout(true);
}
