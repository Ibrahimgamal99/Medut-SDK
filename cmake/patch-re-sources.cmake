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
# 5. ice/candpair.c — a candidate pair that fails and then succeeds must end
#    up Succeeded (two hunks in one file; see the per-hunk note on
#    echosdk_patch above)
#
# libre's ICE state machine treats Failed as terminal: icem_candpair_set_state()
# refuses to move a completed pair, and Failed counts as completed.  Behind a
# carrier NAT the first connectivity check routinely times out before the
# binding exists and the *retransmit* is what gets answered — so the pair is
# already Failed when handle_success() (connchk.c) runs.  Two things then go
# wrong, and both are fixed here:
#
#   (a) The success reveals a peer-reflexive address, so construct_valid_pair()
#       clones the pair for it — and icem_candpair_clone() copied the parent's
#       `state`, so the brand-new pair was born Failed.
#   (b) icem_candpair_make_valid() set `valid`, moved the pair to the valid
#       list and cleared err/scode, but its set_state(SUCCEEDED) was a no-op.
#
# The pair is then selected (comp->cp_sel, which nothing gates on state — so
# media does flow) while its state says Failed.  On-device that is
#   "icecomp: {audio.1} set_selected: invalid state 'Failed'
#    [{comp=1} Failed { V } seth_lte0:prflx:… <---> host:…]"
# — valid, not nominated, and no (err)/[scode] because make_valid just erased
# them.  The consequence that matters: concluding_ice() (chklist.c) picks the
# pair to nominate with icem_candpair_find_st(…, SUCCEEDED), a *state* filter,
# so when this is the only valid pair for the component it finds nothing,
# never sets comp->concluded and never sends USE-CANDIDATE.
#
# Fix (a) starts the clone the way icem_candpair_alloc() starts every pair;
# fix (b) steps a Failed pair back to InProgress so the transition to
# Succeeded is allowed.  RFC 8445 gives Failed no outgoing edge, but that rule
# is about not *re-checking* a failed pair; a response that did arrive is the
# pair working, and the valid list is where this function is putting it.
# Both are plain bug fixes, upstreamable as-is.
# ---------------------------------------------------------------------------
echosdk_patch("${SOURCE_DIR}/src/ice/candpair.c"
[=[
	cp->def       = cp0->def;
	cp->valid     = cp0->valid;
	cp->nominated = cp0->nominated;
	cp->state     = cp0->state;
	cp->pprio     = cp0->pprio;
	cp->err       = cp0->err;
	cp->scode     = cp0->scode;

	list_add_sorted(&cp0->icem->checkl, cp);
]=]
[=[
	cp->def       = cp0->def;
	cp->nominated = cp0->nominated;
	cp->pprio     = cp0->pprio;

	/* EchoSDK-patched: a clone with different candidates is a pair that
	 * has not been checked yet, so it must not inherit the parent's verdict.
	 *
	 * The one caller is construct_valid_pair() (connchk.c), cloning the
	 * pair a STUN success arrived on so the peer-reflexive address the
	 * response revealed gets a pair of its own.  Behind a carrier NAT the
	 * parent has usually already *failed* by then — the first check times
	 * out before the binding opens, and the late answer is what discovers
	 * the prflx — and copying `state` handed the clone ICE_CANDPAIR_FAILED.
	 * Nothing can undo that: icem_candpair_set_state() treats FAILED as
	 * terminal, so the icem_candpair_make_valid() that follows moved the
	 * pair onto the valid list and left it Failed.  Seen on-device as
	 * "set_selected: invalid state 'Failed' [... { V } ...]", and the pair
	 * is then invisible to concluding_ice(), which selects by state, so
	 * USE-CANDIDATE nomination is skipped for the component.
	 *
	 * Start the clone the way icem_candpair_alloc() starts every pair.
	 * The caller makes it valid on the next line anyway; `err` and
	 * `scode` describe a check this pair never ran. */
	cp->valid     = false;
	cp->state     = ICE_CANDPAIR_FROZEN;
	cp->err       = 0;
	cp->scode     = 0;

	list_add_sorted(&cp0->icem->checkl, cp);
]=]
"ICE: a cloned candidate pair does not inherit the parent's Failed state")

echosdk_patch("${SOURCE_DIR}/src/ice/candpair.c"
[=[
	cp->err = 0;
	cp->scode = 0;
	cp->valid = true;

	icem_candpair_set_state(cp, ICE_CANDPAIR_SUCCEEDED);

	list_unlink(&cp->le);
	list_add_sorted(&cp->icem->validl, cp);
]=]
[=[
	cp->err = 0;
	cp->scode = 0;
	cp->valid = true;

	/* EchoSDK-patched: a success that arrives after the pair was given up
	 * on must still be recorded.
	 *
	 * icem_candpair_set_state() refuses to move a completed pair, and
	 * FAILED counts as completed — so a pair whose check timed out and
	 * then answered (the ordinary course of events behind a carrier NAT,
	 * where the first request leaves before the binding exists) stayed
	 * Failed while this function put it on the valid list and cleared the
	 * error that said why.  That mismatch is what "set_selected: invalid
	 * state 'Failed'" reports, and concluding_ice() cannot nominate a pair
	 * it cannot find by state.
	 *
	 * RFC 8445 gives Failed no outgoing edge, but that is about not
	 * re-checking a failed pair; a response that did arrive is the pair
	 * working, and the valid list is where it is going.  Step it back so
	 * the transition below is allowed. */
	if (cp->state == ICE_CANDPAIR_FAILED)
		cp->state = ICE_CANDPAIR_INPROGRESS;

	icem_candpair_set_state(cp, ICE_CANDPAIR_SUCCEEDED);

	list_unlink(&cp->le);
	list_add_sorted(&cp->icem->validl, cp);
]=]
"ICE: a late connectivity-check success moves a Failed pair to Succeeded")

message(STATUS "patch-re-sources: libre patching complete")
