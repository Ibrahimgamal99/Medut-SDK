/**
 * @file sip_status.h  SIP status code → echosdk_error_t classification
 *
 * Header-only and free of re/baresip so the mapping the SDK ships is the
 * mapping the unit test exercises (test/unit/test_sip_status.c), rather than a
 * copy of it.
 */

#ifndef ECHOSDK_SIP_STATUS_H
#define ECHOSDK_SIP_STATUS_H

#include <stdlib.h>
#include "../include/echosdk.h"

/* Parse the leading 3-digit SIP status from a reason string like
 * "403 Forbidden".  Returns 0 if the string doesn't start with a valid
 * 3-digit code. */
static inline int bsdk_sip_reason_code(const char *reason)
{
	if (!reason) return 0;
	char *end;
	long code = strtol(reason, &end, 10);
	if (end == reason || end - reason != 3 || code < 100 || code > 699)
		return 0;
	return (int)code;
}

/**
 * Classify a failed *call* by its SIP status.
 *
 * Returns ECHOSDK_OK for a status that is not a failure (including 0, "no
 * code in the reason string") so the caller can fall through to whatever
 * non-SIP explanation it has, such as a dead transport.
 */
static inline echosdk_error_t bsdk_sip_call_error(int scode)
{
	/* 401/407 are challenges this account could not answer — the app wants
	 * new credentials.  403 is not: the server understood the request and
	 * refused *it*, which on a call is a dialplan or policy decision
	 * ("you may not dial that").  Reporting it as an auth failure sent
	 * apps off to a login screen over a number the user simply isn't
	 * allowed to dial. */
	if (scode == 401 || scode == 407)
		return ECHOSDK_ERR_AUTH;
	if (scode == 403)
		return ECHOSDK_ERR_FORBIDDEN;
	/* 408 is what the setup watchdog cancels with, and what a proxy
	 * sends when its own transaction timed out — both are timeouts,
	 * not the malformed-request sense of ECHOSDK_ERR_INVAL. */
	if (scode == 408)
		return ECHOSDK_ERR_TIMEOUT;
	if (scode >= 500 && scode < 600)
		return ECHOSDK_ERR_SERVER_5XX;
	if (scode >= 400)
		return ECHOSDK_ERR_INVAL;
	return ECHOSDK_OK;
}

#endif /* ECHOSDK_SIP_STATUS_H */
