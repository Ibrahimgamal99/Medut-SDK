/**
 * @file mdt_media.h  Private header of the Medut media layer (src/media/)
 *
 * RTP streams, the jitter buffer, the audio pipeline (source → filters →
 * encoder → RTP, RTP → jitter buffer → decoder → filters → player), the
 * codec and media-encryption registries, and the call layer's view of them.
 *
 * Adapted from baresip's include/baresip.h and src/core.h (BSD-3-Clause,
 * Copyright (C) 2010 Alfred E. Heggestad); see THIRD_PARTY_NOTICES.
 */

#ifndef MDT_MEDIA_H
#define MDT_MEDIA_H

#include <re.h>
#include <re_atomic.h>
#include "../core/mdt_stack.h"

struct auframe;
struct rtp_header;
struct rtpext;
struct rtcp_msg;
struct rtcp_stats;

/* Consistency checks on the stack's own objects: off in release builds. */
#define MDT_MAGIC_DECL
#define MDT_MAGIC_INIT(s)
#define MDT_MAGIC_CHECK(s) do { (void)(s); } while (0)

/* Clock of the audio timestamps the pipeline keeps [ticks per second]. */
#define AUDIO_TIMEBASE 1000000U

/* DTMF key code for "the key was released" (end of an RFC 4733 event). */
#define KEYCODE_REL 0x04

enum {
	AUDIO_BANDWIDTH   = 128000,  /* bandwidth for audio [bit/s]      */
	MDT_STREAM_PRESZ = 4 + 12,  /* headroom for the RTP header     */
};

/* ── Configuration (media/config.c, from medut_config_t) ──────────────── */

struct mdt_range {
	uint32_t min;
	uint32_t max;
};

static inline bool mdt_in_range(const struct mdt_range *rng, uint32_t val)
{
	return rng ? (val >= rng->min && val <= rng->max) : false;
}

enum mdt_jbuf_type {
	MDT_JBUF_OFF,
	MDT_JBUF_FIXED,
	MDT_JBUF_ADAPTIVE
};

/* Where RTP is received: on re_main, or on a thread of the stream's own. */
enum rtp_receive_mode {
	RECEIVE_MODE_MAIN = 0,
	RECEIVE_MODE_THREAD,
};

struct mdt_config_call {
	uint32_t local_timeout;   /* incoming call timeout [s], 0 = off   */
	uint32_t max_calls;       /* 0 = unlimited                         */
	bool     hold_other_calls;
};

struct mdt_config_audio {
	char     src_mod[16];     /* audio source driver                   */
	char     src_dev[128];
	char     play_mod[16];    /* audio player driver                   */
	char     play_dev[128];
	uint32_t srate_play;      /* optional sampling rate for the player */
	uint32_t srate_src;
	uint32_t channels_play;
	uint32_t channels_src;
	bool     level;           /* audio level indication                */
	int      src_fmt;         /* enum aufmt                            */
	int      play_fmt;
	int      enc_fmt;
	int      dec_fmt;
	struct mdt_range buffer; /* playout buffer [ms]                   */
	uint32_t telev_pt;        /* payload type of telephone-event       */
};

struct mdt_config_avt {
	uint8_t  rtp_tos;         /* TOS byte of outgoing RTP              */
	struct mdt_range rtp_ports;
	struct mdt_range rtp_bw; /* RTP bandwidth range [bit/s]           */
	bool     rtcp_mux;
	struct {
		enum mdt_jbuf_type jbtype;
		struct mdt_range   jbuf_del;   /* min/max delay [ms]      */
		uint32_t            jbuf_sz;    /* max size [packets]      */
	} audio;
	bool     rtp_stats;
	uint32_t rtp_timeout;     /* [s], 0 = off                          */
	enum rtp_receive_mode rxmode;
};

struct mdt_config {
	struct mdt_config_call  call;
	struct mdt_config_audio audio;
	struct mdt_config_avt   avt;
};

/* The stack's media configuration, filled from g_bsdk.cfg at init. */
struct mdt_config *mdt_config(void);
void mdt_config_init(const medut_config_t *cfg);

/* ── Audio codecs ────────────────────────────────────────────────────────── */

struct mdt_auenc_param {
	uint32_t bitrate;         /* wanted bitrate [bit/s] */
};

struct mdt_auenc_state;
struct mdt_audec_state;
struct mdt_aucodec;

typedef int (mdt_auenc_update_h)(struct mdt_auenc_state **aesp,
				  const struct mdt_aucodec *ac,
				  struct mdt_auenc_param *prm,
				  const char *fmtp);
typedef int (mdt_auenc_encode_h)(struct mdt_auenc_state *aes,
				  bool *marker, uint8_t *buf, size_t *len,
				  int fmt, const void *sampv, size_t sampc);
typedef int (mdt_audec_update_h)(struct mdt_audec_state **adsp,
				  const struct mdt_aucodec *ac,
				  const char *fmtp);
typedef int (mdt_audec_decode_h)(struct mdt_audec_state *ads,
				  int fmt, void *sampv, size_t *sampc,
				  bool marker, const uint8_t *buf, size_t len);
typedef int (mdt_audec_plc_h)(struct mdt_audec_state *ads,
			       int fmt, void *sampv, size_t *sampc,
			       const uint8_t *buf, size_t len);

struct mdt_aucodec {
	struct le le;
	const char *pt;
	const char *name;
	uint32_t srate;           /* audio sampling rate     */
	uint32_t crate;           /* RTP clock rate          */
	uint8_t  ch;
	uint8_t  pch;             /* RTP packet channels     */
	uint32_t ptime;           /* packet time [ms], 0 = default */
	const char *fmtp;
	mdt_auenc_update_h *encupdh;
	mdt_auenc_encode_h *ench;
	mdt_audec_update_h *decupdh;
	mdt_audec_decode_h *dech;
	mdt_audec_plc_h    *plch;
	sdp_fmtp_enc_h      *fmtp_ench;
	sdp_fmtp_cmp_h      *fmtp_cmph;
};

void mdt_aucodec_register(struct list *aucodecl, struct mdt_aucodec *ac);
void mdt_aucodec_unregister(struct mdt_aucodec *ac);
const struct mdt_aucodec *mdt_aucodec_find(const struct list *aucodecl,
					     const char *name, uint32_t srate,
					     uint8_t ch);
int  mdt_aucodec_print(struct re_printf *pf, const struct mdt_aucodec *ac);
struct list *mdt_aucodecl(void);

/* ── Media encryption ────────────────────────────────────────────────────── */

struct mdt_menc;
struct mdt_menc_sess;
struct mdt_menc_media;
struct mdt_stream;

enum mdt_menc_event {
	MDT_MENC_EVENT_SECURE,
	MDT_MENC_EVENT_VERIFY_REQUEST,
	MDT_MENC_EVENT_PEER_VERIFIED,
};

typedef void (mdt_menc_event_h)(enum mdt_menc_event event, const char *prm,
				 struct mdt_stream *strm, void *arg);
typedef void (mdt_menc_error_h)(int err, void *arg);
typedef int  (mdt_menc_sess_h)(struct mdt_menc_sess **sessp,
				struct sdp_session *sdp, bool offerer,
				mdt_menc_event_h *eventh,
				mdt_menc_error_h *errorh, void *arg);
typedef int  (mdt_menc_media_h)(struct mdt_menc_media **mp,
				 struct mdt_menc_sess *sess,
				 struct rtp_sock *rtp,
				 struct udp_sock *rtpsock,
				 struct udp_sock *rtcpsock,
				 const struct sa *raddr_rtp,
				 const struct sa *raddr_rtcp,
				 struct sdp_media *sdpm,
				 const struct mdt_stream *strm);
typedef int (mdt_menc_txrekey_h)(struct mdt_menc_media *m);

struct mdt_menc {
	struct le le;
	const char *id;
	const char *sdp_proto;
	bool wait_secure;
	mdt_menc_sess_h *sessh;
	mdt_menc_media_h *mediah;
	mdt_menc_txrekey_h *txrekeyh;
};

void mdt_menc_register(struct list *mencl, struct mdt_menc *menc);
void mdt_menc_unregister(struct mdt_menc *menc);
const struct mdt_menc *mdt_menc_find(const struct list *mencl,
				       const char *id);
const char *mdt_menc_event_name(enum mdt_menc_event event);
struct list *mdt_mencl(void);

/* ── Streams ─────────────────────────────────────────────────────────────── */

enum mdt_media_type {
	MDT_MEDIA_AUDIO = 0,
};

struct mdt_stream_param {
	bool use_rtp;
	bool rtcp_mux;
	int af;                   /* wanted address family      */
	const char *cname;        /* RTCP canonical name        */
	const char *peer;         /* peer URI, for diagnostics  */
};

struct mdt_sender;
struct mdt_rtp_receiver;
struct mdt_metric;
struct mdt_jbuf;
struct mdt_audio_recv;

struct mdt_jbuf_stat {
	uint32_t n_put;
	uint32_t n_get;
	uint32_t n_oos;
	uint32_t n_dups;
	uint32_t n_late;
	uint32_t n_late_lost;
	uint32_t n_lost;
	uint32_t n_overflow;
	uint32_t n_flush;
	uint32_t n_gnacks;
	uint32_t c_delay;         /* current delay [ms]         */
	uint32_t c_packets;
	uint32_t c_jitter;        /* current jitter [ms]        */
	int32_t  c_skew;          /* current skew [ms]          */
};

typedef void (mdt_stream_rtp_h)(const struct rtp_header *hdr,
				 struct rtpext *extv, size_t extc,
				 struct mbuf *mb, unsigned lostc,
				 bool new_source, void *arg);
typedef int  (mdt_stream_pt_h)(uint8_t pt, struct mbuf *mb, void *arg);
typedef void (mdt_stream_mnatconn_h)(struct mdt_stream *strm, void *arg);
typedef void (mdt_stream_rtpestab_h)(struct mdt_stream *strm, void *arg);
typedef void (mdt_stream_rtcp_h)(struct mdt_stream *strm,
				  struct rtcp_msg *msg, void *arg);
typedef void (mdt_stream_error_h)(struct mdt_stream *strm, int err,
				   void *arg);
typedef uint64_t (mdt_jbuf_next_play_h)(const struct mdt_jbuf *jb);

/* Incoming RTP timestamps, unwrapped. */
struct mdt_timestamp_recv {
	uint32_t first;
	uint32_t last;
	bool is_set;
	unsigned num_wraps;
};

/* ── Audio ───────────────────────────────────────────────────────────────── */

typedef void (mdt_audio_event_h)(int key, bool end, void *arg);
typedef void (mdt_audio_level_h)(bool tx, double lvl, void *arg);
typedef void (mdt_audio_err_h)(int err, const char *str, void *arg);

/* ── Codecs (src/codec/) and the media layer's own state ─────────────────── */

int  mdt_g711_init(void);
void mdt_g711_close(void);
int  mdt_opus_init(const medut_opus_config_t *cfg, uint32_t expected_loss);
void mdt_opus_close(void);

/* src/menc/: media encryption */
int  mdt_dtls_srtp_init(void);
void mdt_dtls_srtp_close(void);
int  mdt_sdes_srtp_init(void);
void mdt_sdes_srtp_close(void);
/* Frames the Opus decoder rebuilt from FEC / concealed, since it opened. */
int  mdt_opus_decode_fec_stats(const struct mdt_audec_state *ads,
				uint32_t *fec, uint32_t *plc);

/* media_api.c: Opus expected loss [%] for its FEC, read on encoder update */
extern int32_t g_mdt_opus_loss;

/* media_api.c: move established calls onto the current audio driver */
void mdt_media_device_changed(bool input, bool output);

/* ── SDP helpers (media/sdp.c) ───────────────────────────────────────────── */

int sdp_decode_multipart(const struct pl *ctype_prm, struct mbuf *mb);
int sdp_fingerprint_decode(const char *attr, struct pl *hash,
			   uint8_t *md, size_t *sz);

/* ── Functions ───────────────────────────────────────────────────────────── */

/* media/jbuf.c */
int mdt_jbuf_alloc(struct mdt_jbuf **jbp, uint32_t mind, uint32_t maxd, uint32_t maxsz);
void mdt_jbuf_set_srate(struct mdt_jbuf *jb, uint32_t srate);
int mdt_jbuf_set_id(struct mdt_jbuf *jb, const char *id);
int mdt_jbuf_set_type(struct mdt_jbuf *jb, enum mdt_jbuf_type jbtype);
int mdt_jbuf_put(struct mdt_jbuf *jb, const struct rtp_header *hdr, void *mem);
int mdt_jbuf_get(struct mdt_jbuf *jb, struct rtp_header *hdr, void **mem);
void mdt_jbuf_flush(struct mdt_jbuf *jb);
uint32_t mdt_jbuf_packets(const struct mdt_jbuf *jb);
int32_t mdt_jbuf_next_play(const struct mdt_jbuf *jb);
int mdt_jbuf_stats(const struct mdt_jbuf *jb, struct mdt_jbuf_stat *jstat);

/* media/metric.c */
int mdt_metric_init(struct mdt_metric *metric);
void mdt_metric_reset(struct mdt_metric *metric);
struct mdt_metric *mdt_metric_alloc(void);
void mdt_metric_add_packet(struct mdt_metric *metric, size_t packetsize);
double mdt_metric_avg_bitrate(const struct mdt_metric *metric);
uint32_t mdt_metric_n_packets(struct mdt_metric *metric);
uint32_t mdt_metric_n_bytes(struct mdt_metric *metric);
uint32_t mdt_metric_n_err(struct mdt_metric *metric);
uint32_t mdt_metric_bitrate(struct mdt_metric *metric);
void     mdt_metric_inc_err(struct mdt_metric *metric);

/* media/timestamp.c */
int mdt_timestamp_wrap(uint32_t ts_new, uint32_t ts_old);
void mdt_timestamp_set(struct mdt_timestamp_recv *ts, uint32_t rtp_ts);
uint64_t mdt_timestamp_duration(const struct mdt_timestamp_recv *ts);
uint64_t mdt_timestamp_calc_extended(uint32_t num_wraps, uint32_t ts);
double mdt_timestamp_calc_seconds(uint64_t ts, uint32_t clock_rate);

/* media/rtprecv.c */
void mdt_rtprecv_decode(const struct sa *src, const struct rtp_header *hdr,
		     struct mbuf *mb, void *arg);
void mdt_rtprecv_handle_rtcp(const struct sa *src, struct rtcp_msg *msg,
			  void *arg);
void mdt_rtprecv_mnat_connected_handler(const struct sa *raddr1,
				     const struct sa *raddr2, void *arg);
int mdt_rtprecv_start_rtcp(struct mdt_rtp_receiver *rx, const char *cname,
		       const struct sa *peer, bool pinhole);
void mdt_rtprecv_set_socket(struct mdt_rtp_receiver *rx, struct rtp_sock *rtp);
void mdt_rtprecv_set_ssrc(struct mdt_rtp_receiver *rx, uint32_t ssrc);
uint64_t mdt_rtprecv_ts_last(struct mdt_rtp_receiver *rx);
void mdt_rtprecv_set_ts_last(struct mdt_rtp_receiver *rx, uint64_t ts_last);
void mdt_rtprecv_flush(struct mdt_rtp_receiver *rx);
void mdt_rtprecv_enable(struct mdt_rtp_receiver *rx, bool enable);
int mdt_rtprecv_get_ssrc(struct mdt_rtp_receiver *rx, uint32_t *ssrc);
struct mdt_jbuf *mdt_rtprecv_jbuf(struct mdt_rtp_receiver *rx);
void mdt_rtprecv_enable_mux(struct mdt_rtp_receiver *rx, bool enable);
int mdt_rtprecv_alloc(struct mdt_rtp_receiver **rxp,
		   struct mdt_stream *strm,
		   const char *name,
		   const struct mdt_config_avt *cfg,
		   mdt_stream_rtp_h *rtph,
		   mdt_stream_pt_h *pth, void *arg);
int mdt_rtprecv_start_thread(struct mdt_rtp_receiver *rx);
bool mdt_rtprecv_running(const struct mdt_rtp_receiver *rx);
void mdt_rtprecv_set_handlers(struct mdt_rtp_receiver *rx,
			   mdt_stream_rtpestab_h *rtpestabh, void *arg);
struct mdt_metric *mdt_rtprecv_metric(struct mdt_rtp_receiver *rx);
void mdt_rtprecv_set_srate(struct mdt_rtp_receiver *rx, uint32_t srate);

/* media/stream.c */
int mdt_stream_enable_tx(struct mdt_stream *strm, bool enable);
int mdt_stream_enable_rx(struct mdt_stream *strm, bool enable);
void mdt_stream_process_rtcp(struct mdt_stream *strm, struct rtcp_msg *msg);
int mdt_stream_start_mediaenc(struct mdt_stream *strm);
void mdt_stream_mnat_connected(struct mdt_stream *strm, const struct sa *raddr1,
			   const struct sa *raddr2);
int mdt_stream_alloc(struct mdt_stream **sp, struct list *streaml,
		 const struct mdt_stream_param *prm,
		 const struct mdt_config_avt *cfg,
		 struct sdp_session *sdp_sess,
		 enum mdt_media_type type,
		 const struct mdt_mnat *mnat, struct mdt_mnat_sess *mdt_mnat_sess,
		 const struct mdt_menc *menc, struct mdt_menc_sess *mdt_menc_sess,
		 bool offerer,
		 mdt_stream_rtp_h *rtph, mdt_stream_rtcp_h *rtcph, mdt_stream_pt_h *pth,
		 void *arg);
uint8_t mdt_stream_generate_extmap_id(struct mdt_stream *strm);
struct sdp_media *mdt_stream_sdpmedia(const struct mdt_stream *strm);
int mdt_stream_send(struct mdt_stream *s, bool ext, bool marker, int pt, uint32_t ts,
		struct mbuf *mb);
int mdt_stream_update(struct mdt_stream *s);
void mdt_stream_update_encoder(struct mdt_stream *s, int pt_enc);
void mdt_stream_hold(struct mdt_stream *s, bool hold);
void mdt_stream_set_ldir(struct mdt_stream *s, enum sdp_dir dir);
enum sdp_dir mdt_stream_ldir(const struct mdt_stream *s);
void mdt_stream_set_srate(struct mdt_stream *s, uint32_t srate_tx, uint32_t srate_rx);
void mdt_stream_flush(struct mdt_stream *s);
void mdt_stream_enable_rtp_timeout(struct mdt_stream *strm, uint32_t timeout_ms);
void mdt_stream_set_session_handlers(struct mdt_stream *strm,
				 mdt_stream_mnatconn_h *mnatconnh,
				 mdt_stream_rtpestab_h *rtpestabh,
				 mdt_stream_rtcp_h *rtcph,
				 mdt_stream_error_h *errorh, void *arg);
const struct rtcp_stats *mdt_stream_rtcp_stats(const struct mdt_stream *strm);
int mdt_stream_jbuf_stats(const struct mdt_stream *strm, struct mdt_jbuf_stat *stat);
uint32_t mdt_stream_metric_get_tx_n_packets(const struct mdt_stream *strm);
uint32_t mdt_stream_metric_get_tx_n_bytes(const struct mdt_stream *strm);
uint32_t mdt_stream_metric_get_tx_n_err(const struct mdt_stream *strm);
uint32_t mdt_stream_metric_get_tx_bitrate(const struct mdt_stream *strm);
double mdt_stream_metric_get_tx_avg_bitrate(const struct mdt_stream *strm);
uint32_t mdt_stream_metric_get_rx_n_packets(const struct mdt_stream *strm);
uint32_t mdt_stream_metric_get_rx_n_bytes(const struct mdt_stream *strm);
uint32_t mdt_stream_metric_get_rx_n_err(const struct mdt_stream *strm);
uint32_t mdt_stream_metric_get_rx_bitrate(const struct mdt_stream *strm);
double mdt_stream_metric_get_rx_avg_bitrate(const struct mdt_stream *strm);
bool mdt_stream_is_ready(const struct mdt_stream *strm);
void mdt_stream_set_secure(struct mdt_stream *strm, bool secure);
bool mdt_stream_is_secure(const struct mdt_stream *strm);
int mdt_stream_start_rtcp(const struct mdt_stream *strm);
int mdt_stream_enable(struct mdt_stream *strm, bool enable);
const char *mdt_stream_name(const struct mdt_stream *strm);
const struct sa *mdt_stream_raddr(const struct mdt_stream *strm);
enum mdt_media_type mdt_stream_type(const struct mdt_stream *strm);
int mdt_stream_pt_enc(const struct mdt_stream *strm);
struct rtp_sock *mdt_stream_rtp_sock(const struct mdt_stream *strm);
int mdt_stream_ssrc_rx(const struct mdt_stream *strm, uint32_t *ssrc);

/* media/aureceiver.c */
void mdt_aurecv_reset(struct mdt_audio_recv *ar);
void mdt_aurecv_receive(struct mdt_audio_recv *ar, const struct rtp_header *hdr,
		    struct rtpext *extv, size_t extc,
		    struct mbuf *mb, unsigned lostc);
void mdt_aurecv_set_extmap(struct mdt_audio_recv *ar, uint8_t aulevel);
int mdt_aurecv_set_module(struct mdt_audio_recv *ar, const char *module);
int mdt_aurecv_set_device(struct mdt_audio_recv *ar, const char *device);
int mdt_aurecv_alloc(struct mdt_audio_recv **aupp, const struct mdt_config_audio *cfg,
		 size_t sampc, uint32_t ptime);
void mdt_aurecv_flush(struct mdt_audio_recv *ar);
int mdt_aurecv_decoder_set(struct mdt_audio_recv *ar,
		       const struct mdt_aucodec *ac, int pt, const char *params);
int mdt_aurecv_payload_type(const struct mdt_audio_recv *ar);
int mdt_aurecv_filt_append(struct mdt_audio_recv *ar, struct mdt_aufilt_dec_st *decst);
bool mdt_aurecv_filt_empty(const struct mdt_audio_recv *ar);
struct mdt_audec_state *mdt_aurecv_decoder(const struct mdt_audio_recv *ar);
const struct mdt_aucodec *mdt_aurecv_codec(const struct mdt_audio_recv *ar);
void mdt_aurecv_stop(struct mdt_audio_recv *ar);
void mdt_aurecv_stop_auplay(struct mdt_audio_recv *ar);
int mdt_aurecv_start_player(struct mdt_audio_recv *ar, struct list *auplayl);
bool mdt_aurecv_player_started(const struct mdt_audio_recv *ar);
int mdt_aurecv_print_pipeline(struct re_printf *pf, const struct mdt_audio_recv *ar);

/* media/audio.c */
int mdt_audio_alloc(struct mdt_audio **ap, struct list *streaml,
		const struct mdt_stream_param *mdt_stream_prm,
		const struct mdt_config *cfg,
		struct sdp_session *sdp_sess,
		const struct mdt_mnat *mnat, struct mdt_mnat_sess *mdt_mnat_sess,
		const struct mdt_menc *menc, struct mdt_menc_sess *mdt_menc_sess,
		uint32_t ptime, const struct list *aucodecl, bool offerer,
		mdt_audio_event_h *eventh, mdt_audio_level_h *levelh,
		mdt_audio_err_h *errh, void *arg);
int mdt_audio_enable_level(struct mdt_audio *au);
struct mdt_audec_state *mdt_audio_decoder_state(const struct mdt_audio *a);
int mdt_audio_update(struct mdt_audio *a);
void mdt_audio_stop(struct mdt_audio *a);
int mdt_audio_encoder_set(struct mdt_audio *a, const struct mdt_aucodec *ac,
		      int pt_tx, const char *params);
int mdt_audio_decoder_set(struct mdt_audio *a, const struct mdt_aucodec *ac,
		      int pt, const char *params);
struct mdt_stream *mdt_audio_strm(const struct mdt_audio *au);
int mdt_audio_send_digit(struct mdt_audio *a, char key);
void mdt_audio_mute(struct mdt_audio *a, bool muted);
bool mdt_audio_ismuted(const struct mdt_audio *a);
void mdt_audio_sdp_attr_decode(struct mdt_audio *a);
int mdt_audio_set_source(struct mdt_audio *au, const char *mod, const char *device);
int mdt_audio_set_player(struct mdt_audio *a, const char *mod, const char *device);
int mdt_audio_set_bitrate(struct mdt_audio *au, uint32_t bitrate);
const struct mdt_aucodec *mdt_audio_codec(const struct mdt_audio *au, bool tx);

/* media/sdp.c */
int sdp_fingerprint_decode(const char *attr, struct pl *hash,
			   uint8_t *md, size_t *sz);
bool mdt_sdp_media_has_media(const struct sdp_media *m);
int sdp_decode_multipart(const struct pl *ctype_prm, struct mbuf *mb);

#endif /* MDT_MEDIA_H */
