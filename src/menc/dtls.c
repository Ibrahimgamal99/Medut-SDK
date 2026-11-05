/**
 * @file dtls.c DTLS functions
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/dtls_srtp/dtls.c (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */

#include <re.h>
#include "../media/mdt_media.h"
#include "dtls_srtp.h"


int mdt_dtls_print_sha256_fingerprint(struct re_printf *pf, const struct tls *tls)
{
	uint8_t md[32];
	unsigned int i;
	int err = 0;

	if (!tls)
		return EINVAL;

	err = tls_fingerprint(tls, TLS_FINGERPRINT_SHA256, md, sizeof(md));
	if (err)
		return err;

	for (i=0; i<sizeof(md); i++) {
		err |= re_hprintf(pf, "%s%02X", i==0 ? "" : ":", md[i]);
	}

	return err;
}
