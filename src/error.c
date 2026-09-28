/**
 * @file error.c  Human-readable error strings for ECHOSDK_ERR_* codes
 */

#include "echosdk_internal.h"

const char *echosdk_strerror(int err)
{
	switch ((echosdk_error_t)err) {
	case ECHOSDK_OK:                       return "ok";
	case ECHOSDK_ERR_INVAL:                return "invalid argument";
	case ECHOSDK_ERR_NOMEM:                return "out of memory";
	case ECHOSDK_ERR_STATE:                return "wrong lifecycle state";
	case ECHOSDK_ERR_DNS:                  return "DNS resolution failed";
	case ECHOSDK_ERR_TRANSPORT:            return "transport error";
	case ECHOSDK_ERR_AUTH:                 return "authentication failed";
	case ECHOSDK_ERR_SERVER_5XX:           return "server error (5xx)";
	case ECHOSDK_ERR_WS_PROTOCOL_REJECTED: return "WebSocket protocol rejected";
	case ECHOSDK_ERR_TIMEOUT:              return "timeout";
	case ECHOSDK_ERR_ALREADY:              return "already initialized";
	case ECHOSDK_ERR_FORBIDDEN:            return "forbidden (403)";
	default: {
		static char buf[32];
		re_snprintf(buf, sizeof(buf), "unknown error %d", err);
		return buf;
	}
	}
}
