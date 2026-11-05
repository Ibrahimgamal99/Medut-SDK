/**
 * @file ab_call.c  One measured call, for A/B comparison between builds
 *
 * Places one call against an echo service, feeds it a 440 Hz tone through the
 * app-owned audio device and measures what comes back, then prints a single
 * JSON line.  Public API only (include/medut.h), so the same source runs on
 * any build of the SDK — main, a branch, a release — and
 * tools/ab/run-ab.sh compares them call by call.
 *
 *   ab_call --server <url> --user <aor> --pass <pw> --dial <target>
 *           [--menc none|sdes|dtls] [--ice] [--stun <uri>] [--codec <name>]
 *           [--secs <n>] [--tag <label>]
 *
 * Reports: setup_ms (invite → ESTABLISHED), audio_ms (invite → the echoed
 * tone first heard), receive-side quality from the SDK's own MEDIA_STATS
 * (MOS, loss, jitter, jitter-buffer delay, concealment), and the tone that
 * came back: level, a crude frequency, and tone_purity — the share of the
 * received power at 440 Hz (1.0 = a clean tone; concealment, time-stretching
 * and dropouts all lower it).  Exit 0 when the call came up and the tone
 * made the round trip at all.
 *
 * Build:
 *   gcc -O1 -std=gnu11 -Iinclude tools/ab/ab_call.c <dist>/medut.so \
 *       -Wl,-rpath,<dist> -lm -lpthread -o ab_call
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include "medut.h"

#define TONE_HZ  440.0
#define TONE_AMP 8000.0

static medut_account_handle_t g_acct;
static medut_call_handle_t    g_call;

static atomic_int  g_registered, g_reg_failed, g_established, g_ended;
static atomic_int  g_run_audio;
static uint64_t    g_t_invite, g_t_estab;
static atomic_ullong g_t_audio;
static char        g_end_reason[160];
static int         g_end_state = -1;

/* MEDIA_STATS accumulation (event thread only) */
static struct {
	int    n;
	double mos_sum, mos_min, loss_sum, jit_sum, jb_sum, rtt_last;
	unsigned plc, fec, late, rx, tx;
	char   codec[24];
} g_st = { .mos_min = 5.0 };

/* Tone analysis of what is played out (playback thread only) */
static struct {
	unsigned long long samples, crossings;   /* per-channel frames */
	double   sq_sum;
	int16_t  last;
	uint32_t srate;
	/* tone purity: Goertzel power at TONE_HZ over 100 ms windows (a whole
	 * number of cycles), against the windows' total power */
	double   win[4800];
	size_t   winn;
	double   tone_pow, all_pow;
} g_rx;

static void purity_window(void)
{
	const size_t n = g_rx.winn;
	const double w = 2.0 * M_PI * TONE_HZ / g_rx.srate;
	const double coeff = 2.0 * cos(w);
	double s1 = 0, s2 = 0, sq = 0;

	for (size_t i = 0; i < n; i++) {
		double s0 = g_rx.win[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
		sq += g_rx.win[i] * g_rx.win[i];
	}
	/* A pure tone of amplitude A: |X|^2 = (A n / 2)^2, sum x^2 = A^2 n / 2,
	 * so 2 |X|^2 / n over sum x^2 is 1. */
	g_rx.tone_pow += 2.0 * (s1 * s1 + s2 * s2 - coeff * s1 * s2) / n;
	g_rx.all_pow  += sq;
	g_rx.winn = 0;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void on_event(const medut_event_t *ev, void *ud)
{
	(void)ud;

	switch (ev->type) {
	case MEDUT_EV_LOG:
		/* V=1 raises the log level; the lines arrive as events.  On
		 * stdout: the SDK points stderr at /dev/null at init. */
		if (getenv("V") && ev->u.log.message)
			printf("%s", ev->u.log.message);
		break;

	case MEDUT_EV_REG_STATE:
		if (ev->u.reg.state == MEDUT_REG_REGISTERED)
			atomic_store(&g_registered, 1);
		else if (ev->u.reg.state == MEDUT_REG_FAILED ||
		         ev->u.reg.state == MEDUT_REG_RECONNECTING)
			atomic_store(&g_reg_failed, 1);
		break;

	case MEDUT_EV_CALL_STATE:
		if (ev->u.call_state.call != g_call)
			break;
		if (ev->u.call_state.state == MEDUT_CALL_ESTABLISHED &&
		    !atomic_load(&g_established)) {
			g_t_estab = now_ms();
			atomic_store(&g_established, 1);
		}
		if (ev->u.call_state.state >= MEDUT_CALL_ENDED) {
			g_end_state = ev->u.call_state.state;
			snprintf(g_end_reason, sizeof g_end_reason, "%s",
			         ev->u.call_state.reason ? ev->u.call_state.reason
			                                 : "");
			atomic_store(&g_ended, 1);
		}
		break;

	case MEDUT_EV_MEDIA_STATS: {
		const medut_ev_media_stats_t *s = &ev->u.stats;
		if (s->call != g_call || s->is_final || s->mos_lq_rx <= 0.f)
			break;
		g_st.n++;
		g_st.mos_sum  += s->mos_lq_rx;
		if (s->mos_lq_rx < g_st.mos_min)
			g_st.mos_min = s->mos_lq_rx;
		g_st.loss_sum += s->loss_pct_rx;
		g_st.jit_sum  += s->jitter_ms;
		g_st.jb_sum   += s->jitter_buffer_ms;
		g_st.rtt_last  = s->rtt_ms;
		g_st.plc  = s->plc_concealed;
		g_st.fec  = s->fec_recovered;
		g_st.late = s->late_packets;
		g_st.rx   = s->packets_received;
		g_st.tx   = s->packets_sent;
		if (s->codec_name)
			snprintf(g_st.codec, sizeof g_st.codec, "%s",
			         s->codec_name);
		break;
	}

	default:
		break;
	}
}

/* Capture thread: a tone, paced in 20 ms frames at the call's format. */
static void *capture_thread(void *arg)
{
	uint32_t srate = 0;
	uint8_t  ch = 0;
	double   phase = 0;
	int16_t  buf[2 * 48000 / 50];
	struct timespec next;
	(void)arg;

	clock_gettime(CLOCK_MONOTONIC, &next);
	while (atomic_load(&g_run_audio)) {
		if (!srate && medut_audio_external_format(&srate, &ch, NULL))
			srate = 0;
		if (srate && ch) {
			size_t frames = srate / 50;
			for (size_t i = 0; i < frames; i++) {
				int16_t v = (int16_t)(TONE_AMP * sin(phase));
				for (uint8_t c = 0; c < ch; c++)
					buf[i * ch + c] = v;
				phase += 2.0 * M_PI * TONE_HZ / srate;
			}
			(void)medut_audio_external_push(buf, frames * ch);
		}
		next.tv_nsec += 20000000L;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return NULL;
}

/* Playback thread: pull 20 ms frames, find the echoed tone. */
static void *playback_thread(void *arg)
{
	uint32_t srate = 0;
	uint8_t  ch = 0;
	int16_t  buf[2 * 48000 / 50];
	struct timespec next;
	(void)arg;

	clock_gettime(CLOCK_MONOTONIC, &next);
	while (atomic_load(&g_run_audio)) {
		if (!srate && medut_audio_external_format(&srate, &ch, NULL))
			srate = 0;
		if (srate && ch) {
			size_t frames = srate / 50;
			double sq = 0;
			unsigned long long cross = 0;

			(void)medut_audio_external_pull(buf, frames * ch);
			for (size_t i = 0; i < frames; i++) {
				int16_t v = buf[i * ch];
				if ((v >= 0) != (g_rx.last >= 0))
					cross++;
				g_rx.last = v;
				sq += (double)v * v;
				if (atomic_load(&g_t_audio)) {
					g_rx.win[g_rx.winn++] = v;
					if (g_rx.winn >= srate / 10)
						purity_window();
				}
			}
			/* First frame carrying a tone-like level: audio is up. */
			if (!atomic_load(&g_t_audio) &&
			    sqrt(sq / frames) > TONE_AMP / 10.0)
				atomic_store(&g_t_audio, now_ms());
			if (atomic_load(&g_t_audio)) {
				g_rx.srate      = srate;
				g_rx.samples   += frames;
				g_rx.crossings += cross;
				g_rx.sq_sum    += sq;
			}
		}
		next.tv_nsec += 20000000L;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return NULL;
}

static int wait_flag(atomic_int *f, unsigned ms)
{
	for (unsigned t = 0; t < ms; t += 20) {
		if (atomic_load(f) || atomic_load(&g_reg_failed) ||
		    atomic_load(&g_ended))
			return atomic_load(f);
		usleep(20000);
	}
	return atomic_load(f);
}

static void json_str(const char *k, const char *v, int last)
{
	printf("\"%s\":\"", k);
	for (; v && *v; v++) {
		if (*v == '"' || *v == '\\')
			putchar('\\');
		if ((unsigned char)*v >= 0x20)
			putchar(*v);
	}
	printf("\"%s", last ? "" : ",");
}

int main(int argc, char **argv)
{
	const char *server = NULL, *user = NULL, *pass = NULL, *dial = NULL;
	const char *menc = "none", *stun = NULL, *codec = NULL, *tag = "";
	int ice = 0, secs = 10, err;
	pthread_t cap, play;
	medut_config_t cfg;
	medut_account_config_t ac;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if      (!strcmp(a, "--server") && v) { server = v; i++; }
		else if (!strcmp(a, "--user")   && v) { user   = v; i++; }
		else if (!strcmp(a, "--pass")   && v) { pass   = v; i++; }
		else if (!strcmp(a, "--dial")   && v) { dial   = v; i++; }
		else if (!strcmp(a, "--menc")   && v) { menc   = v; i++; }
		else if (!strcmp(a, "--stun")   && v) { stun   = v; i++; }
		else if (!strcmp(a, "--codec")  && v) { codec  = v; i++; }
		else if (!strcmp(a, "--tag")    && v) { tag    = v; i++; }
		else if (!strcmp(a, "--secs")   && v) { secs = atoi(v); i++; }
		else if (!strcmp(a, "--ice"))        { ice = 1; }
		else {
			fprintf(stderr, "usage: %s --server url --user aor --pass pw "
			        "--dial target [--menc none|sdes|dtls] [--ice] "
			        "[--stun uri] [--codec name] [--secs n] "
			        "[--tag label]\n", argv[0]);
			return 2;
		}
	}
	if (!server || !user || !pass || !dial)
		return 2;

	medut_config_init(&cfg);
	cfg.event_cb          = on_event;
	cfg.log_level         = getenv("V") ? 3 : 0;
	cfg.stats_interval_ms = 1000;
	cfg.verify_server     = false;   /* local interop PBXs: self-signed */
	if (medut_init(&cfg)) {
		printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"init\"}\n", tag);
		return 1;
	}
	(void)medut_audio_use_external(true);

	memset(&ac, 0, sizeof ac);
	ac.uri         = user;
	ac.password    = pass;
	ac.server_url  = server;
	ac.stun_server = stun;
	ac.ice_enabled = ice;
	ac.media_enc   = !strcmp(menc, "dtls") ? MEDUT_MEDIA_ENC_DTLS_SRTP
	               : !strcmp(menc, "sdes") ? MEDUT_MEDIA_ENC_SDES
	                                       : MEDUT_MEDIA_ENC_NONE;
	if (ice || ac.media_enc == MEDUT_MEDIA_ENC_DTLS_SRTP) {
		ac.rtcp_mux     = true;
		ac.rtcp_mux_set = true;
	}
	if (codec) {
		ac.audio_codec_name_count = 1;
		snprintf(ac.audio_codec_names[0], sizeof ac.audio_codec_names[0],
		         "%s", codec);
	}

	err = medut_account_create(&ac, &g_acct);
	if (!err)
		err = medut_account_register(g_acct);
	if (err || !wait_flag(&g_registered, 15000)) {
		printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"register\"}\n",
		       tag);
		medut_shutdown();
		return 1;
	}

	atomic_store(&g_run_audio, 1);
	pthread_create(&cap, NULL, capture_thread, NULL);
	pthread_create(&play, NULL, playback_thread, NULL);

	g_t_invite = now_ms();
	err = medut_call_invite(g_acct, dial, &g_call);
	if (!err && wait_flag(&g_established, 20000))
		sleep((unsigned)secs);
	if (g_call && !atomic_load(&g_ended)) {
		(void)medut_call_hangup(g_call);
		wait_flag(&g_ended, 5000);
	}

	atomic_store(&g_run_audio, 0);
	pthread_join(cap, NULL);
	pthread_join(play, NULL);
	medut_account_unregister(g_acct);
	usleep(300000);
	medut_shutdown();

	/* ── Report ──────────────────────────────────────────────────────── */
	uint64_t t_audio = atomic_load(&g_t_audio);
	double secs_rx = 0, hz = 0, rms = 0;
	if (g_rx.samples && g_rx.srate) {
		secs_rx = (double)g_rx.samples / g_rx.srate;
		hz  = (double)g_rx.crossings / 2.0 / secs_rx;
		rms = sqrt(g_rx.sq_sum / (double)g_rx.samples);
	}

	double purity = g_rx.all_pow > 0 ? g_rx.tone_pow / g_rx.all_pow : 0;
	if (purity > 1.0)
		purity = 1.0;
	/* ok: the call came up and the tone made the round trip.  How
	 * faithfully is tone_purity's business, not a pass/fail. */
	int ok = atomic_load(&g_established) && t_audio;
	printf("{");
	json_str("tag", tag, 0);
	printf("\"ok\":%s,", ok ? "true" : "false");
	printf("\"setup_ms\":%lld,", g_t_estab ? (long long)(g_t_estab - g_t_invite)
	                                       : -1LL);
	printf("\"audio_ms\":%lld,", t_audio ? (long long)(t_audio - g_t_invite)
	                                     : -1LL);
	printf("\"ticks\":%d,", g_st.n);
	printf("\"mos_rx_avg\":%.3f,", g_st.n ? g_st.mos_sum / g_st.n : 0.0);
	printf("\"mos_rx_min\":%.3f,", g_st.n ? g_st.mos_min : 0.0);
	printf("\"loss_rx_pct\":%.2f,", g_st.n ? g_st.loss_sum / g_st.n : 0.0);
	printf("\"jitter_ms\":%.2f,", g_st.n ? g_st.jit_sum / g_st.n : 0.0);
	printf("\"jb_ms\":%.1f,", g_st.n ? g_st.jb_sum / g_st.n : 0.0);
	printf("\"rtt_ms\":%.1f,", g_st.rtt_last);
	printf("\"plc\":%u,\"fec\":%u,\"late\":%u,\"rx\":%u,\"tx\":%u,",
	       g_st.plc, g_st.fec, g_st.late, g_st.rx, g_st.tx);
	printf("\"tone_hz\":%.1f,\"tone_rms\":%.0f,\"tone_purity\":%.3f,",
	       hz, rms, purity);
	json_str("codec", g_st.codec, 0);
	printf("\"end_state\":%d,", g_end_state);
	json_str("end_reason", g_end_reason, 1);
	printf("}\n");
	fflush(stdout);

	return ok ? 0 : 1;
}
