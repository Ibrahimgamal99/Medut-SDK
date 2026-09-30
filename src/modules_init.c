/**
 * @file modules_init.c  Static baresip module loading
 *
 * With STATIC=ON baresip embeds modules but still requires explicit loading.
 * module_load(".", name) looks up the name in the generated mod_table[] and
 * calls me->init() — no config file or filesystem access needed.
 *
 * The module list must match what was compiled into the baresip archive
 * (controlled by cmake/modules-desktop.cmake or modules-mobile.cmake).
 */

#include "echosdk_internal.h"

/* Common modules present in both desktop and mobile profiles.
 *
 * Deliberately excluded:
 *   "account" — reads ~/.baresip/accounts from disk; SDK creates all
 *               accounts programmatically via echosdk_account_create()
 *   "contact" — reads ~/.baresip/contacts from disk (template contains
 *               sip:user@domain;presence=p2p which triggers spurious
 *               SUBSCRIBE dialogs); contacts are added via the API
 *   "menu"    — interactive CLI menu; writes to stderr and has no role
 *               in a library SDK
 *   "plc"     — G.711 packet-loss concealment.  Wanted for lossy links, but
 *               baresip's implementation is a thin wrapper over spandsp and
 *               its CMakeLists returns early when SPANDSP is not found, so
 *               listing it here without vendoring spandsp for all five
 *               target platforms would only produce a load warning at every
 *               startup.  Opus needs neither: aureceiver hands every lost
 *               frame to the codec's own PLC (baresip fd532492), and with
 *               cfg.opus_expected_loss_pct rebuilds the last one from FEC —
 *               that is the resilient codec path until spandsp is part of
 *               the build.
 */
static const char *COMMON_MODULES[] = {
	"opus",
	"g711",
	"srtp",
	"dtls_srtp",
	"stun",
	"turn",
	/* "ice": replaced by EchoSDK's own agent (src/ice/), registered from
	 * bsdk_ice_module_init() rather than loaded as a baresip module. */
	"mwi",
	"presence",
	"uuid",
	NULL
};

/* Desktop-only modules — audio plumbing, no codecs: the codec set is
 * identical on desktop and mobile (opus + g711). */
static const char *DESKTOP_EXTRA[] = {
	"aubridge",
	"auconv",
	"auresamp",
	NULL
};

/* Platform audio module selected at compile time */
#if defined(ECHOSDK_AUDIO_OPENSLES)
/* Android: OpenSLES works on every supported API level (minSdk 24).
 * The preferred driver is EchoSDK's own "sles_vc" (voice-communication
 * preset → platform AEC/NS); modules_init() falls back to loading the
 * stock "opensles" module when sles_vc fails to initialize.
 * aaudio needs API >= 26 and is not compiled in — see CMakeLists.txt. */
static const char *PLATFORM_AUDIO[] = { "opensles", NULL };
#elif defined(ECHOSDK_AUDIO_AAUDIO)
static const char *PLATFORM_AUDIO[] = { "aaudio", NULL };
#elif defined(ECHOSDK_AUDIO_AUDIOUNIT)
/* iOS: audiounit covers capture + playback (VoiceProcessingIO = HW AEC).
 * avcapture (video) is deliberately not compiled — see CMakeLists.txt. */
static const char *PLATFORM_AUDIO[] = { "audiounit", NULL };
#elif defined(ECHOSDK_AUDIO_COREAUDIO)
static const char *PLATFORM_AUDIO[] = { "coreaudio", NULL };
#elif defined(ECHOSDK_AUDIO_WASAPI)
static const char *PLATFORM_AUDIO[] = { "wasapi", NULL };
#elif defined(ECHOSDK_AUDIO_PULSE)
static const char *PLATFORM_AUDIO[] = { "pulse", NULL };
#else
static const char *PLATFORM_AUDIO[] = { NULL };
#endif

static void load_list(const char **list)
{
	if (!list)
		return;
	for (int i = 0; list[i]; i++) {
		int err = module_load(".", list[i]);
		if (err && err != EALREADY)
			warning("EchoSDK: module '%s': %m\n", list[i], err);
	}
}

/* Whether the audio driver we ended up with captures through the OS voice
 * path, i.e. the device has already run its own AEC (and NS/AGC) before the
 * first sample reaches us.  Set by modules_init() below and, on Android, per
 * recorder; read per frame by audio_processing.c to decide which of its own
 * filters still have work to do. */
static RE_ATOMIC bool s_platform_aec;
static bool s_voice_path_reported;   /* re thread */

bool bsdk_platform_has_aec(void)
{
	return re_atomic_rlx(&s_platform_aec);
}

void bsdk_platform_voice_path_report(bool active)
{
	bool was = re_atomic_rlx(&s_platform_aec);

	re_atomic_rlx_set(&s_platform_aec, active);

	/* Log the first report and every change, not every call. */
	if (s_voice_path_reported && was == active)
		return;
	s_voice_path_reported = true;

	if (!active)
		warning("EchoSDK: recorder is not on the voice-communication "
		        "path: no platform AEC/NS under us\n");
	bsdk_ap_log_chain("recorder");
}

/* The device module the platform would use on its own, remembered so
 * echosdk_audio_use_external(false) can put it back. */
static char s_platform_audio_mod[32];

const char *bsdk_platform_audio_mod(void)
{
	return s_platform_audio_mod[0] ? s_platform_audio_mod : NULL;
}

int modules_init(void)
{
	const char *audio_mod = PLATFORM_AUDIO[0];

	re_atomic_rlx_set(&s_platform_aec, false);
	s_voice_path_reported = false;

	load_list(COMMON_MODULES);

#if defined(ECHOSDK_PROFILE_DESKTOP)
	load_list(DESKTOP_EXTRA);
#endif

#if defined(ECHOSDK_AUDIO_OPENSLES)
	/* Prefer EchoSDK's voice-communication OpenSLES driver (platform
	 * AEC/NS); fall back to the stock opensles module on failure. */
	if (bsdk_sles_vc_init() == 0) {
		audio_mod = "sles_vc";
		/* VOICE_COMMUNICATION recording preset — the HAL's AEC/NS/AGC.
		 * Assumed until the first recorder says whether the preset
		 * took (bsdk_platform_voice_path_report), so nothing of ours
		 * stacks on it in the meantime. */
		re_atomic_rlx_set(&s_platform_aec, true);
	}
	else {
		/* The stock module records with the generic preset, so nothing
		 * cancels echo below us and the software suppressor is still
		 * the only one there is. */
		load_list(PLATFORM_AUDIO);
	}
#else
	load_list(PLATFORM_AUDIO);
#endif

#if defined(ECHOSDK_AUDIO_AUDIOUNIT)
	/* iOS: audiounit's I/O unit is VoiceProcessingIO, Apple's voice
	 * processing (AEC, NS, AGC).  (The define is only set for iOS — a macOS
	 * build gets coreaudio, which is not echo-cancelled.)  Only if the
	 * module actually registered: otherwise nothing is under us. */
	re_atomic_rlx_set(&s_platform_aec,
	                  ausrc_find(baresip_ausrcl(), "audiounit") != NULL);
#endif

	/* Always available, never automatic: the app opts in with
	 * echosdk_audio_use_external(true). */
	if (bsdk_audio_external_init())
		warning("EchoSDK: app-owned audio device unavailable\n");

	if (audio_mod)
		str_ncpy(s_platform_audio_mod, audio_mod,
		         sizeof(s_platform_audio_mod));

	/* Wire the platform audio module name into baresip config so audio_alloc
	 * picks the real device module.  Must overwrite unconditionally:
	 * aubridge loads earlier (DESKTOP_EXTRA) and ausrc_register/auplay_register
	 * set cfg->audio.{src,play}_mod to the first ausrc/auplay registered.
	 * If we only filled when empty, the platform module would never win. */
	if (audio_mod) {
		struct config *cfg = conf_config();
		str_ncpy(cfg->audio.src_mod, audio_mod,
		         sizeof(cfg->audio.src_mod));
		str_ncpy(cfg->audio.play_mod, audio_mod,
		         sizeof(cfg->audio.play_mod));
#if defined(ECHOSDK_AUDIO_WASAPI)
		/* wasapi src/play accept either a real endpoint ID or the literal
		 * string "default" — an empty device name makes IMMDeviceEnumerator_
		 * GetDevice("") fail with E_INVALIDARG and the audio thread bails
		 * out before producing any frames. */
		if (cfg->audio.src_dev[0] == '\0')
			str_ncpy(cfg->audio.src_dev, "default",
			         sizeof(cfg->audio.src_dev));
		if (cfg->audio.play_dev[0] == '\0')
			str_ncpy(cfg->audio.play_dev, "default",
			         sizeof(cfg->audio.play_dev));
#endif
	}

	/* Not sticky across a restart: the device is the platform's again. */
	bsdk_audio_external_sync();

	return 0;
}
