/**
 * @file coreaudio.h  Apple Coreaudio sound driver -- internal API
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/coreaudio/coreaudio.h (BSD-3-Clause);
 * see THIRD_PARTY_NOTICES.
 */

#if __MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define kAudioObjectPropertyElementMain (kAudioObjectPropertyElementMaster)
#endif

int coreaudio_player_alloc(struct mdt_auplay_st **stp, const struct mdt_auplay *ap,
			   struct mdt_auplay_prm *prm, const char *device,
			   mdt_auplay_write_h *wh, void *arg);
int coreaudio_recorder_alloc(struct mdt_ausrc_st **stp, const struct mdt_ausrc *as,
			     struct mdt_ausrc_prm *prm, const char *device,
			     mdt_ausrc_read_h *rh, mdt_ausrc_error_h *errh, void *arg);
int coreaudio_enum_devices(const char *name, struct list *dev_list,
			    CFStringRef *uid, Boolean is_input);
uint32_t coreaudio_aufmt_to_formatflags(enum aufmt fmt);
int coreaudio_player_init(struct mdt_auplay *ap);
int coreaudio_recorder_init(struct mdt_ausrc *as);
