/**
 * @file log.c  engine log + re dbg → MEDUT_EV_LOG bridge
 */

#include "bsdk.h"
/* re_dbg.h wants a module name and level; only dbg_handler_set() is used. */
#define DEBUG_MODULE "medut"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

/*
 * Engine log levels:   0=debug  1=info  2=warning  3=error
 * SDK    log_level:    3=debug        2=info        1=warn        0=err
 *
 * An engine message at level L is shown when L >= (3 - sdk_log_level).
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

void bsdk_log_deliver(uint32_t level, const char *msg)
{
	if ((int)level < (3 - g_bsdk.cfg.log_level))
		return;

	queue_log_event(msg, 0);
}

/*
 * re library dbg levels: DBG_ERR=3, DBG_WARNING=4, DBG_NOTICE=5,
 *                        DBG_INFO=6, DBG_DEBUG=7
 * Map to the engine levels for the same SDK filter:
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

int bsdk_log_init(void)
{
	/* Route all engine log output through bsdk_log_deliver() only; the
	 * engine stops printing on its own (see mdt_log_init()).
	 *
	 * dbg_handler_set intercepts re library messages (websock:, tls:,
	 * sip:, ...) that use DEBUG_WARNING/re_printf.  Once a handler is
	 * registered, re's dbg_vprintf skips its own stderr write, so
	 * those messages no longer appear in the terminal. */
	mdt_log_init();
	dbg_handler_set(re_dbg_handler, NULL);

	/* The host application's stdout and stderr are its own: nothing here
	 * redirects them.  The re library paths that used to print there
	 * directly (sip/transp.c, mqueue.c) go through dbg as well — libre
	 * patch Z in cmake/patch-re-sources.cmake. */
	return 0;
}

void bsdk_log_close(void)
{
	dbg_handler_set(NULL, NULL);
	mdt_log_close();
}
