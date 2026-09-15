/**
 * @file test_ice_sdp.c  echo-ice: candidate and SDP grammar round-trips
 *
 * Links the engine files against libre only.  The remote SDP bodies are
 * real: the Asterisk/FreePBX answer is the one captured on 2026-09-15
 * (ext 100 → *43), the browser body follows what Chrome's libwebrtc emits
 * through SIP.js, the lite body is rtpengine's shape.
 *
 * Build: make -C test/unit test_ice_sdp && ./test/unit/test_ice_sdp
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <re.h>
#include "../../src/ice/ice_internal.h"

static int g_pass, g_fail;

#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)


static int dummy_send(const struct sa *dst, struct mbuf *mb, void *arg)
{
	(void)dst; (void)mb; (void)arg;
	return 0;
}


static const struct icea_conf conf = ICEA_CONF_DEFAULT;


static struct icea_gen *make_gen(enum ice_role role, unsigned ncomp,
				 struct udp_sock **usp, struct icea_port **portp)
{
	struct icea_gen *gen = NULL;
	struct sa laddr;
	int err;

	err = icea_gen_alloc(&gen, &conf, 1, role, AF_INET, ncomp, NULL);
	CHECK(err == 0 && gen);

	err = udp_alloc_sockless(usp, dummy_send, NULL, NULL);
	CHECK(err == 0);

	sa_set_str(&laddr, "10.0.0.5", 6000);
	err = icea_port_attach(gen, 1, *usp, &laddr, "eth0", 0, false, portp);
	CHECK(err == 0);

	return gen;
}


static struct sdp_session *make_sdp(struct sdp_media **mp)
{
	struct sdp_session *sdp = NULL;
	struct sa laddr;
	int err;

	sa_set_str(&laddr, "10.0.0.5", 0);
	err = sdp_session_alloc(&sdp, &laddr);
	CHECK(err == 0);
	err = sdp_media_add(mp, sdp, "audio", 6000, "UDP/TLS/RTP/SAVPF");
	CHECK(err == 0);
	err = sdp_format_add(NULL, *mp, false, "0", "PCMU", 8000, 1,
			     NULL, NULL, NULL, false, NULL);
	CHECK(err == 0);

	return sdp;
}


static int decode_remote(struct sdp_session *sdp, const char *body)
{
	struct mbuf *mb = mbuf_alloc(strlen(body) + 1);
	struct mbuf *offer = NULL;
	int err;

	/* libre keeps freshly added media on a pending list until an offer
	 * has been encoded — the order baresip follows.  Do the same. */
	err = sdp_encode(&offer, sdp, true);
	mem_deref(offer);
	if (err)
		return err;

	mbuf_write_str(mb, body);
	mb->pos = 0;
	err = sdp_decode(sdp, mb, false);
	if (err)
		fprintf(stderr, "sdp_decode: %s (%d)\n", strerror(err), err);
	mem_deref(mb);
	return err;
}


static bool sdp_contains(struct sdp_session *sdp, const char *needle)
{
	struct mbuf *mb = NULL;
	bool found;

	if (sdp_encode(&mb, sdp, true))
		return false;
	/* the mbuf is not NUL-terminated: bound the search */
	found = memmem(mb->buf, mb->end, needle, strlen(needle)) != NULL;
	mem_deref(mb);
	return found;
}


/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_cand_attr_decode(void)
{
	struct ice_cand_attr a;
	struct sa sa;

	/* Asterisk: capital UDP, no extras */
	CHECK(0 == icea_cand_attr_decode(&a,
		"H52819efd 1 UDP 2130706431 82.129.158.253 17654 typ host"));
	CHECK(a.proto == IPPROTO_UDP && a.compid == 1 &&
	      a.type == ICE_CAND_TYPE_HOST && a.prio == 2130706431u);
	sa_set_str(&sa, "82.129.158.253", 17654);
	CHECK(sa_cmp(&a.addr, &sa, SA_ALL));
	CHECK(0 == strcmp(a.foundation, "H52819efd"));

	/* Browser: lowercase udp, srflx with raddr/rport and trailing extras */
	CHECK(0 == icea_cand_attr_decode(&a,
		"842163049 1 udp 1677729535 198.51.100.7 61665 typ srflx"
		" raddr 192.168.1.2 rport 61665 generation 0 ufrag abcd"
		" network-id 1 network-cost 10"));
	CHECK(a.type == ICE_CAND_TYPE_SRFLX);
	sa_set_str(&sa, "192.168.1.2", 61665);
	CHECK(sa_cmp(&a.rel_addr, &sa, SA_ALL));

	/* rtpengine: slash in the foundation, raddr 0.0.0.0 rport 0 */
	CHECK(0 == icea_cand_attr_decode(&a,
		"ICE/1xR9 2 UDP 2130706430 203.0.113.50 30001 typ host"
		" raddr 0.0.0.0 rport 0"));
	CHECK(a.compid == 2 && 0 == strcmp(a.foundation, "ICE/1xR9"));

	/* TCP with tcptype */
	CHECK(0 == icea_cand_attr_decode(&a,
		"7 1 tcp 1518280447 192.168.1.2 9 typ host tcptype active"
		" generation 0"));
	CHECK(a.proto == IPPROTO_TCP && a.tcptype == ICE_TCP_ACTIVE);

	/* mDNS: not an error we want to surface as malformed */
	CHECK(EAFNOSUPPORT == icea_cand_attr_decode(&a,
		"3 1 udp 2122194687 9a1b2c3d-4e5f.local 51000 typ host"));

	/* Garbage */
	CHECK(0 != icea_cand_attr_decode(&a, "x 1 udp"));
	CHECK(0 != icea_cand_attr_decode(&a,
		"1 1 udp 1 1.2.3.4 5 typo host"));
}


static void test_local_write(void)
{
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us, &port);
	struct sdp_media *m = NULL;
	struct sdp_session *sdp = make_sdp(&m);
	struct icea_cand *host, *srflx;
	struct sa a;
	const struct sa *laddr;

	sa_set_str(&a, "10.0.0.5", 6000);
	host = icea_cand_add_local(gen, ICE_CAND_TYPE_HOST, 1, port, NULL, &a,
				   icea_cand_lpref(port));
	CHECK(host && host->base == host && host->prio == 0x7effffffu);

	sa_set_str(&a, "203.0.113.9", 6000);
	srflx = icea_cand_add_local(gen, ICE_CAND_TYPE_SRFLX, 1, port, host,
				    &a, icea_cand_lpref(port));
	CHECK(srflx && srflx->base == host);
	CHECK(sa_cmp(&srflx->rel, &host->addr, SA_ALL));
	CHECK(strcmp(host->foundation, srflx->foundation) != 0);

	/* sorted: host (126) before srflx (100) */
	CHECK(list_head(&gen->lcandl)->data == host);

	/* default = srflx */
	CHECK(icea_cand_default(gen, 1) == srflx);

	CHECK(0 == icea_sdp_write_local(gen, sdp, m, true));
	CHECK(sdp_contains(sdp, "a=ice-ufrag:"));
	CHECK(sdp_contains(sdp, "a=ice-pwd:"));
	CHECK(sdp_contains(sdp, " typ host"));
	CHECK(sdp_contains(sdp, "203.0.113.9 6000 typ srflx raddr 10.0.0.5"
			   " rport 6000"));
	CHECK(!sdp_contains(sdp, "a=remote-candidates"));

	laddr = sdp_media_laddr(m);
	CHECK(laddr && sa_cmp(laddr, &srflx->addr, SA_ALL));

	/* Same address as host from STUN (no NAT) → redundant */
	{
		struct icea_cand *dup;
		sa_set_str(&a, "10.0.0.5", 6000);
		/* find() dedupes by address regardless of type, so fake a
		 * distinct srflx by using a fresh generation */
		dup = icea_cand_find(&gen->lcandl, 1, &a);
		CHECK(dup == host);
	}

	/* clear removes every ICE media attribute */
	icea_sdp_clear_local(m);
	CHECK(!sdp_contains(sdp, "a=candidate:"));

	mem_deref(sdp);
	mem_deref(gen);
	mem_deref(us);
}


static const char asterisk_answer[] =
	"v=0\r\n"
	"o=- 1021391454 1006853194 IN IP4 172.16.11.52\r\n"
	"s=Asterisk\r\n"
	"c=IN IP4 172.16.11.52\r\n"
	"t=0 0\r\n"
	"m=audio 17654 UDP/TLS/RTP/SAVPF 8 0 96 101\r\n"
	"a=connection:new\r\n"
	"a=setup:active\r\n"
	"a=fingerprint:SHA-256 01:89:C6:44:73:7C:E8:AC:98:E3:A3:7E:CD:2D:5D:38"
	":19:7F:93:9B:45:3E:0A:C4:76:50:3A:CD:BF:2D:69:44\r\n"
	"a=ice-ufrag:1e6af9c41069ba362e202a657a6d96fc\r\n"
	"a=ice-pwd:271116fa6dce4d0d5713c59a19376f50\r\n"
	"a=candidate:H52819efd 1 UDP 2130706431 82.129.158.253 17654 typ host\r\n"
	"a=candidate:Hac100b34 1 UDP 2130706431 172.16.11.52 17654 typ host\r\n"
	"a=candidate:H26bab19d 1 UDP 2130706431 fe80::be24:11ff:fe83:fc90 17654"
	" typ host\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=sendrecv\r\n"
	"a=rtcp-mux\r\n";


static void test_remote_asterisk(void)
{
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us, &port);
	struct sdp_media *m = NULL;
	struct sdp_session *sdp = make_sdp(&m);
	bool changed = true;
	struct sa a;

	CHECK(0 == decode_remote(sdp, asterisk_answer));
	CHECK(0 == icea_sdp_read_remote(gen, sdp, m, &changed));
	CHECK(!changed);                          /* first read: baseline */
	CHECK(gen->rufrag &&
	      0 == strcmp(gen->rufrag, "1e6af9c41069ba362e202a657a6d96fc"));
	CHECK(gen->rpwd &&
	      0 == strcmp(gen->rpwd, "271116fa6dce4d0d5713c59a19376f50"));
	CHECK(list_count(&gen->rcandl) == 2);     /* fe80:: dropped */
	CHECK(!gen->rlite && !gen->renomination);
	CHECK(gen->remote_ready);

	sa_set_str(&a, "172.16.11.52", 17654);
	CHECK(icea_cand_find(&gen->rcandl, 1, &a) != NULL);
	CHECK(icea_sdp_peer_has_ice(gen));
	CHECK(icea_sdp_peer_supports_ice(gen, m));   /* c= is a candidate */
	CHECK(!gen->mismatch);

	/* A second answer with new credentials is a restart */
	{
		struct sdp_media *m2 = NULL;
		struct sdp_session *sdp2 = make_sdp(&m2);
		char body[sizeof(asterisk_answer) + 8];
		char *p;

		strcpy(body, asterisk_answer);
		p = strstr(body, "a=ice-ufrag:1e6af9c4");
		CHECK(p != NULL);
		if (p)
			memcpy(p + strlen("a=ice-ufrag:"), "ffffffff", 8);

		CHECK(0 == decode_remote(sdp2, body));
		CHECK(0 == icea_sdp_read_remote(gen, sdp2, m2, &changed));
		CHECK(changed);
		CHECK(0 == strncmp(gen->rufrag, "ffffffff", 8));
		mem_deref(sdp2);
	}

	mem_deref(sdp);
	mem_deref(gen);
	mem_deref(us);
}


static const char browser_offer_as_answer[] =
	"v=0\r\n"
	"o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
	"s=-\r\n"
	"t=0 0\r\n"
	"a=ice-options:trickle renomination\r\n"
	"m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
	"c=IN IP4 0.0.0.0\r\n"
	"a=rtcp:9 IN IP4 0.0.0.0\r\n"
	"a=ice-ufrag:abcd\r\n"
	"a=ice-pwd:0123456789abcdef0123456789\r\n"
	"a=candidate:842163049 1 udp 1677729535 198.51.100.7 61665 typ srflx"
	" raddr 192.168.1.2 rport 61665 generation 0 ufrag abcd network-id 1"
	" network-cost 10\r\n"
	"a=candidate:1467250027 1 udp 2122260223 192.168.1.2 61665 typ host"
	" generation 0 ufrag abcd network-id 1\r\n"
	"a=candidate:3 1 udp 2122194687 9a1b2c3d-4e5f.local 51000 typ host"
	" generation 0\r\n"
	"a=candidate:7 1 tcp 1518280447 192.168.1.2 9 typ host tcptype active"
	" generation 0\r\n"
	"a=rtpmap:111 opus/48000/2\r\n"
	"a=sendrecv\r\n"
	"a=rtcp-mux\r\n";


static void test_remote_browser(void)
{
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLED, 1, &us, &port);
	struct sdp_media *m = NULL;
	struct sdp_session *sdp = make_sdp(&m);

	CHECK(0 == decode_remote(sdp, browser_offer_as_answer));
	CHECK(0 == icea_sdp_read_remote(gen, sdp, m, NULL));
	CHECK(gen->rufrag && 0 == strcmp(gen->rufrag, "abcd"));
	CHECK(list_count(&gen->rcandl) == 2);     /* mDNS and TCP skipped */
	CHECK(gen->renomination);                 /* session-level option */
	CHECK(!gen->rlite);
	/* c=0.0.0.0 promises nothing: not a mismatch */
	CHECK(icea_sdp_peer_supports_ice(gen, m));
	CHECK(!gen->mismatch);

	/* srflx sorts below host */
	{
		const struct icea_cand *first = list_head(&gen->rcandl)->data;
		CHECK(first->type == ICE_CAND_TYPE_HOST);
	}

	mem_deref(sdp);
	mem_deref(gen);
	mem_deref(us);
}


static const char lite_answer[] =
	"v=0\r\n"
	"o=rtpengine 1 1 IN IP4 203.0.113.50\r\n"
	"s=rtpengine\r\n"
	"c=IN IP4 203.0.113.50\r\n"
	"t=0 0\r\n"
	"a=ice-lite\r\n"
	"a=ice-ufrag:k7Yq\r\n"
	"a=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
	"m=audio 30000 UDP/TLS/RTP/SAVPF 0\r\n"
	"a=candidate:ICE/1xR9 1 UDP 2130706431 203.0.113.50 30000 typ host\r\n"
	"a=candidate:ICE/1xR9 2 UDP 2130706430 203.0.113.50 30001 typ host\r\n"
	"a=sendrecv\r\n";


static void test_remote_lite_session_creds(void)
{
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us, &port);
	struct sdp_media *m = NULL;
	struct sdp_session *sdp = make_sdp(&m);

	CHECK(0 == decode_remote(sdp, lite_answer));
	CHECK(0 == icea_sdp_read_remote(gen, sdp, m, NULL));
	CHECK(gen->rlite);
	CHECK(gen->rufrag && 0 == strcmp(gen->rufrag, "k7Yq"));   /* session */
	CHECK(list_count(&gen->rcandl) == 1);   /* comp 2 dropped, ncomp = 1 */
	CHECK(icea_sdp_peer_supports_ice(gen, m));

	mem_deref(sdp);
	mem_deref(gen);
	mem_deref(us);
}


static const char override_answer[] =
	"v=0\r\n"
	"o=- 1 1 IN IP4 203.0.113.60\r\n"
	"s=-\r\n"
	"c=IN IP4 203.0.113.60\r\n"
	"t=0 0\r\n"
	"a=ice-ufrag:sesslevel\r\n"
	"a=ice-pwd:sesspwd_sesspwd_sesspwd_\r\n"
	"m=audio 40000 UDP/TLS/RTP/SAVPF 0\r\n"
	"a=ice-ufrag:medialevel\r\n"
	"a=ice-pwd:mediapwd_mediapwd_mediapwd\r\n"
	"a=candidate:1 1 UDP 2130706431 203.0.113.61 40000 typ host\r\n";


static void test_remote_media_override_and_mismatch(void)
{
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us, &port);
	struct sdp_media *m = NULL;
	struct sdp_session *sdp = make_sdp(&m);

	CHECK(0 == decode_remote(sdp, override_answer));
	CHECK(0 == icea_sdp_read_remote(gen, sdp, m, NULL));
	CHECK(gen->rufrag && 0 == strcmp(gen->rufrag, "medialevel"));
	CHECK(gen->rpwd && 0 == strcmp(gen->rpwd, "mediapwd_mediapwd_mediapwd"));

	/* c= 203.0.113.60:40000 is not a candidate (.61 is) → mismatch */
	CHECK(!icea_sdp_peer_supports_ice(gen, m));
	CHECK(gen->mismatch);
	CHECK(0 == icea_sdp_write_local(gen, sdp, m, false));
	CHECK(sdp_contains(sdp, "a=ice-mismatch"));
	CHECK(!sdp_contains(sdp, "a=candidate:"));

	mem_deref(sdp);
	mem_deref(gen);
	mem_deref(us);
}


static void test_no_ice_peer(void)
{
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us, &port);
	struct sdp_media *m = NULL;
	struct sdp_session *sdp = make_sdp(&m);
	static const char plain[] =
		"v=0\r\no=- 1 1 IN IP4 203.0.113.70\r\ns=-\r\n"
		"c=IN IP4 203.0.113.70\r\nt=0 0\r\n"
		"m=audio 5004 RTP/AVP 0\r\na=sendrecv\r\n";

	CHECK(0 == decode_remote(sdp, plain));
	CHECK(0 == icea_sdp_read_remote(gen, sdp, m, NULL));
	CHECK(!icea_sdp_peer_has_ice(gen));
	CHECK(!icea_sdp_peer_supports_ice(gen, m));
	CHECK(!gen->mismatch);          /* no ICE at all is not a mismatch */
	CHECK(!gen->remote_ready);

	mem_deref(sdp);
	mem_deref(gen);
	mem_deref(us);
}


static void test_prflx_and_redund(void)
{
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct icea_gen *gen = make_gen(ICE_ROLE_CONTROLLING, 1, &us, &port);
	struct icea_cand *host, *srflx, *pr;
	struct sa a;

	sa_set_str(&a, "10.0.0.5", 6000);
	host = icea_cand_add_local(gen, ICE_CAND_TYPE_HOST, 1, port, NULL, &a,
				   65535);
	/* STUN saw the same address (no NAT) — different type, same addr:
	 * add_local dedupes by address, so nothing is added */
	srflx = icea_cand_add_local(gen, ICE_CAND_TYPE_SRFLX, 1, port, host, &a,
				    65535);
	CHECK(srflx == host);
	CHECK(list_count(&gen->lcandl) == 1);
	icea_cand_redund_elim(gen);
	CHECK(list_count(&gen->lcandl) == 1);

	sa_set_str(&a, "198.51.100.9", 4242);
	pr = icea_cand_add_prflx_remote(gen, 1, 1862270975u, &a);
	CHECK(pr && pr->type == ICE_CAND_TYPE_PRFLX && !pr->local);
	CHECK(icea_cand_add_prflx_remote(gen, 1, 1, &a) == pr);  /* dedupe */
	CHECK(strlen(pr->foundation) == 8);

	/* out-of-range component */
	CHECK(icea_cand_add_prflx_remote(gen, 2, 1, &a) == NULL);

	mem_deref(gen);
	mem_deref(us);
}


static void test_relay_only_policy(void)
{
	struct icea_conf relay = ICEA_CONF_DEFAULT;
	struct icea_gen *gen = NULL;
	struct udp_sock *us = NULL;
	struct icea_port *port = NULL;
	struct sa a;

	relay.policy = ICEA_TRANSPORT_RELAY;
	CHECK(0 == icea_gen_alloc(&gen, &relay, 1, ICE_ROLE_CONTROLLING,
				  AF_INET, 1, NULL));
	CHECK(0 == udp_alloc_sockless(&us, dummy_send, NULL, NULL));
	sa_set_str(&a, "10.0.0.5", 6000);
	CHECK(0 == icea_port_attach(gen, 1, us, &a, "eth0", 0, false, &port));

	CHECK(icea_cand_add_local(gen, ICE_CAND_TYPE_HOST, 1, port, NULL, &a,
				  65535) == NULL);
	sa_set_str(&a, "203.0.113.200", 50000);
	CHECK(icea_cand_add_local(gen, ICE_CAND_TYPE_RELAY, 1, port, NULL, &a,
				  65535) != NULL);
	CHECK(list_count(&gen->lcandl) == 1);
	CHECK(icea_cand_default(gen, 1)->type == ICE_CAND_TYPE_RELAY);

	mem_deref(gen);
	mem_deref(us);
}


int main(void)
{
	int err = libre_init();
	if (err) {
		fprintf(stderr, "libre_init: %s\n", strerror(err));
		return 1;
	}

	test_cand_attr_decode();
	test_local_write();
	test_remote_asterisk();
	test_remote_browser();
	test_remote_lite_session_creds();
	test_remote_media_override_and_mismatch();
	test_no_ice_peer();
	test_prflx_and_redund();
	test_relay_only_policy();

	tmr_debug();
	mem_debug();
	libre_close();

	printf("test_ice_sdp: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
