/**
 * @file test_url_parser.c  Unit tests for URL parsing (src/api/transport.c)
 *
 * Tests bsdk_parse_server_url with various URL forms.
 * This file reimplements the parser logic for standalone testing.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <strings.h>

#define TEST_PASS 0
#define TEST_FAIL 1

static int g_pass = 0;
static int g_fail = 0;

typedef enum {
	TRANSPORT_UDP = 0,
	TRANSPORT_TCP,
	TRANSPORT_TLS,
	TRANSPORT_WS,
	TRANSPORT_WSS,
} transport_t;

static int parse_url(const char *url, transport_t *out_t,
                      char *host, size_t host_sz,
                      uint16_t *port,
                      char *path, size_t path_sz)
{
	*out_t = TRANSPORT_UDP;
	*port = 0;
	host[0] = '\0';
	path[0] = '\0';

	if (!url) return -1;

	const char *p = url;
	uint16_t def_port = 5060;

	if (strncmp(p, "wss://", 6) == 0) {
		*out_t = TRANSPORT_WSS;
		def_port = 8089;
		p += 6;
	} else if (strncmp(p, "ws://", 5) == 0) {
		*out_t = TRANSPORT_WS;
		def_port = 8088;
		p += 5;
	} else if (strncmp(p, "sips://", 7) == 0) {
		*out_t = TRANSPORT_TLS;
		def_port = 5061;
		p += 7;
	} else if (strncmp(p, "sips:", 5) == 0) {
		*out_t = TRANSPORT_TLS;
		def_port = 5061;
		p += 5;
	} else if (strncmp(p, "sip://", 6) == 0) {
		*out_t = TRANSPORT_UDP;
		def_port = 5060;
		p += 6;
	} else if (strncmp(p, "sip:", 4) == 0) {
		*out_t = TRANSPORT_UDP;
		def_port = 5060;
		p += 4;
	} else {
		return -1;
	}

	const char *tp = strstr(p, ";transport=");
	if (tp) {
		const char *tv = tp + 11;
		if (strncasecmp(tv, "tls", 3) == 0) {
			*out_t = TRANSPORT_TLS;
			def_port = 5061;
		} else if (strncasecmp(tv, "tcp", 3) == 0) {
			*out_t = TRANSPORT_TCP;
			def_port = 5060;
		} else if (strncasecmp(tv, "udp", 3) == 0) {
			*out_t = TRANSPORT_UDP;
			def_port = 5060;
		}
	}

	const char *slash = strchr(p, '/');
	const char *semi = strchr(p, ';');
	size_t auth_len;
	if (slash && semi)
		auth_len = (size_t)(slash < semi ? slash : semi) - (size_t)(uintptr_t)p;
	else if (slash)
		auth_len = (size_t)(slash - p);
	else if (semi)
		auth_len = (size_t)(semi - p);
	else
		auth_len = strlen(p);

	if (slash && (!semi || slash < semi)) {
		size_t pi = 0;
		const char *s = slash;
		while (*s && pi < path_sz - 1)
			path[pi++] = *s++;
		path[pi] = '\0';
	}

	const char *colon = memchr(p, ':', auth_len);
	if (colon) {
		size_t h_len = (size_t)(colon - p);
		if (h_len >= host_sz) h_len = host_sz - 1;
		memcpy(host, p, h_len);
		host[h_len] = '\0';
		char *end;
		*port = (uint16_t)strtoul(colon + 1, &end, 10);
		if (*port == 0)
			*port = def_port;
	} else {
		if (auth_len >= host_sz) auth_len = host_sz - 1;
		memcpy(host, p, auth_len);
		host[auth_len] = '\0';
		*port = def_port;
	}

	return 0;
}

#define ASSERT_EQ_INT(name, a, b) do { \
	if ((a) != (b)) { \
		fprintf(stderr, "FAIL: %s = %d (expected %d)\n", name, (a), (b)); \
		g_fail++; \
	} else { g_pass++; } \
} while (0)

#define ASSERT_EQ_STR(name, a, b) do { \
	if (strcmp((a), (b)) != 0) { \
		fprintf(stderr, "FAIL: %s = \"%s\" (expected \"%s\")\n", name, (a), (b)); \
		g_fail++; \
	} else { g_pass++; } \
} while (0)

static void test_wss_with_port(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("wss://pbx.example.com:8089/ws", &t, host, sizeof(host),
	                    &port, path, sizeof(path));
	ASSERT_EQ_INT("wss_rc", rc, 0);
	ASSERT_EQ_INT("wss_transport", t, TRANSPORT_WSS);
	ASSERT_EQ_STR("wss_host", host, "pbx.example.com");
	ASSERT_EQ_INT("wss_port", port, 8089);
	ASSERT_EQ_STR("wss_path", path, "/ws");
}

static void test_ws_no_port(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("ws://internal:8088/ws", &t, host, sizeof(host),
	                    &port, path, sizeof(path));
	ASSERT_EQ_INT("ws_rc", rc, 0);
	ASSERT_EQ_INT("ws_transport", t, TRANSPORT_WS);
	ASSERT_EQ_STR("ws_host", host, "internal");
	ASSERT_EQ_INT("ws_port", port, 8088);
	ASSERT_EQ_STR("ws_path", path, "/ws");
}

static void test_sip_default_port(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("sip://pbx.example.com", &t, host, sizeof(host),
	                    &port, path, sizeof(path));
	ASSERT_EQ_INT("sip_rc", rc, 0);
	ASSERT_EQ_INT("sip_transport", t, TRANSPORT_UDP);
	ASSERT_EQ_STR("sip_host", host, "pbx.example.com");
	ASSERT_EQ_INT("sip_port", port, 5060);
}

static void test_sips_with_port(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("sips://pbx.example.com:5061", &t, host, sizeof(host),
	                    &port, path, sizeof(path));
	ASSERT_EQ_INT("sips_rc", rc, 0);
	ASSERT_EQ_INT("sips_transport", t, TRANSPORT_TLS);
	ASSERT_EQ_INT("sips_port", port, 5061);
}

static void test_null_url(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url(NULL, &t, host, sizeof(host), &port, path, sizeof(path));
	ASSERT_EQ_INT("null_rc", rc, -1);
}

static void test_no_scheme(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("pbx.example.com:5060", &t, host, sizeof(host),
	                    &port, path, sizeof(path));
	ASSERT_EQ_INT("noscheme_rc", rc, -1);
}

static void test_wss_no_path(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("wss://pbx.example.com:8443", &t, host, sizeof(host),
	                    &port, path, sizeof(path));
	ASSERT_EQ_INT("wss_nopath_rc", rc, 0);
	ASSERT_EQ_STR("wss_nopath_host", host, "pbx.example.com");
	ASSERT_EQ_INT("wss_nopath_port", port, 8443);
	ASSERT_EQ_STR("wss_nopath_path", path, "");
}

static void test_sip_transport_tls_no_port(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("sip:pbx.example.com;transport=tls", &t, host,
	                    sizeof(host), &port, path, sizeof(path));
	ASSERT_EQ_INT("sip_tls_rc", rc, 0);
	ASSERT_EQ_INT("sip_tls_transport", t, TRANSPORT_TLS);
	ASSERT_EQ_STR("sip_tls_host", host, "pbx.example.com");
	ASSERT_EQ_INT("sip_tls_port", port, 5061);
}

static void test_sip_transport_tcp(void)
{
	transport_t t;
	char host[256], path[256];
	uint16_t port;
	int rc = parse_url("sip:pbx.example.com:5060;transport=tcp", &t, host,
	                    sizeof(host), &port, path, sizeof(path));
	ASSERT_EQ_INT("sip_tcp_rc", rc, 0);
	ASSERT_EQ_INT("sip_tcp_transport", t, TRANSPORT_TCP);
	ASSERT_EQ_INT("sip_tcp_port", port, 5060);
}

int main(void)
{
	test_wss_with_port();
	test_ws_no_port();
	test_sip_default_port();
	test_sips_with_port();
	test_null_url();
	test_no_scheme();
	test_wss_no_path();
	test_sip_transport_tls_no_port();
	test_sip_transport_tcp();

	printf("URL parser tests: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail > 0 ? TEST_FAIL : TEST_PASS;
}
