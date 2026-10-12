/**
 * @file event_queue_accounting_test.c
 *
 * Regression test: one incoming SIP MESSAGE must not silence the SDK.
 *
 * The event queue is a bounded list plus a length counter.  Producers
 * increment it, the single consumer decrements it once per event drained, and
 * every producer refuses to enqueue once the length reaches the cap.  The
 * counter is a size_t.
 *
 * Four producers used to append to the list *without* incrementing it —
 * incoming SIP MESSAGE (message.c), MWI and presence NOTIFY (presence.c), and
 * the incoming REFER (transfer.c).  Each such event therefore made the counter
 * one lower than the queue really was, and the first one drained from an
 * otherwise-empty queue took it from 0 to SIZE_MAX.  From that instant
 * `len >= max` was permanently true and *every* later event was dropped, for
 * the life of the process: call state, registration, DTMF, handover, logs.
 * An app subscribed to BLF or presence lost its entire event stream seconds
 * after start-up.
 *
 * The bug is invisible from outside until you look for exactly this: an event
 * that arrives *after* one of those four.  So the test drives a real SIP
 * MESSAGE into the stack over the loopback, waits for it, and then asserts the
 * ordinary event flow is still alive.
 *
 * No SIP server is needed.  The account registers against TEST-NET-1
 * (192.0.2.1, RFC 5737), which black-holes: that produces the registration
 * events used as the "still alive" probe, and never completes.  The MESSAGE is
 * sent by this process to the SDK's own pinned UDP port.
 *
 * Note the MESSAGE cannot be sent to 127.0.0.1: baresip's interface filter
 * drops loopback, so the stack never binds it.  The destination comes from
 * medut_network_local_addr() instead, which is the address it did bind.
 * Only the user part of the Request-URI is matched (uag_find_msg against the
 * UA's contact user), so the URI host stays the unroutable AoR host.
 *
 * Build:
 *   gcc -g -O1 -std=gnu11 \
 *       -Iinclude test/event_queue_accounting_test.c \
 *       dist/linux/x86_64/medut.so \
 *       -Wl,-rpath,'$ORIGIN/../dist/linux/x86_64' \
 *       -o test/event_queue_accounting_test && ./test/event_queue_accounting_test
 *
 * Exit 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <threads.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../include/medut.h"

#define SIP_PORT   45678
#define AOR_USER   "alice"
#define AOR_HOST   "192.0.2.1"

static mtx_t      g_lock;
static atomic_int g_msg_seen;      /* the MESSAGE landed                     */
static atomic_int g_events_after;  /* any event at all after the MESSAGE     */
static atomic_int g_reg_after;     /* a REG_STATE event after the MESSAGE    */

static void event_handler(const medut_event_t *ev, void *ud)
{
	(void)ud;

	if (ev->type == MEDUT_EV_MESSAGE) {
		printf("  [event] MESSAGE from %s: %s\n",
		       ev->u.msg.from_uri ? ev->u.msg.from_uri : "(none)",
		       ev->u.msg.body ? ev->u.msg.body : "(empty)");
		fflush(stdout);
		atomic_store(&g_msg_seen, 1);
		return;
	}

	/* Everything from here on is the probe: with the bug, none of it
	 * arrives once the MESSAGE above has been drained. */
	if (!atomic_load(&g_msg_seen))
		return;

	atomic_fetch_add(&g_events_after, 1);

	if (ev->type == MEDUT_EV_REG_STATE) {
		printf("  [event] REG_STATE=%d after MESSAGE\n",
		       (int)ev->u.reg.state);
		fflush(stdout);
		atomic_store(&g_reg_after, 1);
	}
}

/* Send a SIP MESSAGE to the SDK's own listening port.
 *
 * `host` must be an address the SDK actually bound — see the note in the file
 * header about loopback. */
static int send_sip_message(const char *host)
{
	struct sockaddr_in dst, src;
	socklen_t          srclen = sizeof(src);
	char               req[1024];
	char               srcip[INET_ADDRSTRLEN] = "127.0.0.1";
	int                fd, n;

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port   = htons(SIP_PORT);
	if (inet_pton(AF_INET, host, &dst.sin_addr) != 1)
		return -1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	/* connect() so the kernel picks a source address that can reach the
	 * SDK, then read it back for the Via — baresip replies to whatever the
	 * Via names, and a wrong one would just be dropped. */
	if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
		close(fd);
		return -1;
	}
	if (getsockname(fd, (struct sockaddr *)&src, &srclen) == 0)
		inet_ntop(AF_INET, &src.sin_addr, srcip, sizeof(srcip));

	n = snprintf(req, sizeof(req),
		"MESSAGE sip:" AOR_USER "@" AOR_HOST " SIP/2.0\r\n"
		"Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-evqtest-1\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:probe@%s>;tag=evqtest\r\n"
		"To: <sip:" AOR_USER "@" AOR_HOST ">\r\n"
		"Call-ID: evq-accounting-test\r\n"
		"CSeq: 1 MESSAGE\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 5\r\n"
		"\r\n"
		"hello",
		srcip, (unsigned)ntohs(src.sin_port), srcip);

	if (send(fd, req, (size_t)n, 0) != n) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

#define CHECK(cond, ...) do {                   \
	if (!(cond)) {                          \
		printf("FAIL: " __VA_ARGS__);   \
		return 1;                       \
	}                                       \
} while (0)

int main(void)
{
	medut_config_t         cfg;
	medut_account_config_t acfg;
	medut_account_handle_t acct = NULL;
	char                     laddr[64] = {0};
	int                      rc, i;

	mtx_init(&g_lock, mtx_plain);

	medut_config_init(&cfg);
	cfg.event_cb   = event_handler;
	cfg.log_level  = 1;                 /* warnings — keep the output small */
	cfg.transport  = MEDUT_TRANSPORT_UDP;
	cfg.local_port = SIP_PORT;          /* pinned so we can post to it      */

	/* Keep the registration churning so there is a steady supply of events
	 * to prove the queue still works, but with no jitter so the timing is
	 * deterministic. */
	cfg.sip_timer_f_ms         = 2000;
	cfg.reg_retry_initial_ms   = 500;
	cfg.reg_retry_max_ms       = 1000;
	cfg.reg_retry_backoff      = 1.0f;
	cfg.reg_retry_jitter       = 0.f;
	cfg.reg_retry_max_attempts = 0;     /* retry forever */

	/* Silence the sources of unrelated events. */
	cfg.keepalive_interval     = 0;
	cfg.net_monitor_interval_s = 0;
	cfg.stats_interval_ms      = 0;

	rc = medut_init(&cfg);
	CHECK(rc == MEDUT_OK, "medut_init returned %d (%s)\n",
	      rc, medut_strerror(rc));

	memset(&acfg, 0, sizeof(acfg));
	acfg.uri         = "sip:" AOR_USER "@" AOR_HOST;
	acfg.password    = "secret";
	acfg.server_host = AOR_HOST;
	acfg.transport   = MEDUT_TRANSPORT_UDP;

	rc = medut_account_create(&acfg, &acct);
	CHECK(rc == MEDUT_OK, "medut_account_create returned %d (%s)\n",
	      rc, medut_strerror(rc));

	rc = medut_account_register(acct);
	CHECK(rc == MEDUT_OK, "medut_account_register returned %d (%s)\n",
	      rc, medut_strerror(rc));

	/* Let the stack finish binding its transports before posting to them. */
	sleep(1);

	CHECK(medut_network_local_addr(laddr, sizeof(laddr)) == MEDUT_OK &&
	      laddr[0],
	      "medut_network_local_addr() gave no address — this host has no "
	      "usable non-loopback interface, so the SDK bound nothing to send "
	      "to.\n");

	printf("==> sending SIP MESSAGE to %s:%d\n", laddr, SIP_PORT);
	fflush(stdout);
	CHECK(send_sip_message(laddr) == 0,
	      "could not send the SIP MESSAGE to %s:%d (socket error)\n",
	      laddr, SIP_PORT);

	for (i = 0; i < 10 && !atomic_load(&g_msg_seen); i++)
		sleep(1);

	/* Not a pass. If the MESSAGE never arrived, the code path this test
	 * exists to cover was never executed, and a "pass" would be a lie. */
	CHECK(atomic_load(&g_msg_seen),
	      "no MEDUT_EV_MESSAGE arrived — the enqueue path under test was "
	      "never exercised, so this run proves nothing. Check that the SDK "
	      "bound UDP %d on %s and that the request matched the account "
	      "AoR.\n", SIP_PORT, laddr);

	/* The registration retry loop keeps producing events. With the counter
	 * underflowed, every one of them is dropped. */
	printf("==> MESSAGE delivered; watching for later events\n");
	fflush(stdout);

	for (i = 0; i < 12 && !atomic_load(&g_reg_after); i++)
		sleep(1);

	CHECK(atomic_load(&g_events_after) > 0,
	      "no event of any kind was delivered after the SIP MESSAGE. The "
	      "event queue length counter has underflowed, so every producer "
	      "now believes the queue is full and the SDK is permanently "
	      "silent.\n");

	CHECK(atomic_load(&g_reg_after),
	      "events flowed after the MESSAGE (%d) but no REG_STATE among "
	      "them; the registration retry loop should still be reporting.\n",
	      atomic_load(&g_events_after));

	printf("==> %d events delivered after the MESSAGE\n",
	       atomic_load(&g_events_after));

	medut_account_destroy(acct);
	medut_shutdown();
	mtx_destroy(&g_lock);

	printf("PASS\n");
	return 0;
}
