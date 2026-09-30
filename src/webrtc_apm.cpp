/**
 * @file webrtc_apm.cpp  C wrapper over webrtc::AudioProcessing (see .h)
 *
 * Built with -fno-exceptions -fno-rtti -fno-threadsafe-statics and without
 * any std:: container, so the object references nothing but the three
 * AudioProcessingBuilder symbols webrtc_shim.c provides — everything else
 * called here is virtual or inline.  CMake checks that with nm.
 *
 * Two traps that shape this file:
 *  - AudioProcessing::Config must never be default-constructed here: its
 *    inline constructor calls the out-of-line Pipeline::Pipeline(), which
 *    the shim does not provide.  GetConfig() hands back a copy instead.
 *  - Do not include re or echosdk headers: RE_ATOMIC picks its C11 branch
 *    from __STDC_VERSION__, which C++ does not define.
 */

#include <modules/audio_processing/include/audio_processing.h>
#include "webrtc_apm.h"

using webrtc::AudioProcessing;
using webrtc::AudioProcessingBuilder;
using webrtc::StreamConfig;

static inline AudioProcessing *apm_of(struct bsdk_apm *apm)
{
	return reinterpret_cast<AudioProcessing *>(apm);
}

extern "C" struct bsdk_apm *bsdk_apm_create(void)
{
	AudioProcessing *p = AudioProcessingBuilder().Create();
	return reinterpret_cast<struct bsdk_apm *>(p);
}

extern "C" void bsdk_apm_destroy(struct bsdk_apm *apm)
{
	delete apm_of(apm);   /* virtual: the library's own deleting dtor */
}

extern "C" void bsdk_apm_configure(struct bsdk_apm *apm, bool aec, bool ns)
{
	AudioProcessing *p = apm_of(apm);
	if (!p)
		return;

	AudioProcessing::Config c = p->GetConfig();
	c.echo_canceller.enabled    = aec;
	c.echo_canceller.mobile_mode = false;
	c.high_pass_filter.enabled  = true;
	c.noise_suppression.enabled = ns;
	c.noise_suppression.level   = AudioProcessing::Config::NoiseSuppression::kHigh;
	p->ApplyConfig(c);
}

extern "C" int bsdk_apm_capture(struct bsdk_apm *apm, int16_t *sampv,
                                uint32_t srate, uint32_t ch)
{
	AudioProcessing *p = apm_of(apm);
	if (!p)
		return -1;

	StreamConfig sc((int)srate, ch);

	/* AEC3 estimates the echo delay itself (it converges out to ~400 ms);
	 * the reported value only has to be set, as PipeWire does. */
	p->set_stream_delay_ms(0);
	return p->ProcessStream(sampv, sc, sc, sampv);
}

extern "C" int bsdk_apm_render(struct bsdk_apm *apm, const int16_t *src,
                               int16_t *scratch, uint32_t srate, uint32_t ch)
{
	AudioProcessing *p = apm_of(apm);
	if (!p)
		return -1;

	StreamConfig sc((int)srate, ch);
	return p->ProcessReverseStream(src, sc, sc, scratch);
}
