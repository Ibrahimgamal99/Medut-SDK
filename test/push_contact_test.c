/**
 * @file push_contact_test.c  Gate test for RFC 8599 push params on the wire
 *
 * A push-capable registrar only learns where to push from the Contact URI of
 * the REGISTER: <sip:alice@...;pn-provider=fcm;pn-prid=TOKEN;pn-param=...>.
 * This checks the params are really there, against a registrar of our own on
 * 127.0.0.1 that answers every REGISTER 200 and keeps its Contact:
 *
 *   1. the first REGISTER carries pn-provider / pn-prid / pn-param, inside
 *      the URI's angle brackets;
 *   2. medut_account_set_push_token() on a registered account sends a
 *      REGISTER with the new pn-prid at once (RFC 8599 s4.1.3), and over UDP
 *      the old token's binding is removed (expires=0), so the registrar is
 *      not left pushing to a dead token;
 *   3. a token changed while a REGISTER is in flight goes out in a REGISTER
 *      of its own once that one is answered;
 *   R. medut_account_refresh_registration() re-sends the REGISTER from the
 *      client holding the binding (same Call-ID) with no un-REGISTER, and a
 *      re-created client with the same push binding (register(), as on a
 *      handover) un-REGISTERs nothing either: an RFC 8599 server such as
 *      OpenSIPS forwards a held INVITE only on a refresh of the binding;
 *   4. a token too long for the Contact is refused and the old one stays;
 *   5. NULL clears the params.
 *
 * Build (from repo root, after scripts/build-linux.sh):
 *   gcc -g -O1 -std=gnu11 \
 *       -Iinclude test/push_contact_test.c \
 *       dist/linux/x86_64/medut.so \
 *       -Wl,-rpath,'$ORIGIN/../dist/linux/x86_64' \
 *       -o test/push_contact_test && ./test/push_contact_test
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "../include/medut.h"

#define MAX_REGS 32

static mtx_t       g_lock;
static char        g_contacts[MAX_REGS][2048];   /* bindings added */
static char        g_callids[MAX_REGS][256];
static int         g_nregs;
static char        g_removed[MAX_REGS][2048];    /* expires=0 */
static int         g_nremoved;
static atomic_int  g_registered;      /* REGISTERED events seen */
static atomic_int  g_delay_ms;        /* hold the next 200 this long */
static atomic_int  g_stop;
static int         g_sock = -1;

static void event_handler(const medut_event_t *ev, void *ud)
{
	(void)ud;
	if (ev->type == MEDUT_EV_REG_STATE &&
	    ev->u.reg.state == MEDUT_REG_REGISTERED)
		atomic_fetch_add(&g_registered, 1);
}

/* Copy header `name` (with its line ending) from msg into out. */
static void copy_header(const char *msg, const char *name, char *out,
                        size_t sz)
{
	char key[64];
	snprintf(key, sizeof(key), "\r\n%s:", name);
	const char *p = strstr(msg, key);
	if (!p) return;
	p += 2;
	const char *e = strstr(p, "\r\n");
	if (!e) return;
	size_t n = (size_t)(e - p) + 2;
	size_t have = strlen(out);
	if (have + n + 1 > sz) return;
	memcpy(out + have, p, n);
	out[have + n] = '\0';
}

/* A registrar that says yes to everything and remembers what it was told. */
static int registrar(void *arg)
{
	(void)arg;
	char buf[8192];
	char last_txn[512] = "";

	while (!atomic_load(&g_stop)) {
		struct sockaddr_in from;
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0,
		                     (struct sockaddr *)&from, &flen);
		if (n <= 0)
			continue;   /* SO_RCVTIMEO: check g_stop */
		buf[n] = '\0';

		if (strncmp(buf, "REGISTER ", 9) != 0)
			continue;   /* OPTIONS keepalives and the like */

		char contact[2048] = "", txn[512] = "";
		copy_header(buf, "Contact", contact, sizeof(contact));
		copy_header(buf, "Call-ID", txn, sizeof(txn));
		copy_header(buf, "CSeq", txn, sizeof(txn));
		/* A UDP retransmission is the same request, not a new one. */
		bool again = !strcmp(txn, last_txn);
		snprintf(last_txn, sizeof(last_txn), "%s", txn);
		/* baresip releases a UDP/TCP binding before it registers the
		 * next one; those un-REGISTERs are kept apart. */
		bool removal = strstr(contact, ";expires=0") != NULL;
		mtx_lock(&g_lock);
		if (again)
			;
		else if (removal) {
			if (g_nremoved < MAX_REGS)
				snprintf(g_removed[g_nremoved++],
				         sizeof(g_removed[0]), "%s", contact);
		}
		else if (g_nregs < MAX_REGS) {
			g_callids[g_nregs][0] = '\0';
			copy_header(buf, "Call-ID", g_callids[g_nregs],
			            sizeof(g_callids[0]));
			snprintf(g_contacts[g_nregs++], sizeof(g_contacts[0]),
			         "%s", contact);
		}
		mtx_unlock(&g_lock);

		int delay = removal ? 0 : atomic_exchange(&g_delay_ms, 0);
		if (delay)
			usleep((useconds_t)delay * 1000);

		char resp[8192] = "SIP/2.0 200 OK\r\n";
		copy_header(buf, "Via", resp, sizeof(resp));
		copy_header(buf, "From", resp, sizeof(resp));
		copy_header(buf, "To", resp, sizeof(resp));
		copy_header(buf, "Call-ID", resp, sizeof(resp));
		copy_header(buf, "CSeq", resp, sizeof(resp));
		strncat(resp, contact, sizeof(resp) - strlen(resp) - 1);
		strncat(resp, "Content-Length: 0\r\n\r\n",
		        sizeof(resp) - strlen(resp) - 1);
		sendto(g_sock, resp, strlen(resp), 0,
		       (struct sockaddr *)&from, flen);
	}
	return 0;
}

static int nregs(void)
{
	mtx_lock(&g_lock);
	int n = g_nregs;
	mtx_unlock(&g_lock);
	return n;
}

static int nremoved(void)
{
	mtx_lock(&g_lock);
	int n = g_nremoved;
	mtx_unlock(&g_lock);
	return n;
}

static bool same_callid(int a, int b)
{
	mtx_lock(&g_lock);
	bool same = a < g_nregs && b < g_nregs &&
	            0 == strcmp(g_callids[a], g_callids[b]);
	mtx_unlock(&g_lock);
	return same;
}

static bool removed(const char *param)
{
	bool found = false;
	mtx_lock(&g_lock);
	for (int i = 0; i < g_nremoved && !found; i++)
		found = strstr(g_removed[i], param) != NULL;
	mtx_unlock(&g_lock);
	return found;
}

/* Wait until the registrar has seen `count` binding REGISTERs and `regd` REGISTERED
 * events have arrived; false after 5 s. */
static bool wait_for(int count, int regd)
{
	for (int i = 0; i < 500; i++) {
		if (nregs() >= count && atomic_load(&g_registered) >= regd)
			return true;
		usleep(10 * 1000);
	}
	return false;
}

static const char *contact_at(int i)
{
	static char copy[2048];
	mtx_lock(&g_lock);
	snprintf(copy, sizeof(copy), "%s", i < g_nregs ? g_contacts[i] : "");
	mtx_unlock(&g_lock);
	return copy;
}

/* True when `param` sits inside the Contact's <...>, not after it. */
static bool in_uri(const char *contact, const char *param)
{
	const char *lt = strchr(contact, '<');
	const char *gt = lt ? strchr(lt, '>') : NULL;
	const char *p = strstr(contact, param);
	return lt && gt && p && p > lt && p < gt;
}

#define CHECK(cond, ...) do {                       \
	if (!(cond)) {                              \
		printf("FAIL: " __VA_ARGS__);       \
		return 1;                           \
	}                                           \
} while (0)

int main(void)
{
	medut_config_t cfg;
	medut_account_config_t acfg;
	medut_account_handle_t acct = NULL;
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
	char uri[64];
	thrd_t thr;
	int err, n, r;

	mtx_init(&g_lock, mtx_plain);

	g_sock = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(g_sock >= 0, "socket\n");
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(!bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)), "bind\n");
	getsockname(g_sock, (struct sockaddr *)&addr, &alen);
	struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
	setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	thrd_create(&thr, registrar, NULL);

	medut_config_init(&cfg);
	cfg.event_cb               = event_handler;
	cfg.log_level              = 0;
	cfg.keepalive_interval     = 0;
	cfg.net_monitor_interval_s = 0;
	err = medut_init(&cfg);
	CHECK(!err, "medut_init: %d\n", err);

	snprintf(uri, sizeof(uri), "alice@127.0.0.1:%u", ntohs(addr.sin_port));
	memset(&acfg, 0, sizeof(acfg));
	acfg.uri           = uri;
	acfg.password      = "secret";
	acfg.transport     = MEDUT_TRANSPORT_UDP;
	acfg.push_provider = MEDUT_PUSH_PROVIDER_FCM;
	acfg.push_token    = "tokA";
	acfg.push_param    = "medut-test";
	err = medut_account_create(&acfg, &acct);
	CHECK(!err, "account_create: %d\n", err);

	int regd = 0;

	/* 1. The first REGISTER carries the push binding. */
	medut_account_register(acct);
	CHECK(wait_for(1, ++regd), "no REGISTERED from the first REGISTER\n");
	printf("[1] %s", contact_at(0));
	CHECK(in_uri(contact_at(0), ";pn-provider=fcm;pn-prid=tokA;pn-param=medut-test"),
	      "first Contact lacks the RFC 8599 params in its URI\n");

	/* 2. A new token on a registered account is sent at once. */
	err = medut_account_set_push_token(acct, "tokB");
	CHECK(!err, "set_push_token(tokB): %d\n", err);
	CHECK(wait_for(2, ++regd), "no REGISTER after the token change\n");
	printf("[2] %s", contact_at(1));
	CHECK(in_uri(contact_at(1), "pn-prid=tokB"), "new token not sent\n");
	CHECK(removed("pn-prid=tokA"), "old token's binding left in place\n");

	/* R1. A wake-up refresh: same client, same Call-ID, nothing removed. */
	n = nregs();
	r = nremoved();
	err = medut_account_refresh_registration(acct);
	CHECK(!err, "refresh_registration: %d\n", err);
	CHECK(wait_for(n + 1, ++regd), "no REGISTER from the refresh\n");
	printf("[R1] %s", contact_at(n));
	CHECK(in_uri(contact_at(n), "pn-prid=tokB"), "refresh lost the push binding\n");
	CHECK(same_callid(n, n - 1), "refresh came from a new client (new Call-ID)\n");
	CHECK(nremoved() == r, "refresh un-REGISTERed the binding first\n");

	/* R2. A new client for the same push binding (register(), as a
	 *     handover does): the old one leaves without an un-REGISTER. */
	n = nregs();
	r = nremoved();
	medut_account_register(acct);
	CHECK(wait_for(n + 1, ++regd), "no REGISTER from register()\n");
	CHECK(!same_callid(n, n - 1), "register() did not start a new client\n");
	CHECK(nremoved() == r,
	      "a new client un-REGISTERed the push binding it carries on\n");

	/* 3. Changed mid-REGISTER: the answer to the in-flight one is held, the
	 *    token changes under it, and a REGISTER of its own follows. */
	atomic_store(&g_delay_ms, 600);
	n = nregs();
	medut_account_register(acct);
	for (int i = 0; i < 100 && nregs() == n; i++)
		usleep(10 * 1000);
	err = medut_account_set_push_token(acct, "tokC");
	CHECK(!err, "set_push_token(tokC): %d\n", err);
	regd += 2;
	CHECK(wait_for(n + 2, regd), "no REGISTER after the in-flight one\n");
	printf("[3] %s", contact_at(n + 1));
	CHECK(in_uri(contact_at(n + 1), "pn-prid=tokC"),
	      "token changed mid-REGISTER never reached the wire\n");

	/* 4. Too long for the Contact: refused, old token kept. */
	char *huge = malloc(4096);
	memset(huge, 'x', 4095);
	huge[4095] = '\0';
	err = medut_account_set_push_token(acct, huge);
	free(huge);
	CHECK(err == MEDUT_ERR_INVAL, "oversized token: %d, want INVAL\n", err);
	n = nregs();
	medut_account_refresh_registration(acct);
	CHECK(wait_for(n + 1, ++regd), "no REGISTER after the refused token\n");
	CHECK(in_uri(contact_at(n), "pn-prid=tokC"),
	      "a refused token disturbed the one in force\n");

	/* 5. NULL clears them. */
	n = nregs();
	err = medut_account_set_push_token(acct, NULL);
	CHECK(!err, "set_push_token(NULL): %d\n", err);
	CHECK(wait_for(n + 1, ++regd), "no REGISTER after clearing the token\n");
	printf("[5] %s", contact_at(n));
	CHECK(!strstr(contact_at(n), "pn-"), "cleared token still sent\n");

	medut_account_destroy(acct);
	medut_shutdown();
	atomic_store(&g_stop, 1);
	thrd_join(thr, NULL);
	close(g_sock);
	mtx_destroy(&g_lock);

	printf("PASS\n");
	return 0;
}
