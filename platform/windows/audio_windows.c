/**
 * @file audio_windows.c  Windows platform audio stub
 *
 * baresip's wasapi module handles audio playback/capture directly. This file
 * exists only to satisfy the platform source requirement.
 *
 * Future enhancement: IMMNotificationClient registration here for automatic
 * default-device-change notifications (headphone plug/unplug events).
 */

#include "../../src/medut_internal.h"

int bsdk_platform_audio_init(bool activate)
{
	(void)activate;  /* no platform-managed session to activate */
	/* baresip's wasapi module (loaded by modules_init) handles Windows audio */
	return 0;
}
