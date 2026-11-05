/**
 * @file pulse.h  Pulseaudio sound driver (asynchronous API)
 *
 * Copyright (C) 2021 Commend.com - h.ramoser@commend.com
 *                                  c.spielberger@commend.com
 *                                  c.huber@commend.com
 *
 * Adapted from baresip's modules/pulse/pulse.h (BSD-3-Clause); see
 * THIRD_PARTY_NOTICES.
 */


/**
 * Pulseaudio connection struct
 *
 */
struct paconn_st {
	pa_threaded_mainloop *mainloop;
	pa_context *context;
};


/**
 * Pulseaudio stream struct
 *
 */
struct pastream_st {
	char pname[256];
	char device[256];
	char sname[256];
	bool shutdown;

	pa_stream *stream;
	pa_sample_spec ss;
	pa_buffer_attr attr;
	pa_stream_direction_t direction;

	struct {
		size_t overrun;
		size_t underrun;
	} stats;
};


/*player.c*/
int pulse_player_init(struct mdt_auplay *ap);
int pulse_player_alloc(struct mdt_auplay_st **stp, const struct mdt_auplay *ap,
	struct mdt_auplay_prm *prm, const char *device,
	mdt_auplay_write_h *wh, void *arg);
void pulse_stream_write_cb(pa_stream *s, size_t len, void *arg);


/*recorder.c*/
int pulse_recorder_init(struct mdt_ausrc *as);
int pulse_recorder_alloc(struct mdt_ausrc_st **stp, const struct mdt_ausrc *as,
	struct mdt_ausrc_prm *prm, const char *dev, mdt_ausrc_read_h *rh,
	mdt_ausrc_error_h *errh, void *arg);
void pulse_stream_read_cb(pa_stream *s, size_t len, void *arg);


/*pulse.c*/
struct paconn_st *paconn_get(void);
int pulse_set_available_devices(struct list *dev_list,
	pa_operation *(get_dev_info_cb)(pa_context *, struct list*));


/*pastream.c*/
int pastream_alloc(struct pastream_st **bptr, const char *dev,
	const char *pname, const char *sname, pa_stream_direction_t dir,
	uint32_t srate, uint8_t ch, uint32_t ptime, int fmt);
int pastream_start(struct pastream_st *st, void *arg);


