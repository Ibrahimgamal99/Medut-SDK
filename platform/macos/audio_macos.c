/**
 * @file audio_macos.c  macOS platform audio session
 *
 * macOS has no audio session for the SDK to set up or activate: the
 * "coreaudio" driver (coreaudio*.c) opens the devices itself when a call's
 * audio starts.
 */

#include "../../src/core/medut_internal.h"

int bsdk_platform_audio_init(bool activate)
{
	(void)activate;  /* no platform-managed session to activate */
	return 0;
}
