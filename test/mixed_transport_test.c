/**
 * @file mixed_transport_test.c  Regression: WSS account after UDP account
 *
 * Reproduces the sequence a softphone uses when switching transports:
 *   1. register account A over UDP, place+end a call, destroy A
 *   2. register account B over WSS
 * Step 2 intermittently failed with MEDUT_ERR_TRANSPORT.
 *
 * Usage: mixed_transport_test <host> [--call <exten>]
 * Server: Asterisk with UDP :5060 and WSS :8089 (self-signed OK),
 *         users alice/bob password secret123.
 * Exit 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../include/medut.h"

static atomic_int g_reg_state = -1;
static atomic_int g_call_state = -1;
static char g_reg_err[256];

static void event_handler(const medut_event_t *ev, void *ud)
{
	(void)ud;
	switch (ev->type) {
	case MEDUT_EV_REG_STATE:
		if (ev->u.reg.state == MEDUT_REG_REGISTERED)
			atomic_store(&g_reg_state, 1);
		/* RECONNECTING is the same wire failure with a retry armed behind
		 * it; for a gate test it is just as fatal, and reporting it here
		 * keeps the failure fast instead of waiting out the timeout. */
		else if (ev->u.reg.state == MEDUT_REG_FAILED ||
		         ev->u.reg.state == MEDUT_REG_RECONNECTING) {
			snprintf(g_reg_err, sizeof(g_reg_err), "%s",
			         ev->u.reg.error_str ? ev->u.reg.error_str : "?");
			atomic_store(&g_reg_state, 0);
		}
		break;
	case MEDUT_EV_CALL_STATE:
		if (ev->u.call_state.state == MEDUT_CALL_ESTABLISHED)
			atomic_store(&g_call_state, 1);
		else if (ev->u.call_state.state >= MEDUT_CALL_ENDED)
			atomic_store(&g_call_state, 2);
		break;
	case MEDUT_EV_LOG:
		if (getenv("VERBOSE"))
			printf("[LOG] %s", ev->u.log.message);
		break;
	default:
		break;
	}
}

static int wait_for(atomic_int *var, int want, int timeout_s)
{
	for (int i = 0; i < timeout_s * 20; i++) {
		if (atomic_load(var) == want)
			return 0;
		if (var == &g_reg_state && atomic_load(var) == 0)
			return -1; /* failed */
		usleep(50000);
	}
	return -2; /* timeout */
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <host> [--call <exten>]\n", argv[0]);
		return 2;
	}
	const char *host = argv[1];
	const char *exten = (argc >= 4 && !strcmp(argv[2], "--call"))
	                  ? argv[3] : NULL;
	char buf[512];

	medut_config_t cfg;
	medut_config_init(&cfg);
	cfg.log_level     = 3;
	cfg.verify_server = false;
	cfg.event_cb      = event_handler;
	cfg.net_monitor_interval_s = 0;
	if (medut_init(&cfg)) { printf("FAIL init\n"); return 1; }

	/* ── Step 1: UDP account, optional call, destroy ── */
	medut_account_config_t a = {0};
	snprintf(buf, sizeof(buf), "alice@%s", host);
	a.uri = buf;
	a.password = "secret123";
	a.transport = MEDUT_TRANSPORT_UDP;

	medut_account_handle_t alice = NULL;
	if (medut_account_create(&a, &alice)) { printf("FAIL a-create\n"); return 1; }
	atomic_store(&g_reg_state, -1);
	medut_account_register(alice);
	if (wait_for(&g_reg_state, 1, 10)) { printf("FAIL a-register (%s)\n", g_reg_err); return 1; }

	if (exten) {
		char callee[512];
		snprintf(callee, sizeof(callee), "sip:%s@%s", exten, host);
		medut_call_handle_t call = NULL;
		atomic_store(&g_call_state, -1);
		if (medut_call_invite(alice, callee, &call)) {
			printf("FAIL invite\n"); return 1;
		}
		if (wait_for(&g_call_state, 1, 10)) { printf("FAIL establish\n"); return 1; }
		sleep(1);
		medut_call_hangup(call);
		wait_for(&g_call_state, 2, 5);
	}

	medut_account_destroy(alice);

	/* ── Step 2: WSS account ── */
	medut_account_config_t b = {0};
	char uri2[512], url2[512];
	snprintf(uri2, sizeof(uri2), "bob@%s", host);
	snprintf(url2, sizeof(url2), "wss://%s:8089/ws", host);
	b.uri = uri2;
	b.password = "secret123";
	b.server_url = url2;
	b.verify_tls = false;

	medut_account_handle_t bob = NULL;
	if (medut_account_create(&b, &bob)) { printf("FAIL b-create\n"); return 1; }
	atomic_store(&g_reg_state, -1);
	medut_account_register(bob);
	int rc = wait_for(&g_reg_state, 1, 12);
	if (rc) {
		printf("FAIL b-register rc=%d (%s)\n", rc, g_reg_err);
		return 1;
	}

	medut_account_destroy(bob);
	medut_shutdown();
	printf("PASS\n");
	return 0;
}
