/**
 * @file device.c  Medut stack: audio driver selection and listing
 *
 * One driver serves every stream: the platform one by default ("null" on a
 * build without one), "external" while the app owns the device, or any
 * registered driver by name.  Streams opened from now on use it, and the
 * established calls are moved onto it (mdt_media_device_changed()).
 */

#include <string.h>
#include "../media/mdt_media.h"

static char s_driver[32]     = "null";
static char s_platform[32]   = "null";
static char s_dev_in[128];
static char s_dev_out[128];

/* The driver streams open on, and the device names they ask it for. */
const char *mdt_audio_driver(void)  { return s_driver; }
const char *mdt_audio_dev(bool input)
{
	const char *dev = input ? s_dev_in : s_dev_out;

#if defined(MEDUT_AUDIO_WASAPI)
	/* WASAPI takes an endpoint id or the literal "default"; an empty name
	 * fails in IMMDeviceEnumerator::GetDevice() with E_INVALIDARG. */
	if (!dev[0] && !str_cmp(s_driver, "wasapi"))
		return "default";
#endif

	return dev;
}

/* Register the drivers this build has and make the platform's the one
 * streams use.  After the null driver, before any account exists. */
int mdt_audio_drivers_init(void)
{
	const char *platform = NULL;

	bsdk_platform_audio_reset();
	s_dev_in[0] = s_dev_out[0] = '\0';

#if defined(MEDUT_AUDIO_PULSE)
	/* Linux desktop: PulseAudio, or PipeWire through its pulse server. */
	if (mdt_pulse_init() == 0)
		platform = "pulse";
	else
		MDT_WARN("Medut: no PulseAudio server; calls use the silent "
			  "device\n");
#elif defined(MEDUT_AUDIO_COREAUDIO)
	if (mdt_coreaudio_init() == 0)
		platform = "coreaudio";
	else
		MDT_WARN("Medut: Core Audio unavailable; calls use the silent "
			  "device\n");
#elif defined(MEDUT_AUDIO_WASAPI)
	if (mdt_wasapi_init() == 0)
		platform = "wasapi";
	else
		MDT_WARN("Medut: WASAPI unavailable; calls use the silent "
			  "device\n");
#elif defined(MEDUT_AUDIO_AUDIOUNIT)
	/* iOS: the I/O unit is VoiceProcessingIO, Apple's voice processing
	 * (AEC, NS, AGC), so nothing of ours stacks on it. */
	if (mdt_audiounit_init() == 0) {
		platform = "audiounit";
		bsdk_platform_aec_set(true);
	}
	else {
		MDT_WARN("Medut: AudioUnit unavailable; calls use the silent "
			  "device\n");
	}
#elif defined(MEDUT_AUDIO_OPENSLES)
	/* Android: Medut's OpenSL ES driver, recording with the
	 * VOICE_COMMUNICATION preset so the HAL's AEC/NS/AGC run.  Assumed on
	 * until the first recorder reports whether the preset took
	 * (bsdk_platform_voice_path_report), so nothing of ours stacks on it
	 * in the meantime. */
	if (bsdk_sles_vc_init() == 0) {
		platform = "sles_vc";
		bsdk_platform_aec_set(true);
	}
	else {
		MDT_WARN("Medut: sles_vc failed to initialise; calls use "
			  "the silent device\n");
	}
#endif

	/* Always available, never automatic: the app opts in with
	 * medut_audio_use_external(true). */
	if (bsdk_audio_external_init())
		MDT_WARN("Medut: app-owned audio device unavailable\n");

	str_ncpy(s_platform, platform ? platform : "null", sizeof(s_platform));
	str_ncpy(s_driver, s_platform, sizeof(s_driver));

	/* Not sticky across a restart: the device is the platform's again. */
	bsdk_audio_external_sync();

	return 0;
}

/* Unregister the platform driver (shutdown; a later init registers it
 * again).  Android's sles_vc and the external driver are closed by the
 * SDK's shutdown itself. */
void mdt_audio_drivers_close(void)
{
#if defined(MEDUT_AUDIO_PULSE)
	mdt_pulse_close();
#elif defined(MEDUT_AUDIO_COREAUDIO)
	mdt_coreaudio_close();
#elif defined(MEDUT_AUDIO_WASAPI)
	mdt_wasapi_close();
#elif defined(MEDUT_AUDIO_AUDIOUNIT)
	mdt_audiounit_close();
#endif
}

int mdt_audio_set_device(bool input, const char *name)
{
	char *dev = input ? s_dev_in : s_dev_out;

	if (name)
		str_ncpy(dev, name, input ? sizeof(s_dev_in)
		                          : sizeof(s_dev_out));
	else
		dev[0] = '\0';

	mdt_media_device_changed(input, !input);
	return 0;
}

int mdt_audio_use_driver(const char *name)
{
	if (!str_isset(name) ||
	    !mdt_ausrc_find(mdt_ausrcl(), name) ||
	    !mdt_auplay_find(mdt_auplayl(), name))
		return ENOENT;

	str_ncpy(s_driver, name, sizeof(s_driver));
	s_dev_in[0] = s_dev_out[0] = '\0';
	MDT_INFO("Medut: audio device -> '%s'\n", name);
	mdt_media_device_changed(true, true);
	return 0;
}

int mdt_audio_use_external(bool enable)
{
	if (enable) {
		/* Selected even before it registers: the app owns the
		 * device from this call on. */
		str_ncpy(s_driver, "external", sizeof(s_driver));
		s_dev_in[0] = s_dev_out[0] = '\0';
		MDT_INFO("Medut: audio device -> 'external'\n");
		mdt_media_device_changed(true, true);
		return 0;
	}

	return mdt_audio_use_driver(s_platform);
}

bool mdt_audio_is_external(void)
{
	return !str_cmp(s_driver, "external");
}

/* Every registered driver's devices: the ones it enumerated, or the driver
 * itself, under its own name, when it enumerates none. */
static int list_driver_devices(const char *drv, const struct list *dev_list,
			       bool input, medut_audio_device_t *devices,
			       int n, int max_count)
{
	struct le *le;

	if (list_isempty(dev_list)) {
		if (n < max_count) {
			str_ncpy(devices[n].name, drv, sizeof(devices[n].name));
			devices[n].description[0] = '\0';
			devices[n].is_default = true;
			n++;
		}
		return n;
	}

	LIST_FOREACH(dev_list, le) {
		const struct mdt_mediadev *md = le->data;

		if (n >= max_count)
			break;
		str_ncpy(devices[n].name, md->name, sizeof(devices[n].name));
		devices[n].description[0] = '\0';
		devices[n].is_default = input ? md->src.is_default
					      : md->play.is_default;
		n++;
	}

	return n;
}

int mdt_audio_list_devices(bool input, medut_audio_device_t *devices,
			    int max_count)
{
	struct le *le;
	int n = 0;

	LIST_FOREACH(input ? mdt_ausrcl() : mdt_auplayl(), le) {
		const char *name;
		const struct list *dl;

		if (input) {
			const struct mdt_ausrc *as = le->data;
			name = as->name;
			dl = &as->dev_list;
		}
		else {
			const struct mdt_auplay *ap = le->data;
			name = ap->name;
			dl = &ap->dev_list;
		}

		n = list_driver_devices(name, dl, input, devices, n, max_count);
		if (n >= max_count)
			break;
	}

	return n;
}
