/**
 * @file mdt_events.h  What the Medut stack reports to the SDK (typed hooks).
 *
 * The SDK implements these (event.c, sdp.c, transfer.c, presence.c,
 * message.c, log.c); the stack calls them on re_main as things happen: one
 * function per fact, with the fact's data as arguments, so the policy that
 * turns a fact into MEDUT_EV_* events exists once, in the SDK.
 *
 * `acct` / `lc` are the SDK wrappers the stack found for its handles with
 * bsdk_account_find() / bsdk_call_find(); either may be NULL when the
 * wrapper does not exist (yet).
 */

#ifndef MDT_EVENTS_H
#define MDT_EVENTS_H

#include <re.h>
#include "../../include/medut.h"

struct medut_account;
struct medut_call;
struct mdt_call;

/* ── Registration ──────────────────────────────────────────────────────── */

void bsdk_ev_reg_sending(struct medut_account *acct);
void bsdk_ev_reg_ok(struct medut_account *acct);
/* scode: the SIP status of the failure, 0 when the transport failed before
 * one arrived.  reason: human-readable text for the app, may be NULL. */
void bsdk_ev_reg_fail(struct medut_account *acct, uint16_t scode,
                      const char *reason);
void bsdk_ev_reg_unregistering(struct medut_account *acct);
/* The registrar confirmed an un-REGISTER: the binding is gone. */
void bsdk_ev_reg_unregistered(struct medut_account *acct);

/* Not an event, a question: the Contact URI params (RFC 8599 pn-*) the UA's
 * next REGISTER carries, NULL or "" for none.  Answered without the account
 * lock — the stack asks from inside a REGISTER, which an
 * medut_account_foreach() callback may have started while holding it. */
struct mdt_ua;
const char *bsdk_ua_contact_params(const struct mdt_ua *ea);

/* ── Calls ─────────────────────────────────────────────────────────────── */

/* A new incoming call on `acct`.  The SDK wraps `ec`. */
void bsdk_ev_call_incoming(struct medut_account *acct, struct mdt_call *ec,
                           const char *peer_uri, const char *peer_name);
void bsdk_ev_call_ringing(struct medut_call *lc,
                          struct medut_account *acct);
void bsdk_ev_call_established(struct medut_call *lc,
                              struct medut_account *acct);

/* Why a call ended.  `transport_err` marks a close with no SIP status that
 * came from the socket layer ("…[errno]"); `peer_bye` / `peer_cancel` are set
 * by an engine that can tell a remote BYE / CANCEL apart from a dead socket
 * (when it cannot, event.c infers it from the call state). */
struct bsdk_close_cause {
	uint16_t    scode;
	bool        transport_err;
	bool        peer_bye;
	bool        peer_cancel;
	const char *reason;          /* may be NULL */
};
void bsdk_ev_call_closed(struct medut_call *lc,
                         struct medut_account *acct,
                         const struct bsdk_close_cause *cause);

/* The call's hold state changed: `held` — our hold confirmed by the peer,
 * or the peer holding us; `local` — a local hold is wanted (false again
 * after the peer refused one). */
void bsdk_ev_call_hold(struct medut_call *lc,
                       struct medut_account *acct, bool held, bool local);
void bsdk_ev_call_dtmf(struct medut_call *lc, char digit, bool end);
/* One side of an offer/answer was applied.  `answer`: this SDP answered an
 * offer (for the remote side: the peer answered ours). */
void bsdk_ev_call_sdp(struct medut_call *lc, bool remote, bool answer);
/* Media encryption made progress (e.g. DTLS-SRTP secure). */
void bsdk_ev_call_menc(struct medut_call *lc);

void bsdk_ev_transfer_request(struct medut_call *lc,
                              struct medut_account *acct,
                              const char *refer_to);
void bsdk_ev_transfer_failed(struct medut_call *lc,
                             struct medut_account *acct,
                             const char *reason);

/* ── Services ──────────────────────────────────────────────────────────── */

void bsdk_ev_mwi(struct medut_account *acct, const char *body);
/* A watched contact's presence changed (`acct` is the account watching). */
void bsdk_ev_presence(struct medut_account *acct, const char *uri,
                      medut_presence_status_t status);
void bsdk_ev_message(struct medut_account *acct, const struct pl *peer,
                     const struct pl *ctype, struct mbuf *body);

/* ── Logging ───────────────────────────────────────────────────────────── */

/* level: 0 debug, 1 info, 2 warning, 3 error.  Any thread. */
void bsdk_log_deliver(uint32_t level, const char *msg);

#endif /* MDT_EVENTS_H */
