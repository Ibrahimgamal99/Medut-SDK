/**
 * @file webrtc_apm.h  C interface to libwebrtc-audio-processing-1
 *
 * The one piece of Medut written in C++, kept as small as it can be: the
 * filter logic that drives it lives in audio_processing.c.  The library is
 * reached through webrtc_shim.c (dlopen), so every entry point here copes
 * with it being absent — create returns NULL and nothing else is called.
 *
 * All sample buffers are one 10 ms block of interleaved S16 at a native APM
 * rate (8, 16, 32 or 48 kHz).  Returns are 0 or the APM's negative error.
 *
 * Threading: capture and render may run on different threads at the same
 * time — AudioProcessingImpl serialises them internally (render and capture
 * locks).  configure() takes both, so it is safe from either side.
 */

#ifndef BSDK_WEBRTC_APM_H
#define BSDK_WEBRTC_APM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bsdk_apm;

/* webrtc_shim.c: loads the library (once) and reports whether it is there.
 * Called at init so the first call's setup never pays for the dlopen. */
bool bsdk_webrtc_lib_available(void);

struct bsdk_apm *bsdk_apm_create(void);
void bsdk_apm_destroy(struct bsdk_apm *apm);

/* Echo canceller and noise suppressor on or off.  The high-pass filter is
 * always on: AEC3 expects it, and it takes out the DC and rumble a laptop
 * microphone picks up. */
void bsdk_apm_configure(struct bsdk_apm *apm, bool aec, bool ns);

/* Near end (microphone), processed in place. */
int bsdk_apm_capture(struct bsdk_apm *apm, int16_t *sampv,
                     uint32_t srate, uint32_t ch);

/* Far end (what the speaker plays).  src is not modified; scratch receives
 * the APM's copy and must hold one block. */
int bsdk_apm_render(struct bsdk_apm *apm, const int16_t *src,
                    int16_t *scratch, uint32_t srate, uint32_t ch);

#ifdef __cplusplus
}
#endif

#endif /* BSDK_WEBRTC_APM_H */
