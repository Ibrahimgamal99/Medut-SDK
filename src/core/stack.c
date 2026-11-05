/**
 * @file stack.c  Medut stack core: lifecycle, logging, DNS, local addresses
 */

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "mdt_stack.h"
#include "../media/mdt_media.h"

struct mdt_stack g_mdt;

/* ── Logging (bsdk.h) ────────────────────────────────────────────────────── */

void mdt_logf(bool safe, enum mdt_log_level level, const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = safe ? re_vsnprintf_s(buf, sizeof(buf), fmt, ap)
	         : re_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;

	bsdk_log_deliver((uint32_t)level, buf);
}

void mdt_log_init(void)  {}
void mdt_log_close(void) {}

/* ── Local addresses ─────────────────────────────────────────────────────── */

/* Loopback cannot reach a registrar, and a link-local address can bind a
 * socket but not reach an off-link one (and comes and goes with virtual
 * interfaces on phones and would flap the transports).  An explicit
 * bind_interface narrows the list to that interface. */
bool mdt_net_ifaddr_ok(const char *ifname, const struct sa *sa)
{
	const char *bind_if = g_bsdk.cfg.bind_interface;

	if (!sa || !sa_isset(sa, SA_ADDR))
		return false;
	if (sa_is_loopback(sa) || sa_is_linklocal(sa))
		return false;
	if (str_isset(bind_if) && (!ifname || str_casecmp(bind_if, ifname)))
		return false;

	return true;
}

static int add_laddr(const char *ifname, const struct sa *sa)
{
	struct mdt_laddr *la;
	struct le *le;

	if (!sa)
		return EINVAL;

	LIST_FOREACH(&g_mdt.laddrs, le) {
		const struct mdt_laddr *have = le->data;
		if (sa_cmp(&have->sa, sa, SA_ADDR))
			return EALREADY;
	}

	la = mem_zalloc(sizeof(*la), NULL);
	if (!la)
		return ENOMEM;

	if (ifname)
		str_ncpy(la->ifname, ifname, sizeof(la->ifname));
	sa_cpy(&la->sa, sa);
	sa_set_port(&la->sa, 0);
	list_append(&g_mdt.laddrs, &la->le, la);
	return 0;
}

static bool scan_h(const char *ifname, const struct sa *sa, void *arg)
{
	(void)arg;

	if (mdt_net_ifaddr_ok(ifname, sa))
		(void)add_laddr(ifname, sa);

	return false;
}

void mdt_net_scan(void)
{
	(void)net_if_apply(scan_h, NULL);
}

bool mdt_net_laddr_apply(net_ifaddr_h *ifh, void *arg)
{
	struct le *le;

	LIST_FOREACH(&g_mdt.laddrs, le) {
		const struct mdt_laddr *la = le->data;
		if (ifh(la->ifname, &la->sa, arg))
			return true;
	}

	return false;
}

int mdt_net_add_address(const struct sa *sa)
{
	int err = add_laddr(NULL, sa);
	return err == EALREADY ? 0 : err;
}

int mdt_net_rm_address(const struct sa *sa)
{
	struct le *le;

	LIST_FOREACH(&g_mdt.laddrs, le) {
		struct mdt_laddr *la = le->data;
		if (sa_cmp(&la->sa, sa, SA_ADDR)) {
			list_unlink(&la->le);
			mem_deref(la);
			return 0;
		}
	}

	return ENOENT;
}

/* AF_INET6 when the app prefers IPv6 (and then takes only that), else both. */
int mdt_net_af(void)
{
	return g_bsdk.cfg.prefer_ipv6 ? AF_INET6 : AF_UNSPEC;
}

bool mdt_net_af_enabled(int af)
{
	const int cfg_af = mdt_net_af();

	if (af == AF_UNSPEC)
		return false;

	return cfg_af == AF_UNSPEC || af == cfg_af;
}

/* Does the kernel route dst out of src? */
static int check_route(const struct sa *src, const struct sa *dst)
{
	struct sa ip;
	int err;

	err = net_dst_source_addr_get(dst, &ip);
	if (err)
		return err;

	return sa_cmp(src, &ip, SA_ADDR) ? 0 : ECONNREFUSED;
}

/* The first address of family af; with `internet`, the one the kernel
 * routes the Internet through (the default route's interface). */
static const struct sa *find_laddr_af(int af, bool internet)
{
	struct sa dst;
	struct le *le;

	if (sa_set_str(&dst, af == AF_INET6 ? "1::1" : "1.1.1.1", 53))
		return NULL;

	LIST_FOREACH(&g_mdt.laddrs, le) {
		const struct mdt_laddr *la = le->data;

		if (sa_af(&la->sa) != af)
			continue;

		if (internet && check_route(&la->sa, &dst))
			continue;

		return &la->sa;
	}

	return NULL;
}

const struct sa *mdt_net_laddr_af(int af)
{
	const struct sa *sa = find_laddr_af(af, true);

	return sa ? sa : find_laddr_af(af, false);
}

/* The local address the kernel routes dst out of, NULL when none of ours. */
const struct sa *mdt_net_laddr_for(const struct sa *dst)
{
	struct le *le;

	if (!sa_isset(dst, SA_ADDR))
		return NULL;

	LIST_FOREACH(&g_mdt.laddrs, le) {
		const struct mdt_laddr *la = le->data;

		if (sa_af(&la->sa) != sa_af(dst))
			continue;

		if (check_route(&la->sa, dst))
			continue;

		return &la->sa;
	}

	return NULL;
}

/* ── DNS ─────────────────────────────────────────────────────────────────── */

static int dns_servers(struct sa *nsv, uint32_t *nsn)
{
	char domain[64] = "";

	return dns_srv_get(domain, sizeof(domain), nsv, nsn);
}

struct dnsc *mdt_net_dnsc(void)
{
	return g_mdt.dnsc;
}

void mdt_net_dns_refresh(void)
{
	struct sa nsv[8];
	uint32_t nsn = RE_ARRAY_SIZE(nsv);

	if (!g_mdt.dnsc || dns_servers(nsv, &nsn))
		return;

	(void)dnsc_srv_set(g_mdt.dnsc, nsv, nsn);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int mdt_stack_init(const char *conf_dir)
{
	struct sa nsv[8];
	uint32_t nsn = RE_ARRAY_SIZE(nsv);
	int err;

	memset(&g_mdt, 0, sizeof(g_mdt));
	list_init(&g_mdt.laddrs);
	if (conf_dir)
		str_ncpy(g_mdt.conf_dir, conf_dir, sizeof(g_mdt.conf_dir));

	if (dns_servers(nsv, &nsn))
		nsn = 0;

	err = dnsc_alloc(&g_mdt.dnsc, NULL, nsv, nsn);
	if (err)
		return err;

#ifdef __ANDROID__
	/* Android has no /etc/resolv.conf and stopped exposing net.dns* in
	 * Oreo, so the resolver would come up with no nameservers and every
	 * query fail with ENOTSUP before a packet is sent.  Resolve A/AAAA
	 * through bionic's getaddrinfo() instead: it goes via netd, so it
	 * honours the per-network resolver, VPNs and Private DNS. */
	dnsc_getaddrinfo(g_mdt.dnsc, true);
#endif

	mdt_net_scan();

	err = mdt_mod_init();
	if (err)
		return err;

	g_mdt.up = true;
	return 0;
}

int  mdt_events_start(void) { return 0; }
void mdt_events_stop(void)  {}

int mdt_media_init(void)
{
	int err;

	mdt_config_init(&g_bsdk.cfg);

	err = mdt_null_init();
	if (err)
		return err;

	err = mdt_audio_drivers_init();
	if (err)
		return err;

	/* Opus first: the order codecs register is the default offer order. */
	err = mdt_opus_init(&g_bsdk.cfg.opus, g_bsdk.cfg.opus_expected_loss_pct);
	if (err)
		return err;

	err = mdt_g711_init();
	if (err)
		return err;

	err = mdt_sdes_srtp_init();
	if (err)
		return err;

	return mdt_dtls_srtp_init();
}

void mdt_app_unload(void) {}

void mdt_media_close(void)
{
	mdt_dtls_srtp_close();
	mdt_sdes_srtp_close();
	mdt_g711_close();
	mdt_opus_close();
	mdt_audio_drivers_close();
	mdt_null_close();
	mdt_mod_close();
}

void mdt_stack_close(void)
{
	list_flush(&g_mdt.laddrs);
	g_mdt.dnsc = mem_deref(g_mdt.dnsc);
	g_mdt.up = false;
}
