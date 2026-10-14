package dev.medut.internal

import dev.medut.MediaDirection
import dev.medut.Medut
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Entry point for the SDK's audio thread (medut_jni.c, on_media_tap): one
 * frame of PCM for a call that has a media tap installed.
 *
 * Runs on a real-time thread, so it allocates as little as it can and takes
 * no lock but the registry's.  It must not throw and must not block: the
 * listener is the app's, and the same rules apply there.
 */
internal object MediaTapSink {
    @JvmStatic
    fun onFrame(handle: Long, direction: Int, pcm: ByteBuffer, samples: Int,
                sampleRate: Int, channels: Int, timestampMicros: Long) {
        val call = Medut.registry.call(handle) ?: return
        val listener = call.mediaTap ?: return
        try {
            val view = pcm.order(ByteOrder.nativeOrder()).asShortBuffer()
            view.limit(samples)
            listener.onAudioFrame(call, MediaDirection.of(direction), view,
                samples, sampleRate, channels, timestampMicros)
        } catch (t: Throwable) {
            // The app's listener failed; the audio thread must go on.
            System.err.println("Medut: media tap listener threw: $t")
        }
    }
}
