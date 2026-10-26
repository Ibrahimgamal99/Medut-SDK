/**
 * @file error.c  Human-readable error strings for MEDUT_ERR_* codes
 */

#include "medut_internal.h"

const char *medut_strerror(int err)
{
	switch ((medut_error_t)err) {
	case MEDUT_OK:                       return "ok";
	case MEDUT_ERR_INVAL:                return "invalid argument";
	case MEDUT_ERR_NOMEM:                return "out of memory";
	case MEDUT_ERR_STATE:                return "wrong lifecycle state";
	case MEDUT_ERR_DNS:                  return "DNS resolution failed";
	case MEDUT_ERR_TRANSPORT:            return "transport error";
	case MEDUT_ERR_AUTH:                 return "authentication failed";
	case MEDUT_ERR_SERVER_5XX:           return "server error (5xx)";
	case MEDUT_ERR_WS_PROTOCOL_REJECTED: return "WebSocket protocol rejected";
	case MEDUT_ERR_TIMEOUT:              return "timeout";
	case MEDUT_ERR_ALREADY:              return "already initialized";
	case MEDUT_ERR_FORBIDDEN:            return "forbidden (403)";
	case MEDUT_ERR_TLS_CERT:             return "TLS certificate rejected";
	default: {
		static char buf[32];
		re_snprintf(buf, sizeof(buf), "unknown error %d", err);
		return buf;
	}
	}
}
