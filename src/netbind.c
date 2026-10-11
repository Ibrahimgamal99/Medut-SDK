/**
 * @file netbind.c  Pinning a socket to one interface's network
 *
 * One socket per interface only helps when the kernel sends from it over
 * that interface.  Linux routes by destination alone, so a socket bound to
 * the cellular address still leaves over the Wi-Fi default route unless it
 * is tied to the device (SO_BINDTODEVICE); Android additionally needs the
 * socket attached to the platform Network (android_setsocknetwork), which
 * the app's ConnectivityManager callback hands over as a handle through
 * echosdk_network_set_interface_handle(); Darwin takes the interface index
 * (IP_BOUND_IF / IPV6_BOUND_IF).  Windows has no equivalent here.
 *
 * The handle table is written from the API thread and read on re_main;
 * entries are few and short, so one mutex covers both.
 */

#include <string.h>
#include "echosdk_internal.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#elif defined(__APPLE__)
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#elif defined(__linux__)
#include <sys/socket.h>
#include <errno.h>
#endif

enum { BSDK_NETBIND_MAX = 8 };

struct if_handle {
	char     ifname[16];
	uint64_t handle;
	bool     set;
};

static struct if_handle s_ifv[BSDK_NETBIND_MAX];
static mtx_t s_lock;
static bool  s_lock_init;

static void lock_init_once(void)
{
	if (!s_lock_init) {
		(void)mtx_init(&s_lock, mtx_plain);
		s_lock_init = true;
	}
}

static bool handle_lookup(const char *ifname, uint64_t *handle)
{
	bool found = false;

	lock_init_once();
	mtx_lock(&s_lock);
	for (size_t i = 0; i < RE_ARRAY_SIZE(s_ifv); i++) {
		if (s_ifv[i].set && !strcmp(s_ifv[i].ifname, ifname)) {
			if (handle)
				*handle = s_ifv[i].handle;
			found = true;
			break;
		}
	}
	mtx_unlock(&s_lock);
	return found;
}

int echosdk_network_set_interface_handle(const char *ifname, uint64_t handle)
{
	struct if_handle *slot = NULL;

	if (!ifname || !ifname[0] || strlen(ifname) >= sizeof(slot->ifname))
		return ECHOSDK_ERR_INVAL;

	lock_init_once();
	mtx_lock(&s_lock);
	for (size_t i = 0; i < RE_ARRAY_SIZE(s_ifv); i++) {
		if (s_ifv[i].set && !strcmp(s_ifv[i].ifname, ifname)) {
			slot = &s_ifv[i];
			break;
		}
		if (!slot && !s_ifv[i].set)
			slot = &s_ifv[i];
	}
	if (!slot) {
		mtx_unlock(&s_lock);
		return ECHOSDK_ERR_NOMEM;
	}
	str_ncpy(slot->ifname, ifname, sizeof(slot->ifname));
	slot->handle = handle;
	slot->set    = true;
	mtx_unlock(&s_lock);

	info("EchoSDK/net: interface %s is platform network %llu\n", ifname,
	     (unsigned long long)handle);
	return ECHOSDK_OK;
}

int echosdk_network_clear_interface_handle(const char *ifname)
{
	if (!ifname)
		return ECHOSDK_ERR_INVAL;

	lock_init_once();
	mtx_lock(&s_lock);
	for (size_t i = 0; i < RE_ARRAY_SIZE(s_ifv); i++) {
		if (s_ifv[i].set && !strcmp(s_ifv[i].ifname, ifname))
			memset(&s_ifv[i], 0, sizeof(s_ifv[i]));
	}
	mtx_unlock(&s_lock);
	return ECHOSDK_OK;
}

/* Interface cost for pair priority: 0 Wi-Fi/wired, 1 VPN, 2 cellular.
 * Named after the conventions of each platform; an unknown name is 0. */
int bsdk_net_if_cost(const char *ifname)
{
	if (!ifname)
		return 0;
	if (!strncmp(ifname, "rmnet", 5) || !strncmp(ifname, "ccmni", 5) ||
	    !strncmp(ifname, "pdp_ip", 6) || !strncmp(ifname, "wwan", 4) ||
	    !strncmp(ifname, "ppp", 3) || !strncmp(ifname, "v4-rmnet", 8))
		return 2;
	if (!strncmp(ifname, "tun", 3) || !strncmp(ifname, "tailscale", 9) ||
	    !strncmp(ifname, "utun", 4) || !strncmp(ifname, "ipsec", 5) ||
	    !strncmp(ifname, "wg", 2))
		return 1;
	return 0;
}

bool bsdk_net_pin_bind_any(void)
{
#if defined(__ANDROID__)
	return true;    /* libwebrtc's order: setsocknetwork, then bind(any) */
#else
	return false;   /* SO_BINDTODEVICE / IP_BOUND_IF with the address bound */
#endif
}

bool bsdk_net_if_pinnable(const char *ifname)
{
	if (!ifname || !ifname[0])
		return false;
#if defined(__ANDROID__)
	return handle_lookup(ifname, NULL);
#elif defined(__APPLE__)
	return if_nametoindex(ifname) != 0;
#elif defined(__linux__)
	return true;      /* SO_BINDTODEVICE; refused at bind time if not */
#else
	return false;
#endif
}

int bsdk_net_bind_socket(re_sock_t fd, const char *ifname, int af)
{
	if (fd == RE_BAD_SOCK || !ifname || !ifname[0])
		return EINVAL;

#if defined(__ANDROID__)
	{
		typedef int (*setsocknetwork_fn)(uint64_t, int);
		static setsocknetwork_fn fn;
		static bool looked_up;
		uint64_t handle;
		int r;
		(void)af;

		if (!handle_lookup(ifname, &handle))
			return ENOENT;
		if (!looked_up) {
			void *lib = dlopen("libandroid.so", RTLD_NOW);
			if (lib)
				fn = (setsocknetwork_fn)dlsym(lib,
						"android_setsocknetwork");
			looked_up = true;
		}
		if (!fn)
			return ENOTSUP;
		r = fn(handle, (int)fd);
		if (r) {
			warning("EchoSDK/net: binding socket to %s (network %llu)"
				" failed: %d\n", ifname, (unsigned long long)handle, r);
			return EIO;
		}
		debug("EchoSDK/net: socket %d pinned to %s (network %llu)\n",
		      (int)fd, ifname, (unsigned long long)handle);
		return 0;
	}
#elif defined(__APPLE__)
	{
		unsigned idx = if_nametoindex(ifname);
		int r;

		if (!idx)
			return ENOENT;
		if (af == AF_INET6)
			r = setsockopt((int)fd, IPPROTO_IPV6, IPV6_BOUND_IF, &idx,
				       sizeof(idx));
		else
			r = setsockopt((int)fd, IPPROTO_IP, IP_BOUND_IF, &idx,
				       sizeof(idx));
		return r ? errno : 0;
	}
#elif defined(__linux__)
	{
		int r;
		(void)af;

		r = setsockopt((int)fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
			       (socklen_t)strlen(ifname));
		if (r)
			return errno == EPERM || errno == EACCES ? ENOTSUP : errno;
		return 0;
	}
#else
	(void)af;
	return ENOTSUP;
#endif
}
