/**
 * @file audio_platform.c  Does the platform audio path cancel echo for us?
 *
 * Engine-neutral state the audio drivers write and the audio filters read:
 * the engine sets it when it picks a driver, a driver refines it per recorder,
 * audio_processing.c reads it per frame.
 */

#include "bsdk.h"

/* Whether the audio driver we ended up with captures through the OS voice
 * path, i.e. the device has already run its own AEC (and NS/AGC) before the
 * first sample reaches us.  Set by the engine when it picks the driver and,
 * on Android, per recorder; read per frame by audio_processing.c to decide
 * which of its own filters still have work to do. */
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
		MDT_WARN("Medut: recorder is not on the voice-communication "
		        "path: no platform AEC/NS under us\n");
	bsdk_ap_log_chain("recorder");
}

void bsdk_platform_audio_reset(void)
{
	re_atomic_rlx_set(&s_platform_aec, false);
	s_voice_path_reported = false;
}

void bsdk_platform_aec_set(bool on)
{
	re_atomic_rlx_set(&s_platform_aec, on);
}
