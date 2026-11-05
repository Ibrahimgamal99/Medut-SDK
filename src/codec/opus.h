/**
 * @file opus.h Private Opus Interface
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/opus/opus.h (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */


struct mdt_opus_param {
	opus_int32 srate;
	opus_int32 bitrate;
	opus_int32 stereo;
	opus_int32 cbr;
	opus_int32 inband_fec;
	opus_int32 dtx;
};


/* Encode */
int mdt_opus_encode_update(struct mdt_auenc_state **aesp, const struct mdt_aucodec *ac,
		       struct mdt_auenc_param *prm, const char *fmtp);
int mdt_opus_encode_frm(struct mdt_auenc_state *aes,
		    bool *marker, uint8_t *buf, size_t *len,
		    int fmt, const void *sampv, size_t sampc);

extern uint32_t mdt_opus_complexity;
extern opus_int32 mdt_opus_application;
extern opus_int32 mdt_opus_dtx;   /* our own DTX switch */

/* Decode */
int mdt_opus_decode_update(struct mdt_audec_state **adsp, const struct mdt_aucodec *ac,
		       const char *fmtp);
int mdt_opus_decode_frm(struct mdt_audec_state *ads,
		    int fmt, void *sampv, size_t *sampc,
		    bool marker, const uint8_t *buf, size_t len);
int mdt_opus_decode_pkloss(struct mdt_audec_state *st,
		       int fmt, void *sampv, size_t *sampc,
		       const uint8_t *buf, size_t len);


/* SDP */
void mdt_opus_decode_fmtp(struct mdt_opus_param *prm, const char *fmtp);


