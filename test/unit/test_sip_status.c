/**
 * @file test_sip_status.c  Unit tests for SIP status → medut_error_t mapping
 *
 * Exercises the real mapping the SDK ships (src/sip_status.h), not a copy:
 * the header is free of re/baresip precisely so this test can include it.
 */

#include <stdio.h>
#include <string.h>

#include "../../src/sip_status.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_EQ_INT(name, a, b) do { \
	if ((int)(a) != (int)(b)) { \
		fprintf(stderr, "FAIL: %s = %d (expected %d)\n", name, \
		        (int)(a), (int)(b)); \
		g_fail++; \
	} else { g_pass++; } \
} while (0)

static void test_reason_code_parsing(void)
{
	ASSERT_EQ_INT("403 Forbidden", bsdk_sip_reason_code("403 Forbidden"), 403);
	ASSERT_EQ_INT("486 Busy Here", bsdk_sip_reason_code("486 Busy Here"), 486);
	ASSERT_EQ_INT("null",          bsdk_sip_reason_code(NULL), 0);
	ASSERT_EQ_INT("no code",       bsdk_sip_reason_code("Connection reset by peer [104]"), 0);
	ASSERT_EQ_INT("two digits",    bsdk_sip_reason_code("40 Nonsense"), 0);
	ASSERT_EQ_INT("out of range",  bsdk_sip_reason_code("999 Nope"), 0);
}

/* The bug this mapping exists for: a dialplan refusal is not a login failure.
 * 403 used to arrive as MEDUT_ERR_AUTH, which sent apps to a credentials
 * prompt over a number the user simply is not allowed to dial. */
static void test_403_is_forbidden_not_auth(void)
{
	ASSERT_EQ_INT("403", bsdk_sip_call_error(403), MEDUT_ERR_FORBIDDEN);
}

/* ...while the codes that really are credential challenges keep reporting
 * MEDUT_ERR_AUTH. */
static void test_401_407_still_auth(void)
{
	ASSERT_EQ_INT("401", bsdk_sip_call_error(401), MEDUT_ERR_AUTH);
	ASSERT_EQ_INT("407", bsdk_sip_call_error(407), MEDUT_ERR_AUTH);
}

static void test_other_failures_unchanged(void)
{
	ASSERT_EQ_INT("408", bsdk_sip_call_error(408), MEDUT_ERR_TIMEOUT);
	ASSERT_EQ_INT("404", bsdk_sip_call_error(404), MEDUT_ERR_INVAL);
	ASSERT_EQ_INT("486", bsdk_sip_call_error(486), MEDUT_ERR_INVAL);
	ASSERT_EQ_INT("500", bsdk_sip_call_error(500), MEDUT_ERR_SERVER_5XX);
	ASSERT_EQ_INT("503", bsdk_sip_call_error(503), MEDUT_ERR_SERVER_5XX);
	ASSERT_EQ_INT("603", bsdk_sip_call_error(603), MEDUT_ERR_INVAL);
}

/* Not a failure status, and "no code at all": both leave the caller free to
 * fall through to its transport explanation. */
static void test_non_failures_are_ok(void)
{
	ASSERT_EQ_INT("no code", bsdk_sip_call_error(0),   MEDUT_OK);
	ASSERT_EQ_INT("180",     bsdk_sip_call_error(180), MEDUT_OK);
	ASSERT_EQ_INT("200",     bsdk_sip_call_error(200), MEDUT_OK);
}

/* FORBIDDEN was appended to the end of the enum so apps built against an
 * older header keep their own numbering. */
static void test_enum_values_are_stable(void)
{
	ASSERT_EQ_INT("ERR_AUTH",      MEDUT_ERR_AUTH,      -6);
	ASSERT_EQ_INT("ERR_ALREADY",   MEDUT_ERR_ALREADY,   -10);
	ASSERT_EQ_INT("ERR_FORBIDDEN", MEDUT_ERR_FORBIDDEN, -11);
}

int main(void)
{
	test_reason_code_parsing();
	test_403_is_forbidden_not_auth();
	test_401_407_still_auth();
	test_other_failures_unchanged();
	test_non_failures_are_ok();
	test_enum_values_are_stable();

	printf("SIP status tests: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail > 0 ? TEST_FAIL : TEST_PASS;
}
