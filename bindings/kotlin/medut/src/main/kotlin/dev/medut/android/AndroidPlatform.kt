package dev.medut.android

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import dev.medut.Call
import dev.medut.Medut
import dev.medut.PlatformHooks

/**
 * Audio mode and focus around calls, network monitoring and audio routes.
 * Ported from the Flutter plugin (bindings/flutter/android).
 */
internal class AndroidPlatform(
    context: Context,
    private val options: AndroidOptions,
) : PlatformHooks {
    private val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val main = Handler(Looper.getMainLooper())
    private val lock = Any()
    // Guarded by lock.
    private var focusRequest: AudioFocusRequest? = null
    private var audioActive = false
    // Guarded by lock.
    private var appOwnedAudio = false

    /** The app-owned audio device: idle until [setAppOwnedAudio]. */
    val engine = AppOwnedAudioEngine(am) { code, message ->
        main.post { MedutAndroid.onAppOwnedAudioError?.invoke(code, message) }
    }

    // When the app owns the device, nothing else tells its threads to stand
    // down on a focus loss (Siri, a cellular call).  Pre-O focus is keyed on
    // the listener object, so request and abandon pass this same one.
    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> engine.pause()
            AudioManager.AUDIOFOCUS_GAIN -> engine.resume()
        }
    }

    val routes = AudioRoutes(context).apply {
        externalRouting = options.audioMode == AudioModePolicy.TELECOM
        onRouteSettled = { if (synchronized(lock) { appOwnedAudio }) engine.restartStreams() }
        start()
    }

    private val network: NetworkMonitor? =
        if (options.monitorNetwork) NetworkMonitor(context) { runCatching { Medut.network.changed() } }
        else null

    fun started(reattached: Boolean) {
        network?.start(reportInitialPath = reattached)
    }

    fun release() {
        network?.stop()
        engine.disarm()
        routes.stop()
    }

    /** Arm or disarm the app-owned device.  Its streams only run while the
     *  audio mode is in force: armed here if a call already has it, otherwise
     *  when the next call activates audio. */
    fun setAppOwnedAudio(enable: Boolean) = synchronized(lock) {
        appOwnedAudio = enable
        if (audioActive) {
            if (enable) engine.arm() else engine.disarm()
        }
    }

    /** Telecom or the app activated / deactivated call audio itself
     *  (AudioModePolicy.TELECOM or MANUAL). */
    fun externalAudioActive(active: Boolean) = synchronized(lock) {
        audioActive = active
        if (appOwnedAudio) {
            if (active) engine.arm() else engine.disarm()
        }
    }

    // ── PlatformHooks ────────────────────────────────────────────────────

    // Synchronous on purpose: Android fixes a voice stream's routing when the
    // stream is created, and the SDK opens its streams as soon as media
    // starts.  A mode set that lands late gives a call the user cannot hear.
    override fun onCallStarting() {
        if (options.audioMode == AudioModePolicy.AUTOMATIC) activate()
    }

    override fun onCallLive(call: Call) {
        if (options.audioMode == AudioModePolicy.AUTOMATIC) main.post { activate() }
    }

    override fun onCallEnded(call: Call, noCallsLeft: Boolean) {
        if (options.audioMode != AudioModePolicy.AUTOMATIC || !noCallsLeft) return
        // Re-checked on the main thread: a new call may have started since.
        main.post { if (Medut.calls.isEmpty()) deactivate() }
    }

    override fun onShutdown() {
        if (options.audioMode == AudioModePolicy.AUTOMATIC) main.post { deactivate() }
    }

    // ── Audio mode and focus ─────────────────────────────────────────────

    private fun activate() {
        synchronized(lock) { activateLocked() }
    }

    private fun activateLocked() {
        if (audioActive) return
        audioActive = true
        // Mode first, then focus: the mode routes the voice streams and must
        // be set even if the focus request is refused.
        am.mode = AudioManager.MODE_IN_COMMUNICATION
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val req = focusRequest ?: AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build())
                .setOnAudioFocusChangeListener(focusListener, main)
                .build()
                .also { focusRequest = it }
            am.requestAudioFocus(req)
        } else {
            @Suppress("DEPRECATION")
            am.requestAudioFocus(focusListener, AudioManager.STREAM_VOICE_CALL,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
        }
        // The app's streams open strictly after the mode is in force, which
        // is most of why an app takes the device over.
        if (appOwnedAudio) engine.arm()
    }

    private fun deactivate() {
        synchronized(lock) { deactivateLocked() }
    }

    private fun deactivateLocked() {
        if (!audioActive) return
        audioActive = false
        // Streams die before the mode reverts: the reverse of start-up, for
        // the same routing reason.
        if (appOwnedAudio) engine.disarm()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let { am.abandonAudioFocusRequest(it) }
        } else {
            @Suppress("DEPRECATION")
            am.abandonAudioFocus(focusListener)
        }
        routes.resetToDefault()
        am.mode = AudioManager.MODE_NORMAL
    }
}
