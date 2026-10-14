package dev.medut

import dev.medut.internal.Native
import java.nio.ByteBuffer

/**
 * Stack-wide audio controls: [Medut.audio].
 *
 * The processing toggles reach calls that are already up, from their next
 * frame.  On Android the VOICE_COMMUNICATION capture path already cancels echo
 * and runs NS/AGC, so the SDK keeps its own NS/AGC off there (see medut.h).
 */
class MedutAudio internal constructor() {
    private val gate get() = Medut.gate

    /** Turn echo control off, or back to the mode chosen at start. */
    fun setEchoCancellation(enabled: Boolean) = gate.read(null, "medut_set_aec") { Native.setAec(enabled) }

    /** Only OFF <-> the start-time mode is valid at runtime. */
    fun setAecMode(mode: AecMode) = gate.read(null, "medut_set_aec_mode") {
        check(Native.setAecMode(mode.value), "medut_set_aec_mode")
    }

    /** Built-in suppressor strength, 0..1. */
    fun setAecSuppressionLevel(level: Float) =
        gate.read(null, "medut_set_aec_suppression_level") { Native.setAecSuppressionLevel(level) }

    fun setNoiseSuppression(enabled: Boolean) = gate.read(null, "medut_set_ns") { Native.setNs(enabled) }

    fun setAutoGainControl(enabled: Boolean) = gate.read(null, "medut_set_agc") { Native.setAgc(enabled) }

    /** Manual microphone gain in dB, clamped to -20..+20. */
    fun setMicGainDb(db: Float) = gate.read(null, "medut_set_mic_gain_db") { Native.setMicGainDb(db) }

    /** Manual speaker gain in dB, clamped to -20..+20. */
    fun setSpeakerGainDb(db: Float) =
        gate.read(null, "medut_set_speaker_gain_db") { Native.setSpeakerGainDb(db) }

    /** Jitter buffer bounds for calls set up after this; 0 restores a default. */
    fun setJitterBuffer(minMs: Int, maxMs: Int) =
        gate.read(null, "medut_set_jitter_buffer") { Native.setJitterBuffer(minMs, maxMs) }

    fun setJitterBufferType(type: JitterBufferType) =
        gate.read(null, "medut_set_jitter_buffer_type") { Native.setJitterBufferType(type.value) }

    /** Link-adaptive bit rate.  0 for a bound keeps the configured value. */
    @JvmOverloads
    fun setAdaptiveBitrate(enabled: Boolean, minBps: Int = 0, maxBps: Int = 0) =
        gate.read(null, "medut_set_adaptive_bitrate") { Native.setAdaptiveBitrate(enabled, minBps, maxBps) }

    fun inputDevices(): List<AudioDevice> =
        gate.read(null, "medut_audio_list_input_devices") { devices(Native.audioListInputDevices()) }

    fun outputDevices(): List<AudioDevice> =
        gate.read(null, "medut_audio_list_output_devices") { devices(Native.audioListOutputDevices()) }

    /** null selects the platform default.  On Android prefer
     *  `MedutAndroid.audioRoutes`, which goes through AudioManager. */
    fun setInputDevice(name: String?) = gate.read(null, "medut_audio_set_input_device") {
        check(Native.audioSetInputDevice(name), "medut_audio_set_input_device")
    }

    fun setOutputDevice(name: String?) = gate.read(null, "medut_audio_set_output_device") {
        check(Native.audioSetOutputDevice(name), "medut_audio_set_output_device")
    }

    private fun devices(flat: Array<String?>): List<AudioDevice> =
        flat.toList().chunked(3).map { AudioDevice(it[0].orEmpty(), it[1].orEmpty(), it[2] == "1") }

    // ── App-owned audio device ───────────────────────────────────────────
    //
    // With useExternalDevice(true) the SDK opens no OpenSL streams; the app
    // captures and plays PCM itself (AudioRecord / AudioTrack) and moves it
    // with push/pull.  The app then owns echo cancellation too: capture
    // through VOICE_COMMUNICATION and say so with setExternalVoicePath(true).
    // Buffers must be direct ByteBuffers holding S16 native-order samples.
    // push, pull, format and isActive are safe from real-time audio threads.

    fun useExternalDevice(enabled: Boolean) = gate.read(null, "medut_audio_use_external") {
        check(Native.audioUseExternal(enabled), "medut_audio_use_external")
    }

    /** Give the stack [samples] captured samples from [buffer].  Returns false
     *  when no call is capturing. */
    fun pushExternal(buffer: ByteBuffer, samples: Int): Boolean =
        Native.audioExternalPush(buffer, samples) == 0

    /** Fill [buffer] with [samples] samples to play; always fully written,
     *  silence when no call is playing (and then returns false). */
    fun pullExternal(buffer: ByteBuffer, samples: Int): Boolean =
        Native.audioExternalPull(buffer, samples) == 0

    /** The current call's negotiated format, or null before media is up.
     *  Poll it: a re-INVITE can change the codec with no state change. */
    val externalFormat: ExternalAudioFormat?
        get() {
            val out = IntArray(3)
            if (Native.audioExternalFormat(out) != 0) return null
            return ExternalAudioFormat(out[0], out[1], out[2])
        }

    val isExternalActive: Boolean get() = Native.audioExternalIsActive()

    /** Report that the app's device captures through VOICE_COMMUNICATION, so
     *  the SDK keeps its own NS/AGC off.  Call from stream start and stop,
     *  never from the audio callback. */
    fun setExternalVoicePath(active: Boolean) = Native.audioExternalSetVoicePath(active)
}
