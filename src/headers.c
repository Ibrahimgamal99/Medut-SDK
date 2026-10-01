/**
 * @file headers.c  Custom SIP header injection
 *
 * Per-account headers: echosdk_account_add_header() — applies to all
 * outgoing requests for the account. Headers are stored in the account's
 * custom_hdrs list and also applied via baresip's ua_add_custom_hdr().
 *
 * Per-dialog headers: echosdk_call_add_header() (in call.c) — applies to
 * a specific call/dialog via call_add_custom_hdr(). Stored in the call's
 * custom_hdrs list.
 */

#include <string.h>
#include "echosdk_internal.h"

typedef struct {
	struct echosdk_account *acct;
	const char             *name;
	const char             *value;
	int                     result;
} hdr_ctx_t;

static void custom_hdr_destructor(void *data)
{
	struct bsdk_custom_hdr *hdr = data;
	mem_deref(hdr->name);
	mem_deref(hdr->value);
}

static void add_hdr_fn(void *arg)
{
	hdr_ctx_t *ctx = arg;
	if (!ctx->acct->ua) { ctx->result = ENOENT; return; }

	struct pl name_pl, value_pl;
	pl_set_str(&name_pl,  ctx->name);
	pl_set_str(&value_pl, ctx->value);
	ctx->result = ua_add_custom_hdr(ctx->acct->ua, &name_pl, &value_pl);
	if (ctx->result)
		return;

	struct bsdk_custom_hdr *ch = mem_zalloc(sizeof(*ch),
	                                        custom_hdr_destructor);
	if (!ch) return;
	ch->name  = bsdk_strdup(ctx->name);
	ch->value = bsdk_strdup(ctx->value);
	if (!ch->name || !ch->value) {
		mem_deref(ch);
		return;
	}
	list_append(&ctx->acct->custom_hdrs, &ch->le, ch);
}

int echosdk_account_add_header(echosdk_account_handle_t acct,
                                const char *name, const char *value)
{
	if (!acct || !name || !value) return ECHOSDK_ERR_INVAL;
	hdr_ctx_t ctx = {.acct = acct, .name = name, .value = value, .result = 0};
	int err = bsdk_dispatch_sync(add_hdr_fn, &ctx);
	return err ? err : ctx.result;
}

static void add_reg_hdr_fn(void *arg)
{
	hdr_ctx_t *ctx = arg;
	if (!ctx->acct->ua) { ctx->result = ENOENT; return; }

	struct pl name_pl, value_pl;
	pl_set_str(&name_pl,  ctx->name);
	pl_set_str(&value_pl, ctx->value);
	ctx->result = ua_add_custom_hdr(ctx->acct->ua, &name_pl, &value_pl);
}

int echosdk_account_add_register_header(echosdk_account_handle_t acct,
                                         const char *name, const char *value)
{
	if (!acct || !name || !value) return ECHOSDK_ERR_INVAL;
	hdr_ctx_t ctx = {.acct = acct, .name = name, .value = value, .result = 0};
	int err = bsdk_dispatch_sync(add_reg_hdr_fn, &ctx);
	return err ? err : ctx.result;
}
