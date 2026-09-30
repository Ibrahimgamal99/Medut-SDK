# patch-re-sources.cmake
# Apply EchoSDK's libre patches at build time.
# Usage: cmake -DSOURCE_DIR=path/to/re -P patch-re-sources.cmake
#
# third_party/re is fetched by scripts/fetch-third-party.sh at a pinned
# revision and is gitignored — not a submodule — so an in-place patch cannot
# leak into upstream history (the hazard fix-msvc-re.cmake retired over).
# Each patch is idempotent — it is skipped when its replacement text is
# already in the file — and fails loudly when a libre bump moves the text it
# splices at, in the spirit of the configure-time prototype guard in
# CMakeLists.txt.  Every replacement carries an "EchoSDK-patched" comment so
# a patched tree is recognisable by eye and by grep.
#
# Why patches instead of GNU ld's --wrap (which used to carry two of these
# fixes on Linux/Android): Apple's linker has no --wrap, so iOS builds
# silently shipped without them.  The rename patches below are the
# compile-time equivalent — libre's definition moves to __real_*, EchoSDK's
# src/ws_path.c owns the public name, and every call site resolves to it at
# link time on every platform.  No linker flags, and dist/ archives no longer
# require consumers to pass any.

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR must be defined")
endif()

# Splice NEW in place of OLD in FILE, or verify it already happened.
# OLD must match byte-for-byte (tabs included); NEW should carry an
# "EchoSDK-patched" comment so the change is findable in the tree.
#
# Idempotency is decided per hunk, not per file: NEW itself is the marker.
# It used to be "the file contains 'EchoSDK-patched' anywhere", which is one
# bit per file — the first hunk applied to ice/candpair.c would have made the
# second report itself as already done and silently ship unapplied.
function(echosdk_patch FILE OLD NEW WHAT)
  if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "patch-re-sources: ${FILE} does not exist (${WHAT})")
  endif()
  file(READ "${FILE}" _content)

  string(FIND "${_content}" "${NEW}" _applied)
  if(NOT _applied EQUAL -1)
    message(STATUS "patch-re-sources: ${FILE} already patched (${WHAT})")
    return()
  endif()

  string(FIND "${_content}" "${OLD}" _anchor)
  if(_anchor EQUAL -1)
    message(FATAL_ERROR
      "patch-re-sources: anchor not found in ${FILE} for: ${WHAT}\n"
      "The pinned libre revision has moved the code this patch splices at. "
      "Re-derive the patch against the new revision before building — "
      "shipping without it silently reintroduces the bug it fixes.")
  endif()

  string(REPLACE "${OLD}" "${NEW}" _content "${_content}")
  file(WRITE "${FILE}" "${_content}")
  message(STATUS "patch-re-sources: patched ${FILE} (${WHAT})")
endfunction()

# ---------------------------------------------------------------------------
# 1. sipsess/reply.c — hanging up an answered incoming call must send the BYE
#
# A UAS that rang before answering holds two reply records with the same
# INVITE CSeq (the 1xx's and the 2xx's), and cmp_handler matches on the CSeq
# alone.  list_apply() stops at the first hit, so the ACK freed exactly one;
# the survivor made the session destructor's termwait() defer the BYE to the
# record's 64*T1 (32 s) retransmit timeout.  Hanging up an answered incoming
# call reported success with nothing on the wire, and the caller's eventual
# BYE was answered 481.  Drain every record the ACK matches — that is the
# intended semantic anyway: these records exist to retransmit the response
# until the ACK arrives.  A plain bug fix, upstreamable as-is.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/src/sipsess/reply.c"
[=[
int sipsess_reply_ack(struct sipsess *sess, const struct sip_msg *msg)
{
	struct sipsess_reply *reply;

	reply = list_ledata(list_apply(&sess->replyl, false, cmp_handler,
				       (void *)msg));
	if (!reply)
		return ENOENT;

	mem_deref(reply);

	return 0;
}
]=]
[=[
int sipsess_reply_ack(struct sipsess *sess, const struct sip_msg *msg)
{
	/* EchoSDK-patched: drain every reply record this ACK matches.
	 *
	 * A UAS that sent a 1xx and the 2xx holds two records with the same
	 * INVITE CSeq, and cmp_handler matches on the CSeq alone; freeing
	 * only the first left the other alive for the life of the call, and
	 * the session destructor's termwait() then deferred the BYE to that
	 * record's 64*T1 (32 s) timeout — an answered incoming call's hangup
	 * reported success with nothing on the wire.  The records exist to
	 * retransmit the response until the ACK arrives, so the ACK is what
	 * retires them — all of them.  The bound is paranoia: a runaway loop
	 * here would hang the re thread and all of SIP with it. */
	struct sipsess_reply *reply;
	unsigned n = 0;

	while (NULL != (reply = list_ledata(
				list_apply(&sess->replyl, false, cmp_handler,
					   (void *)msg)))) {
		mem_deref(reply);
		if (++n >= 16)
			break;
	}

	return n ? 0 : ENOENT;
}
]=]
"BYE after answered incoming call: ACK drains all matching reply records")

# ---------------------------------------------------------------------------
# 2. sip/dialog.c — route accessor rename behind RE_SIP_DIALOG_ROUTE_OVERRIDE
#
# Every in-dialog request (BYE, re-INVITE, ACK, INFO, PRACK, UPDATE) fetches
# its destination through sip_dialog_route().  When the PBX sends no
# Record-Route, libre falls back to the peer's Contact — behind a reverse
# proxy that names the server's own internal address, so the request dies in
# DNS after being reported sent (RFC 7118 B.2 says a WebSocket client's
# requests belong on the flow the registration established).  EchoSDK decides
# the route in bsdk_ws_route_override() (src/ws_path.c); this rename lets it
# own the public accessor.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/src/sip/dialog.c"
[=[
const struct uri *sip_dialog_route(const struct sip_dialog *dlg)
{
	return dlg ? &dlg->route : NULL;
}
]=]
[=[
#ifdef RE_SIP_DIALOG_ROUTE_OVERRIDE
/* EchoSDK-patched: the SDK owns the public accessor so in-dialog WebSocket
 * requests can be routed over the registration flow (RFC 7118 B.2); see
 * EchoSDK's src/ws_path.c.  Same compile-time rename websock.c carries for
 * RE_WEBSOCK_CONNECT_OVERRIDE — call sites keep the public name and resolve
 * to the SDK's definition at link time; no linker tricks involved. */
const struct uri *__real_sip_dialog_route(const struct sip_dialog *dlg)
#else
const struct uri *sip_dialog_route(const struct sip_dialog *dlg)
#endif
{
	return dlg ? &dlg->route : NULL;
}
]=]
"in-dialog route accessor rename (RFC 7118 B.2 routing hook)")

# ---------------------------------------------------------------------------
# 3. websock/websock.c — connect rename behind RE_WEBSOCK_CONNECT_OVERRIDE
#
# The hook ws_path.c and the MSVC/Apple builds have referenced all along
# (fix-msvc-re.cmake documents it) but which never actually existed in the
# pinned libre — Apple builds could not even link.  EchoSDK's wrapper pins
# the connect URI to the configured server and injects Origin/extra headers
# and the keepalive override.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/src/websock/websock.c"
[=[
int websock_connect(struct websock_conn **connp, struct websock *sock,
		    struct http_cli *cli, const char *uri, unsigned kaint,
		    websock_estab_h *estabh, websock_recv_h *recvh,
		    websock_close_h *closeh, void *arg, const char *fmt, ...)
]=]
[=[
#ifdef RE_WEBSOCK_CONNECT_OVERRIDE
/* EchoSDK-patched: the SDK owns the public name so it can pin the connect
 * URI to the configured server and inject the Origin/extra headers; see
 * EchoSDK's src/ws_path.c. */
int __real_websock_connect(struct websock_conn **connp, struct websock *sock,
		    struct http_cli *cli, const char *uri, unsigned kaint,
		    websock_estab_h *estabh, websock_recv_h *recvh,
		    websock_close_h *closeh, void *arg, const char *fmt, ...)
#else
int websock_connect(struct websock_conn **connp, struct websock *sock,
		    struct http_cli *cli, const char *uri, unsigned kaint,
		    websock_estab_h *estabh, websock_recv_h *recvh,
		    websock_close_h *closeh, void *arg, const char *fmt, ...)
#endif
]=]
"websock_connect rename (WS URI pinning hook)")


# ---------------------------------------------------------------------------
# N. sipreg/reg.c — a WebSocket registration's Contact must not carry the local
#    address (RFC 7118 s5.2.1)
#
# The registrar keys an AOR binding on the Contact URI.  With the local IP in
# it, a device that moves between Wi-Fi and cellular registers a *new* binding
# instead of replacing its own, and the abandoned one — a WebSocket that no
# longer exists — stays in the AOR until it expires.  Inbound calls are then
# delivered to a dead contact on the way to the live one.  Measured on-device
# (2026-08-31): "[2 bindings]" for the whole session, and 6.2 s from INVITE to
# alerting on that extension against 0.33 s on a single-binding one.
#
# The fix is the host RFC 7118 prescribes: a stable per-instance name in the
# ".invalid" domain, taken from the instance-id baresip already advertises in
# the contact params.  The full rationale is in the hunk.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/src/sipreg/reg.c"
[=[
static int send_handler(enum sip_transp tp, struct sa *src,
			const struct sa *dst, struct mbuf *mb,
			struct mbuf **contp, void *arg)
{
	struct sipreg *reg = arg;
	int err;

	(void)contp;
	(void)dst;

	reg->tp = tp;
	if (reg->srcport && tp != SIP_TRANSP_UDP)
		sa_set_port(src, reg->srcport);

	reg->laddr = *src;
	err = mbuf_printf(mb, "Contact: <sip:%s@%J%s%s%s>;expires=%u%s%s",
			  reg->cuser, &reg->laddr, sip_transp_param(reg->tp),
			  reg->cparams ? ";" : "",
			  reg->cparams ? reg->cparams : "",
			  reg->expires,
			  reg->params ? ";" : "",
			  reg->params ? reg->params : "");

	if (reg->regid > 0)
		err |= mbuf_printf(mb, ";reg-id=%d", reg->regid);

	err |= mbuf_printf(mb, "\r\n");
	return err;
}
]=]
[=[
/* EchoSDK-patched: the Contact host for a WebSocket registration.
 *
 * RFC 7118 s5.2.1 asks a WebSocket client to register a Contact whose host is
 * a random name in the ".invalid" domain, stable for the life of the instance.
 * baresip's uuid module already mints such an identifier and persists it
 * across restarts, and advertises it as `+sip.instance="<urn:uuid:...>"`, so
 * take it from there: no second source of truth, and nothing new to keep
 * alive across a restart.
 *
 * Both parameter strings are searched because the two are not interchangeable
 * and callers differ: baresip passes the instance in the header params (which
 * land after `;expires=`), while a caller using
 * sipreg_set_contact_params() puts them inside the URI. */
static int echosdk_ws_contact_host(char *buf, size_t sz,
				   const struct sipreg *reg)
{
	const char *srcv[2];
	struct pl uuid;
	size_t i;

	if (!buf || !sz || !reg)
		return EINVAL;

	srcv[0] = reg->params;
	srcv[1] = reg->cparams;

	for (i=0; i<RE_ARRAY_SIZE(srcv); i++) {

		if (!srcv[i])
			continue;

		if (re_regex(srcv[i], str_len(srcv[i]),
			     "urn:uuid:[0-9a-f]8", &uuid))
			continue;

		if (re_snprintf(buf, sz, "%r.invalid", &uuid) < 0)
			return ENOMEM;

		return 0;
	}

	return ENOENT;
}


static int send_handler(enum sip_transp tp, struct sa *src,
			const struct sa *dst, struct mbuf *mb,
			struct mbuf **contp, void *arg)
{
	struct sipreg *reg = arg;
	char host[64];
	int err;

	(void)contp;
	(void)dst;

	reg->tp = tp;
	if (reg->srcport && tp != SIP_TRANSP_UDP)
		sa_set_port(src, reg->srcport);

	reg->laddr = *src;

	/* EchoSDK-patched: keep the local address out of a WebSocket Contact.
	 *
	 * Over WS the address is meaningless — the server answers down the
	 * connection the REGISTER arrived on — but it is not harmless, because
	 * it is what the registrar keys the binding on.  A device that moves
	 * between Wi-Fi and cellular re-registers under a *different* Contact
	 * URI, so the AOR collects one binding per address the device has ever
	 * used, every one of them alive until it expires (an hour, by default)
	 * and every one pointing at a WebSocket that is gone.  The registrar
	 * then has to try them on the way to the live one.  Measured on-device
	 * (2026-08-31): an AOR stuck at "[2 bindings]" all session, where a call
	 * to that extension took 6.2 s to alert it against 0.33 s for an
	 * extension with a single binding.
	 *
	 * With the per-instance host, a re-registration from anywhere replaces
	 * the same binding.  If the instance-id cannot be read we keep the old
	 * behaviour: a duplicate binding is a delay, while an unroutable Contact
	 * would be a registration that never receives a call at all. */
	if ((tp == SIP_TRANSP_WS || tp == SIP_TRANSP_WSS) &&
	    0 == echosdk_ws_contact_host(host, sizeof(host), reg)) {

		err = mbuf_printf(mb,
				  "Contact: <sip:%s@%s%s%s%s>;expires=%u%s%s",
				  reg->cuser, host, sip_transp_param(reg->tp),
				  reg->cparams ? ";" : "",
				  reg->cparams ? reg->cparams : "",
				  reg->expires,
				  reg->params ? ";" : "",
				  reg->params ? reg->params : "");
	}
	else {
		err = mbuf_printf(mb, "Contact: <sip:%s@%J%s%s%s>;expires=%u%s%s",
				  reg->cuser, &reg->laddr,
				  sip_transp_param(reg->tp),
				  reg->cparams ? ";" : "",
				  reg->cparams ? reg->cparams : "",
				  reg->expires,
				  reg->params ? ";" : "",
				  reg->params ? reg->params : "");
	}

	if (reg->regid > 0)
		err |= mbuf_printf(mb, ";reg-id=%d", reg->regid);

	err |= mbuf_printf(mb, "\r\n");
	return err;
}
]=]
"WebSocket REGISTER Contact: per-instance host, not the local address")


# ---------------------------------------------------------------------------
# O. sipreg/reg.c — a registration client that is going away must not
#    un-REGISTER the binding its successor holds
#
# With the per-instance Contact of hunk N, every REGISTER this instance sends
# names the same binding, whichever registration client sends it.  libre still
# treats the client being freed as the binding's owner and un-REGISTERs it:
#
#   - baresip's reg_register() frees the old client and allocates the new one
#     on every ua_register() — each handover, each retry, each keepalive
#     recovery.  The un-REGISTER goes out first and briefly empties the AOR.
#   - When the old client still has a request in flight, the un-REGISTER is
#     deferred until that request is answered or times out.  After a
#     handover the request sat on a flushed connection, so that is Timer F,
#     32 s later — long after the new client registered.  The late
#     expires=0 then deletes the new binding (RFC 3261 s10.3: a zero expiry
#     removes the binding whatever the Call-ID), and the device stays
#     "registered" with no contact at the registrar and no incoming calls.
#     An app that rebuilds its account (destroy + create) hits the same path
#     through the client's destructor, including the 401 re-challenge of an
#     in-flight REGISTER, which re-sends as an authenticated expires=0.
#     Observed on the wire (2026-09-28): REGISTER expires=0 and a new
#     REGISTER for the identical Contact, back to back, on every handover.
#
# So for an instance Contact the departing client sends nothing: the
# successor's REGISTER refreshes the binding in place.  An explicit
# sipreg_unregister() — sign-out, account destroy after unregister — is
# unaffected, and so is every address-based Contact, where the old binding is
# a different URI and removing it is the point.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/src/sipreg/reg.c"
[=[
static int request(struct sipreg *reg, bool reset_ls);
]=]
[=[
static int request(struct sipreg *reg, bool reset_ls);
/* EchoSDK-patched: see echosdk_contact_is_instance() below. */
static bool echosdk_contact_is_instance(const struct sipreg *reg);
]=]
"forward declaration of echosdk_contact_is_instance")

echosdk_patch("${SOURCE_DIR}/src/sipreg/reg.c"
[=[
	if (reg->terminated && !reg->registered)
		goto out;
]=]
[=[
	/* EchoSDK-patched: a terminated client with an instance Contact does
	 * not answer a challenge either — the retry would go out as an
	 * authenticated expires=0 for the binding a successor now holds. */
	if (reg->terminated &&
	    (!reg->registered || echosdk_contact_is_instance(reg)))
		goto out;
]=]
"terminated instance-Contact registration: no re-challenge")

echosdk_patch("${SOURCE_DIR}/src/sipreg/reg.c"
[=[
	else if (reg->terminated) {
		if (!reg->registered || request(reg, true))
			mem_deref(reg);
	}
]=]
[=[
	else if (reg->terminated) {
		/* EchoSDK-patched: the deferred un-REGISTER of a freed client.
		 * For an instance Contact it would land after the successor's
		 * REGISTER and delete that binding. */
		if (!reg->registered || echosdk_contact_is_instance(reg) ||
		    request(reg, true))
			mem_deref(reg);
	}
]=]
"terminated instance-Contact registration: no deferred un-REGISTER")

echosdk_patch("${SOURCE_DIR}/src/sipreg/reg.c"
[=[
void sipreg_unregister(struct sipreg *reg)
{
	int err;

	if (!reg)
		return;

	reg->expires = 0;

	err = sipreg_send(reg);
	if (err)
		reg->resph(err, NULL, reg->arg);
}
]=]
[=[
void sipreg_unregister(struct sipreg *reg)
{
	int err;

	if (!reg)
		return;

	reg->expires = 0;

	err = sipreg_send(reg);
	if (err)
		reg->resph(err, NULL, reg->arg);
}


/* EchoSDK-patched: does this registration's Contact name the instance rather
 * than the local address?
 *
 * True for a WebSocket registration carrying the per-instance ".invalid"
 * host (see send_handler()).  That Contact is the same URI on every network
 * and from every registration client of this instance, so any later REGISTER
 * from it refreshes the very binding this client holds. */
static bool echosdk_contact_is_instance(const struct sipreg *reg)
{
	char host[64];

	if (!reg)
		return false;

	if (reg->tp != SIP_TRANSP_WS && reg->tp != SIP_TRANSP_WSS)
		return false;

	return 0 == echosdk_ws_contact_host(host, sizeof(host), reg);
}


/**
 * EchoSDK-patched: free a registration client that a new one is replacing
 *
 * Like mem_deref(), except that a client whose Contact names the instance
 * sends no un-REGISTER on the way out: its successor is about to REGISTER
 * the same binding, and removing it first only leaves the AOR empty until
 * that REGISTER is answered.  Any other client is un-REGISTERed as usual.
 *
 * @param reg   SIP Registration client
 *
 * @return NULL, for assignment
 */
struct sipreg *sipreg_release(struct sipreg *reg)
{
	if (reg && echosdk_contact_is_instance(reg))
		reg->registered = false;

	return mem_deref(reg);
}
]=]
"sipreg_release(): free a replaced client without un-REGISTERing")

echosdk_patch("${SOURCE_DIR}/include/re_sipreg.h"
[=[
void sipreg_unregister(struct sipreg *reg);
]=]
[=[
void sipreg_unregister(struct sipreg *reg);
/* EchoSDK-patched: see src/sipreg/reg.c */
struct sipreg *sipreg_release(struct sipreg *reg);
]=]
"sipreg_release() declaration")


# ---------------------------------------------------------------------------
# P. sip/keepalive + sipreg — a registration hears about its connection closing
#
# A registrar reaches a TCP/TLS/WS client down the connection its REGISTER
# arrived on, so when that connection closes the binding is dead: every INVITE
# the registrar sends goes nowhere.  libre only notices the close if the
# registration is watching its connection, and it only watches under RFC 5626
# outbound — when the 200 OK carries `Require: outbound` — and never over
# WebSocket (sip_keepalive_start() answers EPROTONOSUPPORT).  Asterisk does not
# do outbound.  So a WebSocket that died under a registration was invisible:
# the client stayed "registered", the next request (a keepalive OPTIONS)
# silently opened a *new* connection and was answered, and the registrar kept
# routing calls to the dead one until the REGISTER refresh minutes later.
# Observed on-device (2026-09-28, Android, FreePBX over WSS): the connection
# aborted with ECONNABORTED 0.4 s after a handover's REGISTER, two wake pushes
# for the same call were each "ready" within 800 ms, and no INVITE arrived —
# the call was missed.
#
# A browser does not have this hole: SIP.js re-registers when its WebSocket
# closes.  This gives libre the same: sip_keepalive_watch() puts a watch-only
# entry on the connection's keepalive list — no CRLF pings, which a server
# that never agreed to outbound need not answer — and conn_close() already
# signals that list.  sipreg arms it on every successful REGISTER over a
# connection-oriented transport, and its existing flow-failure handler turns
# the close into a registration failure, which the SDK retries at once.
# A transport flush (handover) frees the list without signalling it, so the
# SDK's own re-REGISTER is not doubled.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/src/sip/sip.h"
[=[
int  sip_keepalive_tcp(struct sip_keepalive *ka, struct sip_conn *conn,
		       uint32_t interval);
]=]
[=[
int  sip_keepalive_tcp(struct sip_keepalive *ka, struct sip_conn *conn,
		       uint32_t interval);
/* EchoSDK-patched: see sip_keepalive_watch() */
int  sip_keepalive_conn_watch(struct sip_keepalive *ka, struct sip_conn *conn);
]=]
"sip_keepalive_conn_watch() declaration")

echosdk_patch("${SOURCE_DIR}/src/sip/transp.c"
[=[
		tmr_start(&conn->tmr_ka, sip_keepalive_wait(conn->ka_interval),
			  conn_keepalive_handler, conn);
	}

	return 0;
}
]=]
[=[
		tmr_start(&conn->tmr_ka, sip_keepalive_wait(conn->ka_interval),
			  conn_keepalive_handler, conn);
	}

	return 0;
}


/* EchoSDK-patched: be told when this connection closes, and nothing else —
 * no keepalive pings.  conn_close() signals conn->kal; a connection freed
 * by a transport flush drops the list without signalling it. */
int  sip_keepalive_conn_watch(struct sip_keepalive *ka, struct sip_conn *conn)
{
	if (!ka || !conn)
		return EINVAL;

	list_append(&conn->kal, &ka->le, ka);

	return 0;
}
]=]
"sip_keepalive_conn_watch(): watch a connection for close")

echosdk_patch("${SOURCE_DIR}/src/sip/keepalive.c"
[=[
uint64_t sip_keepalive_wait(uint32_t interval)
{
	return (uint64_t) interval * (800 + rand_u16() % 201);
}
]=]
[=[
uint64_t sip_keepalive_wait(uint32_t interval)
{
	return (uint64_t) interval * (800 + rand_u16() % 201);
}


/**
 * EchoSDK-patched: call a handler when the connection a message arrived on
 * closes
 *
 * The close-detection half of sip_keepalive_start(), without its keepalive
 * pings, for a connection-oriented transport including WebSocket.  Ownership
 * is sip_keepalive_start()'s: the connection owns the entry, *kap is cleared
 * when it goes, and mem_deref(*kap) stops watching.
 *
 * @param kap  Pointer to allocated keepalive object
 * @param sip  SIP Stack instance
 * @param msg  SIP Message received on the connection to watch
 * @param kah  Handler, called once with the close reason
 * @param arg  Handler argument
 *
 * @return 0 if success, EPROTONOSUPPORT for a connectionless transport
 */
int sip_keepalive_watch(struct sip_keepalive **kap, struct sip *sip,
			const struct sip_msg *msg,
			sip_keepalive_h *kah, void *arg)
{
	struct sip_keepalive *ka;
	int err;

	if (!kap || !sip || !msg || !kah || !msg->sock)
		return EINVAL;

	switch (msg->tp) {

	case SIP_TRANSP_TCP:
	case SIP_TRANSP_TLS:
	case SIP_TRANSP_WS:
	case SIP_TRANSP_WSS:
		break;

	default:
		return EPROTONOSUPPORT;
	}

	ka = mem_zalloc(sizeof(*ka), destructor);
	if (!ka)
		return ENOMEM;

	ka->kah = kah;
	ka->arg = arg;

	err = sip_keepalive_conn_watch(ka, (struct sip_conn *)msg->sock);
	if (err) {
		mem_deref(ka);
		return err;
	}

	ka->kap = kap;
	*kap = ka;

	return 0;
}
]=]
"sip_keepalive_watch(): close notification without pings")

echosdk_patch("${SOURCE_DIR}/include/re_sip.h"
[=[
int sip_keepalive_start(struct sip_keepalive **kap, struct sip *sip,
			const struct sip_msg *msg, uint32_t interval,
			sip_keepalive_h *kah, void *arg);
]=]
[=[
int sip_keepalive_start(struct sip_keepalive **kap, struct sip *sip,
			const struct sip_msg *msg, uint32_t interval,
			sip_keepalive_h *kah, void *arg);
/* EchoSDK-patched: see src/sip/keepalive.c */
int sip_keepalive_watch(struct sip_keepalive **kap, struct sip *sip,
			const struct sip_msg *msg,
			sip_keepalive_h *kah, void *arg);
]=]
"sip_keepalive_watch() declaration")

echosdk_patch("${SOURCE_DIR}/src/sipreg/reg.c"
[=[
		if (reg->regid > 0 && !reg->terminated && !reg->ka)
			start_outbound(reg, msg);
		goto out;
]=]
[=[
		if (reg->regid > 0 && !reg->terminated && !reg->ka)
			start_outbound(reg, msg);

		/* EchoSDK-patched: without outbound, still hear about the
		 * connection this binding lives on closing — keepalive_handler()
		 * turns that into a registration failure, as it does for an
		 * outbound flow.  A no-op over UDP.  See sip_keepalive_watch(). */
		if (!reg->terminated && !reg->ka)
			(void)sip_keepalive_watch(&reg->ka, reg->sip, msg,
						  keepalive_handler, reg);
		goto out;
]=]
"sipreg: watch the registration's connection for close")


# ---------------------------------------------------------------------------
# Q. rtp/sess — keep the highest sequence number the peer has received
#
# A receiver report counts as lost only the gaps below the highest sequence
# number it has seen (RFC 3550 A.3).  When none of our packets arrive, that
# number stops, and every report says nothing was lost: a dead uplink reads
# as a clean one.  Measured live (2026-09-30, Asterisk echo): 45 s of 100%
# uplink loss reported as 0%, then a single report of 100% once the link was
# back.  libre parses the number (rtcp_rr.last_seq) and drops it; keeping it
# lets src/adapt.c see a report that has not moved while we kept sending.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/include/re_rtp.h"
[=[
	struct {
		uint32_t sent;  /**< Tx RTP Packets                  */
		int lost;       /**< Tx RTP Packets Lost             */
		uint32_t jit;   /**< Tx Inter-arrival Jitter in [us] */
	} tx;
]=]
[=[
	struct {
		uint32_t sent;  /**< Tx RTP Packets                  */
		int lost;       /**< Tx RTP Packets Lost             */
		uint32_t jit;   /**< Tx Inter-arrival Jitter in [us] */
		/* EchoSDK-patched: see src/rtp/sess.c handle_rr_block() */
		uint32_t ehsn;  /**< Highest seq. the peer has received */
	} tx;
]=]
"rtcp_stats: peer's extended highest sequence number")

echosdk_patch("${SOURCE_DIR}/src/rtp/rtcp.h"
[=[
	uint32_t rtt;             /**< Round-trip time in [us]             */
};
]=]
[=[
	uint32_t rtt;             /**< Round-trip time in [us]             */
	uint32_t ehsn;            /**< EchoSDK-patched: peer's highest seq */
};
]=]
"rtp_member: peer's extended highest sequence number")

echosdk_patch("${SOURCE_DIR}/src/rtp/sess.c"
[=[
	/* Lost */
	mbr->cum_lost = rr->lost;
]=]
[=[
	/* Lost */
	mbr->cum_lost = rr->lost;

	/* EchoSDK-patched: what the peer has heard of us at all */
	mbr->ehsn = rr->last_seq;
]=]
"handle_rr_block: keep the peer's extended highest sequence number")

echosdk_patch("${SOURCE_DIR}/src/rtp/sess.c"
[=[
	stats->tx.lost = mbr->cum_lost;
	stats->tx.jit  = mbr->jit;
]=]
[=[
	stats->tx.lost = mbr->cum_lost;
	stats->tx.jit  = mbr->jit;
	stats->tx.ehsn = mbr->ehsn;   /* EchoSDK-patched */
]=]
"rtcp_stats: report the peer's extended highest sequence number")
