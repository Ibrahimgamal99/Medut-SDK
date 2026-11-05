/**
 * @file audiounit.h  AudioUnit sound driver -- Internal interface
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/audiounit/audiounit.h (BSD-3-Clause);
 * see THIRD_PARTY_NOTICES.
 */

#if __MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define kAudioObjectPropertyElementMain (kAudioObjectPropertyElementMaster)
#endif

extern AudioComponent audiounit_comp_io;
extern AudioComponent audiounit_comp_conv;


struct audiosess;
struct audiosess_st;
struct conv_buf;


typedef void (audiosess_int_h)(bool start, void *arg);

int  audiosess_alloc(struct audiosess_st **stp,
		     audiosess_int_h *inth, void *arg);
void audiosess_interrupt(bool interrupted);


int audiounit_conv_buf_alloc(struct conv_buf **bufp, size_t framesz);
int  audiounit_get_nb_frames(struct conv_buf *buf, uint32_t *nb_frames);
OSStatus init_data_write(struct conv_buf *buf, void **data,
			 size_t framesz, uint32_t nb_frames);
OSStatus init_data_read(struct conv_buf *buf, void **data,
			size_t framesz, uint32_t nb_frames);


int audiounit_player_alloc(struct mdt_auplay_st **stp, const struct mdt_auplay *ap,
			   struct mdt_auplay_prm *prm, const char *device,
			   mdt_auplay_write_h *wh, void *arg);
int audiounit_recorder_alloc(struct mdt_ausrc_st **stp, const struct mdt_ausrc *as,
			     struct mdt_ausrc_prm *prm, const char *device,
			     mdt_ausrc_read_h *rh, mdt_ausrc_error_h *errh, void *arg);


uint32_t audiounit_aufmt_to_formatflags(enum aufmt fmt);
