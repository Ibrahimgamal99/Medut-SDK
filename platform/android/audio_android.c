/**
 * @file audio_android.c  Android platform audio stub
 *
 * baresip's aaudio/opensles modules handle audio directly. No custom driver
 * code is needed.
 *
 * Future enhancement: AudioRecord/AudioTrack JNI calls for low-latency capture
 * or AEC bypass if baresip's aaudio module proves insufficient.
 */

#include "../../src/medut_internal.h"

int bsdk_platform_audio_init(bool activate)
{
	(void)activate;  /* no platform-managed session to activate */
	/* baresip's aaudio module (loaded by modules_init) handles Android audio.
	 * OpenSL ES fallback is also loaded for devices below API 26. */
	return 0;
}
