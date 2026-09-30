/**
 * @file core.c  Singleton lifecycle — echosdk_init / echosdk_shutdown
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include "echosdk_internal.h"

/* ── Global singleton ────────────────────────────────────────────────────── */

struct bsdk_ctx g_bsdk;

#define ECHOSDK_EV_QUEUE_MAX 4096

/* Cached result of getenv("ECHOSDK_DEBUG_INIT"); set on first call. */
int bsdk_trace_enabled(void)
{
	static int cached = -1;
	if (cached == -1) {
		const char *v = getenv("ECHOSDK_DEBUG_INIT");
		cached = (v && *v && *v != '0') ? 1 : 0;
	}
	return cached;
}

/* ── Deep-copy helpers ───────────────────────────────────────────────────── */

char *bsdk_strdup(const char *s)
{
	if (!s) return NULL;
	size_t len = strlen(s);
	char *dup = mem_alloc(len + 1, NULL);
	if (dup) {
		memcpy(dup, s, len + 1);
	}
	return dup;
}

static char *bsdk_strdup_arr_elem(char **arr, const char *s)
{
	(void)arr;
	return bsdk_strdup(s);
}

static char **bsdk_strdup_strv(const char * const *src)
{
	if (!src) return NULL;
	size_t count = 0;
	while (src[count]) count++;
	char **dst = mem_alloc((count + 1) * sizeof(char *), NULL);
	if (!dst) return NULL;
	for (size_t i = 0; i < count; i++) {
		dst[i] = bsdk_strdup(src[i]);
		if (!dst[i]) {
			for (size_t j = 0; j < i; j++) mem_deref(dst[j]);
			mem_deref(dst);
			return NULL;
		}
	}
	dst[count] = NULL;
	return dst;
}

static void bsdk_free_strv(char **arr)
{
	if (!arr) return;
	for (size_t i = 0; arr[i]; i++)
		mem_deref(arr[i]);
	mem_deref(arr);
}

void bsdk_cfg_deep_copy(echosdk_config_t *dst, const echosdk_config_t *src,
                         struct bsdk_ctx *ctx)
{
	memcpy(dst, src, sizeof(*src));

	ctx->cfg_local_ip         = bsdk_strdup(src->local_ip);
	ctx->cfg_sip_domain       = bsdk_strdup(src->sip_domain);
	ctx->cfg_server_url       = bsdk_strdup(src->server_url);
	ctx->cfg_server_host      = bsdk_strdup(src->server_host);
	ctx->cfg_outbound_proxy   = bsdk_strdup(src->outbound_proxy);
	ctx->cfg_ca_cert_path     = bsdk_strdup(src->ca_cert_path);
	ctx->cfg_client_cert      = bsdk_strdup(src->client_cert);
	ctx->cfg_client_key       = bsdk_strdup(src->client_key);
	ctx->cfg_sni_hostname     = bsdk_strdup(src->sni_hostname);
	ctx->cfg_user_agent       = bsdk_strdup(src->user_agent);
	ctx->cfg_ws_origin        = bsdk_strdup(src->ws_origin);
	ctx->cfg_ws_extra_headers = bsdk_strdup_strv(src->ws_extra_headers);
	ctx->cfg_stun_server      = bsdk_strdup(src->stun_server);
	ctx->cfg_turn_server      = bsdk_strdup(src->turn_server);
	ctx->cfg_turn_user        = bsdk_strdup(src->turn_user);
	ctx->cfg_turn_pass        = bsdk_strdup(src->turn_pass);
	ctx->cfg_pcap_path        = bsdk_strdup(src->pcap_path);
	ctx->cfg_tmp_dir          = bsdk_strdup(src->tmp_dir);

	dst->local_ip         = ctx->cfg_local_ip;
	dst->sip_domain       = ctx->cfg_sip_domain;
	dst->server_url       = ctx->cfg_server_url;
	dst->server_host      = ctx->cfg_server_host;
	dst->outbound_proxy   = ctx->cfg_outbound_proxy;
	dst->ca_cert_path     = ctx->cfg_ca_cert_path;
	dst->client_cert      = ctx->cfg_client_cert;
	dst->client_key       = ctx->cfg_client_key;
	dst->sni_hostname     = ctx->cfg_sni_hostname;
	dst->user_agent       = ctx->cfg_user_agent;
	dst->ws_origin        = ctx->cfg_ws_origin;
	dst->ws_extra_headers = (const char **)ctx->cfg_ws_extra_headers;
	dst->stun_server      = ctx->cfg_stun_server;
	dst->turn_server      = ctx->cfg_turn_server;
	dst->turn_user        = ctx->cfg_turn_user;
	dst->turn_pass        = ctx->cfg_turn_pass;
	dst->pcap_path        = ctx->cfg_pcap_path;
	dst->tmp_dir          = ctx->cfg_tmp_dir;
}

void bsdk_cfg_deep_free(struct bsdk_ctx *ctx)
{
	mem_deref(ctx->cfg_local_ip);         ctx->cfg_local_ip = NULL;
	mem_deref(ctx->cfg_sip_domain);       ctx->cfg_sip_domain = NULL;
	mem_deref(ctx->cfg_server_url);       ctx->cfg_server_url = NULL;
	mem_deref(ctx->cfg_server_host);      ctx->cfg_server_host = NULL;
	mem_deref(ctx->cfg_outbound_proxy);   ctx->cfg_outbound_proxy = NULL;
	mem_deref(ctx->cfg_ca_cert_path);     ctx->cfg_ca_cert_path = NULL;
	mem_deref(ctx->cfg_client_cert);      ctx->cfg_client_cert = NULL;
	mem_deref(ctx->cfg_client_key);       ctx->cfg_client_key = NULL;
	mem_deref(ctx->cfg_sni_hostname);     ctx->cfg_sni_hostname = NULL;
	mem_deref(ctx->cfg_user_agent);       ctx->cfg_user_agent = NULL;
	mem_deref(ctx->cfg_ws_origin);        ctx->cfg_ws_origin = NULL;
	bsdk_free_strv(ctx->cfg_ws_extra_headers); ctx->cfg_ws_extra_headers = NULL;
	mem_deref(ctx->cfg_stun_server);      ctx->cfg_stun_server = NULL;
	mem_deref(ctx->cfg_turn_server);      ctx->cfg_turn_server = NULL;
	mem_deref(ctx->cfg_turn_user);        ctx->cfg_turn_user = NULL;
	mem_deref(ctx->cfg_turn_pass);        ctx->cfg_turn_pass = NULL;
	mem_deref(ctx->cfg_pcap_path);        ctx->cfg_pcap_path = NULL;
	mem_deref(ctx->cfg_tmp_dir);          ctx->cfg_tmp_dir   = NULL;
}

/* ── echosdk_config_init ─────────────────────────────────────────────────── */

void echosdk_config_init(echosdk_config_t *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->version     = ECHOSDK_CONFIG_VERSION;
	cfg->struct_size = sizeof(echosdk_config_t);

	cfg->transport             = ECHOSDK_TRANSPORT_UDP;
	cfg->verify_server         = true;
	cfg->reg_expires           = 3600;
	cfg->reg_refresh_pct       = 75;
	cfg->keepalive_interval    = 30000;
	cfg->reg_retry_initial_ms  = 2000;
	cfg->reg_retry_max_ms      = 300000;
	cfg->reg_retry_backoff     = 2.0f;
	cfg->reg_retry_jitter      = 0.2f;
	cfg->sip_t1_ms             = 500;
	cfg->sip_t2_ms             = 4000;
	cfg->sip_timer_b_ms        = 32000;
	cfg->ice_gathering_timeout_ms = 2000;
	cfg->sip_timer_f_ms        = 32000;
	cfg->session_timer_enabled = true;
	cfg->session_expires_s     = 1800;
	cfg->session_min_se_s      = 90;
	cfg->mos_method            = ECHOSDK_MOS_EMODEL;
	cfg->log_level             = 1;
	cfg->rtcp_mux              = true;
	cfg->aec_mode              = ECHOSDK_AEC_SUPPRESSOR;
	cfg->aec_suppression_level = 1.0f;
	/* Activate the platform audio session at init (iOS only). CallKit apps
	 * must set this to false — see the field docs. */
	cfg->platform_audio_activate = true;
	cfg->opus.complexity       = -1;
	/* Where Opus speech quality levels off: PESQ-WB 4.53, against 4.56 at
	 * the 50-99 kbps it picks by itself.  Also the adaptive ceiling
	 * (adapt_max_bitrate), so a call starts where it can return to, and a
	 * sudden bandwidth drop costs less before the first RTCP report. */
	cfg->opus.bitrate          = 32000;
	/* Ask the peer for in-band FEC.  Our own redundancy then follows the
	 * loss it reports (adapt.c) while opus_expected_loss_pct stays 0. */
	cfg->opus.fec              = true;
	/* mic_gain_db and speaker_gain_db default to 0.0f (unity) from memset */

	/* Degraded-link handling.  Stats polling is on by default because
	 * everything below reads from it: with stats_interval_ms at 0 baresip
	 * does not even accumulate RTCP, so quality alerts, media-stall
	 * detection and bitrate adaptation all silently do nothing.  The three
	 * alert thresholds are set to the values the field docs recommend.
	 *
	 * rtp_timeout_s stays 0 — ending a call is destructive and belongs to
	 * the app.  media_stall_ms gives the same information without it. */
	cfg->stats_interval_ms     = 2000;
	cfg->mos_alert_threshold   = 3.5f;
	cfg->loss_alert_threshold  = 5.0f;
	cfg->jitter_alert_threshold = 40.0f;
	cfg->media_stall_ms        = 4000;
	cfg->keepalive_reregister  = true;
	cfg->dns_srv_failover      = true;
	cfg->adaptive_bitrate      = true;
	cfg->adapt_min_bitrate     = 12000;
	cfg->adapt_max_bitrate     = 32000;
	cfg->adapt_loss_down_pct   = 5.0f;
	cfg->adapt_loss_up_pct     = 1.0f;
	cfg->adapt_recover_ticks   = 5;
	/* rtp_timeout_s, opus_expected_loss_pct (= switched with the link) and
	 * net_ice_handover (= BEST_EFFORT) all default to 0/false from memset */

	/* Network handover — poll as a safety net on platforms with no OS
	 * connectivity callback; mobile apps should set this to 0 and drive
	 * echosdk_network_changed() from ConnectivityManager / NWPathMonitor. */
	cfg->net_monitor_interval_s = 10;
	cfg->net_settle_ms          = 1500;
	cfg->net_reinvite_calls     = true;
	cfg->net_verify_ms          = 4000;
	cfg->net_max_attempts       = 6;
	/* net_hangup_on_migration_failure defaults to false from memset */
}

/* ── Platform temp-dir helper ────────────────────────────────────────────── */

static void bsdk_resolve_tmpdir(const char *override, char *buf, size_t sz)
{
	if (override && *override) {
		str_ncpy(buf, override, sz);
		return;
	}
#if defined(_WIN32)
	DWORD n = GetTempPath((DWORD)sz, buf);
	if (n > 0 && n < (DWORD)sz)
		return;
	str_ncpy(buf, "C:\\Temp", sz);
#else
	/* $TMPDIR is set by iOS for the app sandbox and may be set on Android */
	const char *t = getenv("TMPDIR");
	str_ncpy(buf, (t && *t) ? t : "/tmp", sz);
#endif
}

/* ── echosdk_init ────────────────────────────────────────────────────────── */

int echosdk_init(const echosdk_config_t *cfg)
{
	int err;

	if (!cfg || !cfg->event_cb)
		return ECHOSDK_ERR_INVAL;
	if (cfg->version != ECHOSDK_CONFIG_VERSION)
		return ECHOSDK_ERR_INVAL;
	if (cfg->struct_size != sizeof(echosdk_config_t))
		return ECHOSDK_ERR_INVAL;
	if (cfg->audio_codec_count < 0 || cfg->audio_codec_count > 8)
		return ECHOSDK_ERR_INVAL;
	if (cfg->audio_codec_name_count < 0 || cfg->audio_codec_name_count > 8)
		return ECHOSDK_ERR_INVAL;

	static bool once = false;
	if (!once) {
		once = true;
		mtx_init(&g_bsdk.lock, mtx_plain);
	}

	mtx_lock(&g_bsdk.lock);

	if (g_bsdk.initialized) {
		mtx_unlock(&g_bsdk.lock);
		return ECHOSDK_ERR_ALREADY;
	}

	BSDK_TRACE("[bsdk] step 1: deep_copy\n");
	bsdk_cfg_deep_copy(&g_bsdk.cfg, cfg, &g_bsdk);

	list_init(&g_bsdk.ev_queue);
	list_init(&g_bsdk.accounts);
	g_bsdk.ev_queue_max = ECHOSDK_EV_QUEUE_MAX;
	g_bsdk.ev_queue_len = 0;
	mtx_init(&g_bsdk.ev_lock, mtx_plain);
	cnd_init(&g_bsdk.ev_cond);
	cnd_init(&g_bsdk.ev_idle_cond);
	g_bsdk.ev_delivering = false;
	mtx_init(&g_bsdk.acct_lock, mtx_plain);
	mtx_init(&g_bsdk.pcap_lock, mtx_plain);
	{
		const uint64_t *p = (const uint64_t*)&g_bsdk.pcap_lock;
		BSDK_TRACE("[bsdk] pcap_lock init: addr=%p bytes=%016llx %016llx %016llx %016llx %016llx\n",
		       (void*)p, p[0], p[1], p[2], p[3], p[4]);
	}
	bsdk_call_global_init();

	BSDK_TRACE("[bsdk] step 2: log_init\n");
	err = bsdk_log_init();
	if (err)
		goto fail;

	BSDK_TRACE("[bsdk] step 3: libre_init\n");
	err = libre_init();
	if (err)
		goto fail;

	BSDK_TRACE("[bsdk] step 4: conf_path\n");
	/* Redirect baresip's config directory so it never finds or reads
	 * ~/.config/baresip/{config,accounts,contacts,...} from disk.
	 * All SDK configuration is driven exclusively through echosdk_config_t.
	 * Directory must exist: the uuid module (required for WSS/outbound) writes into it. */
	{
		char _tmpbase[512];
		char _confdir[640];
		bsdk_resolve_tmpdir(g_bsdk.cfg.tmp_dir, _tmpbase, sizeof(_tmpbase));
		(void)re_snprintf(_confdir, sizeof(_confdir), "%s/.echosdk", _tmpbase);
		(void)fs_mkdir(_confdir, 0700);
		conf_path_set(_confdir);

		/* No CAfile means an empty X509_STORE: libre never calls
		 * SSL_CTX_set_default_verify_paths(), so unless the app named
		 * a bundle every TLS/WSS handshake fails verification with
		 * "unable to get local issuer certificate" (surfacing as
		 * "Register: Protocol error [100]").  Fall back to whatever
		 * the platform can offer — see platform/<os>/ca_*.c. */
		if (!g_bsdk.cfg.ca_cert_path)
			g_bsdk.cfg.ca_cert_path =
				bsdk_platform_ca_bundle(_confdir);
	}
	conf_configure_buf((const uint8_t *)"#\n", 2);

#ifdef __ANDROID__
	/* Android has no /etc/resolv.conf and stopped exposing net.dns* in
	 * Oreo, so re's resolver comes up with zero nameservers and every
	 * dnsc_query() returns ENOTSUP — registration fails with "Operation
	 * not supported on transport endpoint [95]" before a packet is sent.
	 *
	 * Route A/AAAA through bionic's getaddrinfo() instead: it goes via
	 * netd, so it honours the per-network resolver, VPNs and Private DNS.
	 * re handles the rest — with getaddrinfo on and no nameservers,
	 * dnsc_getaddrinfo_only() is true and sip_request_send() resolves the
	 * host directly instead of trying an SRV lookup it cannot perform.
	 *
	 * Must precede baresip_init(), which is what allocates the resolver.
	 */
	conf_config()->net.use_getaddrinfo = true;
#endif

	/* Ignore v4/v6 link-local addresses (baresip defaults them on).
	 *
	 * A link-local address can bind a socket but cannot reach an off-link
	 * registrar, so it is useless to a registrar-based SIP client — and it
	 * is worse than useless on a phone. Android devices carry virtual
	 * interfaces (a Samsung handset has dummy0 with an fe80:: address) that
	 * appear and disappear independently of real connectivity. netmon.c
	 * reconciles baresip's address list against the kernel's every poll, so
	 * those addresses got added and removed in a loop: each flip looked like
	 * a network change, fired a handover, and reset the SIP transports and
	 * the audio path every few seconds. Mid-call that means silence.
	 *
	 * Must precede baresip_init(), which is what snapshots the interfaces.
	 */
	conf_config()->net.use_linklocal = false;

	BSDK_TRACE("[bsdk] step 5: baresip_init (cfg=%p)\n", (void*)conf_config());
#ifdef _WIN32
	{
		DWORD _seh = 0;
		PVOID _crash_addr = NULL;
		__try { err = baresip_init(conf_config()); }
		__except ((_crash_addr = ((EXCEPTION_POINTERS*)GetExceptionInformation())->ExceptionRecord->ExceptionAddress),
		          EXCEPTION_EXECUTE_HANDLER) {
			_seh = GetExceptionCode();
			err = -1;
		}
		if (_seh) {
			HMODULE _hm = GetModuleHandleA("echosdk.dll");
			BSDK_TRACE("[bsdk] baresip_init SEH crash! code=0x%08lX at %p (RVA=0x%llX echosdk_base=%p)\n",
			       _seh, _crash_addr,
			       _hm ? (unsigned long long)((char*)_crash_addr - (char*)_hm) : 0,
			       (void*)_hm);
			goto fail;
		}
	}
#else
	err = baresip_init(conf_config());
#endif
	BSDK_TRACE("[bsdk] step 5 done: err=%d\n", err);
	if (err)
		goto fail;

	/* baresip_init tries to re-read the (missing) config path and may replace
	 * conf_cur() with NULL. Re-seed so modules_init never sees a NULL conf. */
	conf_configure_buf((const uint8_t *)"#\n", 2);

	conf_config()->call.accept = true;

	BSDK_TRACE("[bsdk] step 6: dns_init\n");
	err = bsdk_dns_init();
	if (err)
		goto fail;

	bsdk_timers_configure(&g_bsdk.cfg);

	/* Pre-configure the transport mask before ua_init.
	 * Bits: UDP=1<<0, TCP=1<<1, TLS=1<<2, WS=1<<3, WSS=1<<4 */
	conf_config()->sip.transports = (1u << SIP_TRANSP_UDP) |
	                                (1u << SIP_TRANSP_TCP) |
	                                (1u << SIP_TRANSP_TLS) |
	                                (1u << SIP_TRANSP_WS)  |
	                                (1u << SIP_TRANSP_WSS);

	BSDK_TRACE("[bsdk] step 7: ua_init\n");
	{
		const char *sw = g_bsdk.cfg.user_agent ? g_bsdk.cfg.user_agent
		                                       : "EchoSDK/1.0";
		err = ua_init(sw, true, true, true);
	}
	if (err)
		goto fail;

	{
		struct tls *tls = uag_tls();
		if (tls) {
			if (g_bsdk.cfg.ca_cert_path)
				tls_add_ca(tls, g_bsdk.cfg.ca_cert_path);
			if (!g_bsdk.cfg.verify_server)
				tls_disable_verify_server(tls);
		}
	}

	BSDK_TRACE("[bsdk] step 8: event_init\n");
	err = bsdk_event_init();
	if (err)
		goto fail;

	if (g_bsdk.cfg.trace_sip) {
		err = bsdk_trace_init();
		if (err)
			goto fail;
	}

	/* Before modules_init: the opus module reads every opus_* key once, in its
	 * module_init(), into globals and its SDP fmtp.  Written any later, none
	 * of opus.* or opus_expected_loss_pct reaches the encoder or decoder. */
	{
		const echosdk_opus_config_t *op = &g_bsdk.cfg.opus;
		char obuf[256];
		int  olen = 0;
		if (op->bitrate > 0)
			olen += re_snprintf(obuf + olen, sizeof(obuf) - olen,
			                   "opus_bitrate %d\n", op->bitrate);
		if (op->complexity >= 0)
			olen += re_snprintf(obuf + olen, sizeof(obuf) - olen,
			                   "opus_complexity %d\n", op->complexity);
		if (op->cbr)
			olen += re_snprintf(obuf + olen, sizeof(obuf) - olen, "opus_cbr yes\n");
		if (op->dtx)
			olen += re_snprintf(obuf + olen, sizeof(obuf) - olen, "opus_dtx yes\n");
		if (op->fec)
			olen += re_snprintf(obuf + olen, sizeof(obuf) - olen, "opus_inbandfec yes\n");
		/* opus_inbandfec asks the peer for FEC; opus_packet_loss is what
		 * sizes our own redundant LBRR frame.  Pinned here when the app set
		 * it; left at 0 otherwise, for adapt.c to switch with the link. */
		if (g_bsdk.cfg.opus_expected_loss_pct) {
			uint32_t pl = g_bsdk.cfg.opus_expected_loss_pct;
			if (pl > 100)
				pl = 100;
			olen += re_snprintf(obuf + olen, sizeof(obuf) - olen,
			                    "opus_packet_loss %u\n", pl);
		}
		/* Both ways, always: the opus module asks for and sends stereo
		 * unless told otherwise, which for a mono microphone is the same
		 * voice coded twice — twice the automatic bitrate (99 kbps at
		 * 48 kHz) for no measurable quality. */
		olen += re_snprintf(obuf + olen, sizeof(obuf) - olen,
		                    "opus_stereo %s\nopus_sprop_stereo %s\n",
		                    op->stereo ? "yes" : "no",
		                    op->stereo ? "yes" : "no");
		if (olen > 0)
			conf_configure_buf((const uint8_t *)obuf, (size_t)olen);
	}

	BSDK_TRACE("[bsdk] step 9: modules_init\n");
	err = modules_init();
	if (err)
		goto fail;
	BSDK_TRACE("[bsdk] step 9 done\n");

	/* After modules_init (baresip's media-NAT list exists) and before any
	 * account can ask for medianat "ice": register EchoSDK's own ICE agent. */
	if (bsdk_ice_module_init() == 0)
		BSDK_TRACE("[bsdk] step 9b: echo-ice media-NAT registered\n");

	/* Platform audio session setup (iOS AVAudioSession; no-op elsewhere).
	 * Non-fatal: a session category the OS refuses right now (e.g. during
	 * a CallKit-owned activation) still leaves the stack usable. */
	if (bsdk_platform_audio_init(g_bsdk.cfg.platform_audio_activate))
		warning("EchoSDK: platform audio init failed\n");

	if (g_bsdk.cfg.jbuf_type == ECHOSDK_JBUF_FIXED) {
		struct config *c = conf_config();
		c->avt.audio.jbtype = JBUF_FIXED;
	}

	BSDK_TRACE("[bsdk] step 10: audio_processing_init\n");
	bsdk_audio_processing_init(g_bsdk.cfg.ns, g_bsdk.cfg.agc,
	                           g_bsdk.cfg.aec_mode,
	                           g_bsdk.cfg.aec_suppression_level,
	                           g_bsdk.cfg.mic_gain_db,
	                           g_bsdk.cfg.speaker_gain_db);
	bsdk_tap_global_init();

	BSDK_TRACE("[bsdk] step 11: message_init\n");
	err = bsdk_message_init();
	if (err)
		goto fail;

	BSDK_TRACE("[bsdk] step 12: presence_init\n");
	err = bsdk_presence_init();
	if (err)
		goto fail;

	if (g_bsdk.cfg.pcap_path) {
		err = bsdk_pcap_open(g_bsdk.cfg.pcap_path);
		if (err)
			goto fail;
	}

	if (g_bsdk.cfg.stats_interval_ms > 0) {
		err = bsdk_stats_init();
		if (err)
			goto fail;
	}

	BSDK_TRACE("[bsdk] step 13: netmon_init\n");
	err = bsdk_netmon_init();
	if (err)
		goto fail;

	BSDK_TRACE("[bsdk] step 14: re_loop_start\n");
	err = bsdk_re_loop_start();
	if (err)
		goto fail;

	BSDK_TRACE("[bsdk] step 15: done\n");
	g_bsdk.initialized = true;
	mtx_unlock(&g_bsdk.lock);
	return ECHOSDK_OK;

fail:
	BSDK_TRACE("[bsdk] fail: cleanup start\n");
	warning("EchoSDK: init failed: %m\n", err);
	bsdk_re_loop_stop();
	bsdk_netmon_close();
	bsdk_call_setup_watch_close();
	bsdk_stats_close();
	bsdk_trace_close();
	bsdk_event_close();
	ua_close();
	bsdk_call_global_reset();
	/* Before mod_close: a retry re-registers auconv/auresamp, and ours
	 * must go back in after them, not stay linked ahead. */
	bsdk_audio_processing_close();
	bsdk_ice_module_close();
	mod_close();   /* see echosdk_shutdown(): a retry of init must reload */
	bsdk_dns_close();
#ifdef _WIN32
	{ __try { baresip_close(); } __except(EXCEPTION_EXECUTE_HANDLER) {
		BSDK_TRACE("[bsdk] baresip_close crash: 0x%08lX\n", GetExceptionCode()); } }
#else
	baresip_close();
#endif
	libre_close();
	bsdk_log_close();
	bsdk_pcap_close();
	bsdk_cfg_deep_free(&g_bsdk);
	memset(&g_bsdk, 0, sizeof(g_bsdk));
	mtx_unlock(&g_bsdk.lock);
	return err ? err : ECHOSDK_ERR_STATE;
}

/* ── echosdk_is_initialized ──────────────────────────────────────────────── */

bool echosdk_is_initialized(void)
{
	/* g_bsdk.lock is only initialized on the first echosdk_init(); before
	 * that the zeroed struct is answer enough and locking would be UB. */
	if (!g_bsdk.initialized)
		return false;

	mtx_lock(&g_bsdk.lock);
	bool up = g_bsdk.initialized;
	mtx_unlock(&g_bsdk.lock);
	return up;
}

/* ── echosdk_set_event_handler ───────────────────────────────────────────── */

int echosdk_set_event_handler(echosdk_event_cb_t cb, void *userdata,
                               bool deliver_owned_events)
{
	if (!g_bsdk.initialized)
		return ECHOSDK_ERR_STATE;

	mtx_lock(&g_bsdk.lock);
	if (!g_bsdk.initialized) {
		mtx_unlock(&g_bsdk.lock);
		return ECHOSDK_ERR_STATE;
	}

	/* ev_lock is the one the event thread holds around its snapshot of
	 * these three fields, so taking it here means a delivery either sees
	 * the whole old handler or the whole new one — never a callback
	 * paired with the wrong userdata or ownership mode. */
	mtx_lock(&g_bsdk.ev_lock);
	g_bsdk.cfg.event_cb             = cb;
	g_bsdk.cfg.event_userdata       = userdata;
	g_bsdk.cfg.deliver_owned_events = deliver_owned_events;

	/* Then wait out a delivery that had already snapshotted the old
	 * handler, so callers can free it (close a Dart NativeCallable, unload
	 * a plugin) the moment this returns.  Skipped when called from inside
	 * the callback itself — that delivery is this thread, and waiting for
	 * it would wait forever. */
	if (!thrd_equal(thrd_current(), g_bsdk.ev_thread)) {
		while (g_bsdk.ev_delivering)
			cnd_wait(&g_bsdk.ev_idle_cond, &g_bsdk.ev_lock);
	}
	mtx_unlock(&g_bsdk.ev_lock);

	mtx_unlock(&g_bsdk.lock);
	return ECHOSDK_OK;
}

/* ── echosdk_shutdown ────────────────────────────────────────────────────── */

/* Runs on the re thread: hang up all calls and free the UA. Audio drivers
 * (PulseAudio, CoreAudio, WASAPI, …) must be torn down from the same thread
 * that opened the streams, which is the re thread. */
static void hangup_ua_fn(void *arg)
{
	struct ua **pua = arg;
	ua_hangup(*pua, NULL, 0, NULL);
	*pua = mem_deref(*pua);
}

void echosdk_shutdown(void)
{
	mtx_lock(&g_bsdk.lock);
	if (!g_bsdk.initialized) {
		mtx_unlock(&g_bsdk.lock);
		return;
	}

	/* Stop the handover state machine before anything it touches (UAs,
	 * calls) is torn down.  Timers are cancelled after the re loop stops. */
	bsdk_netmon_stop();

	BSDK_TRACE("[bsdk] shutdown: event_close\n");
	bsdk_event_close();

	BSDK_TRACE("[bsdk] shutdown: stats/trace/msg/presence/audio\n");
	bsdk_stats_close();
	bsdk_trace_close();
	bsdk_message_close();
	bsdk_presence_close();
	bsdk_audio_processing_close();

	BSDK_TRACE("[bsdk] shutdown: account loop\n");
	/* Hang up all active calls and free UAs on the re thread BEFORE stopping
	 * the event loop.  ua_hangup() triggers audio stream teardown; audio
	 * drivers interact with their own mainloops and must be called from the
	 * same thread that opened the streams. */
	struct le *le, *le_tmp;
	LIST_FOREACH_SAFE(&g_bsdk.accounts, le, le_tmp) {
		struct echosdk_account *acct = le->data;
		acct->destroyed = true;
		tmr_cancel(&acct->retry_tmr);
		tmr_cancel(&acct->reg_watch_tmr);
		tmr_cancel(&acct->ka_tmr);
		if (acct->ua)
			bsdk_dispatch_sync(hangup_ua_fn, &acct->ua);
		bsdk_acct_cfg_deep_free(acct);
		list_unlink(&acct->le);
		mem_deref(acct);
	}

	BSDK_TRACE("[bsdk] shutdown: re_loop_stop\n");
	bsdk_re_loop_stop();

	/* After the loop has stopped so no handover timer can fire mid-teardown. */
	BSDK_TRACE("[bsdk] shutdown: netmon_close\n");
	bsdk_netmon_close();
	bsdk_call_setup_watch_close();

	BSDK_TRACE("[bsdk] shutdown: ua_close\n");
	ua_close();
	BSDK_TRACE("[bsdk] shutdown: module_app_unload\n");
	module_app_unload();
	BSDK_TRACE("[bsdk] shutdown: audio_external_close\n");
	/* After ua_close/module unload — every audio stream is gone, so no
	 * device of ours can still be open — and before baresip_close(), whose
	 * baresip_init() counterpart re-inits ausrcl/auplayl and would strand
	 * a registration we still believe we hold. */
	bsdk_audio_external_close();
#ifdef __ANDROID__
	/* After ua_close/module unload — all audio streams are gone, the
	 * OpenSLES engine can be destroyed. */
	bsdk_sles_vc_close();
#endif
	/* Unregister our media-NAT before baresip_close() frees the list. */
	bsdk_ice_module_close();

	/* Unload the static modules.  libre_close() does not: it is
	 * fd_setsize(0) + net_sock_close() + re_thread_close(), and the module
	 * registry (re's mod.c `modl`) survives it.  baresip_close() meanwhile
	 * frees the codec / media-encryption / media-NAT lists the modules had
	 * registered into.  So a second echosdk_init() in the same process saw
	 * module_load() answer EALREADY for every module — mod_find() still knew
	 * them — and never re-ran their init(): zero codecs, no dtls_srtp, no
	 * ice, and echosdk_account_create() still returned 0.  (Two Flutter
	 * engines each treating the SDK as their own is how it was hit.)
	 *
	 * Here, and not later: the module close handlers unlink from baresip's
	 * lists, so those must still exist.  After ua_close() and the ice-shim
	 * restore, so nothing is mid-call and the vtable we hand back is
	 * baresip's own. */
	BSDK_TRACE("[bsdk] shutdown: mod_close\n");
	mod_close();

	BSDK_TRACE("[bsdk] shutdown: dns_close\n");
	bsdk_dns_close();
	BSDK_TRACE("[bsdk] shutdown: baresip_close\n");
	baresip_close();
	BSDK_TRACE("[bsdk] shutdown: cfg_deep_free\n");
	bsdk_cfg_deep_free(&g_bsdk);
	BSDK_TRACE("[bsdk] shutdown: pcap_close\n");
	bsdk_pcap_close();
	BSDK_TRACE("[bsdk] shutdown: libre_close\n");
	libre_close();
	BSDK_TRACE("[bsdk] shutdown: log_close\n");
	bsdk_log_close();

	BSDK_TRACE("[bsdk] shutdown: call/tap reset\n");
	bsdk_call_global_reset();
	bsdk_tap_global_reset();

	BSDK_TRACE("[bsdk] shutdown: done\n");
	g_bsdk.initialized = false;
	mtx_unlock(&g_bsdk.lock);
}

/* ── echosdk_version ─────────────────────────────────────────────────────── */

const char *echosdk_version(void)
{
	return "1.0.0";
}
