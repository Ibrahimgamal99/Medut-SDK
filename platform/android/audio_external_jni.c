/**
 * @file audio_external_jni.c  JNI entry points for the app-owned audio device
 *
 * Lets Android code drive medut_audio_external_push()/pull() directly from a
 * realtime audio thread, without going through Dart.  That routing matters:
 * the loop has a 10–20 ms deadline per frame, and a managed-runtime GC pause
 * on the capture path is a dropped frame with nowhere to catch it.  Dart flips
 * the mode and reads the format; Kotlin moves the samples.
 *
 * The entry points live in libmedut.so rather than in a separate shim so
 * that consuming this needs no NDK toolchain in the host app — the Flutter
 * plugin ships a prebuilt .so and has no native build of its own.
 *
 * The Java class is deliberately `dev.medut.ExternalAudio`, not anything
 * under `dev.medut.flutter`: this is a plain Android binding, and a Flutter
 * package name compiled into the core would be wrong for every other consumer.
 *
 * PCM crosses as a direct ByteBuffer so there is no copy and no allocation per
 * frame; the caller owns the buffer and reuses it.
 */

#include <jni.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include "medut.h"

#define JNI_FN(name) JNICALL Java_dev_medut_ExternalAudio_##name

/* Byte length of a direct buffer, or -1 if it is not one.  A non-direct buffer
 * has no stable address to hand C, and silently treating that as an error the
 * caller can see beats reading whatever GetDirectBufferAddress returns. */
static jlong direct_capacity(JNIEnv *env, jobject buf)
{
	if (!buf)
		return -1;
	if (!(*env)->GetDirectBufferAddress(env, buf))
		return -1;
	return (*env)->GetDirectBufferCapacity(env, buf);
}

JNIEXPORT jint JNI_FN(nativeUseExternal)(JNIEnv *env, jclass cls,
                                         jboolean enable)
{
	(void)env; (void)cls;
	return medut_audio_use_external(enable == JNI_TRUE);
}

/**
 * Push captured microphone audio.  [buf] is a direct ByteBuffer of S16LE
 * samples, [nsamp] the number of samples (frames x channels) to take from its
 * start.  Returns 0, ENODEV between calls, or EINVAL.
 */
JNIEXPORT jint JNI_FN(nativePush)(JNIEnv *env, jclass cls, jobject buf,
                                  jint nsamp)
{
	(void)cls;

	jlong cap = direct_capacity(env, buf);
	if (cap < 0 || nsamp <= 0 || (jlong)nsamp * 2 > cap)
		return EINVAL;

	const int16_t *pcm = (*env)->GetDirectBufferAddress(env, buf);
	return medut_audio_external_push(pcm, (size_t)nsamp);
}

/**
 * Pull decoded audio to play into a direct ByteBuffer.  Always fills [nsamp]
 * samples — silence when no call is up — so the caller can hand the buffer
 * straight to an AudioTrack without checking the return.
 */
JNIEXPORT jint JNI_FN(nativePull)(JNIEnv *env, jclass cls, jobject buf,
                                  jint nsamp)
{
	(void)cls;

	jlong cap = direct_capacity(env, buf);
	if (cap < 0 || nsamp <= 0 || (jlong)nsamp * 2 > cap)
		return EINVAL;

	int16_t *pcm = (*env)->GetDirectBufferAddress(env, buf);
	return medut_audio_external_pull(pcm, (size_t)nsamp);
}

/**
 * Write {srate, channels, ptime_ms} into [out], which must hold 3 ints.  The
 * caller passes the same array every time, so this costs no allocation on the
 * polling path.  Returns 0, or ENODEV until the call has media.
 */
JNIEXPORT jint JNI_FN(nativeFormat)(JNIEnv *env, jclass cls, jintArray out)
{
	uint32_t srate = 0, ptime = 0;
	uint8_t  ch    = 0;
	jint     vals[3];
	int      err;

	(void)cls;

	if (!out || (*env)->GetArrayLength(env, out) < 3)
		return EINVAL;

	err = medut_audio_external_format(&srate, &ch, &ptime);
	if (err)
		return err;

	vals[0] = (jint)srate;
	vals[1] = (jint)ch;
	vals[2] = (jint)ptime;
	(*env)->SetIntArrayRegion(env, out, 0, 3, vals);

	return 0;
}

JNIEXPORT jboolean JNI_FN(nativeIsActive)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return medut_audio_external_is_active() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Whether the app's capture runs through the VOICE_COMMUNICATION source, i.e.
 * the platform's AEC/NS/AGC.  From the stream start and stop, never from the
 * capture loop: it logs.
 */
JNIEXPORT void JNI_FN(nativeSetVoicePath)(JNIEnv *env, jclass cls,
                                          jboolean active)
{
	(void)env; (void)cls;
	medut_audio_external_set_voice_path(active == JNI_TRUE);
}
