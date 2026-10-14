package dev.medut

import java.nio.ShortBuffer

/**
 * Receives a call's PCM audio: what the SDK decoded from the far end
 * ([MediaDirection.RX]) and what it is about to encode from the microphone
 * ([MediaDirection.TX]).  Install one with [Call.setMediaTap].
 *
 * Called on the SDK's audio thread for every frame, so [onAudioFrame] must
 * return quickly and never block: hand the samples to your own thread.
 * [pcm] is a view over the SDK's buffer and is valid only until the method
 * returns; copy what you keep.
 */
fun interface MediaTapListener {
    /**
     * @param pcm           Signed 16-bit samples, interleaved when [channels]
     *                      is 2; `remaining()` is [samples].
     * @param sampleRate    Hz.
     * @param channels      1 or 2.
     * @param timestampMicros  Capture / playout time in microseconds.
     */
    fun onAudioFrame(call: Call, direction: MediaDirection, pcm: ShortBuffer,
                     samples: Int, sampleRate: Int, channels: Int, timestampMicros: Long)
}
