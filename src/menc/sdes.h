/**
 * @file sdes.h  SDP Security Descriptions for Media Streams API
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/srtp/sdes.h (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */


struct mdt_crypto {
	uint32_t tag;
	struct pl suite;
	struct pl key_method;
	struct pl key_info;
	struct pl lifetime;  /* optional */
	struct pl mki;       /* optional */
	struct pl sess_prms; /* optional */
};

extern const char mdt_sdp_attr_crypto[];

int mdt_sdes_encode_crypto(struct sdp_media *m, uint32_t tag, const char *suite,
		       const char *key, size_t key_len);
int mdt_sdes_decode_crypto(struct mdt_crypto *c, const char *val);
