/**
 * @file sip_stack.c  The SIP stack: transports, TLS, listeners, routing
 *
 * One libre `struct sip` for the process, with a transport of every kind
 * (UDP, TCP, TLS, WS, WSS) on every usable local address, and the listeners
 * that hand incoming requests to the user agent they are addressed to.
 *
 * Adapted from baresip's src/uag.c and the instance-id part of
 * modules/uuid/uuid.c (BSD-3-Clause, Copyright (C) 2010 Alfred E.
 * Heggestad); see THIRD_PARTY_NOTICES.
 */

#include <string.h>
#include <stdio.h>
#include "mdt_sip.h"

struct mdt_sip g_sip;

/* Every transport libre has.  An account picks one by its outbound route. */
static const uint32_t TRANSPORTS = (1u << SIP_TRANSP_UDP) |
				   (1u << SIP_TRANSP_TCP) |
				   (1u << SIP_TRANSP_TLS) |
				   (1u << SIP_TRANSP_WS)  |
				   (1u << SIP_TRANSP_WSS);

static bool tp_enabled(enum sip_transp tp)
{
	return (TRANSPORTS & (1u << tp)) != 0;
}

/* ── Instance id (RFC 5626 +sip.instance) ────────────────────────────────── */

enum { UUID_LEN = 36 };

static bool uuid_valid(const char *s, size_t len)
{
	return len == UUID_LEN &&
	       0 == re_regex(s, len, "[0-9a-f]8-[0-9a-f]4-[0-9a-f]4-"
			     "[0-9a-f]4-[0-9a-f]6[0-9a-f]6",
			     NULL, NULL, NULL, NULL, NULL, NULL);
}

/* The id lives in <conf_dir>/uuid, the file earlier Medut releases kept
 * it in, so an installation keeps the instance id it registered with. */
static void uuid_load(void)
{
	char path[600];
	char buf[UUID_LEN + 8] = "";
	FILE *f;

	g_sip.uuid[0] = '\0';

	if (!g_mdt.conf_dir[0])
		return;

	(void)re_snprintf(path, sizeof(path), "%s/uuid", g_mdt.conf_dir);

	f = fopen(path, "r");
	if (f) {
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		(void)fclose(f);
		buf[n] = '\0';
		if (uuid_valid(buf, n)) {
			str_ncpy(g_sip.uuid, buf, sizeof(g_sip.uuid));
			return;
		}
	}

	(void)re_snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%08x%04x",
			  rand_u32(), rand_u16(), rand_u16(), rand_u16(),
			  rand_u32(), rand_u16());

	f = fopen(path, "w");
	if (!f) {
		MDT_WARN("sip: cannot write the instance id to %s (%m); "
			  "RFC 5626 outbound is off\n", path, errno);
		return;
	}

	if (fputs(buf, f) < 0) {
		(void)fclose(f);
		return;
	}
	if (fclose(f))
		return;

	str_ncpy(g_sip.uuid, buf, sizeof(g_sip.uuid));
	MDT_INFO("sip: new instance id in %s\n", path);
}

/* ── Transports ──────────────────────────────────────────────────────────── */

/* The local SIP address the app configured (medut_config_t local_ip /
 * local_port), or false when it configured none.  An address-less port
 * keeps the port and takes each local address in turn. */
static bool cfg_local(struct sa *local)
{
	const medut_config_t *cfg = &g_bsdk.cfg;

	sa_init(local, AF_UNSPEC);

	if (str_isset(cfg->local_ip)) {
		if (sa_set_str(local, cfg->local_ip, cfg->local_port)) {
			MDT_WARN("sip: local_ip '%s' is not an address; "
				  "ignored\n", cfg->local_ip);
			sa_init(local, AF_UNSPEC);
			return cfg->local_port != 0;
		}
		return true;
	}

	return cfg->local_port != 0;
}

static int tls_ctx(struct tls **tlsp, bool wss)
{
	const medut_config_t *cfg = &g_bsdk.cfg;
	int err;

	if (*tlsp)
		return 0;

	err = tls_alloc(tlsp, TLS_METHOD_TLS,
			wss ? NULL : cfg->client_cert, NULL);
	if (err) {
		MDT_WARN("sip: %s tls_alloc: %m\n", wss ? "wss" : "tls", err);
		return err;
	}

	if (wss) {
		err = tls_set_verify_purpose(*tlsp, "sslserver");
		if (err) {
			MDT_WARN("sip: wss tls_set_verify_purpose: %m\n", err);
			*tlsp = mem_deref(*tlsp);
			return err;
		}
	}

	if (str_isset(cfg->ca_cert_path)) {
		err = tls_add_cafile_path(*tlsp, cfg->ca_cert_path, NULL);
		if (err)
			MDT_WARN("sip: %s CA file %s: %m\n",
				  wss ? "wss" : "tls", cfg->ca_cert_path, err);
	}

	/* Left verifying whatever cfg->verify_server says: the global setting
	 * and each account's verify_tls are applied per connection by libre,
	 * which asks tls_verify_handler() below and verifies only when told to
	 * (cmake/patch-re-sources.cmake, hunk AB).  Switching it off here would
	 * switch it off for every account sharing this context. */

	if (!wss)
		tls_set_resumption(*tlsp, TLS_RESUMPTION_ALL);

	return 0;
}

/* libre's question before it picks or opens a TLS or WSS connection: should
 * the server's certificate be checked?  The SDK answers from the accounts
 * that send to that host (bsdk_tls_verify_host(), src/api/account.c). */
static bool tls_verify_handler(const char *host, bool *verify, void *arg)
{
	(void)arg;
	return bsdk_tls_verify_host(host, verify);
}

static int transp_add(const struct sa *laddr)
{
	const medut_config_t *cfg = &g_bsdk.cfg;
	struct sa local;
	int err = 0;

	if (!sa_isset(laddr, SA_ADDR))
		return EINVAL;

	if (cfg_local(&local)) {
		/* A port alone: every local address, on that port.  (Not
		 * sa_port(&local): an AF_UNSPEC address holds no port.) */
		if (!sa_isset(&local, SA_ADDR)) {
			sa_cpy(&local, laddr);
			sa_set_port(&local, cfg->local_port);
		}
		/* Bind only the address the app asked for. */
		if (!sa_cmp(laddr, &local, SA_ADDR))
			return 0;
	}
	else {
		sa_cpy(&local, laddr);
		sa_set_port(&local, 0);
	}

	if (tp_enabled(SIP_TRANSP_UDP))
		err |= sip_transp_add(g_sip.sip, SIP_TRANSP_UDP, &local);
	if (tp_enabled(SIP_TRANSP_TCP))
		err |= sip_transp_add_sock(g_sip.sip, SIP_TRANSP_TCP, true,
					   &local);
	if (err) {
		MDT_WARN("sip: transport on %j: %m\n", &local, err);
		return err;
	}

	if (tp_enabled(SIP_TRANSP_TLS)) {
		struct sa tl = local;

		err = tls_ctx(&g_sip.tls, false);
		if (err)
			return err;

		/* A fixed port is the UDP/TCP one; TLS takes the next. */
		if (sa_isset(&tl, SA_PORT))
			sa_set_port(&tl, sa_port(&tl) + 1);

		err = sip_transp_add_sock(g_sip.sip, SIP_TRANSP_TLS, true,
					  &tl, g_sip.tls);
		if (err) {
			MDT_WARN("sip: TLS transport on %j: %m\n", &tl, err);
			return err;
		}
	}

	if (tp_enabled(SIP_TRANSP_WS)) {
		err = sip_transp_add_websock(g_sip.sip, SIP_TRANSP_WS, &local,
					     false, NULL, NULL);
		if (err) {
			MDT_WARN("sip: WS transport on %j: %m\n", &local, err);
			return err;
		}
	}

	if (tp_enabled(SIP_TRANSP_WSS)) {
		err = tls_ctx(&g_sip.wss_tls, true);
		if (err)
			return err;

		err = sip_transp_add_websock(g_sip.sip, SIP_TRANSP_WSS, &local,
					     false, cfg->client_cert,
					     g_sip.wss_tls);
		if (err) {
			MDT_WARN("sip: WSS transport on %j: %m\n", &local,
				  err);
			return err;
		}
	}

	/* The config holds a DSCP value; the socket takes the TOS byte it
	 * sits in.  Again after every add: a new transport starts at 0. */
	(void)sip_settos(g_sip.sip, (uint8_t)(cfg->dscp_sip << 2));

	return 0;
}

static bool transp_add_h(const char *ifname, const struct sa *sa, void *arg)
{
	int *errp = arg;
	int err;
	(void)ifname;

	err = transp_add(sa);
	if (err) {
		*errp = err;
		return true;
	}

	return false;
}

static int transp_add_all(void)
{
	int err = 0;

	(void)mdt_net_laddr_apply(transp_add_h, &err);
	(void)sip_transp_set_default(g_sip.sip, SIP_TRANSP_UDP);

	return err;
}

int mdt_sip_reset_transp(void)
{
	if (!g_sip.sip)
		return EINVAL;

	sip_transp_flush(g_sip.sip);

	return transp_add_all();
}

struct sip *mdt_sip_stack(void)
{
	return g_sip.sip;
}

/* ── Routing incoming requests ───────────────────────────────────────────── */

static bool uri_match_af(const struct uri *accu, const struct uri *peeru)
{
	struct sa sa1, sa2;

	if (peeru->af == AF_UNSPEC || accu->af == AF_UNSPEC)
		return true;

	if (accu->af != peeru->af)
		return false;

	if (accu->af == AF_INET6) {
		if (sa_set(&sa1, &accu->host, 0) ||
		    sa_set(&sa2, &peeru->host, 0))
			return false;

		return sa_is_linklocal(&sa1) == sa_is_linklocal(&sa2);
	}

	return true;
}

struct mdt_ua *mdt_sip_find_msg(const struct sip_msg *msg)
{
	const struct pl *cuser;
	struct le *le;

	if (!msg)
		return NULL;

	cuser = &msg->uri.user;

	/* A registered account: by the Contact user it registered. */
	LIST_FOREACH(&g_sip.ual, le) {
		struct mdt_ua *ua = le->data;

		if (ua->regint && 0 == pl_strcasecmp(cuser, ua->cuser))
			return ua;
	}

	/* Peer to peer (an account that does not register): by the user of
	 * its address, on the same address family. */
	LIST_FOREACH(&g_sip.ual, le) {
		struct mdt_ua *ua = le->data;

		if (ua->regint)
			continue;

		if (!uri_match_af(&ua->luri, &msg->uri))
			continue;

		if (0 == pl_casecmp(cuser, &ua->luri.user))
			return ua;
	}

	return NULL;
}

uint32_t mdt_sip_call_count(void)
{
	uint32_t n = 0;
	struct le *le;

	LIST_FOREACH(&g_sip.ual, le) {
		const struct mdt_ua *ua = le->data;
		n += list_count(&ua->calls);
	}

	return n;
}

/* OPTIONS and MESSAGE outside a dialog, and a NOTIFY with an unsolicited
 * message summary.  Anything else is left to the listeners after this one
 * — a NOTIFY to libre's event socket — or to libre, which answers 501 (a
 * REFER among them). */
static bool request_handler(const struct sip_msg *msg, void *arg)
{
	struct mdt_ua *ua;
	bool options, message;
	(void)arg;

	if (!pl_strcmp(&msg->met, "NOTIFY")) {
		ua = mdt_sip_find_msg(msg);
		return ua && mdt_mwi_unsolicited(ua, msg);
	}

	options = !pl_strcmp(&msg->met, "OPTIONS");
	message = !pl_strcmp(&msg->met, "MESSAGE");
	if (!options && !message)
		return false;

	ua = mdt_sip_find_msg(msg);
	if (!ua) {
		MDT_INFO("sip: %r for unknown user %r\n", &msg->met,
			  &msg->uri.user);
		(void)sip_treply(NULL, g_sip.sip, msg, 404, "Not Found");
		return true;
	}

	if (options)
		mdt_ua_handle_options(ua, msg);
	else
		mdt_message_recv(ua, msg);

	return true;
}

static bool sub_handler(const struct sip_msg *msg, void *arg)
{
	struct mdt_ua *ua;
	(void)arg;

	ua = mdt_sip_find_msg(msg);
	if (!ua) {
		MDT_WARN("sip: SUBSCRIBE for unknown user %r\n",
			  &msg->uri.user);
		(void)sip_treply(NULL, g_sip.sip, msg, 404, "Not Found");
		return true;
	}

	if (g_sip.subh)
		return g_sip.subh(msg, ua);

	(void)sip_treplyf(NULL, NULL, g_sip.sip, msg, false, 405,
			  "Method Not Allowed",
			  "Allow: %H\r\n"
			  "Content-Length: 0\r\n\r\n",
			  mdt_ua_print_allowed, ua);
	return true;
}

/* An option-tag of the INVITE's Require: that the UA does not support. */
static bool require_h(const struct sip_hdr *hdr, const struct sip_msg *msg,
		      void *arg)
{
	(void)msg;
	return !mdt_ua_supports(arg, &hdr->val);
}

/* A new INVITE: route it to a UA and check what it requires of us, then
 * hand it to the call layer. */
static void conn_handler(const struct sip_msg *msg, void *arg)
{
	const char magic_branch[] = RE_RFC3261_BRANCH_ID;
	const struct sip_hdr *hdr;
	struct mdt_ua *ua;
	(void)arg;

	if (pl_strncmp(&msg->via.branch, magic_branch,
		       sizeof(magic_branch) - 1)) {
		MDT_INFO("sip: INVITE with a pre-RFC 3261 Via branch\n");
		(void)sip_treply(NULL, g_sip.sip, msg, 606, "Not Acceptable");
		return;
	}

	ua = mdt_sip_find_msg(msg);
	if (!ua) {
		MDT_INFO("sip: %r: no account for %H\n", &msg->from.auri,
			  uri_encode, &msg->uri);
		(void)sip_treply(NULL, g_sip.sip, msg, 404, "Not Found");
		return;
	}

	if (mdt_sip_call_count() + 1 > MDT_MAX_CALLS) {
		MDT_INFO("sip: rejected a call from %r (%d calls is the "
			  "most)\n", &msg->from.auri, MDT_MAX_CALLS);
		(void)sip_treply(NULL, g_sip.sip, msg, 486, "Max Calls");
		return;
	}

	hdr = sip_msg_hdr_apply(msg, true, SIP_HDR_REQUIRE, require_h, ua);
	if (hdr) {
		MDT_INFO("sip: call from %r rejected with 420: option-tag "
			  "'%r' not supported\n", &msg->from.auri, &hdr->val);
		(void)sip_treplyf(NULL, NULL, g_sip.sip, msg, false,
				  420, "Bad Extension",
				  "Unsupported: %r\r\n"
				  "Content-Length: 0\r\n\r\n",
				  &hdr->val);
		return;
	}

	if (ua->rel100 == REL100_REQUIRED &&
	    !(sip_msg_hdr_has_value(msg, SIP_HDR_SUPPORTED, "100rel") ||
	      sip_msg_hdr_has_value(msg, SIP_HDR_REQUIRE, "100rel"))) {
		MDT_INFO("sip: call from %r rejected with 421: the caller "
			  "does not support 100rel\n", &msg->from.auri);
		(void)sip_treplyf(NULL, NULL, g_sip.sip, msg, false,
				  421, "Extension required",
				  "Require: 100rel\r\n"
				  "Content-Length: 0\r\n\r\n");
		return;
	}

	(void)mdt_call_accept(ua, msg);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int mdt_sip_init(void)
{
	const uint32_t bsize = 16;
	const char *software;
	char dflt[32];
	int err;

	/* The User-Agent / Server header: the app's, or "Medut/<version>"
	 * from the header's version macros (medut_version()).  libre keeps
	 * its own copy. */
	(void)re_snprintf(dflt, sizeof(dflt), "Medut/%s", medut_version());
	software = str_isset(g_bsdk.cfg.user_agent) ? g_bsdk.cfg.user_agent
						    : dflt;

	list_init(&g_sip.ual);
	uuid_load();

	err = sip_alloc(&g_sip.sip, mdt_net_dnsc(), bsize, bsize, bsize,
			software, NULL, NULL);
	if (err) {
		MDT_WARN("sip: stack: %m\n", err);
		goto out;
	}

	/* Before any transport exists, so that no TLS or WSS connection is
	 * opened without the per-account decision. */
	sip_transp_set_verify_handler(g_sip.sip, tls_verify_handler, NULL);

	err = transp_add_all();
	if (err)
		goto out;

	err = sip_listen(&g_sip.lsnr, g_sip.sip, true, request_handler, NULL);
	if (err)
		goto out;

	err = sipsess_listen(&g_sip.sock, g_sip.sip, bsize, conn_handler,
			     NULL);
	if (err)
		goto out;

	err = sipevent_listen(&g_sip.evsock, g_sip.sip, bsize, bsize,
			      sub_handler, NULL);
	if (err)
		goto out;

	/* SUBSCRIBEs to our own presence (and SUBSCRIBE in Allow). */
	g_sip.subh = mdt_presence_sub_handler;

 out:
	if (err) {
		MDT_WARN("sip: init: %m\n", err);
		mdt_ua_close_all();
	}
	return err;
}

/* Shutdown, on the re thread, once the UAs are freed: take the listeners
 * down, so that what still holds the SIP stack is only the requests the UAs
 * left on their way out — un-REGISTERs, un-SUBSCRIBEs — each owner freeing
 * itself when its request ends.  mdt_sip_busy() reports them. */
void mdt_sip_quiesce(void)
{
	g_sip.evsock = mem_deref(g_sip.evsock);
	g_sip.sock   = mem_deref(g_sip.sock);
	g_sip.lsnr   = mem_deref(g_sip.lsnr);
}

/* True while anything other than g_sip holds the SIP stack.  After
 * mdt_sip_quiesce() that is a registration or subscription client whose
 * last request is still out: an un-REGISTER that met a 401 needs the loop
 * running for its second try.  Any thread: the count is atomic. */
bool mdt_sip_busy(void)
{
	return g_sip.sip && mem_nrefs(g_sip.sip) > 1;
}

void mdt_ua_close_all(void)
{
	struct le *le;

	/* The SDK frees its UAs first; any left are unlinked, not freed —
	 * their owner still holds them. */
	BSDK_TRACE("[bsdk] ua_close_all: unlink UAs\n");
	while ((le = list_head(&g_sip.ual)))
		list_unlink(le);

	/* Each UA ended its own; this is whatever a failed init left. */
	BSDK_TRACE("[bsdk] ua_close_all: mwi/presence close\n");
	mdt_mwi_close();
	mdt_presence_close();

	BSDK_TRACE("[bsdk] ua_close_all: quiesce\n");
	mdt_sip_quiesce();

	/* Abort what is still out (medut_shutdown() waited for it, up to a
	 * bound): each request's owner then frees itself and lets go of the
	 * stack.  Without this, one un-REGISTER left unanswered when the loop
	 * stopped kept the stack, its sockets, TLS contexts and DNS client
	 * allocated for the life of the process. */
	BSDK_TRACE("[bsdk] ua_close_all: sip_close\n");
	sip_close(g_sip.sip, true);
	BSDK_TRACE("[bsdk] ua_close_all: free SIP stack\n");
	g_sip.sip     = mem_deref(g_sip.sip);
	BSDK_TRACE("[bsdk] ua_close_all: free TLS contexts\n");
	g_sip.tls     = mem_deref(g_sip.tls);
	g_sip.wss_tls = mem_deref(g_sip.wss_tls);
	g_sip.subh    = NULL;
	BSDK_TRACE("[bsdk] ua_close_all: done\n");
}
