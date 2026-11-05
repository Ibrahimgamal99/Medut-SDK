/**
 * @file audio_android.c  Android platform audio session
 *
 * Android has no audio session for the SDK to set up or activate: the
 * "sles_vc" driver (sles_vc.c, OpenSL ES with the VOICE_COMMUNICATION
 * preset) opens the devices itself when a call's audio starts.
 */

#include "../../src/core/medut_internal.h"

int bsdk_platform_audio_init(bool activate)
{
	(void)activate;  /* no platform-managed session to activate */
	return 0;
}
