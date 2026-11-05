/**
 * @file mdt_sip.h  Private header of the Medut SIP layer (src/sip/)
 *
 * The SIP user agent on libre: transports and TLS, registration,
 * authenticated out-of-dialog requests, routing incoming requests to the
 * user agent they are for, and the services outside a dialog — MESSAGE,
 * presence (watch, be watched, publish) and message waiting.
 *
 * Adapted from baresip's uag.c, ua.c, reg.c, sipreq.c, custom_hdrs.c,
 * message.c and its mwi and presence modules (BSD-3-Clause, Copyright (C)
 * 2010 Alfred E. Heggestad); see THIRD_PARTY_NOTICES.
 *
 * Everything here runs on re_main.
 */

#ifndef MDT_SIP_H
#define MDT_SIP_H

#include "../core/mdt_stack.h"
#include "../media/mdt_media.h"

/* Calls per process an incoming INVITE may add to (486 "Max Calls" past it). */
enum { MDT_MAX_CALLS = 4 };

/* ── sip_stack.c: the one SIP stack ──────────────────────────────────────── */

struct mdt_sip {
	struct sip           *sip;
	struct sip_lsnr      *lsnr;     /* OPTIONS / REFER outside a dialog  */
	struct sipsess_sock  *sock;     /* INVITE                            */
	struct sipevent_sock *evsock;   /* SUBSCRIBE / NOTIFY                */
	struct tls           *tls;      /* SIP over TLS                      */
	struct tls           *wss_tls;  /* SIP over secure WebSocket         */
	struct list           ual;      /* struct mdt_ua                    */
	sip_msg_h            *subh;     /* incoming SUBSCRIBE; NULL → 405    */
	char                  uuid[40]; /* +sip.instance; "" when none       */
};

extern struct mdt_sip g_sip;

/* The user agent an incoming request is for, NULL when none is. */
struct mdt_ua *mdt_sip_find_msg(const struct sip_msg *msg);
/* Calls across every user agent. */
uint32_t mdt_sip_call_count(void);

/* ── ua.c: one user agent (one medut account) ──────────────────────────── */

struct mdt_ua {
	struct le    le;               /* in g_sip.ual                       */

	/* The account, as mdt_ua_prm resolved it (owned copies) */
	char        *buf;              /* the AOR text laddr points into     */
	struct sip_addr laddr;         /* the AOR, decoded, with its params  */
	struct uri   luri;             /* its URI                            */
	char        *aor;              /* luri, encoded                      */
	char        *auth_user;
	char        *auth_pass;
	char        *dispname;
	uint32_t     regint;           /* REGISTER expires; 0 = no register  */
	uint32_t     rwait;            /* refresh at % of expires; 0 = libre */
	char        *outbound;         /* route of every request, may be NULL */
	bool         sip_outbound;     /* RFC 5626: reg-id, path, outbound   */
	enum rel100_mode rel100;
	medut_media_enc_t menc;
	const struct mdt_menc *mencp; /* its media encryption, NULL = none  */
	const struct mdt_mnat *mnat;  /* "ice" or NULL                      */
	bool         rtcp_mux;
	struct mdt_stun_uri *stun_host; /* the NAT server, NULL = none     */
	char        *stun_user;
	char        *stun_pass;
	struct list  aucodecl;         /* references to registered codecs   */
	struct le    acv[16];          /* ... and their list elements       */
	uint32_t     ptime;            /* packet time [ms]                   */
	medut_dtmf_mode_t dtmf;
	struct list  custom_hdrs;      /* struct sip_hdr, on every request   */

	/* The user agent */
	struct list  regl;             /* struct mdt_reg                    */
	struct list  calls;            /* struct mdt_call                   */
	char        *cuser;            /* Contact user                       */
	char        *pub_gruu;         /* from the registrar, may be NULL    */
	medut_presence_status_t pstat; /* published; UNKNOWN = never      */
};

/* The codecs the UA offers: its own list, or every registered codec. */
struct list *mdt_ua_aucodecl(const struct mdt_ua *ua);

/* Complete "600", "bob@host" or a URI into a URI ("sip:", and the account's
 * domain when there is no host).  *strp is allocated. */
int  mdt_ua_uri_complete(const struct mdt_ua *ua, char **strp,
			  const char *dial);
/* The request URI for a MESSAGE / SUBSCRIBE target given as a dial string,
 * a URI or a name-addr ("Bob <sip:bob@host>").  *urip is allocated. */
int  mdt_ua_target(const struct mdt_ua *ua, char **urip,
		    const char *target);

/* Contact user to send: the public GRUU when the registrar gave one. */
const char *mdt_ua_cuser(const struct mdt_ua *ua);
/* sip_auth_h for the UA's credentials (arg = the UA). */
int  mdt_ua_auth(char **username, char **password, const char *realm,
		  void *arg);
/* Local address of the UA's first working registration, NULL if none. */
const struct sa *mdt_ua_regladdr(const struct mdt_ua *ua);
const struct sa *mdt_ua_reg_conn_laddr(const struct mdt_ua *ua);

int  mdt_ua_print_allowed(struct re_printf *pf, const struct mdt_ua *ua);
/* A whole "Supported: ...\r\n" line. */
int  mdt_ua_print_supported(struct re_printf *pf, const struct mdt_ua *ua);
/* "Require: 100rel\r\n" when 100rel is required, else nothing. */
int  mdt_ua_print_require(struct re_printf *pf, const struct mdt_ua *ua);
/* Does the UA support option-tag `tag` (an incoming Require)? */
bool mdt_ua_supports(const struct mdt_ua *ua, const struct pl *tag);

void mdt_ua_handle_options(struct mdt_ua *ua, const struct sip_msg *msg);

/* Custom headers: a list of struct sip_hdr with owned name and value. */
int  mdt_hdrs_add(struct list *hdrs, const char *name, const char *fmt, ...);
int  mdt_hdrs_print(struct re_printf *pf, const struct list *hdrs);

/* ── reg.c: one registration of a user agent ─────────────────────────────── */

struct mdt_reg;

int  mdt_reg_add(struct list *lst, struct mdt_ua *ua, int regid);
/* (Re)create the client and send a REGISTER.  params: Contact params
 * starting with ';' or "". */
int  mdt_reg_register(struct mdt_reg *reg, const char *reg_uri,
		       const char *params, const char *outbound);
void mdt_reg_unregister(struct mdt_reg *reg);
bool mdt_reg_isok(const struct mdt_reg *reg);
/* Refresh the binding in place (RFC 8599 push); see sipreg_refresh(). */
int  mdt_reg_refresh(struct mdt_reg *reg);
const struct sa *mdt_reg_laddr(const struct mdt_reg *reg);
const struct sa *mdt_reg_conn_laddr(const struct mdt_reg *reg);

/* ── sipreq.c: authenticated request outside a dialog ────────────────────── */

int  mdt_sip_req_send(struct mdt_ua *ua, const char *method,
		       const char *uri, sip_resp_h *resph, void *arg,
		       const char *fmt, ...);

/* ── message.c, presence.c, mwi.c: services outside a dialog ─────────────── */

/* An incoming MESSAGE for `ua`: to the SDK, and answered. */
void mdt_message_recv(struct mdt_ua *ua, const struct sip_msg *msg);

/* g_sip.subh: a SUBSCRIBE to a UA's own presence (arg = the UA). */
bool mdt_presence_sub_handler(const struct sip_msg *msg, void *arg);
/* A UA going away: its watched contacts, its watchers, its publication. */
void mdt_presence_ua_close(struct mdt_ua *ua);
void mdt_presence_close(void);

/* A UA registered: subscribe it to its message summary, once. */
void mdt_mwi_reg_ok(struct mdt_ua *ua);
/* A NOTIFY outside a dialog: true when it was a message summary for `ua`
 * (reported and answered). */
bool mdt_mwi_unsolicited(struct mdt_ua *ua, const struct sip_msg *msg);
void mdt_mwi_ua_close(struct mdt_ua *ua);
void mdt_mwi_close(void);

/* ── The call layer's entry points the SIP layer calls ───────────────────── */

/* Take an incoming INVITE that passed routing.  Replies itself on error. */
int  mdt_call_accept(struct mdt_ua *ua, const struct sip_msg *msg);
/* End every call of a UA that is going away. */
void mdt_ua_calls_close(struct mdt_ua *ua);

#endif /* MDT_SIP_H */
