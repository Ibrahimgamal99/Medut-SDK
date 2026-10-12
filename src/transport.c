/**
 * @file transport.c  Server URL parsing and transport helpers
 *
 * Parses "wss://host:8089/ws", "sips://host", etc. into components that
 * baresip's account AOR and outbound proxy strings expect.
 */

#include <string.h>
#include <stdlib.h>
#include "medut_internal.h"

const char *bsdk_transport_str(medut_transport_t t)
{
	switch (t) {
	case MEDUT_TRANSPORT_UDP: return "udp";
	case MEDUT_TRANSPORT_TCP: return "tcp";
	case MEDUT_TRANSPORT_TLS: return "tls";
	case MEDUT_TRANSPORT_WS:  return "ws";
	case MEDUT_TRANSPORT_WSS: return "wss";
	default:                    return "udp";
	}
}

const char *bsdk_mediaenc_str(medut_media_enc_t enc)
{
	switch (enc) {
	case MEDUT_MEDIA_ENC_SDES:      return "srtp";
	case MEDUT_MEDIA_ENC_DTLS_SRTP: return "dtls_srtp";
	default:                          return NULL;
	}
}

/**
 * Parse a server URL into transport + host + port + path.
 *
 * Supported schemes and their default ports:
 *   sip:   → UDP, port 5060
 *   sips:  → TLS, port 5061
 *   ws:    → WS,  port 80
 *   wss:   → WSS, port 443
 *
 * The ;transport=XXX suffix on sip:/sips: URIs overrides the default
 * transport derived from the scheme. Examples:
 *
 *   "wss://pbx.example.com/ws"               → WSS, host=pbx.example.com, 443, /ws
 *   "wss://pbx.example.com:443/ws"           → WSS, host=pbx.example.com,  443, /ws
 *   "ws://pbx.internal:8088/ws"              → WS,  host=pbx.internal,    8088, /ws
 *   "sip:pbx.example.com"                    → UDP, host=pbx.example.com,  5060, ""
 *   "sip:pbx.example.com:443;transport=wss"        → WSS, host=pbx.example.com,        443, ""
 *   "sip:pbx.example.com;transport=tls"            → TLS, host=pbx.example.com,       5061, ""
 */
int bsdk_parse_server_url(const char *url,
                           medut_transport_t *out_transport,
                           char *host, size_t host_sz,
                           uint16_t *port,
                           char *path, size_t path_sz)
{
	if (!url || !out_transport || !host || !port || !path)
		return EINVAL;

	host[0] = '\0';
	path[0] = '\0';
	*port   = 0;

	medut_transport_t transport;
	uint16_t default_port;
	const char *rest;

	/* A portless ws:// / wss:// URL means the WebSocket default (RFC 6455
	 * §3): 443 for wss, 80 for ws — the same ports a browser would use, and
	 * what an edge proxy in front of the PBX terminates on.  NOT Asterisk's
	 * 8088/8089: those are a convention of one PBX's own listener, and anyone
	 * pointing at it writes them explicitly.  Defaulting to them instead sent
	 * "wss://pbx.example.com/ws" to a port nothing answers on, which presents
	 * as REGISTER timing out against a server that is plainly reachable. */
	if (strncmp(url, "wss://", 6) == 0) {
		transport    = MEDUT_TRANSPORT_WSS;
		default_port = 443;
		rest         = url + 6;
	} else if (strncmp(url, "ws://", 5) == 0) {
		transport    = MEDUT_TRANSPORT_WS;
		default_port = 80;
		rest         = url + 5;
	} else if (strncmp(url, "sips:", 5) == 0) {
		transport    = MEDUT_TRANSPORT_TLS;
		default_port = 5061;
		rest         = url + 5;
		if (rest[0] == '/' && rest[1] == '/') rest += 2;
	} else if (strncmp(url, "sip:", 4) == 0) {
		transport    = MEDUT_TRANSPORT_UDP;
		default_port = 5060;
		rest         = url + 4;
		if (rest[0] == '/' && rest[1] == '/') rest += 2;
	} else {
		transport    = MEDUT_TRANSPORT_UDP;
		default_port = 5060;
		rest         = url;
	}

	/* Extract ;transport=XXX override from the end of the URI */
	const char *tp = strstr(rest, ";transport=");
	if (tp) {
		const char *tv = tp + 11;
		if (strncasecmp(tv, "wss", 3) == 0) {
			transport    = MEDUT_TRANSPORT_WSS;
			default_port = 443;
		} else if (strncasecmp(tv, "ws", 2) == 0) {
			transport    = MEDUT_TRANSPORT_WS;
			default_port = 80;
		} else if (strncasecmp(tv, "tls", 3) == 0) {
			transport    = MEDUT_TRANSPORT_TLS;
			default_port = 5061;
		} else if (strncasecmp(tv, "tcp", 3) == 0) {
			transport    = MEDUT_TRANSPORT_TCP;
			default_port = 5060;
		} else if (strncasecmp(tv, "udp", 3) == 0) {
			transport    = MEDUT_TRANSPORT_UDP;
			default_port = 5060;
		}
	}

	*out_transport = transport;

	/* Authority is everything before '/' or ';' — whichever comes first */
	const char *slash = strchr(rest, '/');
	const char *semi  = strchr(rest, ';');
	size_t authority_len;
	if (slash && semi)
		authority_len = (size_t)(slash < semi ? slash : semi) - (size_t)(uintptr_t)rest;
	else if (slash)
		authority_len = (size_t)(slash - rest);
	else if (semi)
		authority_len = (size_t)(semi - rest);
	else
		authority_len = strlen(rest);

	/* Skip user part (before '@') in authority if present */
	const char *at = memchr(rest, '@', authority_len);
	if (at) {
		size_t user_len = (size_t)(at - rest) + 1;
		rest += user_len;
		authority_len -= user_len;
	}

	if (slash && (!semi || slash < semi)) {
		/* A bare "/" is not a meaningful path — leave path empty so the
		 * caller can apply its own default (e.g. "/ws" for WebSocket). */
		if (slash[1] != '\0' && slash[1] != ';')
			str_ncpy(path, slash, path_sz);
	}

	const char *colon = memchr(rest, ':', authority_len);
	if (colon) {
		size_t h_len = (size_t)(colon - rest);
		if (h_len >= host_sz) h_len = host_sz - 1;
		memcpy(host, rest, h_len);
		host[h_len] = '\0';
		char *end;
		*port = (uint16_t)strtoul(colon + 1, &end, 10);
		if (*port == 0)
			*port = default_port;
	} else {
		if (authority_len >= host_sz) authority_len = host_sz - 1;
		memcpy(host, rest, authority_len);
		host[authority_len] = '\0';
		*port = default_port;
	}

	return 0;
}

/**
 * Build the baresip outbound proxy string.
 *
 * sip_dialog_alloc wraps this in "Route: <%s;lr>", so the value here
 * must be a plain SIP URI without angle brackets — e.g.:
 *   sip:172.16.0.1:5060;transport=udp
 *   sip:pbx.example.com:443;transport=wss
 *
 * server_url, if non-NULL, takes precedence over server_host/server_port.
 */
int bsdk_build_outbound(const char *server_url,
                         const char *server_host, uint16_t server_port,
                         medut_transport_t transport,
                         char *buf, size_t buf_sz)
{
	char host[256] = {0};
	char path[256] = {0};
	uint16_t port  = server_port;

	if (server_url) {
		bsdk_parse_server_url(server_url, &transport,
		                      host, sizeof(host),
		                      &port, path, sizeof(path));
	} else {
		str_ncpy(host, server_host ? server_host : "", sizeof(host));
		if (!port) {
			switch (transport) {
			case MEDUT_TRANSPORT_TLS: port = 5061; break;
			/* WebSocket defaults, as in bsdk_parse_server_url. */
			case MEDUT_TRANSPORT_WS:  port = 80; break;
			case MEDUT_TRANSPORT_WSS: port = 443; break;
			default:                    port = 5060; break;
			}
		}
	}

	re_snprintf(buf, buf_sz, "sip:%s:%u;transport=%s",
	            host, port, bsdk_transport_str(transport));
	return 0;
}
