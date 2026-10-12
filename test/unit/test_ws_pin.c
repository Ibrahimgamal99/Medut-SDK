/**
 * @file test_ws_pin.c  Unit tests for WebSocket connection pinning (ws_path.c)
 *
 * Links the real ws_path.c (compiled into this binary) against libre, stubs
 * __real_websock_connect (libre's renamed definition — see
 * cmake/patch-re-sources.cmake) to capture the URI the wrapper decided on,
 * and drives the registry the way account.c does — one set() per account
 * create, one unset() per destroy.
 *
 * What is worth pinning down here, because getting it wrong is silent and only
 * shows up as calls that die ~32 s in:
 *   - one server  → pinned, and a loopback Record-Route is redirected to it,
 *   - two servers → not pinned (never route one account to the other's server),
 *     but a target that cannot work is still redirected rather than dialled,
 *   - destroying the extra account brings pinning back (it used to latch off
 *     for the life of the process).
 *
 * Build: make test_ws_pin  (needs the linux-x86_64 sysroot from a host build)
 */

#include <stdio.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "../../src/medut_internal.h"

/* ws_path.c reads g_bsdk.cfg for ws_origin / ws_extra_headers / keepalive;
 * core.c is not linked in, so own the definition here. */
struct bsdk_ctx g_bsdk;

static int g_pass, g_fail;

#define CHECK(cond, ...)                                                     \
	do {                                                                 \
		if (cond) { g_pass++; }                                      \
		else {                                                       \
			g_fail++;                                            \
			printf("  FAIL %s:%d: ", __func__, __LINE__);        \
			printf(__VA_ARGS__);                                 \
			printf("\n");                                        \
		}                                                            \
	} while (0)

/* ── Capture what the wrapper passes down ──────────────────────────────── */

static char g_used[512];

int __real_websock_connect(struct websock_conn **connp, struct websock *sock,
                           struct http_cli *cli, const char *uri,
                           unsigned kaint,
                           websock_estab_h *estabh, websock_recv_h *recvh,
                           websock_close_h *closeh, void *arg,
                           const char *fmt, ...);

int __real_websock_connect(struct websock_conn **connp, struct websock *sock,
                           struct http_cli *cli, const char *uri,
                           unsigned kaint,
                           websock_estab_h *estabh, websock_recv_h *recvh,
                           websock_close_h *closeh, void *arg,
                           const char *fmt, ...)
{
	(void)connp; (void)sock; (void)cli; (void)kaint;
	(void)estabh; (void)recvh; (void)closeh; (void)arg; (void)fmt;

	str_ncpy(g_used, uri ? uri : "(null)", sizeof(g_used));
	return 0;
}

/* ws_path.c's sip_dialog_route glue references libre's renamed accessor; the
 * route tests below exercise bsdk_ws_route_override() directly, so the glue
 * is never called here.  Weak: if the link pulls libre's dialog.o (which
 * defines the real one under the rename) the real definition wins, and if it
 * does not, this stub satisfies the reference. */
struct sip_dialog;
__attribute__((weak))
const struct uri *__real_sip_dialog_route(const struct sip_dialog *dlg)
{
	(void)dlg;
	return NULL;
}

/* The URI the wrapper would actually connect to, for a libre-style call.
 * websock_connect() resolves to ws_path.c's definition — the public name it
 * owns in the shipped library too (libre's is renamed to __real_*). */
static const char *connect_to(const char *uri)
{
	g_used[0] = '\0';
	websock_connect(NULL, NULL, NULL, uri, 0, NULL, NULL, NULL,
	                NULL, "Sec-WebSocket-Protocol: sip\r\n");
	return g_used;
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

static void test_single_server(void)
{
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443,
	                   "/ws");

	CHECK(!strcmp(g_bsdk_ws_server, "wss://pbx.example.com:443"),
	      "pin is '%s'", g_bsdk_ws_server);
	CHECK(!strcmp(g_bsdk_ws_path, "/ws"), "path is '%s'", g_bsdk_ws_path);

	/* libre hands us the address it resolved from the dialog's
	 * Record-Route; behind a reverse proxy that is the server's loopback. */
	CHECK(!strcmp(connect_to("wss://127.0.0.1:8088/"),
	              "wss://pbx.example.com:443/ws"),
	      "loopback went to '%s'", g_used);

	/* The initial connect libre builds from the account's own address. */
	CHECK(!strcmp(connect_to("wss://82.129.158.253:443/"),
	              "wss://pbx.example.com:443/ws"),
	      "pinned connect went to '%s'", g_used);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443,
	                     "/ws");
	CHECK(!g_bsdk_ws_server[0], "pin survived destroy: '%s'",
	      g_bsdk_ws_server);
	CHECK(!g_bsdk_ws_path[0], "path survived destroy: '%s'",
	      g_bsdk_ws_path);
}

/* A second account on another server makes pinning ambiguous — and destroying
 * it must make it unambiguous again.  The regression: a mistyped account left
 * behind by the app disabled pinning for every later call in the process. */
static void test_stale_account_releases_pin(void)
{
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "typo.example.com", 443, "");
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443, "");

	CHECK(!g_bsdk_ws_server[0], "pinned to '%s' with two servers live",
	      g_bsdk_ws_server);

	/* Ambiguous: a real address is left alone, because sending it to the
	 * wrong server is worse than libre's routing. */
	CHECK(!strcmp(connect_to("wss://82.129.158.253:443/"),
	              "wss://82.129.158.253:443/"),
	      "real target rewritten to '%s'", g_used);

	/* A loopback target cannot work either way, so the newest server wins. */
	CHECK(!strcmp(connect_to("wss://127.0.0.1:8088/"),
	              "wss://pbx.example.com:443/"),
	      "dead target went to '%s'", g_used);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "typo.example.com", 443,
	                     "");
	CHECK(!strcmp(g_bsdk_ws_server, "wss://pbx.example.com:443"),
	      "pin after destroy is '%s'", g_bsdk_ws_server);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443, "");
}

/* Same server twice (two accounts on one PBX) is not a conflict. */
static void test_refcount_same_server(void)
{
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443,
	                   "/ws");
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443,
	                   "/ws");

	CHECK(!strcmp(g_bsdk_ws_server, "wss://pbx.example.com:443"),
	      "pin is '%s'", g_bsdk_ws_server);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443,
	                     "/ws");
	CHECK(!strcmp(g_bsdk_ws_server, "wss://pbx.example.com:443"),
	      "one destroy dropped the pin: '%s'", g_bsdk_ws_server);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443,
	                     "/ws");
	CHECK(!g_bsdk_ws_server[0], "pin is '%s' after both destroys",
	      g_bsdk_ws_server);
}

/* Host ambiguous, path agreed: substituting the path is still correct. */
static void test_path_survives_host_conflict(void)
{
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "a.example.com", 443, "/ws");
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "b.example.com", 443, "/ws");

	CHECK(!g_bsdk_ws_server[0], "pinned to '%s'", g_bsdk_ws_server);
	CHECK(!strcmp(g_bsdk_ws_path, "/ws"), "path is '%s'", g_bsdk_ws_path);
	CHECK(!strcmp(connect_to("wss://a.example.com:443/"),
	              "wss://a.example.com:443/ws"),
	      "path-only substitution gave '%s'", g_used);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "a.example.com", 443, "/ws");
	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "b.example.com", 443, "/ws");
}

/* Paths disagree: substituting either one would break the other's handshake. */
static void test_conflicting_paths_disable_substitution(void)
{
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "a.example.com", 443, "/ws");
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "b.example.com", 443, "/sip");

	CHECK(!g_bsdk_ws_path[0], "path is '%s' with two paths live",
	      g_bsdk_ws_path);
	CHECK(!strcmp(connect_to("wss://a.example.com:443/"),
	              "wss://a.example.com:443/"),
	      "path substituted anyway: '%s'", g_used);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "a.example.com", 443, "/ws");
	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "b.example.com", 443,
	                     "/sip");
}

/* A PBX genuinely on loopback (desktop against a local Asterisk) is ours, and
 * must not be "rescued" to somewhere else. */
static void test_own_loopback_server_untouched(void)
{
	bsdk_ws_set_server(MEDUT_TRANSPORT_WS, "127.0.0.1", 8088, "/ws");
	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443, "");

	CHECK(!strcmp(connect_to("ws://127.0.0.1:8088/"),
	              "ws://127.0.0.1:8088/"),
	      "own loopback server rewritten to '%s'", g_used);

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WS, "127.0.0.1", 8088, "/ws");
	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443, "");
}

/* Nothing registered (non-WS accounts only): pass everything through. */
static void test_no_servers_passthrough(void)
{
	CHECK(!strcmp(connect_to("wss://127.0.0.1:8088/"),
	              "wss://127.0.0.1:8088/"),
	      "rewrote '%s' with no servers registered", g_used);
}

/* ── In-dialog routing (RFC 7118 §B.2) ───────────────────────────────────────
 *
 * The regression this covers: an incoming call arrives with no Record-Route and
 * a Contact naming the PBX's internal address (`sip:asterisk@echo:5060`), so
 * libre routes in-dialog requests to a host that resolves nowhere.  The BYE was
 * accepted by sipsess_bye() and then died in DNS, asynchronously and silently —
 * the caller stayed on a call the app had already reported as ended.
 */

/* The decision under test.  Exercised directly rather than through the
 * sip_dialog_route() glue: handing libre's real accessor a synthetic dialog
 * is not safe. */
const struct uri *bsdk_ws_route_override(const struct uri *route);

static struct uri g_dlg_route;
static char       g_dlg_buf[256];

/* Parse `uri` as a dialog route and return whatever the SDK decides to use. */
static const struct uri *route_for(const char *uri)
{
	struct pl pl;

	str_ncpy(g_dlg_buf, uri, sizeof(g_dlg_buf));
	pl_set_str(&pl, g_dlg_buf);
	if (uri_decode(&g_dlg_route, &pl))
		return NULL;

	return bsdk_ws_route_override(&g_dlg_route);
}

static void test_indialog_route_follows_flow(void)
{
	const struct uri *r;

	bsdk_ws_set_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443, "/ws");

	/* The failing case: Asterisk's own Contact, unroutable from here. */
	r = route_for("sip:asterisk@echo:5060;transport=ws");
	CHECK(r != NULL, "route was dropped entirely");
	CHECK(r && !pl_strcmp(&r->host, "pbx.example.com"),
	      "in-dialog route was not moved to the flow");
	CHECK(r && r->port == 443, "port is %u", r ? r->port : 0);

	/* Already on the flow — must be returned untouched, not rebuilt. */
	r = route_for("sip:pbx.example.com:443;transport=wss;lr");
	CHECK(r != NULL && !pl_strcmp(&r->host, "pbx.example.com"),
	      "flow route was disturbed");

	/* A non-WebSocket dialog is none of our business: a UDP or TLS account
	 * routes by its own rules and must be passed through untouched. */
	r = route_for("sip:proxy.example.com:5060;transport=udp");
	CHECK(r != NULL && !pl_strcmp(&r->host, "proxy.example.com"),
	      "a UDP dialog was rerouted onto the WebSocket flow");

	/* No transport parameter at all — equally not ours to redirect. */
	r = route_for("sip:proxy.example.com:5060");
	CHECK(r != NULL && !pl_strcmp(&r->host, "proxy.example.com"),
	      "a transport-less route was rerouted");

	bsdk_ws_unset_server(MEDUT_TRANSPORT_WSS, "pbx.example.com", 443,
	                     "/ws");
}

/* With no pinned server there is no flow to prefer, so libre's own decision
 * stands — the same reasoning that disables URI pinning when servers are
 * ambiguous. */
static void test_indialog_route_unpinned_passthrough(void)
{
	const struct uri *r = route_for("sip:asterisk@echo:5060;transport=ws");

	CHECK(r != NULL && !pl_strcmp(&r->host, "echo"),
	      "route was rewritten with no server pinned");
}

int main(void)
{
	printf("ws pinning tests\n");

	test_single_server();
	test_stale_account_releases_pin();
	test_refcount_same_server();
	test_path_survives_host_conflict();
	test_conflicting_paths_disable_substitution();
	test_own_loopback_server_untouched();
	test_no_servers_passthrough();
	test_indialog_route_follows_flow();
	test_indialog_route_unpinned_passthrough();

	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
