/**
 * @file ua.c  A SIP user agent: one medut account on the stack
 *
 * Holds what the account resolved to (mdt_ua_prm), the registration
 * clients, the calls, and the user agent's own SIP identity: the Contact
 * user, the extensions it supports, the methods it allows.
 *
 * Adapted from baresip's src/ua.c, src/account.c and src/custom_hdrs.c
 * (BSD-3-Clause, Copyright (C) 2010 Alfred E. Heggestad); see
 * THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include "mdt_sip.h"

/* ── Custom headers ──────────────────────────────────────────────────────── */

static void hdr_destructor(void *arg)
{
	struct sip_hdr *hdr = arg;

	mem_deref((char *)hdr->name.p);
	mem_deref((char *)hdr->val.p);
}

int mdt_hdrs_add(struct list *hdrs, const char *name, const char *fmt, ...)
{
	struct sip_hdr *hdr;
	struct pl npl;
	char *value = NULL;
	va_list ap;
	int err;

	if (!hdrs || !name || !fmt)
		return EINVAL;

	va_start(ap, fmt);
	err = re_vsdprintf(&value, fmt, ap);
	va_end(ap);
	if (err)
		return err;

	hdr = mem_zalloc(sizeof(*hdr), hdr_destructor);
	if (!hdr) {
		mem_deref(value);
		return ENOMEM;
	}

	pl_set_str(&npl, name);
	err = pl_dup(&hdr->name, &npl);
	if (err) {
		mem_deref(value);
		mem_deref(hdr);
		return err;
	}

	pl_set_str(&hdr->val, value);
	hdr->id = SIP_HDR_NONE;

	list_append(hdrs, &hdr->le, hdr);
	return 0;
}

int mdt_hdrs_print(struct re_printf *pf, const struct list *hdrs)
{
	struct le *le;
	int err = 0;

	LIST_FOREACH(hdrs, le) {
		const struct sip_hdr *hdr = le->data;

		err = re_hprintf(pf, "%r: %r\r\n", &hdr->name, &hdr->val);
		if (err)
			return err;
	}

	return 0;
}

/* ── Identity ────────────────────────────────────────────────────────────── */

const char *mdt_ua_cuser(const struct mdt_ua *ua)
{
	if (!ua)
		return NULL;

	return str_isset(ua->pub_gruu) ? ua->pub_gruu : ua->cuser;
}

int mdt_ua_auth(char **username, char **password, const char *realm,
		 void *arg)
{
	const struct mdt_ua *ua = arg;
	int err = 0;
	(void)realm;

	if (!ua)
		return EINVAL;

	if (ua->auth_user)
		*username = mem_ref(ua->auth_user);
	else
		err = pl_strdup(username, &ua->luri.user);

	*password = mem_ref(ua->auth_pass);

	return err;
}

const struct sa *mdt_ua_regladdr(const struct mdt_ua *ua)
{
	struct le *le;

	LIST_FOREACH(&ua->regl, le) {
		const struct mdt_reg *reg = le->data;
		if (mdt_reg_isok(reg))
			return mdt_reg_laddr(reg);
	}

	return NULL;
}

/* Our end of the connection the live registration was last answered on — for
 * WS/WSS the connection's own address, not the listen address the REGISTER
 * names.  NULL when not registered. */
const struct sa *mdt_ua_reg_conn_laddr(const struct mdt_ua *ua)
{
	struct le *le;

	LIST_FOREACH(&ua->regl, le) {
		const struct mdt_reg *reg = le->data;
		if (mdt_reg_isok(reg))
			return mdt_reg_conn_laddr(reg);
	}

	return NULL;
}

/* RFC 5626 needs the instance id: without one, register as a plain UA. */
static bool ua_outbound(const struct mdt_ua *ua)
{
	return ua->sip_outbound && g_sip.uuid[0];
}

/* The option-tags the UA supports, in the order they are advertised. */
static size_t ua_extensions(const struct mdt_ua *ua, const char *v[8])
{
	size_t n = 0;

	if (g_sip.uuid[0])
		v[n++] = "gruu";
	if (ua_outbound(ua)) {
		v[n++] = "path";
		v[n++] = "outbound";
	}
	v[n++] = "replaces";
	v[n++] = "norefersub";
	if (ua->mnat && !str_casecmp(ua->mnat->id, "ice"))
		v[n++] = "ice";
	if (ua->rel100 != REL100_DISABLED)
		v[n++] = "100rel";

	return n;
}

bool mdt_ua_supports(const struct mdt_ua *ua, const struct pl *tag)
{
	const char *v[8];
	size_t i, n;

	if (!ua || !tag)
		return false;

	n = ua_extensions(ua, v);
	for (i = 0; i < n; i++) {
		if (!pl_strcasecmp(tag, v[i]))
			return true;
	}

	return false;
}

int mdt_ua_print_supported(struct re_printf *pf, const struct mdt_ua *ua)
{
	const char *v[8];
	size_t i, n;
	int err;

	if (!ua)
		return 0;

	n = ua_extensions(ua, v);

	err = re_hprintf(pf, "Supported:");
	for (i = 0; i < n; i++)
		err |= re_hprintf(pf, "%s%s", i ? "," : " ", v[i]);
	err |= re_hprintf(pf, "\r\n");

	return err;
}

int mdt_ua_print_allowed(struct re_printf *pf, const struct mdt_ua *ua)
{
	int err;

	if (!ua)
		return 0;

	err = re_hprintf(pf, "INVITE,ACK,BYE,CANCEL,OPTIONS,"
			 "NOTIFY,INFO,MESSAGE,UPDATE");

	if (g_sip.subh)
		err |= re_hprintf(pf, ",SUBSCRIBE");

	if (ua->rel100 != REL100_DISABLED)
		err |= re_hprintf(pf, ",PRACK");

	err |= re_hprintf(pf, ",REFER");

	return err;
}

int mdt_ua_print_require(struct re_printf *pf, const struct mdt_ua *ua)
{
	if (!ua || ua->rel100 != REL100_REQUIRED)
		return 0;

	return re_hprintf(pf, "Require: 100rel\r\n");
}

/* An OPTIONS addressed to the UA (a reachability ping or a capability
 * query): what the UA allows and supports, and where to reach it. */
void mdt_ua_handle_options(struct mdt_ua *ua, const struct sip_msg *msg)
{
	struct sip_contact contact;
	int err;

	sip_contact_set(&contact, mdt_ua_cuser(ua), &msg->dst, msg->tp);

	err = sip_treplyf(NULL, NULL, g_sip.sip, msg, true, 200, "OK",
			  "Allow: %H\r\n"
			  "%H"
			  "%H"
			  "Content-Length: 0\r\n"
			  "\r\n",
			  mdt_ua_print_allowed, ua,
			  mdt_ua_print_supported, ua,
			  sip_contact_print, &contact);
	if (err)
		MDT_WARN("sip: reply to OPTIONS: %m\n", err);
}

/* Complete a dial string (or a MESSAGE / presence target) into a URI: add "sip:", and the account's
 * domain (and port, when not 5060) to anything that is not an IP literal and
 * has no host.  A strict registrar drops "INVITE sip:*43" at the parser
 * rather than answer it (Asterisk: "PJSIP syntax error ... Request Line"). */
int mdt_ua_uri_complete(const struct mdt_ua *ua, char **strp,
			 const char *dial)
{
	struct pl trimmed, user, params = {NULL, 0};
	struct mbuf *mb;
	size_t i;
	int err = 0;

	pl_set_str(&trimmed, dial);
	err = pl_ltrim(&trimmed);
	if (err)
		return err;

	mb = mbuf_alloc(64);
	if (!mb)
		return ENOMEM;

	if (!re_regex(trimmed.p, trimmed.l, "sip:")) {
		err = mbuf_write_pl(mb, &trimmed);
		goto out;
	}

	/* "600;transport=udp": the user part, then the URI params (and
	 * headers) — the domain goes between them, not after. */
	user = trimmed;
	for (i = 0; i < trimmed.l; i++) {
		if (trimmed.p[i] == ';' || trimmed.p[i] == '?') {
			user.l     = i;
			params.p   = trimmed.p + i;
			params.l   = trimmed.l - i;
			break;
		}
	}

	err |= mbuf_printf(mb, "sip:");
	err |= mbuf_write_pl(mb, &user);
	if (err)
		goto out;

	{
		char *host = NULL, *c;
		struct sa sa;
		bool is_ip;

		err = pl_strdup(&host, &user);
		if (err)
			goto out;

		is_ip = !sa_decode(&sa, host, strlen(host)) ||
			!sa_set_str(&sa, host, 0);
		if (!is_ip && host[0] == '[' && (c = strchr(host, ']'))) {
			*c = 0;
			is_ip = !sa_set_str(&sa, host + 1, 0);
		}

		mem_deref(host);

		if (!is_ip && !pl_strchr(&user, '@')) {
			if (ua->luri.af == AF_INET6)
				err |= mbuf_printf(mb, "@[%r]", &ua->luri.host);
			else
				err |= mbuf_printf(mb, "@%r", &ua->luri.host);

			if (ua->luri.port && ua->luri.port != SIP_PORT)
				err |= mbuf_printf(mb, ":%u", ua->luri.port);
		}
	}

	if (pl_isset(&params))
		err |= mbuf_write_pl(mb, &params);

 out:
	if (!err) {
		mbuf_set_pos(mb, 0);
		err = mbuf_strdup(mb, strp, mbuf_get_left(mb));
	}

	mem_deref(mb);
	return err;
}

/* The request URI for a target outside a dialog (MESSAGE, SUBSCRIBE):
 * completed as a dial string, then the URI (with its params) out of
 * "Bob <sip:bob@host;p=1>" as much as out of "sip:bob@host". */
int mdt_ua_target(const struct mdt_ua *ua, char **urip, const char *target)
{
	struct sip_addr addr;
	struct pl pl;
	char *full = NULL;
	int err;

	if (!ua || !urip || !target)
		return EINVAL;

	err = mdt_ua_uri_complete(ua, &full, target);
	if (err)
		return err;

	pl_set_str(&pl, full);
	err = sip_addr_decode(&addr, &pl);
	if (!err) {
		if (pl_isset(&addr.params))
			err = re_sdprintf(urip, "%r%r", &addr.auri,
					  &addr.params);
		else
			err = pl_strdup(urip, &addr.auri);
	}

	mem_deref(full);

	/* Whatever the forms above made of it, a URI has no blanks, quotes or
	 * brackets: "not a <uri" is not a target. */
	if (!err && strpbrk(*urip, " \t\r\n\"<>")) {
		*urip = mem_deref(*urip);
		err = EINVAL;
	}

	return err;
}

/* ── Registration ────────────────────────────────────────────────────────── */

static int create_register_clients(struct mdt_ua *ua)
{
	if (!ua->regint)
		return 0;

	/* reg-id 1 for the one outbound flow (RFC 5626), else none. */
	return mdt_reg_add(&ua->regl, ua,
			    ua_outbound(ua) && ua->outbound ? 1 : 0);
}

int mdt_ua_register(struct mdt_ua *ea)
{
	struct medut_account *acct;
	struct uri uri;
	char *reg_uri = NULL;
	char params[256] = "";
	size_t len;
	struct le *le;
	int err;

	if (!ea)
		return EINVAL;

	if (!ea->regint)
		return 0;

	acct = bsdk_account_find(ea);

	/* The registrar is the AOR's domain. */
	uri = ea->luri;
	uri.user = pl_null;

	err = re_sdprintf(&reg_uri, "%H", uri_encode, &uri);
	if (err)
		goto out;

	if (g_sip.uuid[0])
		(void)re_snprintf(params, sizeof(params),
				  ";+sip.instance=\"<urn:uuid:%s>\"",
				  g_sip.uuid);

	if (ea->mnat && ea->mnat->ftag) {
		len = strlen(params);
		(void)re_snprintf(params + len, sizeof(params) - len, ";%s",
				  ea->mnat->ftag);
	}

	if (list_isempty(&ea->regl)) {
		err = create_register_clients(ea);
		if (err)
			goto out;
	}

	if (!list_isempty(&ea->regl))
		bsdk_ev_reg_sending(acct);

	LIST_FOREACH(&ea->regl, le) {
		err = mdt_reg_register(le->data, reg_uri, params,
					ea->outbound);
		if (err) {
			char reason[128];

			MDT_WARN("sip: %s: register: %m\n", ea->aor, err);
			(void)re_snprintf(reason, sizeof(reason), "%m", err);
			bsdk_ev_reg_fail(acct, 0, reason);
			goto out;
		}
	}

 out:
	mem_deref(reg_uri);
	return err;
}

void mdt_ua_unregister(struct mdt_ua *ea)
{
	struct le *le;

	if (!ea)
		return;

	if (!list_isempty(&ea->regl))
		bsdk_ev_reg_unregistering(bsdk_account_find(ea));

	/* With RFC 5626 outbound the subscription rides the registration's
	 * flow, which is going: end it (the next REGISTER makes it again). */
	if (ea->sip_outbound)
		mdt_mwi_ua_close(ea);

	LIST_FOREACH(&ea->regl, le)
		mdt_reg_unregister(le->data);
}

bool mdt_ua_isregistered(const struct mdt_ua *ea)
{
	struct le *le;

	if (!ea)
		return false;

	/* One working registration is enough. */
	LIST_FOREACH(&ea->regl, le) {
		if (mdt_reg_isok(le->data))
			return true;
	}

	return false;
}

int mdt_ua_refresh_register(struct mdt_ua *ea)
{
	struct le *le;
	int err = ENOENT;

	if (!ea)
		return EINVAL;

	LIST_FOREACH(&ea->regl, le) {
		int e = mdt_reg_refresh(le->data);

		if (e == 0 || e == EALREADY)
			return 0;
		if (err == ENOENT)
			err = e;
	}

	return err;
}

/* ── The account ─────────────────────────────────────────────────────────── */

/* The media encryption an SDK mode names.  SDES is SRTP on an RTP/SAVP
 * media line, as RFC 3711 / RFC 4568 have it: a best-effort a=crypto on
 * RTP/AVP is refused outright by RFC-strict servers (FreeSWITCH: 488). */
static const char *menc_id(medut_media_enc_t enc)
{
	switch (enc) {
	case MEDUT_MEDIA_ENC_SDES:      return "srtp-mand";
	case MEDUT_MEDIA_ENC_DTLS_SRTP: return "dtls_srtp";
	default:                          return NULL;
	}
}

static void stun_uri_destructor(void *arg)
{
	struct mdt_stun_uri *su = arg;
	mem_deref(su->host);
}

/* stun:, stuns:, turn:, turns: URIs (RFC 7064 / 7065), with an optional
 * ?transport=udp|tcp. */
static int stun_uri_decode(struct mdt_stun_uri **sup, const char *str)
{
	enum mdt_stun_scheme scheme;
	struct mdt_stun_uri *su;
	struct uri uri;
	struct pl pl, tp;
	int proto = IPPROTO_UDP;
	int err;

	pl_set_str(&pl, str);
	err = uri_decode(&uri, &pl);
	if (err) {
		MDT_WARN("sip: NAT server URI '%s': %m\n", str, err);
		return err;
	}

	if (0 == pl_strcasecmp(&uri.scheme, "stun"))
		scheme = MDT_STUN_SCHEME_STUN;
	else if (0 == pl_strcasecmp(&uri.scheme, "stuns"))
		scheme = MDT_STUN_SCHEME_STUNS;
	else if (0 == pl_strcasecmp(&uri.scheme, "turn"))
		scheme = MDT_STUN_SCHEME_TURN;
	else if (0 == pl_strcasecmp(&uri.scheme, "turns"))
		scheme = MDT_STUN_SCHEME_TURNS;
	else {
		MDT_WARN("sip: NAT server scheme not supported (%r)\n",
			  &uri.scheme);
		return ENOTSUP;
	}

	if (0 == re_regex(uri.headers.p, uri.headers.l,
			  "transport=[a-z]+", &tp)) {
		if (0 == pl_strcasecmp(&tp, "udp"))
			proto = IPPROTO_UDP;
		else if (0 == pl_strcasecmp(&tp, "tcp"))
			proto = IPPROTO_TCP;
		else {
			MDT_WARN("sip: NAT server transport '%r' not "
				  "supported\n", &tp);
			return EPROTONOSUPPORT;
		}
	}

	su = mem_zalloc(sizeof(*su), stun_uri_destructor);
	if (!su)
		return ENOMEM;

	su->scheme = scheme;
	su->port   = uri.port;
	su->proto  = proto;
	err = pl_strdup(&su->host, &uri.host);
	if (err)
		mem_deref(su);
	else
		*sup = su;

	return err;
}

/* Take the next item of a comma-separated list off the front of pl. */
static int csl_parse(struct pl *pl, char *str, size_t sz)
{
	struct pl ws = PL_INIT, val, ws2 = PL_INIT, cma = PL_INIT;
	int err;

	err = re_regex(pl->p, pl->l, "[ \t]*[^, \t]+[ \t]*[,]*",
		       &ws, &val, &ws2, &cma);
	if (err)
		return err;

	pl_advance(pl, ws.l + val.l + ws2.l + cma.l);
	(void)pl_strcpy(&val, str, sz);

	return 0;
}

/* "name[/srate[/ch]],..." → references to the registered codecs. */
static void codecs_set(struct mdt_ua *ua, const char *codecs)
{
	struct pl acs;
	char cname[64];
	unsigned i = 0;

	pl_set_str(&acs, codecs);

	while (0 == csl_parse(&acs, cname, sizeof(cname))) {
		struct pl pl_cname, pl_srate, pl_ch = PL_INIT;
		const struct mdt_aucodec *ac;
		uint32_t srate = 8000;
		uint8_t ch = 1;

		if (0 == re_regex(cname, str_len(cname),
				  "[^/]+/[0-9]+[/]*[0-9]*",
				  &pl_cname, &pl_srate, NULL, &pl_ch)) {
			(void)pl_strcpy(&pl_cname, cname, sizeof(cname));
			srate = pl_u32(&pl_srate);
			if (pl_isset(&pl_ch))
				ch = (uint8_t)pl_u32(&pl_ch);
		}

		ac = mdt_aucodec_find(mdt_aucodecl(), cname, srate, ch);
		if (!ac) {
			MDT_WARN("sip: audio codec not found: %s/%u/%u\n",
				  cname, srate, ch);
			continue;
		}

		/* References into the registry, in the account's order. */
		list_append(&ua->aucodecl, &ua->acv[i++], (void *)ac);
		if (i >= RE_ARRAY_SIZE(ua->acv))
			break;
	}
}

struct list *mdt_ua_aucodecl(const struct mdt_ua *ua)
{
	return (ua && !list_isempty(&ua->aucodecl))
		? (struct list *)&ua->aucodecl : mdt_aucodecl();
}

static int dup_opt(char **dst, const char *src)
{
	*dst = mem_deref(*dst);
	return src ? str_dup(dst, src) : 0;
}

int mdt_ua_set_outbound(struct mdt_ua *ea, const char *uri)
{
	if (!ea)
		return EINVAL;

	return dup_opt(&ea->outbound, uri);
}

int mdt_ua_set_100rel(struct mdt_ua *ea, medut_100rel_mode_t mode)
{
	if (!ea)
		return EINVAL;

	switch (mode) {
	case MEDUT_100REL_DISABLED: ea->rel100 = REL100_DISABLED; break;
	case MEDUT_100REL_ENABLED:  ea->rel100 = REL100_ENABLED;  break;
	case MEDUT_100REL_REQUIRED: ea->rel100 = REL100_REQUIRED; break;
	default:
		return EINVAL;
	}

	return 0;
}

int mdt_ua_add_header(struct mdt_ua *ea, const char *name,
		       const char *value)
{
	if (!ea || !name || !value)
		return EINVAL;

	return mdt_hdrs_add(&ea->custom_hdrs, name, "%s", value);
}

int mdt_ua_options_send(struct mdt_ua *ea, const char *uri,
			 mdt_resp_h *resph, void *arg)
{
	int err;

	if (!ea || !str_isset(uri))
		return EINVAL;

	err = mdt_sip_req_send(ea, "OPTIONS", uri, resph, arg,
				"Accept: application/sdp\r\n"
				"Content-Length: 0\r\n"
				"\r\n");
	if (err)
		MDT_WARN("sip: send OPTIONS: %m\n", err);

	return err;
}

bool mdt_ua_has_calls(const struct mdt_ua *ea)
{
	return ea && !list_isempty(&ea->calls);
}

/* Which host the UA's requests are sent to, for the per-server TLS
 * verification decision (bsdk_tls_verify_host()).  With an outbound route
 * every request goes there, so that host is the server; without one the
 * requests go to the AOR's host. */
bool mdt_ua_sends_to(const struct mdt_ua *ea, const char *host)
{
	if (!ea || !str_isset(host))
		return false;

	if (str_isset(ea->outbound)) {
		struct pl pl;
		struct uri uri;

		pl_set_str(&pl, ea->outbound);
		if (!uri_decode(&uri, &pl))
			return 0 == pl_strcasecmp(&uri.host, host);
	}

	return 0 == pl_strcasecmp(&ea->luri.host, host);
}

/* The Contact user: the AOR's user, made unique when another UA of this
 * process already uses it (two accounts of one user on two servers). */
static int cuser_gen(struct mdt_ua *ua)
{
	struct le *le;

	LIST_FOREACH(&g_sip.ual, le) {
		const struct mdt_ua *o = le->data;

		if (o != ua && !pl_cmp(&o->luri.user, &ua->luri.user)) {
			char buf[16];

			rand_str(buf, sizeof(buf));
			return re_sdprintf(&ua->cuser, "%r-%s",
					   &ua->luri.user, buf);
		}
	}

	return pl_strdup(&ua->cuser, &ua->luri.user);
}

static void ua_destructor(void *arg)
{
	struct mdt_ua *ua = arg;

	list_unlink(&ua->le);
	list_flush(&ua->regl);

	mem_deref(ua->buf);
	mem_deref(ua->aor);
	mem_deref(ua->auth_user);
	mem_deref(ua->auth_pass);
	mem_deref(ua->dispname);
	mem_deref(ua->outbound);
	mem_deref(ua->stun_host);
	mem_deref(ua->stun_user);
	mem_deref(ua->stun_pass);
	mem_deref(ua->cuser);
	mem_deref(ua->pub_gruu);
	list_flush(&ua->custom_hdrs);
	list_clear(&ua->aucodecl);
}

int mdt_ua_alloc(struct mdt_ua **eap, const struct mdt_ua_prm *prm)
{
	struct mdt_ua *ua;
	struct pl pl;
	uint32_t rwait = 0;
	int err;

	if (!eap || !prm || !prm->aor)
		return EINVAL;

	if (!g_sip.sip)
		return MEDUT_ERR_STATE;

	ua = mem_zalloc(sizeof(*ua), ua_destructor);
	if (!ua)
		return ENOMEM;

	list_init(&ua->regl);
	list_init(&ua->calls);
	list_init(&ua->custom_hdrs);
	list_init(&ua->aucodecl);

	err = str_dup(&ua->buf, prm->aor);
	if (err)
		goto out;

	pl_set_str(&pl, ua->buf);
	err = sip_addr_decode(&ua->laddr, &pl);
	if (err) {
		MDT_WARN("sip: cannot parse the account address '%s'\n",
			  prm->aor);
		goto out;
	}

	ua->luri = ua->laddr.uri;

	err = re_sdprintf(&ua->aor, "%H", uri_encode, &ua->luri);
	if (err)
		goto out;

	/* Refresh at rwait % of the interval, clamped as libre clamps it. */
	if (0 == msg_param_decode(&ua->laddr.params, "rwait", &pl)) {
		rwait = pl_u32(&pl);
		if (rwait > 95)
			rwait = 95;
		if (rwait && rwait < 5)
			rwait = 5;
	}
	ua->rwait = rwait;

	ua->regint       = prm->reg_expires;
	ua->sip_outbound = prm->sip_outbound;
	ua->rel100       = REL100_DISABLED;
	ua->menc         = prm->media_enc;
	ua->rtcp_mux     = prm->rtcp_mux;
	ua->dtmf         = prm->dtmf_mode;

	err  = dup_opt(&ua->auth_user, prm->auth_user);
	err |= dup_opt(&ua->auth_pass, prm->auth_pass);
	err |= dup_opt(&ua->dispname,  prm->display_name);
	err |= dup_opt(&ua->outbound,  prm->outbound);
	err |= dup_opt(&ua->stun_user, prm->stun_user);
	err |= dup_opt(&ua->stun_pass, prm->stun_pass);
	if (err)
		goto out;

	ua->ptime = 20;

	/* A bad NAT server URI leaves the account without one, as before. */
	if (prm->stun_uri)
		(void)stun_uri_decode(&ua->stun_host, prm->stun_uri);

	if (prm->media_enc != MEDUT_MEDIA_ENC_NONE) {
		/* Refuse an account that would fall back to plain RTP: the
		 * app asked for encryption. */
		const char *id = menc_id(prm->media_enc);

		ua->mencp = mdt_menc_find(mdt_mencl(), id);
		if (!ua->mencp) {
			MDT_WARN("sip: account %s: media encryption '%s' is "
				  "not registered\n", ua->aor, id);
			err = MEDUT_ERR_STATE;
			goto out;
		}
	}

	if (prm->codecs) {
		/* No codec at all is a stack that cannot negotiate a call. */
		if (list_isempty(mdt_aucodecl())) {
			MDT_WARN("sip: account %s: no audio codec is "
				  "registered\n", ua->aor);
			err = MEDUT_ERR_STATE;
			goto out;
		}
		codecs_set(ua, prm->codecs);
		if (list_isempty(&ua->aucodecl))
			MDT_WARN("sip: account %s: codec list \"%s\" matched "
				  "no codec — offering all codecs instead\n",
				  ua->aor, prm->codecs);
	}

	if (prm->ice) {
		ua->mnat = mdt_mnat_find(mdt_mnatl(), "ice");
		if (!ua->mnat) {
			MDT_WARN("sip: account %s wants ICE, but the ice "
				  "media-NAT is not registered\n", ua->aor);
			err = MEDUT_ERR_STATE;
			goto out;
		}
	}

	if (prm->sip_outbound && !g_sip.uuid[0])
		MDT_WARN("sip: %s: no instance id, so no RFC 5626 outbound\n",
			  ua->aor);

	err = cuser_gen(ua);
	if (err)
		goto out;

	err = create_register_clients(ua);
	if (err)
		goto out;

	list_append(&g_sip.ual, &ua->le, ua);

 out:
	if (err)
		mem_deref(ua);
	else
		*eap = ua;

	return err;
}

void mdt_ua_free(struct mdt_ua *ea)
{
	if (!ea)
		return;

	list_unlink(&ea->le);

	/* The calls hold the UA: end them first. */
	mdt_ua_calls_close(ea);

	/* Subscriptions and publication end while the registration — and
	 * so the route they use — is still there. */
	mdt_mwi_ua_close(ea);
	mdt_presence_ua_close(ea);

	if (!list_isempty(&ea->regl))
		bsdk_ev_reg_unregistering(bsdk_account_find(ea));

	/* A registered client un-REGISTERs on its way out (libre's sipreg
	 * destructor), holding the UA for its credentials until it is done. */
	list_flush(&ea->regl);

	mem_deref(ea);
}
