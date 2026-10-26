/**
 * @file account.c  Multi-account registration FSM
 *
 * medut_account_t wraps a baresip ua. One UA per account.
 * Registration retry uses exponential backoff with jitter.
 * All string fields in the account config are deep-copied.
 */

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include "medut_internal.h"

/* Post a non-fatal registrar warning.
 *
 * MEDUT_EV_REGISTRAR_WARNING has been declared in medut.h, cloned in
 * event.c and decoded by every binding since the event set was written, but
 * nothing ever posted it — so the one channel meant to explain a registration
 * that is technically alive but behaving oddly delivered nothing, and the
 * explanations went to warning() and were thrown away.  The two callers below
 * are the conditions a host actually needs in order to answer "why did it
 * reconnect?" without trawling logs after the fact.
 *
 * String ownership follows the producer idiom in log.c: the message is packed
 * into the queued event's own inline buf, because bsdk_event_post() is a
 * shallow copy and qev_clone() rebases pointers that live in that buf.
 */
static void post_registrar_warning(const char *fmt, ...)
{
	struct medut_queued_event *qev = bsdk_qev_alloc();
	va_list ap;

	if (!qev)
		return;

	va_start(ap, fmt);
	(void)re_vsnprintf(qev->buf, sizeof(qev->buf), fmt, ap);
	va_end(ap);

	qev->ev.type               = MEDUT_EV_REGISTRAR_WARNING;
	qev->ev.u.reg_warn.message = qev->buf;
	bsdk_event_post_qev(qev);   /* warns and frees on failure */
}

/* ── URI parser: "user@host", "user@host:port", or "sip:user@host" ──────── */

static void parse_account_uri(const char *uri,
                               char *user, size_t user_sz,
                               char *host, size_t host_sz,
                               uint16_t *port)
{
	user[0] = host[0] = '\0';
	*port = 0;

	if (!uri) return;

	/* strip sip: or sips: scheme */
	if (strncasecmp(uri, "sip:", 4) == 0)  uri += 4;
	else if (strncasecmp(uri, "sips:", 5) == 0) uri += 5;

	const char *at = strchr(uri, '@');
	if (!at) {
		/* no user part — treat everything as host */
		const char *colon = strchr(uri, ':');
		if (colon) {
			size_t hlen = (size_t)(colon - uri);
			if (hlen >= host_sz) hlen = host_sz - 1;
			memcpy(host, uri, hlen); host[hlen] = '\0';
			*port = (uint16_t)strtoul(colon + 1, NULL, 10);
		} else {
			str_ncpy(host, uri, host_sz);
		}
		return;
	}

	/* user part */
	size_t ulen = (size_t)(at - uri);
	if (ulen >= user_sz) ulen = user_sz - 1;
	memcpy(user, uri, ulen); user[ulen] = '\0';

	/* host[:port] part — stop at ';' URI params */
	const char *hp = at + 1;
	const char *semi = strchr(hp, ';');
	size_t hp_len = semi ? (size_t)(semi - hp) : strlen(hp);

	if (hp[0] == '[') {
		/* IPv6 literal — find closing bracket */
		const char *cbracket = memchr(hp, ']', hp_len);
		if (cbracket) {
			size_t hlen = (size_t)(cbracket - hp - 1);
			if (hlen >= host_sz) hlen = host_sz - 1;
			memcpy(host, hp + 1, hlen);
			host[hlen] = '\0';
			/* Optional port after ']' */
			if (cbracket + 1 < hp + hp_len && cbracket[1] == ':')
				*port = (uint16_t)strtoul(cbracket + 2, NULL, 10);
		} else {
			/* Malformed bracket — copy as-is */
			if (hp_len >= host_sz) hp_len = host_sz - 1;
			memcpy(host, hp, hp_len); host[hp_len] = '\0';
		}
	} else {
		const char *colon = memchr(hp, ':', hp_len);
		if (colon) {
			size_t hlen = (size_t)(colon - hp);
			if (hlen >= host_sz) hlen = host_sz - 1;
			memcpy(host, hp, hlen); host[hlen] = '\0';
			*port = (uint16_t)strtoul(colon + 1, NULL, 10);
		} else {
			if (hp_len >= host_sz) hp_len = host_sz - 1;
			memcpy(host, hp, hp_len); host[hp_len] = '\0';
		}
	}
}

/* ── Deep-copy helpers for account config ───────────────────────────────── */

void bsdk_acct_cfg_deep_copy(medut_account_config_t *dst,
                              const medut_account_config_t *src,
                              struct medut_account *acct)
{
	*dst = *src;
	acct->cfg_uri         = bsdk_strdup(src->uri);
	acct->cfg_password    = bsdk_strdup(src->password);
	acct->cfg_server_host = bsdk_strdup(src->server_host);
	acct->cfg_server_url  = bsdk_strdup(src->server_url);
	acct->cfg_auth_user   = bsdk_strdup(src->auth_user);
	acct->cfg_display_name = bsdk_strdup(src->display_name);
	acct->cfg_stun_server = bsdk_strdup(src->stun_server);
	acct->cfg_turn_server = bsdk_strdup(src->turn_server);
	acct->cfg_turn_user   = bsdk_strdup(src->turn_user);
	acct->cfg_turn_pass   = bsdk_strdup(src->turn_pass);
	acct->cfg_outbound    = bsdk_strdup(src->outbound);
	acct->cfg_outbound_proxy = bsdk_strdup(src->outbound_proxy);
	acct->cfg_push_token  = bsdk_strdup(src->push_token);
	acct->cfg_push_param  = bsdk_strdup(src->push_param);
	(void)bsdk_ice_servers_copy(&acct->ice_srvv, &acct->ice_srv_api,
	                            &acct->ice_srvc, src->ice_servers,
	                            src->ice_server_count);
	acct->ice_policy = src->ice_transport_policy;

	dst->uri          = acct->cfg_uri;
	dst->password     = acct->cfg_password;
	dst->server_host  = acct->cfg_server_host;
	dst->server_url   = acct->cfg_server_url;
	dst->auth_user    = acct->cfg_auth_user;
	dst->display_name = acct->cfg_display_name;
	dst->stun_server  = acct->cfg_stun_server;
	dst->turn_server  = acct->cfg_turn_server;
	dst->turn_user    = acct->cfg_turn_user;
	dst->turn_pass    = acct->cfg_turn_pass;
	dst->outbound     = acct->cfg_outbound;
	dst->outbound_proxy = acct->cfg_outbound_proxy;
	dst->push_token   = acct->cfg_push_token;
	dst->push_param   = acct->cfg_push_param;
	dst->ice_servers      = acct->ice_srv_api;
	dst->ice_server_count = acct->ice_srvc;
}

void bsdk_acct_cfg_deep_free(struct medut_account *acct)
{
	mem_deref(acct->cfg_uri);          acct->cfg_uri = NULL;
	mem_deref(acct->cfg_password);     acct->cfg_password = NULL;
	mem_deref(acct->cfg_server_host);  acct->cfg_server_host = NULL;
	mem_deref(acct->cfg_server_url);   acct->cfg_server_url = NULL;
	mem_deref(acct->cfg_auth_user);    acct->cfg_auth_user = NULL;
	mem_deref(acct->cfg_display_name); acct->cfg_display_name = NULL;
	mem_deref(acct->cfg_stun_server);  acct->cfg_stun_server = NULL;
	mem_deref(acct->cfg_turn_server);  acct->cfg_turn_server = NULL;
	mem_deref(acct->cfg_turn_user);    acct->cfg_turn_user = NULL;
	mem_deref(acct->cfg_turn_pass);    acct->cfg_turn_pass = NULL;
	mem_deref(acct->cfg_outbound);     acct->cfg_outbound = NULL;
	mem_deref(acct->cfg_outbound_proxy); acct->cfg_outbound_proxy = NULL;
	mem_deref(acct->cfg_push_token);   acct->cfg_push_token = NULL;
	mem_deref(acct->cfg_push_param);   acct->cfg_push_param = NULL;
	mem_deref(acct->pn_cparams);       acct->pn_cparams = NULL;
	bsdk_ice_servers_free(&acct->ice_srvv, &acct->ice_srv_api, &acct->ice_srvc);
	acct->cfg.ice_servers = NULL;
	acct->cfg.ice_server_count = 0;
}

/* ── ICE server lists ───────────────────────────────────────────────────── */

static void ice_srvv_destructor(void *arg)
{
	struct bsdk_ice_server *v = arg;

	/* Arrays are allocated one entry long and zeroed, so the first entry
	 * without a URL terminates the list (entries are filled front to back). */
	for (size_t i = 0; v[i].url; i++) {
		mem_deref(v[i].url);
		mem_deref(v[i].user);
		mem_deref(v[i].pass);
	}
}

int bsdk_ice_servers_copy(struct bsdk_ice_server **vp,
                          medut_ice_server_t **apip, uint32_t *np,
                          const medut_ice_server_t *src, uint32_t n)
{
	struct bsdk_ice_server *v;
	medut_ice_server_t *api;
	uint32_t kept = 0;

	if (!vp || !apip || !np)
		return EINVAL;

	bsdk_ice_servers_free(vp, apip, np);

	if (!src || !n)
		return 0;

	v   = mem_zalloc((n + 1u) * sizeof(*v), ice_srvv_destructor);
	api = mem_zalloc(n * sizeof(*api), NULL);
	if (!v || !api) {
		mem_deref(v);
		mem_deref(api);
		return ENOMEM;
	}

	for (uint32_t i = 0; i < n; i++) {
		if (!src[i].url || !src[i].url[0])
			continue;
		v[kept].url  = bsdk_strdup(src[i].url);
		v[kept].user = bsdk_strdup(src[i].user);
		v[kept].pass = bsdk_strdup(src[i].pass);
		api[kept].url  = v[kept].url;
		api[kept].user = v[kept].user;
		api[kept].pass = v[kept].pass;
		kept++;
	}

	*vp   = v;
	*apip = api;
	*np   = kept;
	return 0;
}

void bsdk_ice_servers_free(struct bsdk_ice_server **vp,
                           medut_ice_server_t **apip, uint32_t *np)
{
	if (vp)   *vp   = mem_deref(*vp);
	if (apip) *apip = mem_deref(*apip);
	if (np)   *np   = 0;
}

static int snap_add(struct bsdk_ice_server_snap *snap, uint32_t cap,
                    const char *url, const char *user, const char *pass)
{
	struct bsdk_ice_server *e;

	if (!url || !url[0] || snap->n >= cap)
		return 0;
	e = &snap->v[snap->n];
	e->url  = bsdk_strdup(url);
	e->user = bsdk_strdup(user);
	e->pass = bsdk_strdup(pass);
	if (!e->url)
		return ENOMEM;
	snap->n++;
	return 0;
}

/* Legacy single-server fields, account over global, TURN before STUN —
 * the same precedence account.c has always applied to baresip's one slot,
 * except that both servers are now used instead of TURN shadowing STUN. */
static void snap_add_legacy(struct bsdk_ice_server_snap *snap, uint32_t cap,
                            const struct medut_account *acct)
{
	const medut_config_t *cfg = &g_bsdk.cfg;
	const char *stun = acct && acct->cfg.stun_server
	                 ? acct->cfg.stun_server : cfg->stun_server;
	const char *turn = acct && acct->cfg.turn_server
	                 ? acct->cfg.turn_server : cfg->turn_server;
	const char *tu   = acct && acct->cfg.turn_user
	                 ? acct->cfg.turn_user   : cfg->turn_user;
	const char *tp   = acct && acct->cfg.turn_pass
	                 ? acct->cfg.turn_pass   : cfg->turn_pass;

	(void)snap_add(snap, cap, turn, tu, tp);
	if (!stun || !turn || strcmp(stun, turn))
		(void)snap_add(snap, cap, stun, NULL, NULL);
}

int bsdk_ice_servers_for_call(const struct call *bc,
                              struct bsdk_ice_server_snap *snap)
{
	const struct medut_account *acct = NULL;
	const struct bsdk_ice_server *src = NULL;
	uint32_t n = 0, cap;
	int err = 0;

	if (!snap)
		return EINVAL;
	memset(snap, 0, sizeof(*snap));

	if (bc)
		acct = bsdk_account_find_by_ua(call_get_ua(bc));

	if (acct && acct->ice_srvc) {
		src = acct->ice_srvv;
		n   = acct->ice_srvc;
		snap->policy = acct->ice_policy;
	}
	else if (g_bsdk.cfg_ice_srvc) {
		src = g_bsdk.cfg_ice_srvv;
		n   = g_bsdk.cfg_ice_srvc;
		snap->policy = acct && acct->ice_policy
		             ? acct->ice_policy : g_bsdk.cfg.ice_transport_policy;
	}
	else {
		snap->policy = acct && acct->ice_policy
		             ? acct->ice_policy : g_bsdk.cfg.ice_transport_policy;
	}

	cap = n ? n : 2;
	snap->v = mem_zalloc((cap + 1u) * sizeof(*snap->v), ice_srvv_destructor);
	if (!snap->v)
		return ENOMEM;

	if (src) {
		for (uint32_t i = 0; i < n && !err; i++)
			err = snap_add(snap, cap, src[i].url, src[i].user,
			               src[i].pass);
	}
	else {
		snap_add_legacy(snap, cap, acct);
	}

	if (err)
		bsdk_ice_server_snap_clear(snap);
	return err;
}

void bsdk_ice_server_snap_clear(struct bsdk_ice_server_snap *snap)
{
	if (!snap)
		return;
	snap->v = mem_deref(snap->v);
	snap->n = 0;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Opus as baresip's account codec lookup must spell it.  The lookup matches
 * the channel count the opus module registered with, which is 1 unless
 * cfg.opus.stereo (core.c writes opus_stereo either way) — not the /2 the
 * SDP rtpmap always carries.  Spelled /2 against a mono module, the account
 * got no Opus at all and every call fell back to G.711. */
static const char *opus_codec_str(void)
{
	return g_bsdk.cfg.opus.stereo ? "opus/48000/2" : "opus/48000/1";
}

/* Normalize a codec name/alias to the "name/rate/channels" format baresip expects.
 * Returns a static or caller-guaranteed string, or NULL for empty input. */
static const char *normalize_codec_name(const char *name)
{
	if (!name || !name[0]) return NULL;
	if (strcasecmp(name, "opus")               == 0) return opus_codec_str();
	if (strcasecmp(name, "ulaw")               == 0 ||
	    strcasecmp(name, "g711u")              == 0 ||
	    strcasecmp(name, "pcmu")               == 0) return "PCMU/8000/1";
	if (strcasecmp(name, "alaw")               == 0 ||
	    strcasecmp(name, "g711a")              == 0 ||
	    strcasecmp(name, "pcma")               == 0) return "PCMA/8000/1";
	/* No module in the build registers these, so they resolve to nothing and
	 * get dropped from the offer with a warning.  The spellings are kept so
	 * that adding the corresponding module (g722 to the profile, an external
	 * g729/g726) is a build change only, with no code edit here. */
	if (strcasecmp(name, "g722")               == 0) return "G722/8000/1";
	if (strcasecmp(name, "g729")               == 0) return "G729/8000/1";
	if (strcasecmp(name, "g726")               == 0 ||
	    strcasecmp(name, "g726-32")            == 0) return "G726-32/8000/1";
	/* Unknown name passed as-is — lets callers use any codec baresip has loaded */
	return name;
}

/* Codec list offered when neither the account nor the global config names any.
 * Without this, baresip falls back to every codec its loaded modules register,
 * so the default SDP offer would silently follow whatever the build happens to
 * link.  Pinning it here gives every platform the same default: Opus first
 * (wideband), G.711 as the universally-interoperable fallback.  Both profiles
 * compile exactly these two codec modules — see cmake/modules-{desktop,mobile}
 * .cmake — so this list is always fully satisfiable. */
#define BSDK_DEFAULT_AUDIO_CODECS_G711 "PCMU/8000/1,PCMA/8000/1"

/* Build codec list string from enum array: "opus/48000/2,PCMU/8000/1" */
static void codec_list_str(const medut_codec_t *codecs, int count,
                             char *buf, size_t sz)
{
	buf[0] = '\0';
	for (int i = 0; i < count && i < 8; i++) {
		const char *name;
		switch (codecs[i]) {
		case MEDUT_CODEC_OPUS: name = opus_codec_str(); break;
		case MEDUT_CODEC_PCMU: name = "PCMU/8000/1";   break;
		case MEDUT_CODEC_PCMA: name = "PCMA/8000/1";   break;
		case MEDUT_CODEC_G722:    name = "G722/8000/1";    break;
		case MEDUT_CODEC_G726_32: name = "G726-32/8000/1"; break;
		default:                    name = NULL;              break;
		}
		if (!name) continue;
		if (buf[0]) strncat(buf, ",", sz - strlen(buf) - 1);
		strncat(buf, name, sz - strlen(buf) - 1);
	}
}

/* Build codec list string from name array: "opus/48000/2,PCMU/8000/1" */
static void codec_names_list_str(const char names[][32], int count,
                                  char *buf, size_t sz)
{
	buf[0] = '\0';
	for (int i = 0; i < count && i < 8; i++) {
		const char *resolved = normalize_codec_name(names[i]);
		if (!resolved) continue;
		if (buf[0]) strncat(buf, ",", sz - strlen(buf) - 1);
		strncat(buf, resolved, sz - strlen(buf) - 1);
	}
}

/* Maximum buffer for the RFC 8599 Contact URI params string.
 * Worst case: "pn-provider=apns-sandbox" (24) + ";pn-prid=" (9) +
 * token (~256) + ";pn-param=" (10) + param (~200) + NUL. */
#define BSDK_PUSH_PARAMS_BUFSZ 1024

/* Build the RFC 8599 Contact URI params string into buf.
 * Returns length written (>0), 0 if push is disabled/unconfigured,
 * or -1 on overflow. */
static int build_push_contact_params(const struct medut_account *acct,
                                      char *buf, size_t buf_sz)
{
	const char *provider_str;

	if (acct->cfg.push_provider == MEDUT_PUSH_PROVIDER_NONE)
		return 0;
	if (!acct->cfg.push_token || !acct->cfg.push_token[0])
		return 0;

	switch (acct->cfg.push_provider) {
	case MEDUT_PUSH_PROVIDER_APNS:         provider_str = "apns";         break;
	case MEDUT_PUSH_PROVIDER_APNS_SANDBOX: provider_str = "apns-sandbox"; break;
	case MEDUT_PUSH_PROVIDER_FCM:          provider_str = "fcm";          break;
	default: return 0;
	}

	/* Pre-flight length check — refuse silent truncation */
	size_t need = strlen("pn-provider=") + strlen(provider_str)
	            + strlen(";pn-prid=") + strlen(acct->cfg.push_token) + 1;
	if (acct->cfg.push_param && acct->cfg.push_param[0])
		need += strlen(";pn-param=") + strlen(acct->cfg.push_param);
	if (need > buf_sz)
		return -1;

	int n;
	if (acct->cfg.push_param && acct->cfg.push_param[0])
		n = re_snprintf(buf, buf_sz,
		                "pn-provider=%s;pn-prid=%s;pn-param=%s",
		                provider_str,
		                acct->cfg.push_token,
		                acct->cfg.push_param);
	else
		n = re_snprintf(buf, buf_sz,
		                "pn-provider=%s;pn-prid=%s",
		                provider_str,
		                acct->cfg.push_token);

	return (n > 0) ? n : -1;
}

/* Rebuild acct->pn_cparams from the push config.  re_main only. */
static int push_cparams_update(struct medut_account *acct)
{
	char buf[BSDK_PUSH_PARAMS_BUFSZ];
	int n = build_push_contact_params(acct, buf, sizeof(buf));
	if (n < 0)
		return MEDUT_ERR_INVAL;   /* token too long */

	acct->pn_cparams = mem_deref(acct->pn_cparams);
	if (n > 0 && str_dup(&acct->pn_cparams, buf))
		return MEDUT_ERR_NOMEM;
	return MEDUT_OK;
}

/* Defined by job 11 of cmake/patch-baresip-sources.cmake. */
extern const char *(*bsdk_reg_cparams_h)(const struct ua *ua);

/* baresip's reg_register(), on re_main, asking for the Contact URI params of
 * the REGISTER it is about to send.  The account list is only ever changed
 * on re_main, so it is walked here without acct_lock: a medut_account_foreach()
 * callback that re-registers would otherwise take that lock twice. */
static const char *reg_cparams_handler(const struct ua *ua)
{
	struct le *le;
	LIST_FOREACH(&g_bsdk.accounts, le) {
		const struct medut_account *acct = le->data;
		if (acct->ua == ua)
			return acct->pn_cparams;
	}
	return NULL;
}

void bsdk_account_push_init(void)
{
	bsdk_reg_cparams_h = reg_cparams_handler;
}

/* ── Per-account server certificate verification ─────────────────────────
 *
 * libre asks this, on re_main, before it picks or opens a TLS or WSS
 * connection (cmake/patch-re-sources.cmake, sip_transp_set_verify_handler).
 * It can only say which host the connection is for: the message is written
 * after the connection is chosen.  So the accounts that send to that host
 * decide — each by its own verify_tls when verify_tls_set, else the global
 * verify_server — and when they disagree the server is verified, since an
 * account that asked for the check must not lose it to one that did not.
 * A host no account sends to follows the global setting.
 *
 * Walked without acct_lock for the reason reg_cparams_handler() gives. */
static bool account_sends_to(const struct medut_account *acct,
                             const char *host)
{
	const struct account *acc = ua_account(acct->ua);
	const char *ob = account_outbound(acc, 0);
	const struct uri *luri;

	if (ob) {
		struct pl pl;
		struct uri uri;

		pl_set_str(&pl, ob);
		if (!uri_decode(&uri, &pl))
			return 0 == pl_strcasecmp(&uri.host, host);
	}

	luri = account_luri(acc);
	return luri && 0 == pl_strcasecmp(&luri->host, host);
}

static void tls_verify_handler(const char *host, bool *verify, void *arg)
{
	struct le *le;
	bool matched = false;
	(void)arg;

	*verify = g_bsdk.cfg.verify_server;

	if (!str_isset(host))
		return;

	LIST_FOREACH(&g_bsdk.accounts, le) {
		const struct medut_account *acct = le->data;
		bool want;

		if (acct->destroyed || !acct->ua || !account_sends_to(acct, host))
			continue;

		want = acct->cfg.verify_tls_set ? acct->cfg.verify_tls
		                                : g_bsdk.cfg.verify_server;

		*verify  = matched ? (*verify || want) : want;
		matched  = true;
	}
}

void bsdk_account_tls_verify_init(void)
{
	sip_transp_set_verify_handler(uag_sip(), tls_verify_handler, NULL);
}

void bsdk_account_push_settled(struct medut_account *acct)
{
	if (!acct || !acct->pn_refresh || !acct->ua || !acct->reg_wanted)
		return;
	acct->pn_refresh = false;
	ua_register(acct->ua);
}


/* Configure a baresip account object.
 * acct->parsed_* fields (user, host, port, transport) must be set first.
 *
 * Returns 0, or MEDUT_ERR_STATE when the stack cannot honour the request
 * because a module the account needs is not registered — see the checks
 * below.  baresip itself only warns ("mediaenc not found") and carries on
 * with an account that will offer plain RTP or no codecs at all. */
static int configure_baresip_account(struct medut_account *acct)
{
	struct account *ba = ua_account(acct->ua);
	const medut_config_t *cfg = &g_bsdk.cfg;

	/* Auth — password required; auth_user defaults to uri user part */
	if (acct->cfg.password)
		account_set_auth_pass(ba, acct->cfg.password);
	{
		const char *au = (acct->cfg.auth_user && acct->cfg.auth_user[0])
		                 ? acct->cfg.auth_user : acct->parsed_user;
		if (au && au[0])
			account_set_auth_user(ba, au);
	}
	if (acct->cfg.display_name)
		account_set_display_name(ba, acct->cfg.display_name);

	/* Registration interval */
	account_set_regint(ba, cfg->reg_expires);

	/* Outbound proxy — account outbound → account outbound_proxy → global → auto */
	{
		char ob[512];
		const char *ob_str = acct->cfg.outbound;
		if (!ob_str) {
			ob_str = acct->cfg.outbound_proxy;   /* check alias field */
		}
		if (!ob_str) {
			/* Try global outbound_proxy from SDK config */
			ob_str = cfg->outbound_proxy;
		}
		if (!ob_str) {
			const char *surl = acct->cfg.server_url;
			if (!surl && acct->auto_server_url[0])
				surl = acct->auto_server_url;
			const char *shost = (acct->cfg.server_host && acct->cfg.server_host[0])
			                    ? acct->cfg.server_host : acct->parsed_host;
			uint16_t sport = acct->cfg.server_port ? acct->cfg.server_port
			                                       : acct->parsed_port;
			if (surl || shost[0]) {
				bsdk_build_outbound(surl, shost, sport,
				                    acct->parsed_transport, ob, sizeof(ob));
				ob_str = ob;
			}
		}
		if (ob_str)
			account_set_outbound(ba, ob_str, 0);
	}

	/* Media encryption — account overrides global */
	{
		medut_media_enc_t enc = acct->cfg.media_enc
		                        ? acct->cfg.media_enc : cfg->media_enc;
		const char *menc = bsdk_mediaenc_str(enc);
		if (menc) {
			/* Fail here rather than let baresip's account_set_mediaenc()
			 * warn "mediaenc not found" and hand back an account that
			 * offers plain RTP — the app asked for encryption, and an
			 * empty registry means the stack is not in a usable state
			 * (the classic cause was a second medut_init() before
			 * shutdown unloaded the modules; see core.c). */
			if (!menc_find(baresip_mencl(), menc)) {
				warning("Medut/account: media encryption '%s' is not "
				        "registered — refusing to create an account "
				        "that would silently fall back to plain RTP\n",
				        menc);
				return MEDUT_ERR_STATE;
			}
			account_set_mediaenc(ba, menc);
		}
	}

	/* ICE / NAT — account OR global */
	if (acct->cfg.ice_enabled || cfg->ice_enabled) {
		if (!mnat_find(baresip_mnatl(), "ice")) {
			warning("Medut/account: ICE requested but the ice module "
			        "is not registered — refusing to create the "
			        "account\n");
			return MEDUT_ERR_STATE;
		}
		account_set_medianat(ba, "ice");
	}

	/* RFC 5626 (SIP outbound) for WebSocket accounts.
	 *
	 * baresip assigns a registration id only when the account carries
	 * sipnat=outbound (ua.c, create_register_clients: reg_add(…, i+1) per
	 * outbound proxy, else reg_add(…, 0)), and libre writes ";reg-id=N" and
	 * "Supported: gruu, outbound, path" only for regid > 0.  Without it a
	 * WebSocket client re-registers from every new connection as a *new*
	 * binding — the registrar keys on the Contact URI, and with PJSIP's
	 * rewrite_contact that URI carries the per-connection source address —
	 * so an AOR collects one binding per connection the phone has ever
	 * opened ("[2 bindings]"), and inbound calls are tried on dead ones
	 * first.  RFC 5626 gives the registrar what it needs to *replace* the
	 * binding instead: the same +sip.instance (already sent, from the uuid
	 * module) plus a reg-id.  RFC 7118 §5.2 expects exactly this of a
	 * WebSocket client.
	 *
	 * WS/WSS only: they always go through the one outbound proxy the SDK
	 * sets, and the transport is what makes the binding churn.  UDP/TCP/TLS
	 * keep baresip's default.  Not a config knob on purpose — the config
	 * structs are mirrored by generated bindings (ffigen) and a new field
	 * would change struct_size under them.
	 *
	 * Side effects, all checked: libre's flow keepalive (start_outbound) only
	 * starts if the 200 carries "Require: outbound", and for WS transports
	 * sip_keepalive_start() returns EPROTONOSUPPORT, which sipreg ignores.
	 * The uuid the instance-id comes from is a precondition — baresip
	 * refuses outbound without one — so skip when it is missing rather than
	 * turn every register into ENOSYS. */
	if ((acct->parsed_transport == MEDUT_TRANSPORT_WS ||
	     acct->parsed_transport == MEDUT_TRANSPORT_WSS) &&
	    str_isset(conf_config()->sip.uuid)) {
		if (account_set_sipnat(ba, "outbound")) {
			warning("Medut/account: could not enable RFC 5626 "
			        "outbound for the WebSocket account\n");
		}
		else {
			/* ua_alloc() already built the registration list — one
			 * reg with id 0, from an account that had neither sipnat
			 * nor an outbound proxy yet (ua.c:1272).  Setting the flag
			 * now changes nothing until that list is rebuilt from the
			 * account as it stands: ua_update_account() flushes it and
			 * the extension list and re-runs create_register_clients(),
			 * which is where the reg-id is assigned.  Verified on the
			 * wire — without this call the REGISTER still had no
			 * reg-id and no Supported: outbound. */
			int uerr = ua_update_account(acct->ua);
			if (uerr)
				warning("Medut/account: ua_update_account: %m\n",
				        uerr);
		}
	}

	/* RTCP multiplexing (RFC 5761) — per-account override; fall back to global */
	account_set_rtcp_mux(ba, acct->cfg.rtcp_mux_set ? acct->cfg.rtcp_mux : cfg->rtcp_mux);

	/* STUN/TURN — account overrides global */
	{
		const char *stun = acct->cfg.stun_server
		                 ? acct->cfg.stun_server : cfg->stun_server;
		const char *turn = acct->cfg.turn_server
		                 ? acct->cfg.turn_server : cfg->turn_server;
		const char *tu   = acct->cfg.turn_user
		                 ? acct->cfg.turn_user   : cfg->turn_user;
		const char *tp   = acct->cfg.turn_pass
		                 ? acct->cfg.turn_pass   : cfg->turn_pass;
		/* TURN includes relay functionality; prefer it over bare STUN.
		 * baresip's account has a single NAT server slot — TURN takes it
		 * when both are set, as TURN servers also respond to STUN requests. */
		if (turn)
			account_set_stun_uri(ba, turn);
		else if (stun)
			account_set_stun_uri(ba, stun);
		if (tu)   account_set_stun_user(ba, tu);
		if (tp)   account_set_stun_pass(ba, tp);

		/* ICE with nothing to learn a public address from offers only
		 * this host's own addresses.  Behind NAT that is a private one,
		 * and FreeSWITCH drops private candidates by default
		 * (apply-candidate-acl), finds none left and answers 488 —
		 * measured on 2026-10-07 against FreeSWITCH 1.11. */
		if ((acct->cfg.ice_enabled || cfg->ice_enabled) &&
		    !(stun && stun[0]) && !(turn && turn[0]) &&
		    !acct->ice_srvc && !g_bsdk.cfg_ice_srvc) {
			warning("Medut/account: %s has ICE on but no STUN or "
			        "TURN server: its calls offer only local "
			        "addresses, which a peer across NAT cannot "
			        "reach (FreeSWITCH rejects them with 488)\n",
			        acct->cfg.uri);
		}
	}

	/* Audio codecs — account names > account enums > global names >
	 * global enums */
	{
		char codecs[256];
		codecs[0] = '\0';

		if (acct->cfg.audio_codec_name_count > 0) {
			codec_names_list_str(acct->cfg.audio_codec_names,
			                     acct->cfg.audio_codec_name_count,
			                     codecs, sizeof(codecs));
		} else if (acct->cfg.audio_codec_count > 0) {
			codec_list_str(acct->cfg.audio_codecs,
			               acct->cfg.audio_codec_count,
			               codecs, sizeof(codecs));
		} else if (cfg->audio_codec_name_count > 0) {
			codec_names_list_str(cfg->audio_codec_names,
			                     cfg->audio_codec_name_count,
			                     codecs, sizeof(codecs));
		} else if (cfg->audio_codec_count > 0) {
			codec_list_str(cfg->audio_codecs,
			               cfg->audio_codec_count,
			               codecs, sizeof(codecs));
		}

		/* Nothing configured anywhere, or every configured name was empty */
		if (!codecs[0]) {
			(void)re_snprintf(codecs, sizeof(codecs), "%s,%s",
			                  opus_codec_str(),
			                  BSDK_DEFAULT_AUDIO_CODECS_G711);
		}

		if (codecs[0]) {
			/* No codec at all is the same broken stack as above, and a
			 * call with no codecs cannot be negotiated. */
			if (list_isempty(baresip_aucodecl())) {
				warning("Medut/account: no audio codec is registered "
				        "— refusing to create the account\n");
				return MEDUT_ERR_STATE;
			}
			account_set_audio_codecs(ba, codecs);
			/* Unknown names are passed to baresip as-is so any codec a
			 * loaded module registers can be named. When none of them
			 * resolve, the account list stays empty and baresip quietly
			 * falls back to offering every loaded codec — the opposite
			 * of the restriction that was asked for. Detect that by the
			 * fallback identity and say so. */
			if (account_aucodecl(ba) == baresip_aucodecl()) {
				warning("Medut/account: codec list \"%s\" matched no "
				        "loaded codec — offering all codecs instead\n",
				        codecs);
			}
		}
	}

	/* DTMF via RTP events */
	{
		static const enum dtmfmode dtmf_map[] = {
			[MEDUT_DTMF_RFC4733]  = DTMFMODE_RTP_EVENT,
			[MEDUT_DTMF_SIP_INFO] = DTMFMODE_SIP_INFO,
			[MEDUT_DTMF_AUTO]     = DTMFMODE_AUTO,
		};
		medut_dtmf_mode_t m = acct->cfg.dtmf_mode;
		if ((unsigned)m <= MEDUT_DTMF_AUTO)
			account_set_dtmfmode(ba, dtmf_map[m]);
		else
			account_set_dtmfmode(ba, DTMFMODE_RTP_EVENT);
	}

	/* Call transfer support */
	account_set_call_transfer(ba, true);

	return 0;
}

/* ── Reconnecting vs failed ──────────────────────────────────────────────────
 *
 * A registration that drops on a bad link is not the same event as one the
 * registrar refused, but the SDK used to report both as FAILED — so an app had
 * no way to tell "we are getting it back, say nothing" from "this needs the
 * user".  Every REGISTER on a train tunnel showed up as a hard failure while
 * the retry loop, one second later, quietly fixed it.
 *
 * So a failure the SDK will act on itself is RECONNECTING, and FAILED is kept
 * for the ones it has stopped acting on: wrong credentials, a server certificate
 * that failed verification, an exhausted retry budget, a retry the app
 * cancelled.  The decision is one function because three call sites make it —
 * the REGISTER_FAIL event, the registration watchdog and the keepalive probe —
 * and they must agree.
 */

static bool retry_budget_left(const struct medut_account *acct)
{
	uint32_t max_attempts = acct->retry_policy_set
	        ? acct->retry_max_attempts
	        : g_bsdk.cfg.reg_retry_max_attempts;

	return max_attempts == 0 || acct->retry_attempt < max_attempts;
}

medut_reg_state_t bsdk_account_reg_fail_state(struct medut_account *acct,
                                                medut_error_t err)
{
	/* An armed retry settles it: that attempt is going out whatever the
	 * budget now says, and schedule_retry() already counted it. */
	bool will_retry = acct->reg_wanted && !acct->destroyed &&
	        err != MEDUT_ERR_AUTH && err != MEDUT_ERR_TLS_CERT &&
	        (tmr_isrunning(&acct->retry_tmr) || retry_budget_left(acct));

	acct->reconnecting = will_retry;

	return will_retry ? MEDUT_REG_RECONNECTING : MEDUT_REG_FAILED;
}

void bsdk_account_reconnect_reset(struct medut_account *acct)
{
	if (!acct)
		return;

	acct->last_rc_valid  = false;
	acct->retry_delay_ms = 0;
}

bool bsdk_account_reg_ev_prepare(struct medut_account *acct,
                                 medut_event_t *ev)
{
	if (!acct || !ev)
		return true;

	/* Anything but RECONNECTING ends the reconnect, so clear the armed
	 * countdown before stamping — a REGISTERED carrying the delay of the
	 * retry that just succeeded would read as "reconnecting again in 2 s".
	 * retry_attempt survives: on a terminal FAILED it is how many attempts
	 * were spent, and REGISTERED has already zeroed it itself. */
	if (ev->u.reg.state != MEDUT_REG_RECONNECTING)
		bsdk_account_reconnect_reset(acct);

	ev->u.reg.retry_attempt  = acct->retry_attempt;
	ev->u.reg.retry_delay_ms = acct->retry_delay_ms;

	if (ev->u.reg.state != MEDUT_REG_RECONNECTING)
		return true;

	if (acct->last_rc_valid &&
	    acct->last_rc_error    == ev->u.reg.error &&
	    acct->last_rc_attempt  == acct->retry_attempt &&
	    acct->last_rc_delay_ms == acct->retry_delay_ms)
		return false;

	acct->last_rc_valid    = true;
	acct->last_rc_error    = ev->u.reg.error;
	acct->last_rc_attempt  = acct->retry_attempt;
	acct->last_rc_delay_ms = acct->retry_delay_ms;
	return true;
}

void bsdk_account_reg_reconnecting(struct medut_account *acct)
{
	medut_event_t ev = {0};

	if (!acct || acct->destroyed || !acct->reg_wanted)
		return;

	/* Only a registration that was up (or on its way up) has anything to
	 * lose here.  One that is already RECONNECTING keeps the state it has,
	 * and a terminal FAILED is not turned back into hope by a new link. */
	if (acct->reg_state != MEDUT_REG_REGISTERED &&
	    acct->reg_state != MEDUT_REG_REGISTERING)
		return;

	acct->reconnecting = true;
	acct->reg_state    = MEDUT_REG_RECONNECTING;
	/* The binding is stale and the path may be gone; probing it would only
	 * report the outage we already know about.  The re-REGISTER re-arms. */
	bsdk_account_keepalive_cancel(acct);

	ev.type          = MEDUT_EV_REG_STATE;
	ev.u.reg.state   = MEDUT_REG_RECONNECTING;
	ev.u.reg.error   = MEDUT_OK;
	ev.u.reg.account = acct;
	if (bsdk_account_reg_ev_prepare(acct, &ev))
		bsdk_event_post(&ev);
}

/* ── Registration watchdog (fires on re_main) ───────────────────────────────
 *
 * Registration state is reported to the consumer from baresip events. That is
 * the only signal, which makes a missed event indistinguishable from a
 * registration that is still in flight: the SDK sits in REGISTERING forever,
 * with no event, no error and no timeout, and the app has nothing to act on.
 * That has happened for real — see the FALLBACK_OK case in event.c — and a
 * REGISTER can also simply go unanswered.
 *
 * So do not trust the event stream as the only source of truth. After every
 * ua_register(), poll baresip's own view with ua_isregistered() and reconcile:
 *
 *   - registered, but the consumer was never told → emit REGISTERED (and warn,
 *     because a synthesised event means an event went missing upstream),
 *   - still nothing after the timeout → emit FAILED/TIMEOUT and hand over to
 *     the normal retry policy.
 *
 * The watchdog only ever *adds* a transition the consumer is owed; if the
 * event arrives normally, reg_state is already terminal and this is a no-op.
 *
 * The timeout is cfg.sip_timer_f_ms, defaulting to the 32 s of RFC 3261
 * Timer F.  That default is deliberately not *longer* than Timer F any more:
 * libre's own transaction timeout is a compile-time constant, so an app that
 * wants a REGISTER on a dead link to fail in eight seconds instead of
 * thirty-two has only this watchdog to say it with.  Both paths converge on
 * bsdk_account_schedule_retry(), which ignores a second request while a retry
 * is already armed, so whichever fires first wins and the other is a no-op.
 */

enum {
	BSDK_REG_WATCH_INTERVAL_MS = 500,
};

static uint32_t reg_watch_timeout_ms(void)
{
	return g_bsdk.cfg.sip_timer_f_ms;
}

static void reg_watch_handler(void *arg)
{
	struct medut_account *acct = arg;

	if (acct->destroyed || !acct->ua || !acct->reg_wanted)
		return;

	/* The consumer already has a terminal answer — nothing owed. */
	if (acct->reg_state == MEDUT_REG_REGISTERED ||
	    acct->reg_state == MEDUT_REG_FAILED)
		return;

	/* A retry is armed, so this REGISTER has already been answered for as
	 * far as the app is concerned (RECONNECTING, attempt N in M ms) and the
	 * next attempt re-arms this watchdog with a fresh budget.  Reporting a
	 * second timeout in the meantime would only overwrite the error the
	 * failure carried.  Note this deliberately keeps watching a RECONNECTING
	 * account with no timer running — a handover's re-REGISTER, and the
	 * retry that has already fired — so a REGISTER that vanishes without
	 * even a REGISTERING event still comes back here and gets retried. */
	if (tmr_isrunning(&acct->retry_tmr))
		return;

	if (ua_isregistered(acct->ua)) {
		medut_event_t ev = {0};

		warning("Medut: account %s is registered but baresip emitted "
		        "no event; synthesising REGISTERED\n",
		        acct->cfg_uri ? acct->cfg_uri : "(unknown)");

		post_registrar_warning("registration is up but the stack emitted "
		                       "no event; state was synthesised from "
		                       "ua_isregistered()");

		acct->reg_state     = MEDUT_REG_REGISTERED;
		acct->retry_attempt = 0;
		acct->reconnecting  = false;
		bsdk_account_keepalive_arm(acct);

		ev.type          = MEDUT_EV_REG_STATE;
		ev.u.reg.state   = MEDUT_REG_REGISTERED;
		ev.u.reg.error   = MEDUT_OK;
		ev.u.reg.account = acct;
		(void)bsdk_account_reg_ev_prepare(acct, &ev);
		bsdk_event_post(&ev);
		return;
	}

	acct->reg_watch_elapsed_ms += BSDK_REG_WATCH_INTERVAL_MS;

	if (reg_watch_timeout_ms() &&
	    acct->reg_watch_elapsed_ms >= reg_watch_timeout_ms()) {
		medut_event_t ev = {0};

		warning("Medut: account %s got no registration answer in "
		        "%u ms; reporting timeout\n",
		        acct->cfg_uri ? acct->cfg_uri : "(unknown)",
		        acct->reg_watch_elapsed_ms);

		acct->reg_error = MEDUT_ERR_TIMEOUT;
		acct->reg_state = bsdk_account_reg_fail_state(acct,
		                                             MEDUT_ERR_TIMEOUT);
		str_ncpy(acct->reg_error_str, "registration timed out",
		         sizeof(acct->reg_error_str));

		ev.type            = MEDUT_EV_REG_STATE;
		ev.u.reg.state     = acct->reg_state;
		ev.u.reg.error     = MEDUT_ERR_TIMEOUT;
		ev.u.reg.error_str = acct->reg_error_str;
		ev.u.reg.account   = acct;
		if (bsdk_account_reg_ev_prepare(acct, &ev))
			bsdk_event_post(&ev);

		bsdk_account_schedule_retry(acct);
		return;
	}

	tmr_start(&acct->reg_watch_tmr, BSDK_REG_WATCH_INTERVAL_MS,
	          reg_watch_handler, acct);
}

/* Arm the watchdog for a registration that was just requested. Call on
 * re_main, immediately after ua_register(). */
void bsdk_account_watch_registration(struct medut_account *acct)
{
	if (!acct || acct->destroyed || !acct->ua)
		return;

	acct->reg_watch_elapsed_ms = 0;
	tmr_start(&acct->reg_watch_tmr, BSDK_REG_WATCH_INTERVAL_MS,
	          reg_watch_handler, acct);
}

/* ── Retry timer (fires on re_main) ─────────────────────────────────────── */

/* ── RFC 3263 SRV failover ───────────────────────────────────────────────────
 *
 * SRV records exist so that a domain can name more than one proxy and say in
 * which order to try them.  A retry loop that re-sends to the same host it
 * just timed out on never consults that order, so a down primary is retried
 * forever while the secondary sits idle — which is the failure the records
 * were published to prevent.
 *
 * The lookup runs once per account, asynchronously, on first register; the
 * answer is kept as ready-to-use outbound-proxy URIs.  Each failed attempt
 * advances one target, wrapping at the end so a transient outage of every
 * proxy still returns to the primary.
 *
 * Deliberately skipped, because in each case there is no ordered list to walk
 * and pretending otherwise would override an explicit operator decision:
 *
 *   - an outbound proxy pinned in the account or global config,
 *   - a server given by IP literal (nothing to resolve),
 *   - an explicit port (RFC 3263 §4 step 1: skip NAPTR/SRV),
 *   - WS/WSS, where the server is a URL with a path rather than a SIP domain.
 */

static bool host_is_ip_literal(const char *host)
{
	struct sa sa;
	return host && sa_set_str(&sa, host, 0) == 0;
}

static bool srv_failover_eligible(const struct medut_account *acct)
{
	const medut_config_t *cfg = &g_bsdk.cfg;

	if (!cfg->dns_srv_failover)
		return false;
	if (acct->cfg.outbound || acct->cfg.outbound_proxy || cfg->outbound_proxy)
		return false;
	/* Only a server_url the APP supplied is an operator decision that
	 * overrides SRV.  auto_server_url is one WE synthesise (see the
	 * "Auto-generate server_url for all transports" branch in
	 * bsdk_account_create_h), and it is populated on every account that
	 * did not pass one — so testing it here made this predicate always
	 * false and the whole RFC 3263 failover path below dead code: the
	 * retry ladder re-sent to the same host forever, which is exactly
	 * what SRV records are published to prevent.  Rotating the outbound
	 * proxy (srv_advance) is independent of the registrar URI, so an
	 * auto-derived server_url stays valid while we walk the targets. */
	if (acct->cfg.server_url)
		return false;
	if (acct->parsed_transport == MEDUT_TRANSPORT_WS ||
	    acct->parsed_transport == MEDUT_TRANSPORT_WSS)
		return false;
	if (acct->cfg.server_port || acct->parsed_port)
		return false;

	{
		const char *host = (acct->cfg.server_host && acct->cfg.server_host[0])
		                   ? acct->cfg.server_host : acct->parsed_host;
		if (!host || !host[0] || host_is_ip_literal(host))
			return false;
	}

	return true;
}

/* Is this account still in the live list?  The DNS callback can outlive an
 * account the app destroyed while the query was in flight, and the pointer
 * would then dangle. */
static bool acct_is_live(const struct medut_account *acct)
{
	struct le *le;
	bool found = false;

	mtx_lock(&g_bsdk.acct_lock);
	LIST_FOREACH(&g_bsdk.accounts, le) {
		if (le->data == acct) {
			found = true;
			break;
		}
	}
	mtx_unlock(&g_bsdk.acct_lock);

	return found && !acct->destroyed;
}

static void srv_done_handler(const struct bsdk_dns_result *res, void *arg)
{
	struct medut_account *acct = arg;
	size_t n, i;

	if (!acct_is_live(acct))
		return;

	acct->srv_pending = false;

	if (bsdk_dns_result_err(res))
		return;

	n = bsdk_dns_result_count(res);
	for (i = 0; i < n && acct->srv_count < BSDK_SRV_MAX_TARGETS; i++) {
		medut_transport_t t;
		char     host[256];
		uint16_t port = 0;

		if (bsdk_dns_result_get(res, i, &t, host, sizeof(host), &port))
			continue;

		bsdk_build_outbound(NULL, host, port, t,
		                    acct->srv_uri[acct->srv_count],
		                    sizeof(acct->srv_uri[0]));
		if (acct->srv_uri[acct->srv_count][0])
			acct->srv_count++;
	}

	if (acct->srv_count > 1)
		info("Medut: %u SRV targets available for failover\n",
		     acct->srv_count);
}

void bsdk_account_srv_resolve(struct medut_account *acct)
{
	const char *host;

	if (!acct || acct->srv_tried || acct->srv_pending)
		return;
	if (!srv_failover_eligible(acct))
		return;

	acct->srv_tried = true;

	host = (acct->cfg.server_host && acct->cfg.server_host[0])
	       ? acct->cfg.server_host : acct->parsed_host;

	acct->srv_pending = true;
	/* port_hint 0 — eligibility already established there is no explicit
	 * port, which is what makes the NAPTR/SRV chain applicable. */
	if (bsdk_dns_resolve(host, acct->parsed_transport, 0,
	                     srv_done_handler, acct))
		acct->srv_pending = false;
}

/**
 * Point this account's outbound proxy at the next SRV target.
 * No-op unless there is more than one target to move between.
 */
static void srv_advance(struct medut_account *acct)
{
	struct account *ba;

	if (acct->srv_count < 2 || !acct->ua)
		return;

	acct->srv_idx = (uint8_t)((acct->srv_idx + 1u) % acct->srv_count);

	ba = ua_account(acct->ua);
	if (!ba)
		return;

	if (account_set_outbound(ba, acct->srv_uri[acct->srv_idx], 0)) {
		warning("Medut: SRV failover: could not set outbound %s\n",
		        acct->srv_uri[acct->srv_idx]);
		return;
	}

	info("Medut: SRV failover: trying proxy %u/%u (%s)\n",
	     acct->srv_idx + 1u, acct->srv_count,
	     acct->srv_uri[acct->srv_idx]);
}

static void retry_timer_handler(void *arg)
{
	struct medut_account *acct = arg;
	if (acct->destroyed || !acct->ua || !acct->reg_wanted)
		return;

	/* Move to the next proxy before re-sending, so consecutive attempts walk
	 * the SRV list instead of hammering one host. */
	srv_advance(acct);

	/* schedule_retry() already counted this one, and the RECONNECTING event
	 * the app got carries the same number — do not print it one ahead. */
	info("Medut: re-registering account (attempt %u)\n",
	     acct->retry_attempt);
	ua_register(acct->ua);
	bsdk_account_watch_registration(acct);
}

/* ── Keepalive / reachability probe ──────────────────────────────────────────
 *
 * A registration can be alive on paper and unreachable in fact: the local
 * address never changed, so handover sees nothing, and the next REGISTER
 * refresh is up to reg_expires away — an hour, at the default.  Meanwhile the
 * carrier NAT has dropped the UDP binding (30–180 s is typical) and every
 * inbound INVITE is delivered to a mapping that no longer exists.
 *
 * An OPTIONS request each interval fixes both halves: the request itself
 * refreshes the binding, and its answer — or the absence of one — is a
 * reachability test.  Any response counts as reachable, including a 405
 * Method Not Allowed: a proxy that refuses OPTIONS still had to receive it.
 */

static void keepalive_handler(void *arg);

static void keepalive_resp_handler(int err, const struct sip_msg *msg,
                                    void *arg)
{
	struct medut_account *acct = arg;
	medut_event_t ev = {0};

	if (!acct_is_live(acct))
		return;

	acct->ka_in_flight = false;

	/* Sent before the keepalive was last cancelled — by a handover, a failed
	 * REGISTER, an unregister.  The handover case is the one that bites: its
	 * transport flush destroys the connection without failing what is
	 * pending on it, so this is Timer F firing 32 s later for a socket that
	 * is already gone.  Acting on it reported a second reconnect, and a
	 * second re-REGISTER, right after the first one had succeeded.  The
	 * path it describes is not the one we are on; whoever cancelled owns
	 * the schedule now. */
	if (acct->ka_sent_gen != acct->ka_gen)
		return;

	if (!err && msg) {
		/* Reachable.  Normally nothing to report — an app that wants to
		 * see the probes can watch SIP tracing.
		 *
		 * Unless we told it the path was gone: a probe answering again is
		 * the only signal such a registration ever gets back, since the
		 * binding at the registrar never lapsed and no REGISTER is coming
		 * to report success.  Without this the app would sit on
		 * "Reconnecting…" until the next refresh, up to reg_expires away,
		 * with a working registration underneath. */
		if (acct->ka_failed) {
			acct->ka_failed = false;

			if (acct->reg_state == MEDUT_REG_RECONNECTING &&
			    ua_isregistered(acct->ua)) {
				medut_event_t ok = {0};

				info("Medut: keepalive probe answered again; "
				     "path to proxy recovered\n");

				acct->reg_state     = MEDUT_REG_REGISTERED;
				acct->reg_error     = MEDUT_OK;
				acct->retry_attempt = 0;
				acct->reconnecting  = false;
				acct->reg_error_str[0] = '\0';

				ok.type          = MEDUT_EV_REG_STATE;
				ok.u.reg.state   = MEDUT_REG_REGISTERED;
				ok.u.reg.error   = MEDUT_OK;
				ok.u.reg.account = acct;
				(void)bsdk_account_reg_ev_prepare(acct, &ok);
				bsdk_event_post(&ok);
			}
		}

		bsdk_account_keepalive_arm(acct);
		return;
	}

	warning("Medut: keepalive probe failed (%m); path to proxy is "
	        "unreachable\n", err ? err : ETIMEDOUT);

	/* The registrar answered our last REGISTER and has not refused
	 * anything since — the PATH to it died.  REG_STATE below reports the
	 * resulting reconnect, but not this cause, which is the one thing a
	 * diagnostics view needs to distinguish "the proxy rejected us" from
	 * "the network under us went away". */
	post_registrar_warning("keepalive probe went unanswered (%m); the path "
	                       "to the proxy is unreachable",
	                       err ? err : ETIMEDOUT);

	acct->ka_failed = true;
	acct->reg_error = MEDUT_ERR_TIMEOUT;

	/* Unreachable, not refused.  With keepalive_reregister the retry policy
	 * decides whether this is still recoverable; without it we keep probing,
	 * so an account the app wants registered is recovering either way and a
	 * later answer (above) is what ends it. */
	if (g_bsdk.cfg.keepalive_reregister && acct->reg_wanted) {
		acct->reg_state = bsdk_account_reg_fail_state(acct,
		                                             MEDUT_ERR_TIMEOUT);
	}
	else {
		acct->reg_state    = acct->reg_wanted ? MEDUT_REG_RECONNECTING
		                                      : MEDUT_REG_FAILED;
		acct->reconnecting = acct->reg_wanted;
	}

	str_ncpy(acct->reg_error_str, "keepalive probe timed out",
	         sizeof(acct->reg_error_str));

	if (g_bsdk.cfg.keepalive_reregister && acct->reg_wanted) {
		/* Straight to a retry rather than an immediate ua_register(): the
		 * retry path is what walks the SRV list and applies the backoff, and
		 * a proxy that just stopped answering is exactly when both matter.
		 *
		 * Armed before the failure is reported, as REGISTER_FAIL does
		 * (event.c): scheduling settles the attempt and the countdown, and
		 * schedule_retry() posts the RECONNECTING itself.  Reported first,
		 * every probe failure reached the app twice — attempt 0 with no
		 * countdown, then the same reconnect again with one. */
		bsdk_account_schedule_retry(acct);
	}
	else {
		bsdk_account_keepalive_arm(acct);
	}

	ev.type            = MEDUT_EV_REG_STATE;
	ev.u.reg.state     = acct->reg_state;
	ev.u.reg.error     = MEDUT_ERR_TIMEOUT;
	ev.u.reg.error_str = acct->reg_error_str;
	ev.u.reg.account   = acct;
	if (bsdk_account_reg_ev_prepare(acct, &ev))
		bsdk_event_post(&ev);
}

int bsdk_account_probe_send(struct medut_account *acct,
                            options_resp_h *resph, void *arg)
{
	char uri[320];
	const char *host;

	if (!acct || acct->destroyed || !acct->ua)
		return EINVAL;

	host = (acct->cfg.server_host && acct->cfg.server_host[0])
	       ? acct->cfg.server_host : acct->parsed_host;
	if (!host || !host[0])
		return EINVAL;

	/* Address the domain, not a specific proxy: baresip routes the request
	 * through the account's outbound proxy, which is what we are probing. */
	if (re_snprintf(uri, sizeof(uri), "sip:%s", host) < 0)
		return ENOMEM;

	return ua_options_send(acct->ua, uri, resph, arg);
}

static void keepalive_handler(void *arg)
{
	struct medut_account *acct = arg;
	int err;

	if (acct->destroyed || !acct->ua || !acct->reg_wanted)
		return;
	if (acct->ka_in_flight) {
		/* Previous probe still outstanding.  A current one re-arms from
		 * its own answer.  One from before the last cancel says nothing
		 * about this path and may not come back until Timer F, so keep
		 * the schedule running around it instead of stalling on it. */
		if (acct->ka_sent_gen != acct->ka_gen)
			bsdk_account_keepalive_arm(acct);
		return;
	}

	/* RTP on an active call already holds the NAT binding open, and an
	 * OPTIONS competing with media for a congested uplink is exactly the
	 * wrong request to add.  Skip this round. */
	if (!list_isempty(ua_calls(acct->ua))) {
		bsdk_account_keepalive_arm(acct);
		return;
	}

	acct->ka_in_flight = true;
	acct->ka_sent_gen  = acct->ka_gen;
	err = bsdk_account_probe_send(acct, keepalive_resp_handler, acct);
	if (err) {
		acct->ka_in_flight = false;
		/* No host to probe is not going to change on the next tick. */
		if (err != EINVAL)
			bsdk_account_keepalive_arm(acct);
	}
}

void bsdk_account_keepalive_arm(struct medut_account *acct)
{
	uint32_t iv;

	if (!acct || acct->destroyed || !acct->reg_wanted)
		return;

	iv = g_bsdk.cfg.keepalive_interval;
	if (!iv)
		return;

	tmr_start(&acct->ka_tmr, iv, keepalive_handler, acct);
}

void bsdk_account_keepalive_cancel(struct medut_account *acct)
{
	if (!acct)
		return;

	tmr_cancel(&acct->ka_tmr);
	/* A probe still in flight now answers for a path we have stopped
	 * asking about — see keepalive_resp_handler(). */
	acct->ka_gen++;
}

/**
 * Spread a retry delay out over ±`jitter`·delay.
 *
 * Every device that lost the same Wi-Fi runs the same backoff from the same
 * instant, so without this they all re-REGISTER in step and the registrar
 * takes the whole fleet as one burst — the outage turns into a thundering
 * herd on recovery, and the herd re-forms on every subsequent attempt because
 * the schedules never diverge.
 *
 * rand_u32(), not rand_u16(): reg_retry_max_ms defaults to 300000, so the
 * window can be 120000 ms wide and `rand_u16() % (span + 1)` would degenerate
 * to plain rand_u16() — capped at 65535, it could only ever shorten such a
 * delay, biasing the whole fleet earlier instead of spreading it.
 */
static uint32_t apply_jitter(uint32_t delay, float jitter)
{
	uint32_t span;
	int32_t  offset;

	if (jitter <= 0.f || delay == 0)
		return delay;
	if (jitter > 1.f)
		jitter = 1.f;

	/* Full width of the window, i.e. 2·jitter·delay. */
	span = (uint32_t)((float)delay * jitter * 2.f);
	if (span == 0)
		return delay;

	offset = (int32_t)(rand_u32() % (span + 1)) - (int32_t)(span / 2);
	if (offset < 0 && (uint32_t)(-offset) >= delay)
		return 1;   /* never schedule a zero-delay retry */

	return (uint32_t)((int32_t)delay + offset);
}

void bsdk_account_schedule_retry(struct medut_account *acct)
{
	const medut_config_t *cfg = &g_bsdk.cfg;

	uint32_t initial_ms   = acct->retry_policy_set ? acct->retry_initial_ms   : cfg->reg_retry_initial_ms;
	uint32_t max_ms       = acct->retry_policy_set ? acct->retry_max_ms       : cfg->reg_retry_max_ms;
	float    backoff      = acct->retry_policy_set ? acct->retry_backoff      : cfg->reg_retry_backoff;

	/* The app has let go of this registration.  The failure that got here
	 * is its own un-REGISTER — libre reports one that cannot be sent as a
	 * REGISTER_FAIL — and retrying it would REGISTER the account again. */
	if (acct->destroyed || !acct->reg_wanted)
		return;

	/* A failure can reach us twice for the same REGISTER — the watchdog and
	 * baresip's own transaction timeout both report it — and once from a
	 * failed keepalive probe on top.  Without this guard each one would
	 * bump retry_attempt and re-arm the timer, so the backoff would climb
	 * at two or three times the configured rate and blow through
	 * max_attempts early. */
	if (tmr_isrunning(&acct->retry_tmr))
		return;

	if (!retry_budget_left(acct)) {
		medut_event_t done = {0};

		info("Medut: max retry attempts reached for account\n");

		acct->reconnecting = false;

		/* Nothing owed unless the app is still holding RECONNECTING, which
		 * promises another attempt: going quiet on that would leave it
		 * rendering "Reconnecting…" for ever with nothing left to reconnect
		 * it.  A caller that already reported FAILED (an exhausted budget is
		 * visible to bsdk_account_reg_fail_state() too) needs no second
		 * event. */
		if (acct->reg_state != MEDUT_REG_RECONNECTING)
			return;

		acct->reg_state = MEDUT_REG_FAILED;

		done.type            = MEDUT_EV_REG_STATE;
		done.u.reg.state     = MEDUT_REG_FAILED;
		done.u.reg.error     = acct->reg_error;
		done.u.reg.error_str = acct->reg_error_str[0]
		        ? acct->reg_error_str : NULL;
		done.u.reg.account   = acct;
		(void)bsdk_account_reg_ev_prepare(acct, &done);
		bsdk_event_post(&done);
		return;
	}

	uint32_t delay = initial_ms;
	for (uint32_t i = 0; i < acct->retry_attempt; i++) {
		delay = (uint32_t)((float)delay * backoff);
		if (delay >= max_ms) {
			delay = max_ms;
			break;
		}
	}

	/* Jitter is global only: medut_account_set_retry_policy() predates it
	 * and has no parameter for it, so there is no per-account value to
	 * prefer here. */
	delay = apply_jitter(delay, cfg->reg_retry_jitter);

	acct->retry_attempt++;
	/* Published on every RECONNECTING event from here until the retry
	 * resolves, so they all agree on the countdown. */
	acct->retry_delay_ms = delay;

	/* Post retry event so consumer can show UI.  RECONNECTING, with the
	 * attempt and the delay: an attempt is armed, so this is a countdown and
	 * not a failure the app has to act on. */
	acct->reconnecting = true;
	acct->reg_state    = MEDUT_REG_RECONNECTING;

	medut_event_t ev = {0};
	ev.type            = MEDUT_EV_REG_STATE;
	ev.u.reg.state     = MEDUT_REG_RECONNECTING;
	ev.u.reg.error     = acct->reg_error;
	ev.u.reg.error_str = acct->reg_error_str[0]
	        ? acct->reg_error_str : NULL;
	ev.u.reg.account   = acct;
	if (bsdk_account_reg_ev_prepare(acct, &ev))
		bsdk_event_post(&ev);

	tmr_start(&acct->retry_tmr, delay, retry_timer_handler, acct);
}

/* ── account_destructor ──────────────────────────────────────────────────── */

static void account_destructor(void *data)
{
	struct medut_account *acct = data;
	tmr_cancel(&acct->retry_tmr);
	tmr_cancel(&acct->reg_watch_tmr);
	tmr_cancel(&acct->ka_tmr);
	if (acct->ws_port) {
		bsdk_ws_unset_server(acct->parsed_transport, acct->ws_host,
		                     acct->ws_port, acct->ws_pin_path);
		acct->ws_port = 0;
	}
	struct le *le, *le_tmp;
	LIST_FOREACH_SAFE(&acct->custom_hdrs, le, le_tmp) {
		struct bsdk_custom_hdr *hdr = le->data;
		list_unlink(&hdr->le);
		mem_deref(hdr);
	}
	bsdk_acct_cfg_deep_free(acct);
}

/* ── bsdk_account_find_by_ua ─────────────────────────────────────────────── */

struct medut_account *bsdk_account_find_by_ua(const struct ua *ua)
{
	struct le *le;
	mtx_lock(&g_bsdk.acct_lock);
	LIST_FOREACH(&g_bsdk.accounts, le) {
		struct medut_account *acct = le->data;
		if (acct->ua == ua) {
			mtx_unlock(&g_bsdk.acct_lock);
			return acct;
		}
	}
	mtx_unlock(&g_bsdk.acct_lock);
	return NULL;
}

/* ── Public API — dispatch wrappers ─────────────────────────────────────── */

typedef struct {
	medut_account_config_t  cfg;
	medut_account_handle_t *out;
	int                       result;
} create_ctx_t;

static void create_fn(void *arg)
{
	create_ctx_t *ctx = arg;
	int err;

	struct medut_account *acct = mem_alloc(sizeof(*acct), account_destructor);
	if (!acct) { ctx->result = ENOMEM; return; }
	memset(acct, 0, sizeof(*acct));
	tmr_init(&acct->retry_tmr);
	tmr_init(&acct->reg_watch_tmr);
	tmr_init(&acct->ka_tmr);
	list_init(&acct->custom_hdrs);

	bsdk_acct_cfg_deep_copy(&acct->cfg, &ctx->cfg, acct);
	acct->reg_state = MEDUT_REG_UNREGISTERED;

	if (!acct->cfg.uri) {
		mem_deref(acct);
		ctx->result = MEDUT_ERR_INVAL;
		return;
	}

	/* RFC 8599: the push binding rides in the Contact of every REGISTER.
	 * A token too long for it is refused here rather than sent truncated. */
	err = push_cparams_update(acct);
	if (err) {
		mem_deref(acct);
		ctx->result = err;
		return;
	}

	/* Parse "user@host" or "user@host:port" from uri */
	parse_account_uri(acct->cfg.uri,
	                  acct->parsed_user, sizeof(acct->parsed_user),
	                  acct->parsed_host, sizeof(acct->parsed_host),
	                  &acct->parsed_port);

	/* Determine transport: server_url → account transport → global default */
	medut_transport_t tp = acct->cfg.transport;
	char ws_path[256] = ""; /* set only if server_url contains an explicit path */
	char ws_host[256] = ""; /* WS server authority — pinned in ws_path.c */
	uint16_t ws_port = 0;

	if (acct->cfg.server_url) {
		char sv_host[256];
		uint16_t sv_port;
		bsdk_parse_server_url(acct->cfg.server_url, &tp,
		                      sv_host, sizeof(sv_host),
		                      &sv_port, ws_path, sizeof(ws_path));
		if (!acct->parsed_host[0])
			str_ncpy(acct->parsed_host, sv_host, sizeof(acct->parsed_host));
		/* The WS server is the server_url host, which may legitimately
		 * differ from the AOR domain (edge proxy vs SIP domain).  So its
		 * port is where the connection goes (the outbound proxy takes it
		 * from the URL), not part of the identity: parsed_port stays the
		 * uri's own.  Copying it here put ":443" in every From, To and
		 * Request-URI, and an edge proxy's port on the SIP domain. */
		str_ncpy(ws_host, sv_host, sizeof(ws_host));
		ws_port = sv_port;
	} else {
		/* Auto-generate server_url for all transports */
		uint16_t port = acct->parsed_port;
		if (!port) {
			switch (tp) {
			/* WebSocket defaults, as in bsdk_parse_server_url. */
			case MEDUT_TRANSPORT_WSS: port = 443; break;
			case MEDUT_TRANSPORT_WS:  port = 80; break;
			case MEDUT_TRANSPORT_TLS: port = 5061; break;
			case MEDUT_TRANSPORT_TCP: port = 5060; break;
			case MEDUT_TRANSPORT_UDP:
			default:                     port = 5060; break;
			}
		}
		const char *scheme;
		switch (tp) {
		case MEDUT_TRANSPORT_WSS: scheme = "wss"; break;
		case MEDUT_TRANSPORT_WS:  scheme = "ws";  break;
		case MEDUT_TRANSPORT_TLS: scheme = "sips"; break;
		case MEDUT_TRANSPORT_TCP: scheme = "sip";  break;
		case MEDUT_TRANSPORT_UDP:
		default:                     scheme = "sip"; break;
		}

		re_snprintf(acct->auto_server_url, sizeof(acct->auto_server_url),
		            "%s://%s:%u%s", scheme, acct->parsed_host, port, ws_path);

		str_ncpy(ws_host, acct->parsed_host, sizeof(ws_host));
		ws_port = port;
	}
	acct->parsed_transport = tp;

	/* Claim the server + explicit path for __wrap_websock_connect
	 * (ws_path.c).  Released in account_destructor, so a destroyed account
	 * stops counting against connection pinning. */
	if (tp == MEDUT_TRANSPORT_WS || tp == MEDUT_TRANSPORT_WSS) {
		str_ncpy(acct->ws_host, ws_host, sizeof(acct->ws_host));
		str_ncpy(acct->ws_pin_path, ws_path, sizeof(acct->ws_pin_path));
		acct->ws_port = ws_port;
		bsdk_ws_set_server(tp, acct->ws_host, acct->ws_port,
		                   acct->ws_pin_path);
	}

	/* Build AOR: sip:user@host[:port];transport=proto
	 * IPv6 literals must be wrapped in brackets per RFC 3261. */
	char aor[512];
	bool ipv6 = strchr(acct->parsed_host, ':') != NULL;
	/* cfg.reg_refresh_pct is the documented "refresh at N% of expires" knob
	 * (default 75).  It used to be written by medut_config_init() and read
	 * NOWHERE, so an app tuning it silently got libre's built-in 90% —
	 * worse than absent, because the config, the docs and four bindings all
	 * advertise it.  baresip takes this as the AOR ";rwait=" parameter
	 * (account.c: param_u32(&acc->rwait, ..., "rwait")) and hands it to
	 * sipreg_set_rwait(), which clamps to 5..95; 0 means "leave libre's
	 * default alone", so an out-of-range value is dropped rather than
	 * silently clamped to something the app did not ask for. */
	char rwait[24] = "";
	if (g_bsdk.cfg.reg_refresh_pct >= 5 && g_bsdk.cfg.reg_refresh_pct <= 95) {
		re_snprintf(rwait, sizeof(rwait), ";rwait=%u",
		            (unsigned)g_bsdk.cfg.reg_refresh_pct);
	}
	if (acct->parsed_port) {
		re_snprintf(aor, sizeof(aor),
		            ipv6 ? "sip:%s@[%s]:%u;transport=%s%s"
		                 : "sip:%s@%s:%u;transport=%s%s",
		            acct->parsed_user, acct->parsed_host,
		            (unsigned)acct->parsed_port, bsdk_transport_str(tp),
		            rwait);
	} else {
		re_snprintf(aor, sizeof(aor),
		            ipv6 ? "sip:%s@[%s];transport=%s%s"
		                 : "sip:%s@%s;transport=%s%s",
		            acct->parsed_user, acct->parsed_host,
		            bsdk_transport_str(tp), rwait);
	}

	err = ua_alloc(&acct->ua, aor);
	if (err) { mem_deref(acct); ctx->result = err; return; }

	err = configure_baresip_account(acct);
	if (err) {
		/* The UA is not owned by account_destructor (destroy_fn releases
		 * it), so drop it here before the wrapper. */
		acct->ua = mem_deref(acct->ua);
		mem_deref(acct);
		ctx->result = err;
		return;
	}

	mtx_lock(&g_bsdk.acct_lock);
	list_append(&g_bsdk.accounts, &acct->le, acct);
	mtx_unlock(&g_bsdk.acct_lock);

	*ctx->out = acct;
	ctx->result = 0;
}

int medut_account_create(const medut_account_config_t *cfg,
                            medut_account_handle_t *out)
{
	if (!cfg || !cfg->uri || !out)
		return MEDUT_ERR_INVAL;

	create_ctx_t ctx = { .cfg = *cfg, .out = out, .result = 0 };
	int err = bsdk_dispatch_sync(create_fn, &ctx);
	return err ? err : ctx.result;
}

static void destroy_fn(void *arg)
{
	struct medut_account *acct = arg;
	acct->destroyed = true;
	tmr_cancel(&acct->retry_tmr);
	tmr_cancel(&acct->reg_watch_tmr);
	if (acct->ua) {
		ua_hangup(acct->ua, NULL, 0, NULL);
		mem_deref(acct->ua);
		acct->ua = NULL;
	}
	mtx_lock(&g_bsdk.acct_lock);
	list_unlink(&acct->le);
	mtx_unlock(&g_bsdk.acct_lock);
	mem_deref(acct);
}

void medut_account_destroy(medut_account_handle_t acct)
{
	if (!acct) return;
	bsdk_dispatch_sync(destroy_fn, acct);
}

/* ── Enumeration & state readers ─────────────────────────────────────────── */

void medut_account_foreach(medut_account_iter_fn fn, void *arg)
{
	if (!fn) return;

	mtx_lock(&g_bsdk.acct_lock);
	struct le *le;
	LIST_FOREACH(&g_bsdk.accounts, le) {
		struct medut_account *acct = le->data;
		if (!acct->destroyed)
			fn((medut_account_handle_t)acct, arg);
	}
	mtx_unlock(&g_bsdk.acct_lock);
}

int medut_account_get_aor(medut_account_handle_t acct, char *buf, size_t sz)
{
	if (!acct || !buf || sz == 0)
		return MEDUT_ERR_INVAL;

	/* parsed_user/parsed_host are set once at create time and never
	 * mutated, so no lock is needed. */
	int n = re_snprintf(buf, sz, strchr(acct->parsed_host, ':')
	                             ? "sip:%s@[%s]" : "sip:%s@%s",
	                    acct->parsed_user, acct->parsed_host);
	if (n < 0) {
		buf[0] = '\0';
		return MEDUT_ERR_NOMEM;
	}
	return MEDUT_OK;
}

medut_reg_state_t medut_account_get_reg_state(medut_account_handle_t acct)
{
	if (!acct) return MEDUT_REG_UNREGISTERED;
	return acct->reg_state;
}

static void register_fn(void *arg)
{
	struct medut_account *acct = arg;
	if (acct->ua) {
		acct->reg_wanted = true;   /* netmon.c re-REGISTERs on handover */
		/* An explicit register() is a fresh intent, not the tail of a
		 * recovery: report it as REGISTERING even if the account was
		 * RECONNECTING when the app asked. */
		acct->reconnecting = false;
		/* Fire the SRV lookup alongside the first REGISTER rather than
		 * before it: the answer is only needed if this attempt fails, and
		 * delaying the REGISTER on a DNS round-trip would slow down every
		 * successful registration to help the rare failing one. */
		bsdk_account_srv_resolve(acct);
		ua_register(acct->ua);
		bsdk_account_watch_registration(acct);
	}
}

int medut_account_register(medut_account_handle_t acct)
{
	if (!acct) return MEDUT_ERR_INVAL;
	return bsdk_dispatch(register_fn, acct);
}

/* Defined by job 11 of cmake/patch-baresip-sources.cmake. */
extern int bsdk_ua_refresh_register(struct ua *ua);

static void refresh_registration_fn(void *arg)
{
	struct medut_account *acct = arg;
	if (acct->destroyed || !acct->ua)
		return;

	/* A binding to refresh, or a REGISTER already on its way: that is the
	 * refresh.  Anything else (never registered, failed, mid-retry) needs
	 * a REGISTER from a new client, as register() sends. */
	if (acct->reg_state == MEDUT_REG_REGISTERED &&
	    0 == bsdk_ua_refresh_register(acct->ua)) {
		acct->reg_wanted = true;
		return;
	}
	register_fn(acct);
}

int medut_account_refresh_registration(medut_account_handle_t acct)
{
	if (!acct) return MEDUT_ERR_INVAL;
	return bsdk_dispatch(refresh_registration_fn, acct);
}

static void unregister_fn(void *arg)
{
	struct medut_account *acct = arg;
	if (acct->ua) {
		acct->reg_wanted   = false;
		acct->reconnecting = false;
		acct->ka_failed    = false;
		acct->reg_state = MEDUT_REG_UNREGISTERING;
		tmr_cancel(&acct->reg_watch_tmr);
		tmr_cancel(&acct->retry_tmr);
		bsdk_account_keepalive_cancel(acct);
		ua_unregister(acct->ua);
	}
}

int medut_account_unregister(medut_account_handle_t acct)
{
	if (!acct) return MEDUT_ERR_INVAL;
	return bsdk_dispatch(unregister_fn, acct);
}

static void keepalive_now_fn(void *arg)
{
	struct medut_account *acct = arg;

	/* Cancel the pending tick first: keepalive_handler() re-arms on every
	 * exit path, so firing it early without this would leave the schedule
	 * shifted by however long was left on the old timer. */
	tmr_cancel(&acct->ka_tmr);
	keepalive_handler(acct);
}

int medut_account_keepalive_now(medut_account_handle_t acct)
{
	if (!acct)
		return MEDUT_ERR_INVAL;
	if (!acct->ua || acct->destroyed || !acct->reg_wanted)
		return MEDUT_ERR_STATE;

	return bsdk_dispatch(keepalive_now_fn, acct);
}

/* ── Retry control ───────────────────────────────────────────────────────── */

typedef struct {
	medut_account_handle_t acct;
	uint32_t initial_ms;
	uint32_t max_ms;
	float    backoff;
	uint32_t max_attempts;
} retry_policy_ctx_t;

static void set_retry_policy_fn(void *arg)
{
	retry_policy_ctx_t *ctx = arg;
	struct medut_account *acct = ctx->acct;
	acct->retry_policy_set   = true;
	acct->retry_initial_ms   = ctx->initial_ms;
	acct->retry_max_ms       = ctx->max_ms;
	acct->retry_backoff      = ctx->backoff;
	acct->retry_max_attempts = ctx->max_attempts;
}

int medut_account_set_retry_policy(medut_account_handle_t acct,
                                      uint32_t initial_ms, uint32_t max_ms,
                                      float backoff, uint32_t max_attempts)
{
	if (!acct) return MEDUT_ERR_INVAL;
	retry_policy_ctx_t ctx = { acct, initial_ms, max_ms, backoff, max_attempts };
	return bsdk_dispatch_sync(set_retry_policy_fn, &ctx);
}

static void cancel_retry_fn(void *arg)
{
	struct medut_account *acct = arg;

	tmr_cancel(&acct->retry_tmr);
	tmr_cancel(&acct->reg_watch_tmr);
	acct->retry_attempt = 0;

	/* The recovery was the SDK's and the app just took it away, so the
	 * registration is now down for good as far as the SDK is concerned.
	 * Report it, or the app is left rendering "Reconnecting…" against a
	 * retry it cancelled itself. */
	if (acct->reg_state == MEDUT_REG_RECONNECTING) {
		medut_event_t ev = {0};

		acct->reconnecting = false;
		acct->reg_state    = MEDUT_REG_FAILED;

		ev.type            = MEDUT_EV_REG_STATE;
		ev.u.reg.state     = MEDUT_REG_FAILED;
		ev.u.reg.error     = acct->reg_error;
		ev.u.reg.error_str = acct->reg_error_str[0]
		        ? acct->reg_error_str : NULL;
		ev.u.reg.account   = acct;
		bsdk_event_post(&ev);
	}
}

int medut_account_cancel_retry(medut_account_handle_t acct)
{
	if (!acct) return MEDUT_ERR_INVAL;
	return bsdk_dispatch_sync(cancel_retry_fn, acct);
}

static void retry_now_fn(void *arg)
{
	struct medut_account *acct = arg;
	tmr_cancel(&acct->retry_tmr);
	acct->retry_attempt = 0;
	if (!acct->destroyed && acct->ua) {
		acct->reg_wanted = true;
		ua_register(acct->ua);
		bsdk_account_watch_registration(acct);
	}
}

int medut_account_retry_now(medut_account_handle_t acct)
{
	if (!acct) return MEDUT_ERR_INVAL;
	return bsdk_dispatch_sync(retry_now_fn, acct);
}

/* ── ICE server runtime update ──────────────────────────────────────────── */

typedef struct {
	medut_account_handle_t        acct;
	const medut_ice_server_t     *servers;   /* caller-owned; copied under
	                                              bsdk_dispatch_sync */
	uint32_t                        count;
	medut_ice_transport_policy_t  policy;
	int                             err;
} set_ice_servers_ctx_t;

static void set_ice_servers_fn(void *arg)
{
	set_ice_servers_ctx_t *ctx = arg;
	struct medut_account *acct = ctx->acct;

	ctx->err = bsdk_ice_servers_copy(&acct->ice_srvv, &acct->ice_srv_api,
	                                 &acct->ice_srvc, ctx->servers,
	                                 ctx->count);
	acct->cfg.ice_servers      = acct->ice_srv_api;
	acct->cfg.ice_server_count = acct->ice_srvc;
	acct->ice_policy           = ctx->policy;
	acct->cfg.ice_transport_policy = ctx->policy;

	info("Medut/account: %s: ICE servers replaced (%u entr%s, policy %s)"
	     " — applies to the next gather or restart\n",
	     acct->cfg.uri ? acct->cfg.uri : "?", acct->ice_srvc,
	     acct->ice_srvc == 1 ? "y" : "ies",
	     ctx->policy == MEDUT_ICE_TRANSPORT_RELAY ? "relay" : "all");
}

int medut_account_set_ice_servers(medut_account_handle_t acct,
                                    const medut_ice_server_t *servers,
                                    uint32_t count,
                                    medut_ice_transport_policy_t policy)
{
	set_ice_servers_ctx_t ctx = { .acct = acct, .servers = servers,
	                              .count = servers ? count : 0,
	                              .policy = policy };
	int err;

	if (!acct) return MEDUT_ERR_INVAL;
	err = bsdk_dispatch_sync(set_ice_servers_fn, &ctx);
	return err ? err : ctx.err;
}

/* ── Push token runtime update ──────────────────────────────────────────── */

typedef struct {
	medut_account_handle_t  acct;
	/* Caller-owned string pointer. Safe because bsdk_dispatch_sync blocks
	 * the caller until set_push_token_fn returns, so the string cannot be
	 * freed before bsdk_strdup() copies it. */
	const char               *push_token;
	int                       result;
} push_token_ctx_t;

static void set_push_token_fn(void *arg)
{
	push_token_ctx_t *ctx = arg;
	struct medut_account *acct = ctx->acct;

	if (!acct->ua) {
		ctx->result = MEDUT_ERR_STATE;
		return;
	}

	/* All or nothing: a token too long for the Contact leaves the old
	 * one, and the binding it is in, as they were. */
	char *old_token = acct->cfg_push_token;
	acct->cfg_push_token = bsdk_strdup(ctx->push_token);
	acct->cfg.push_token = acct->cfg_push_token;

	ctx->result = push_cparams_update(acct);
	if (ctx->result) {
		mem_deref(acct->cfg_push_token);
		acct->cfg_push_token = old_token;
		acct->cfg.push_token = old_token;
		return;
	}
	mem_deref(old_token);

	/* The new params reach the wire with the next client reg_register()
	 * allocates, so send a REGISTER now (RFC 8599 s4.1.3) — unless one is in
	 * flight or a retry is armed.  A retry re-registers anyway; an
	 * in-flight one is answered first, and its REGISTERED event sends
	 * the refresh (bsdk_account_push_settled). */
	if (!acct->reg_wanted)
		return;   /* the app's own register() will carry them */
	if (acct->cfg.push_provider == MEDUT_PUSH_PROVIDER_NONE)
		return;   /* no push binding: nothing on the wire changes */

	bool reg_in_flight = (acct->reg_state == MEDUT_REG_REGISTERING ||
	                      acct->reg_state == MEDUT_REG_UNREGISTERING ||
	                      acct->reg_state == MEDUT_REG_RECONNECTING);
	bool retry_pending = tmr_isrunning(&acct->retry_tmr);

	if (retry_pending)
		return;
	if (reg_in_flight) {
		acct->pn_refresh = true;
		return;
	}
	ua_register(acct->ua);
}

int medut_account_set_push_token(medut_account_handle_t acct,
                                    const char *push_token)
{
	if (!acct) return MEDUT_ERR_INVAL;
	push_token_ctx_t ctx = { .acct = acct, .push_token = push_token,
	                          .result = 0 };
	int err = bsdk_dispatch_sync(set_push_token_fn, &ctx);
	return err ? err : ctx.result;
}
