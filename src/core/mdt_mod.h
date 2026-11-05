/**
 * @file mdt_mod.h  The module interfaces plug-ins register with
 *
 * The audio drivers (platform/android/sles_vc.c, src/audio/external.c,
 * src/audio/null.c, ...), the audio filters (src/audio/processing.c,
 * src/audio/tap.c) and the ICE agent (src/ice/ice_mod.c) plug into the
 * Medut stack through these.
 *
 * The contract is pinned by test/compat_contract_test.c.  In short:
 *   - alloc and the state's destructor (mem_deref) run on re_main; the
 *     destructor stops any device thread before it returns;
 *   - a source's read handler and a player's write handler are called from
 *     the driver's own thread; frames are prm.fmt at prm.srate / prm.ch,
 *     prm.srate * prm.ch * prm.ptime / 1000 samples;
 *   - errh is called on re_main; err != 0 ends the call with str as reason;
 *   - filter encupd / decupd run on re_main, ench on the TX thread, dech on
 *     re_main, with frames in the format their update announced.
 */

#ifndef MDT_MOD_H
#define MDT_MOD_H

#include <re.h>
/* Not <rem.h>: libre's rem headers carry no include guards, and plug-ins
 * include <rem.h> themselves.  Frames only travel by pointer here. */

struct auframe;
struct dnsc;
struct sdp_session;
struct sdp_media;
struct udp_sock;

/* Stack objects the plug-in API refers to; opaque to plug-ins. */
struct mdt_call;      /* one call                          */
struct mdt_audio;     /* the audio half of a call's media  */
struct mdt_stream;    /* one RTP stream                    */

/* ── Devices a driver enumerates ───────────────────────────────────────── */

/* One device of a driver, in its dev_list (mdt_ausrc / mdt_auplay). */
struct mdt_mediadev {
	struct le le;
	char *name;
	struct {
		uint32_t channels;
		bool is_default;
	} src, play;
	int host_index;           /* driver specific */
	int device_index;         /* driver specific */
};

/* Add `name` to a driver's device list (no-op when it is there). */
int  mdt_mediadev_add(struct list *dev_list, const char *name);
struct mdt_mediadev *mdt_mediadev_find(const struct list *dev_list,
					 const char *name);

/* ── Audio source (capture) ────────────────────────────────────────────── */

struct mdt_ausrc;
struct mdt_ausrc_st;  /* the driver's own state, defined by the driver */

struct mdt_ausrc_prm {
	uint32_t   srate;       /* sampling rate [Hz]                  */
	uint8_t    ch;          /* channels                            */
	uint32_t   ptime;       /* packet time [ms]                    */
	int        fmt;         /* enum aufmt                          */
	size_t     duration;    /* [ms], 0 for infinite                */
};

typedef void (mdt_ausrc_read_h)(struct auframe *af, void *arg);
typedef void (mdt_ausrc_error_h)(int err, const char *str, void *arg);
typedef int  (mdt_ausrc_alloc_h)(struct mdt_ausrc_st **stp,
				  const struct mdt_ausrc *mdt_ausrc,
				  struct mdt_ausrc_prm *prm, const char *device,
				  mdt_ausrc_read_h *rh,
				  mdt_ausrc_error_h *errh, void *arg);
typedef int  (mdt_ausrc_info_h)(const struct mdt_ausrc *mdt_ausrc,
				 struct mdt_ausrc_prm *prm,
				 const char *device);

struct mdt_ausrc {
	struct le            le;
	const char          *name;
	struct list          dev_list;
	mdt_ausrc_alloc_h  *alloch;
	mdt_ausrc_info_h   *infoh;
};

/* Registered until *asp is mem_deref'd. */
int  mdt_ausrc_register(struct mdt_ausrc **asp, struct list *ausrcl,
			 const char *name, mdt_ausrc_alloc_h *alloch);
const struct mdt_ausrc *mdt_ausrc_find(const struct list *ausrcl,
					 const char *name);
int  mdt_ausrc_alloc(struct mdt_ausrc_st **stp, struct list *ausrcl,
		      const char *name, struct mdt_ausrc_prm *prm,
		      const char *device, mdt_ausrc_read_h *rh,
		      mdt_ausrc_error_h *errh, void *arg);
struct list *mdt_ausrcl(void);

/* ── Audio player (playback) ───────────────────────────────────────────── */

struct mdt_auplay;
struct mdt_auplay_st;

struct mdt_auplay_prm {
	uint32_t   srate;
	uint8_t    ch;
	uint32_t   ptime;
	int        fmt;
};

typedef void (mdt_auplay_write_h)(struct auframe *af, void *arg);
typedef int  (mdt_auplay_alloc_h)(struct mdt_auplay_st **stp,
				   const struct mdt_auplay *ap,
				   struct mdt_auplay_prm *prm,
				   const char *device,
				   mdt_auplay_write_h *wh, void *arg);

struct mdt_auplay {
	struct le             le;
	const char           *name;
	struct list           dev_list;
	mdt_auplay_alloc_h  *alloch;
};

int  mdt_auplay_register(struct mdt_auplay **app, struct list *auplayl,
			  const char *name, mdt_auplay_alloc_h *alloch);
const struct mdt_auplay *mdt_auplay_find(const struct list *auplayl,
					   const char *name);
int  mdt_auplay_alloc(struct mdt_auplay_st **stp, struct list *auplayl,
		       const char *name, struct mdt_auplay_prm *prm,
		       const char *device, mdt_auplay_write_h *wh,
		       void *arg);
struct list *mdt_auplayl(void);

/* ── Audio filter ──────────────────────────────────────────────────────── */

struct mdt_aufilt;

/* A filter's per-stream state starts with one of these. */
struct mdt_aufilt_enc_st {
	const struct mdt_aufilt *af;
	struct le le;
};

struct mdt_aufilt_dec_st {
	const struct mdt_aufilt *af;
	struct le le;
};

struct mdt_aufilt_prm {
	uint32_t srate;
	uint8_t  ch;
	int      fmt;       /* enum aufmt */
};

typedef int (mdt_aufilt_encupd_h)(struct mdt_aufilt_enc_st **stp,
				   void **ctx, const struct mdt_aufilt *af,
				   struct mdt_aufilt_prm *prm,
				   const struct mdt_audio *au);
typedef int (mdt_aufilt_encode_h)(struct mdt_aufilt_enc_st *st,
				   struct auframe *af);
typedef int (mdt_aufilt_decupd_h)(struct mdt_aufilt_dec_st **stp,
				   void **ctx, const struct mdt_aufilt *af,
				   struct mdt_aufilt_prm *prm,
				   const struct mdt_audio *au);
typedef int (mdt_aufilt_decode_h)(struct mdt_aufilt_dec_st *st,
				   struct auframe *af);

struct mdt_aufilt {
	struct le le;
	const char *name;
	bool enabled;
	mdt_aufilt_encupd_h *encupdh;
	mdt_aufilt_encode_h *ench;
	mdt_aufilt_decupd_h *decupdh;
	mdt_aufilt_decode_h *dech;
};

void mdt_aufilt_register(struct list *aufiltl, struct mdt_aufilt *af);
void mdt_aufilt_unregister(struct mdt_aufilt *af);
void mdt_aufilt_enable(struct list *aufiltl, const char *name, bool enable);
struct list *mdt_aufiltl(void);

/* ── Media NAT (ICE) ───────────────────────────────────────────────────── */

struct mdt_mnat;
struct mdt_mnat_sess;    /* the media-NAT's own session state */
struct mdt_mnat_media;   /* ... and per-stream state          */

enum mdt_stun_scheme {
	MDT_STUN_SCHEME_STUN,
	MDT_STUN_SCHEME_STUNS,
	MDT_STUN_SCHEME_TURN,
	MDT_STUN_SCHEME_TURNS,
};

struct mdt_stun_uri {
	enum mdt_stun_scheme scheme;
	char *host;
	uint16_t port;
	int proto;
};

typedef void (mdt_mnat_estab_h)(int err, uint16_t scode, const char *reason,
				 void *arg);
typedef void (mdt_mnat_connected_h)(const struct sa *raddr1,
				     const struct sa *raddr2, void *arg);
typedef int  (mdt_mnat_sess_h)(struct mdt_mnat_sess **sessp,
				const struct mdt_mnat *mdt_mnat,
				struct dnsc *dnsc, int af,
				const struct mdt_stun_uri *srv,
				const char *user, const char *pass,
				struct sdp_session *sdp, bool offerer,
				mdt_mnat_estab_h *estabh, void *arg);
typedef int  (mdt_mnat_media_h)(struct mdt_mnat_media **mp,
				 struct mdt_mnat_sess *sess,
				 struct udp_sock *sock1,
				 struct udp_sock *sock2,
				 struct sdp_media *sdpm,
				 mdt_mnat_connected_h *connh, void *arg);
typedef int  (mdt_mnat_update_h)(struct mdt_mnat_sess *sess);
typedef void (mdt_mnat_attr_h)(struct mdt_mnat_media *mm,
				const char *name, const char *value);

struct mdt_mnat {
	struct le le;
	const char *id;
	const char *ftag;
	bool wait_connected;
	mdt_mnat_sess_h *sessh;
	mdt_mnat_media_h *mediah;
	mdt_mnat_update_h *updateh;
	mdt_mnat_attr_h *attrh;
};

void mdt_mnat_register(struct list *mnatl, struct mdt_mnat *mdt_mnat);
void mdt_mnat_unregister(struct mdt_mnat *mdt_mnat);
const struct mdt_mnat *mdt_mnat_find(const struct list *mnatl,
				       const char *id);
struct list *mdt_mnatl(void);

/* ── Engine objects, as far as plug-ins look into them ─────────────────── */

/* Call states, numbered as plug-ins compare them. */
enum mdt_call_state {
	MDT_CALL_IDLE = 0,
	MDT_CALL_INCOMING,
	MDT_CALL_OUTGOING,
	MDT_CALL_RINGING,
	MDT_CALL_EARLY,
	MDT_CALL_ESTABLISHED,
	MDT_CALL_TERMINATED,
	MDT_CALL_TRANSFER,
	MDT_CALL_UNKNOWN,
};

enum mdt_call_state mdt_call_state(const struct mdt_call *call);
enum sdp_neg_state    mdt_call_sdp_neg_state(const struct mdt_call *call);
struct list          *mdt_call_streaml(const struct mdt_call *call);
struct mdt_audio   *mdt_call_audio(const struct mdt_call *call);
bool mdt_stream_is_secure(const struct mdt_stream *strm);

/* The engine's local address of family af, NULL when it has none. */
const struct sa *mdt_net_laddr_af(int af);

/* RTP port range streams bind in; {0, 0} = any. */
struct mdt_port_range { uint16_t min, max; };
const struct mdt_port_range *mdt_rtp_ports(void);

/* A remote format was negotiated and the remote port is not 0. */
bool mdt_sdp_media_has_media(const struct sdp_media *m);

/* Register the plug-in lists (core/mod.c).  Engine init / close. */
int  mdt_mod_init(void);
void mdt_mod_close(void);

#endif /* MDT_MOD_H */
