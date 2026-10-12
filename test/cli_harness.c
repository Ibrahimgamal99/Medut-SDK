/**
 * @file cli_harness.c  Integration test harness
 *
 * Usage:
 *   ./cli_harness --server wss://pbx:8089/ws \
 *                 --user sip:alice@pbx       \
 *                 --pass secret              \
 *                 [--call sip:bob@pbx]       \
 *                 [--timeout 30]
 *
 * Exit 0 on success (REGISTERED + optional ESTABLISHED + ENDED).
 * Exit 1 on any failure or timeout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "../include/medut.h"

/* ── State machine ───────────────────────────────────────────────────────── */

static atomic_int g_registered = 0;
static atomic_int g_established = 0;
static atomic_int g_ended = 0;
static atomic_int g_failed = 0;

static medut_account_handle_t g_acct = NULL;
static medut_call_handle_t    g_call = NULL;
static const char              *g_call_target = NULL;

/* ── Event callback ──────────────────────────────────────────────────────── */

static void event_handler(const medut_event_t *ev, void *ud)
{
	(void)ud;

	switch (ev->type) {

	case MEDUT_EV_REG_STATE:
		switch (ev->u.reg.state) {
		case MEDUT_REG_REGISTERED:
			printf("[REG] Registered\n");
			atomic_store(&g_registered, 1);

			/* Place outgoing call if requested */
			if (g_call_target && !g_call) {
				int err = medut_call_invite(g_acct,
				                              g_call_target, &g_call);
				if (err) {
					printf("[ERR] invite failed: %d\n", err);
					atomic_store(&g_failed, 1);
				} else {
					printf("[CALL] Inviting %s\n", g_call_target);
				}
			} else if (!g_call_target) {
				/* Register-only test: success */
				atomic_store(&g_ended, 1);
			}
			break;
		/* RECONNECTING carries the same failure with a retry armed; the
		 * harness has one shot, so it reports and stops either way. */
		case MEDUT_REG_RECONNECTING:
		case MEDUT_REG_FAILED:
			fprintf(stderr, "[REG] %s: %s\n",
			        ev->u.reg.state == MEDUT_REG_RECONNECTING
			                ? "Reconnecting" : "Failed",
			        ev->u.reg.error_str ? ev->u.reg.error_str : "?");
			atomic_store(&g_failed, 1);
			break;
		default:
			break;
		}
		break;

	case MEDUT_EV_CALL_STATE:
		switch (ev->u.call_state.state) {
		case MEDUT_CALL_ESTABLISHED:
			printf("[CALL] Established\n");
			atomic_store(&g_established, 1);
			/* Immediately hang up for test */
			medut_call_hangup(ev->u.call_state.call);
			break;
		case MEDUT_CALL_ENDED:
			printf("[CALL] Ended\n");
			atomic_store(&g_ended, 1);
			break;
		case MEDUT_CALL_FAILED:
			fprintf(stderr, "[CALL] Failed: %s\n",
			        ev->u.call_state.reason ? ev->u.call_state.reason : "?");
			atomic_store(&g_failed, 1);
			break;
		default:
			break;
		}
		break;

	case MEDUT_EV_LOG:
		if (getenv("HARNESS_VERBOSE"))
			printf("[LOG] %s", ev->u.log.message);
		break;

	case MEDUT_EV_SDP_NEGOTIATION:
		printf("[SDP] codec=%s crypto=%s\n",
		       ev->u.sdp.negotiated_codec  ? ev->u.sdp.negotiated_codec  : "?",
		       ev->u.sdp.negotiated_crypto ? ev->u.sdp.negotiated_crypto : "?");
		break;

	default:
		break;
	}
}

/* ── Argument parsing ────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
	fprintf(stderr,
	        "Usage: %s --server <url> --user <aor> --pass <password>"
	        " [--call <uri>] [--timeout <sec>]\n", prog);
	exit(1);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	const char *server  = NULL;
	const char *user    = NULL;
	const char *pass    = NULL;
	const char *call_to = NULL;
	int timeout_s       = 30;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--server") && i + 1 < argc)
			server = argv[++i];
		else if (!strcmp(argv[i], "--user") && i + 1 < argc)
			user = argv[++i];
		else if (!strcmp(argv[i], "--pass") && i + 1 < argc)
			pass = argv[++i];
		else if (!strcmp(argv[i], "--call") && i + 1 < argc)
			call_to = argv[++i];
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
			timeout_s = atoi(argv[++i]);
		else
			usage(argv[0]);
	}

	if (!server || !user || !pass)
		usage(argv[0]);

	g_call_target = call_to;

	/* Init */
	medut_config_t cfg;
	medut_config_init(&cfg);
	cfg.event_cb   = event_handler;
	cfg.server_url = server;
	cfg.log_level  = getenv("HARNESS_VERBOSE") ? 3 : 0;

	int err = medut_init(&cfg);
	if (err) {
		fprintf(stderr, "[ERR] medut_init: %d\n", err);
		return 1;
	}

	/* Create account */
	medut_account_config_t acfg = {
		.uri      = user,
		.password = pass,
	};
	err = medut_account_create(&acfg, &g_acct);
	if (err) {
		fprintf(stderr, "[ERR] account_create: %d\n", err);
		medut_shutdown();
		return 1;
	}

	err = medut_account_register(g_acct);
	if (err) {
		fprintf(stderr, "[ERR] account_register: %d\n", err);
		medut_shutdown();
		return 1;
	}

	/* Poll for completion */
	time_t deadline = time(NULL) + timeout_s;
	while (time(NULL) < deadline) {
		if (atomic_load(&g_failed)) {
			fprintf(stderr, "[FAIL] Operation failed\n");
			medut_shutdown();
			return 1;
		}
		if (atomic_load(&g_ended))
			break;
		usleep(50000); /* 50 ms */
	}

	int result = 0;
	if (!atomic_load(&g_ended)) {
		fprintf(stderr, "[FAIL] Timed out after %d seconds\n", timeout_s);
		result = 1;
	} else {
		printf("[OK] Test passed\n");
	}

	medut_shutdown();
	return result;
}
