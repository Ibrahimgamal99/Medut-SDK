/**
 * @file coreaudio.c  Apple Coreaudio sound driver
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 *
 * Adapted from baresip's modules/coreaudio/coreaudio.c (BSD-3-Clause);
 * see THIRD_PARTY_NOTICES.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <re.h>
#include <rem.h>
#include "../../src/core/medut_internal.h"
#include "coreaudio.h"


/**
 * @defgroup coreaudio coreaudio
 *
 * Audio driver module for OSX CoreAudio
 */


static struct mdt_auplay *auplay;
static struct mdt_ausrc *ausrc;


int coreaudio_enum_devices(const char *name, struct list *dev_list,
			    CFStringRef *uid, Boolean is_input)
{
	AudioObjectPropertyAddress propertyAddress = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};

	AudioDeviceID *audioDevices = NULL;
	UInt32 dataSize = 0;
	UInt32 deviceCount, i;
	OSStatus status;

	int err = 0;

	if (!dev_list && !uid)
		return EINVAL;

	if (uid) {
		*uid = NULL;

		if (!str_isset(name))
			return 0;
	}

	status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
						&propertyAddress,
						0,
						NULL,
						&dataSize);
	if (kAudioHardwareNoError != status) {
		MDT_WARN("AudioObjectGetPropertyDataSize"
			" (kAudioHardwarePropertyDevices) failed: %i\n",
			status);
		err = ENODEV;
		goto out;
	}

	deviceCount = dataSize / sizeof(AudioDeviceID);

	audioDevices = mem_zalloc(dataSize, NULL);
	if (NULL == audioDevices) {
		err = ENOMEM;
		goto out;
	}

	status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
					    &propertyAddress,
					    0,
					    NULL,
					    &dataSize,
					    audioDevices);
	if (kAudioHardwareNoError != status) {
		MDT_WARN("AudioObjectGetPropertyData"
			" (kAudioHardwarePropertyDevices) failed: %i\n",
			status);
		err = ENODEV;
		goto out;
	}

	if (is_input)
		propertyAddress.mScope = kAudioDevicePropertyScopeInput;
	else
		propertyAddress.mScope = kAudioDevicePropertyScopeOutput;

	for (i = 0; i < deviceCount; ++i) {

		CFStringRef deviceUID = NULL;
		CFStringRef deviceName = NULL;
		const char *name_str;
		/* fallback if CFStringGetCStringPtr fails */
		char name_buf[64];

		propertyAddress.mSelector   = kAudioDevicePropertyStreams;
		status = AudioObjectGetPropertyDataSize(audioDevices[i],
							&propertyAddress,
							0,
							NULL,
							&dataSize);
		if (kAudioHardwareNoError != status || dataSize == 0)
			continue;

		dataSize = sizeof(deviceUID);
		propertyAddress.mSelector = kAudioDevicePropertyDeviceUID;
		status = AudioObjectGetPropertyData(audioDevices[i],
						    &propertyAddress,
						    0,
						    NULL,
						    &dataSize,
						    &deviceUID);
		if (kAudioHardwareNoError != status) {
			MDT_WARN("AudioObjectGetPropertyData"
				" (kAudioDevicePropertyDeviceUID) "
				"failed: %i\n", status);
			continue;
		}

		dataSize = sizeof(deviceName);
		propertyAddress.mSelector =
			kAudioDevicePropertyDeviceNameCFString;

		status = AudioObjectGetPropertyData(audioDevices[i],
						    &propertyAddress,
						    0,
						    NULL,
						    &dataSize,
						    &deviceName);
		if (kAudioHardwareNoError != status) {
			MDT_WARN("AudioObjectGetPropertyData"
				" (kAudioDevicePropertyDeviceNameCFString)"
				" failed: %i\n", status);
			continue;
		}

		name_str = CFStringGetCStringPtr(deviceName,
						 kCFStringEncodingUTF8);

		/* CFStringGetCStringPtr can and does fail
		 * (documented behavior) */
		if (0 == name_str) {
			if (!CFStringGetCString(deviceName,
						name_buf,
						sizeof(name_buf),
						kCFStringEncodingUTF8)) {
				MDT_WARN("CFStringGetCString "
					" failed: %i\n", status);
				continue;
			}
			name_str = name_buf;
		}

		if (uid) {
			if (0 == str_casecmp(name, name_str)) {
				*uid = deviceUID;
				break;
			}
		}
		else {
			err = mdt_mediadev_add(dev_list, name_str);
			if (err)
				break;
		}
	}

 out:
	mem_deref(audioDevices);

	return err;
}


uint32_t coreaudio_aufmt_to_formatflags(enum aufmt fmt)
{
	switch (fmt) {

	case AUFMT_S16LE:  return kLinearPCMFormatFlagIsSignedInteger;
	case AUFMT_S24_3LE:return kLinearPCMFormatFlagIsSignedInteger;
	case AUFMT_FLOAT:  return kLinearPCMFormatFlagIsFloat;
	default: return 0;
	}
}


int mdt_coreaudio_init(void)
{
	int err;

	err  = mdt_auplay_register(&auplay, mdt_auplayl(),
			       "coreaudio", coreaudio_player_alloc);
	err |= mdt_ausrc_register(&ausrc, mdt_ausrcl(),
			      "coreaudio", coreaudio_recorder_alloc);

	if (err)
		return err;

	err  = coreaudio_player_init(auplay);
	err |= coreaudio_recorder_init(ausrc);

	return err;
}


void mdt_coreaudio_close(void)
{
	auplay = mem_deref(auplay);
	ausrc  = mem_deref(ausrc);
}
