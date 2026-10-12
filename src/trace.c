/**
 * @file trace.c  SIP message trace hook → MEDUT_EV_SIP_TRACE
 *
 * Registers a sip_trace_h with the libre SIP stack via uag_sip().
 * Each raw SIP message is forwarded to:
 *   1. The consumer as MEDUT_EV_SIP_TRACE (via event queue)
 *   2. bsdk_pcap_write_sip() if pcap recording is active
 *
 * The handler fires on the re_main thread; we enqueue and return immediately.
 */

#include "medut_internal.h"

/* ── Transport name helper ───────────────────────────────────────────────── */

static const char *transp_name(enum sip_transp tp)
{
	switch (tp) {
	case SIP_TRANSP_UDP: return "UDP";
	case SIP_TRANSP_TCP: return "TCP";
	case SIP_TRANSP_TLS: return "TLS";
	case SIP_TRANSP_WS:  return "WS";
	case SIP_TRANSP_WSS: return "WSS";
	default:             return "?";
	}
}

/* ── SIP trace callback (re_main thread) ─────────────────────────────────── */

static void sip_trace_handler(bool tx, enum sip_transp tp,
                               const struct sa *src, const struct sa *dst,
                               const uint8_t *pkt, size_t len, void *arg)
{
	(void)arg;

	/* Write to pcap if enabled */
	if (g_bsdk.pcap_file)
		bsdk_pcap_write_sip((const char *)pkt, len,
		                    tx ? NULL : src,
		                    tx ? dst  : NULL,
		                    tp == SIP_TRANSP_UDP);

	if (!g_bsdk.cfg.trace_sip || !g_bsdk.cfg.event_cb)
		return;

	/* Build remote address string: "host:port" */
	char remote[64] = {0};
	const struct sa *peer = tx ? dst : src;
	if (peer)
		re_snprintf(remote, sizeof(remote), "%H", sa_print_addr, peer);

	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type = MEDUT_EV_SIP_TRACE;
	qev->ev.u.sip_trace.timestamp_us = tmr_jiffies() * 1000ULL;

	/* Pack strings into buf: transport\0remote_addr\0raw_message\0 */
	size_t off = 0;

	const char *tn = transp_name(tp);
	str_ncpy(qev->buf + off, tn, sizeof(qev->buf) - off);
	qev->ev.u.sip_trace.transport = qev->buf + off;
	off += strlen(tn) + 1;

	str_ncpy(qev->buf + off, remote, sizeof(qev->buf) - off);
	qev->ev.u.sip_trace.remote_addr = qev->buf + off;
	off += strlen(remote) + 1;

	size_t copy_len = len;
	if (copy_len > sizeof(qev->buf) - off - 1)
		copy_len = sizeof(qev->buf) - off - 1;
	memcpy(qev->buf + off, pkt, copy_len);
	qev->buf[off + copy_len] = '\0';
	qev->ev.u.sip_trace.raw_message = qev->buf + off;

	qev->ev.u.sip_trace.dir = tx ? MEDUT_MEDIA_DIR_TX : MEDUT_MEDIA_DIR_RX;

	/* Hand-rolled rather than bsdk_event_post_qev() so a dropped trace is
	 * silent: tracing is debug output, and warning once per message on a
	 * backed-up queue would bury the events the app actually needs.  The
	 * ev_queue_len++ below is what keeps that safe — omitting it underflows
	 * the counter and wedges the whole queue (see event.c). */
	mtx_lock(&g_bsdk.ev_lock);
	if (g_bsdk.ev_queue_len >= g_bsdk.ev_queue_max) {
		mtx_unlock(&g_bsdk.ev_lock);
		mem_deref(qev);
		return;
	}
	list_append(&g_bsdk.ev_queue, &qev->le, qev);
	g_bsdk.ev_queue_len++;
	cnd_signal(&g_bsdk.ev_cond);
	mtx_unlock(&g_bsdk.ev_lock);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bsdk_trace_init(void)
{
	if (!g_bsdk.cfg.trace_sip && !g_bsdk.cfg.pcap_path)
		return 0;

	struct sip *sip = uag_sip();
	if (!sip)
		return ENOENT;

	sip_set_trace_handler(sip, sip_trace_handler);
	return 0;
}

void bsdk_trace_close(void)
{
	struct sip *sip = uag_sip();
	if (sip)
		sip_set_trace_handler(sip, NULL);
}
