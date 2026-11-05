/**
 * @file dtls_srtp.h DTLS-SRTP Internal api
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/dtls_srtp/dtls_srtp.h (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */


enum {
	LAYER_SRTP = 20,
	LAYER_DTLS = 20, /* must be above zero */
};

struct comp {
	const struct dtls_srtp *ds; /* parent */
	struct dtls_sock *dtls_sock;
	struct tls_conn *tls_conn;
	struct mdt_srtp_stream *tx;
	struct mdt_srtp_stream *rx;
	struct udp_helper *uh_srtp;
	struct udp_sock *app_sock;
	bool negotiated;
	bool is_rtp;
};

/* dtls.c */
int mdt_dtls_print_sha256_fingerprint(struct re_printf *pf, const struct tls *tls);


/* srtp.c */
int  mdt_srtp_stream_add(struct mdt_srtp_stream **sp, enum srtp_suite suite,
		     const uint8_t *key, size_t key_size, bool tx);
int  mdt_srtp_install(struct comp *comp);
