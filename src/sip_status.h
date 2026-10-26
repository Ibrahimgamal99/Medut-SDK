/**
 * @file sip_status.h  SIP status code → medut_error_t classification
 *
 * Header-only and free of re/baresip so the mapping the SDK ships is the
 * mapping the unit test exercises (test/unit/test_sip_status.c), rather than a
 * copy of it.
 */

#ifndef MEDUT_SIP_STATUS_H
#define MEDUT_SIP_STATUS_H

#include <stdlib.h>
#include <string.h>
#include "../include/medut.h"

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

/* Parse the errno from a reason string libre rendered with "%m", which reads
 * "<strerror text> [<errno>]" — e.g. "Protocol error [71]".  Returns 0 when
 * the string does not end that way (a SIP status, or no reason at all). */
static inline int bsdk_reason_errno(const char *reason)
{
	if (!reason) return 0;
	const char *open = strrchr(reason, '[');
	if (!open) return 0;
	char *end;
	long n = strtol(open + 1, &end, 10);
	if (end == open + 1 || end[0] != ']' || end[1] != '\0' || n <= 0)
		return 0;
	return (int)n;
}

/**
 * Classify a failed *call* by its SIP status.
 *
 * Returns MEDUT_OK for a status that is not a failure (including 0, "no
 * code in the reason string") so the caller can fall through to whatever
 * non-SIP explanation it has, such as a dead transport.
 */
static inline medut_error_t bsdk_sip_call_error(int scode)
{
	/* 401/407 are challenges this account could not answer — the app wants
	 * new credentials.  403 is not: the server understood the request and
	 * refused *it*, which on a call is a dialplan or policy decision
	 * ("you may not dial that").  Reporting it as an auth failure sent
	 * apps off to a login screen over a number the user simply isn't
	 * allowed to dial. */
	if (scode == 401 || scode == 407)
		return MEDUT_ERR_AUTH;
	if (scode == 403)
		return MEDUT_ERR_FORBIDDEN;
	/* 408 is what the setup watchdog cancels with, and what a proxy
	 * sends when its own transaction timed out — both are timeouts,
	 * not the malformed-request sense of MEDUT_ERR_INVAL. */
	if (scode == 408)
		return MEDUT_ERR_TIMEOUT;
	if (scode >= 500 && scode < 600)
		return MEDUT_ERR_SERVER_5XX;
	if (scode >= 400)
		return MEDUT_ERR_INVAL;
	return MEDUT_OK;
}

#endif /* MEDUT_SIP_STATUS_H */
