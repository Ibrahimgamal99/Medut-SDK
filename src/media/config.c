/**
 * @file config.c  The media layer's configuration, from medut_config_t
 *
 * Filled once by mdt_config_init() at mdt_media_init(); streams copy what
 * they need when they are allocated.  The jitter buffer can be re-set later
 * (mdt_jbuf_configure()) and applies to streams opened from then on.
 *
 * The defaults are the values calls ran with before this stack (the old
 * engine's defaults, overridden by what the SDK configured), so calls
 * behave the same unless a behaviour change in the CHANGELOG says
 * otherwise.
 */

#include <string.h>
#include <re.h>
#include <rem.h>
#include "mdt_media.h"

static struct mdt_config s_cfg;
static struct mdt_port_range s_rtp_ports;

/* Jitter-buffer bounds for a 0 in the config.  Fixed keeps {100, 200}: a
 * fixed buffer sits at its minimum, and 40 ms would be too shallow. */
enum {
	JB_ADAPTIVE_MIN_MS = 40,
	JB_ADAPTIVE_MAX_MS = 400,
	JB_FIXED_MIN_MS    = 100,
	JB_FIXED_MAX_MS    = 200,
	JB_MAX_PACKETS     = 50,
};

struct mdt_config *mdt_config(void)
{
	return &s_cfg;
}

const struct mdt_port_range *mdt_rtp_ports(void)
{
	return &s_rtp_ports;
}

void mdt_jbuf_configure(medut_jbuf_type_t type, uint32_t min_ms,
			 uint32_t max_ms)
{
	const bool fixed = (type == MEDUT_JBUF_FIXED);

	if (!min_ms)
		min_ms = fixed ? JB_FIXED_MIN_MS : JB_ADAPTIVE_MIN_MS;
	if (!max_ms)
		max_ms = fixed ? JB_FIXED_MAX_MS : JB_ADAPTIVE_MAX_MS;
	if (max_ms < min_ms)
		max_ms = min_ms;

	s_cfg.avt.audio.jbtype       = fixed ? MDT_JBUF_FIXED
					     : MDT_JBUF_ADAPTIVE;
	s_cfg.avt.audio.jbuf_del.min = min_ms;
	s_cfg.avt.audio.jbuf_del.max = max_ms;
	s_cfg.avt.audio.jbuf_sz      = JB_MAX_PACKETS;
}

void mdt_config_init(const medut_config_t *cfg)
{
	memset(&s_cfg, 0, sizeof(s_cfg));

	/* Calls: an unanswered incoming call times out after 120 s, unless
	 * the app turned session timers off (which has always turned this off
	 * too — kept so behaviour does not change). */
	s_cfg.call.local_timeout    = cfg->session_timer_enabled ? 120 : 0;
	s_cfg.call.max_calls        = 4;
	s_cfg.call.hold_other_calls = true;

	/* Audio pipeline: 16-bit PCM throughout, a 20..160 ms playout
	 * buffer, telephone-event on payload type 101. */
	s_cfg.audio.src_fmt    = AUFMT_S16LE;
	s_cfg.audio.play_fmt   = AUFMT_S16LE;
	s_cfg.audio.enc_fmt    = AUFMT_S16LE;
	s_cfg.audio.dec_fmt    = AUFMT_S16LE;
	s_cfg.audio.buffer.min = 20;
	s_cfg.audio.buffer.max = 160;
	s_cfg.audio.telev_pt   = 101;

	/* RTP.  dscp_rtp is a DSCP value; the socket takes the TOS byte. */
	s_cfg.avt.rtp_tos       = (uint8_t)(cfg->dscp_rtp << 2);
	s_cfg.avt.rtp_ports.min = 1024;
	s_cfg.avt.rtp_ports.max = 49152;
	s_cfg.avt.rtp_stats     = cfg->stats_interval_ms > 0;
	s_cfg.avt.rtp_timeout   = cfg->rtp_timeout_s;
	s_cfg.avt.rxmode        = RECEIVE_MODE_MAIN;

	/* Always applied: a fixed 100 ms buffer would drop every packet
	 * later than that on a jittery link instead of growing. */
	mdt_jbuf_configure(cfg->jbuf_type, cfg->jitter_buffer_min_ms,
			    cfg->jitter_buffer_max_ms);

	s_rtp_ports.min = (uint16_t)s_cfg.avt.rtp_ports.min;
	s_rtp_ports.max = (uint16_t)s_cfg.avt.rtp_ports.max;
}
