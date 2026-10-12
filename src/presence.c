/**
 * @file presence.c  Presence publish + BLF/MWI consumer
 *
 * Presence publish: medut_account_publish_presence() →
 *   ua_presence_status_set() — baresip's presence module sends PUBLISH.
 *
 * MWI: BEVENT_MWI_NOTIFY fires with bevent_get_text() = raw NOTIFY body.
 *   We parse "Messages-Waiting: yes/no" and "Voice-Message: new/old (urgent)"
 *   then post MEDUT_EV_MWI.
 *
 * Contact/BLF: contact_update_h fires when a subscribed contact's presence
 *   state changes (via NOTIFY from the PBX). We forward it as
 *   MEDUT_EV_PRESENCE_STATE. The presence + contact modules handle the
 *   SUBSCRIBE/NOTIFY dialog automatically.
 *
 * 100rel: medut_account_set_100rel() maps to account_set_rel100_mode().
 */

#include <string.h>
#include <stdlib.h>
#include "medut_internal.h"

/* ── Presence publish ────────────────────────────────────────────────────── */

typedef struct {
	struct medut_account   *acct;
	medut_presence_status_t status;
	int                       result;
} pub_ctx_t;

static enum presence_status to_baresip_status(medut_presence_status_t s)
{
	switch (s) {
	case MEDUT_PRESENCE_OPEN:   return PRESENCE_OPEN;
	case MEDUT_PRESENCE_CLOSED: return PRESENCE_CLOSED;
	case MEDUT_PRESENCE_BUSY:   return PRESENCE_BUSY;
	default:                      return PRESENCE_UNKNOWN;
	}
}

static void publish_fn(void *arg)
{
	pub_ctx_t *ctx = arg;
	if (!ctx->acct->ua) { ctx->result = ENOENT; return; }
	ua_presence_status_set(ctx->acct->ua, to_baresip_status(ctx->status));
	ctx->result = 0;
}

int medut_account_publish_presence(medut_account_handle_t account,
                                      medut_presence_status_t status)
{
	if (!account) return MEDUT_ERR_INVAL;
	pub_ctx_t ctx = {.acct = account, .status = status, .result = 0};
	int err = bsdk_dispatch_sync(publish_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── 100rel ──────────────────────────────────────────────────────────────── */

typedef struct {
	struct medut_account *acct;
	medut_100rel_mode_t   mode;
	int                     result;
} rel100_ctx_t;

static void set_100rel_fn(void *arg)
{
	rel100_ctx_t *ctx = arg;
	if (!ctx->acct->ua) { ctx->result = ENOENT; return; }
	struct account *ba = ua_account(ctx->acct->ua);
	if (!ba) { ctx->result = ENOENT; return; }
	ctx->result = account_set_rel100_mode(ba, (enum rel100_mode)ctx->mode);
}

int medut_account_set_100rel(medut_account_handle_t account,
                                medut_100rel_mode_t mode)
{
	if (!account) return MEDUT_ERR_INVAL;
	rel100_ctx_t ctx = {.acct = account, .mode = mode, .result = 0};
	int err = bsdk_dispatch_sync(set_100rel_fn, &ctx);
	return err ? err : ctx.result;
}

/* ── MWI NOTIFY parser ───────────────────────────────────────────────────── */

/*
 * Body format (RFC 3842):
 *   Messages-Waiting: yes
 *   Voice-Message: 2/4 (0/1)   <- new/old (new-urgent/old-urgent)
 */
static void parse_mwi_body(const char *body,
                             bool *waiting,
                             uint32_t *new_voice, uint32_t *old_voice,
                             uint32_t *new_urgent, uint32_t *old_urgent)
{
	*waiting   = false;
	*new_voice = *old_voice = *new_urgent = *old_urgent = 0;

	const char *p = body;
	while (p && *p) {
		if (strncasecmp(p, "Messages-Waiting:", 17) == 0) {
			p += 17;
			while (*p == ' ') p++;
			*waiting = (strncasecmp(p, "yes", 3) == 0);
		} else if (strncasecmp(p, "Voice-Message:", 14) == 0) {
			p += 14;
			while (*p == ' ') p++;
			/* "new/old (new-urgent/old-urgent)" */
			char *end;
			*new_voice  = (uint32_t)strtoul(p, &end, 10);
			if (*end == '/') *old_voice  = (uint32_t)strtoul(end+1, &end, 10);
			if (*end == ' ' && *(end+1) == '(') {
				end += 2;
				*new_urgent = (uint32_t)strtoul(end, &end, 10);
				if (*end == '/') *old_urgent = (uint32_t)strtoul(end+1, NULL, 10);
			}
		}
		/* advance to next line */
		p = strchr(p, '\n');
		if (p) p++;
	}
}

/* Called from event.c for BEVENT_MWI_NOTIFY */
void bsdk_presence_handle_mwi(struct bevent *event)
{
	struct ua      *ua   = bevent_get_ua(event);
	const char     *body = bevent_get_text(event);

	struct medut_account *acct = ua ? bsdk_account_find_by_ua(ua) : NULL;

	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type = MEDUT_EV_MWI;
	medut_ev_mwi_t *m = &qev->ev.u.mwi;
	m->account = acct;

	if (body) {
		parse_mwi_body(body,
		               &m->messages_waiting,
		               &m->new_voice, &m->old_voice,
		               &m->new_urgent, &m->old_urgent);
		str_ncpy(qev->buf, body, sizeof(qev->buf));
		m->raw_body = qev->buf;
	}

	bsdk_event_post_qev(qev);   /* warns and frees qev when the queue is full */
}

/* ── Contact/BLF presence update handler ────────────────────────────────── */

static medut_presence_status_t from_baresip_status(enum presence_status s)
{
	switch (s) {
	case PRESENCE_OPEN:   return MEDUT_PRESENCE_OPEN;
	case PRESENCE_CLOSED: return MEDUT_PRESENCE_CLOSED;
	case PRESENCE_BUSY:   return MEDUT_PRESENCE_BUSY;
	default:              return MEDUT_PRESENCE_UNKNOWN;
	}
}

static void contact_update_handler(struct contact *c, bool removed, void *arg)
{
	(void)arg;
	if (removed)
		return;

	const char *uri = contact_uri(c);
	medut_presence_status_t status =
		from_baresip_status(contact_presence(c));

	struct medut_queued_event *qev = bsdk_qev_alloc();
	if (!qev)
		return;

	qev->ev.type = MEDUT_EV_PRESENCE_STATE;
	medut_ev_presence_state_t *ps = &qev->ev.u.presence;
	ps->account    = NULL; /* global — contact not tied to one account */
	ps->status     = status;

	if (uri) {
		str_ncpy(qev->buf, uri, sizeof(qev->buf));
		ps->target_uri = qev->buf;
	}

	bsdk_event_post_qev(qev);   /* warns and frees qev when the queue is full */
}

/* ── Presence subscribe / unsubscribe ─────────────────────────────────── */

typedef struct {
	struct medut_account *acct;
	char                   *uri;
	int                     result;
} sub_ctx_t;

static void subscribe_fn(void *arg)
{
	sub_ctx_t *ctx = arg;
	if (!ctx->acct->ua) { ctx->result = ENOENT; return; }

	struct contacts *contacts = baresip_contacts();
	if (!contacts) { ctx->result = ENOENT; return; }

	struct contact *c = contact_find(contacts, ctx->uri);
	if (c) {
		ctx->result = 0;
		return;
	}

	char buf[512];
	re_snprintf(buf, sizeof(buf), "\"%s\" <%s>", ctx->uri, ctx->uri);
	struct pl pl;
	pl_set_str(&pl, buf);
	ctx->result = contact_add(contacts, &c, &pl);
	if (ctx->result == 0 && c)
		contact_set_presence(c, true);
}

int medut_account_subscribe_presence(medut_account_handle_t account,
                                        const char *target_uri)
{
	if (!account || !target_uri) return MEDUT_ERR_INVAL;
	sub_ctx_t ctx = {.acct = account, .result = 0};
	ctx.uri = bsdk_strdup(target_uri);
	if (!ctx.uri) return MEDUT_ERR_NOMEM;
	int err = bsdk_dispatch_sync(subscribe_fn, &ctx);
	mem_deref(ctx.uri);
	return err ? err : ctx.result;
}

typedef struct {
	struct medut_account *acct;
	char                   *uri;
	int                     result;
} unsub_ctx_t;

static void unsubscribe_fn(void *arg)
{
	unsub_ctx_t *ctx = arg;
	if (!ctx->acct->ua) { ctx->result = ENOENT; return; }

	struct contacts *contacts = baresip_contacts();
	if (!contacts) { ctx->result = ENOENT; return; }

	struct contact *c = contact_find(contacts, ctx->uri);
	if (c) {
		contact_set_presence(c, false);
		contact_remove(contacts, c);
		ctx->result = 0;
	} else {
		ctx->result = 0;
	}
}

int medut_account_unsubscribe_presence(medut_account_handle_t account,
                                          const char *target_uri)
{
	if (!account || !target_uri) return MEDUT_ERR_INVAL;
	unsub_ctx_t ctx = {.acct = account, .result = 0};
	ctx.uri = bsdk_strdup(target_uri);
	if (!ctx.uri) return MEDUT_ERR_NOMEM;
	int err = bsdk_dispatch_sync(unsubscribe_fn, &ctx);
	mem_deref(ctx.uri);
	return err ? err : ctx.result;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bsdk_presence_init(void)
{
	struct contacts *contacts = baresip_contacts();
	if (contacts)
		contact_set_update_handler(contacts, contact_update_handler, NULL);
	return 0;
}

void bsdk_presence_close(void)
{
	struct contacts *contacts = baresip_contacts();
	if (contacts)
		contact_set_update_handler(contacts, NULL, NULL);
}
