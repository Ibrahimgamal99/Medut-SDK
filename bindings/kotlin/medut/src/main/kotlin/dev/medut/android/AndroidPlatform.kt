package dev.medut.android

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.NoiseSuppressor
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import dev.medut.Call
import dev.medut.Medut
import dev.medut.PlatformHooks
import dev.medut.android.telecom.TelecomBridge
import java.util.concurrent.ConcurrentHashMap

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
    // Guarded by lock: the mode and focus are set for a fallback call (see
    // [useAutomaticAudio]), not by Telecom, so this class must undo them.
    private var fallbackAudio = false

    /** Calls Telecom never took (its addNewIncomingCall / placeCall threw):
     *  under AudioModePolicy.TELECOM nobody else would set the audio mode
     *  for them, so they get the AUTOMATIC handling. */
    private val fallbackCalls: MutableSet<Call> = ConcurrentHashMap.newKeySet()

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

    /** The SDK's own Telecom integration, when asked for (API 26+). */
    val telecom: TelecomBridge? =
        options.telecom?.takeIf { Build.VERSION.SDK_INT >= Build.VERSION_CODES.O }
            ?.let { TelecomBridge(context, it, this) }

    // The listener list is MedutAndroid's, so route listeners outlive a
    // shutdown() / start() cycle, which builds a new AudioRoutes.
    val routes = AudioRoutes(context, MedutAndroid.routeListeners).apply {
        // Report-only when something else owns Telecom; with the SDK's own
        // bridge, selection goes through the active connection instead.
        externalRouting = options.audioMode == AudioModePolicy.TELECOM && telecom == null
        telecomRouter = telecom
        onRouteSettled = { if (synchronized(lock) { appOwnedAudio }) engine.restartStreams() }
        start()
    }

    private val network: NetworkMonitor? =
        if (options.monitorNetwork) NetworkMonitor(context) { runCatching { Medut.network.changed() } }
        else null

    private val cellular = CellularMultipath(context)

    fun started(reattached: Boolean, iceMultipath: Boolean) {
        network?.start(reportInitialPath = reattached)
        if (iceMultipath) cellular.start()
        telecom?.start()
    }

    fun release() {
        telecom?.stop()
        cellular.stop()
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
        // A fallback call set the mode itself and still holds it.
        if (!active && fallbackAudio) return@synchronized
        audioActive = active
        if (appOwnedAudio) {
            if (active) engine.arm() else engine.disarm()
        }
    }

    // ── PlatformHooks ────────────────────────────────────────────────────

    // Synchronous on purpose: Android fixes a voice stream's routing when the
    // stream is created, and the SDK opens its streams as soon as media
    // starts.  A mode set that lands late gives a call the user cannot hear.
    override fun onCallStarting(call: Call?) {
        if (automaticFor(call)) activate()
    }

    /**
     * Telecom refused to carry [call] (no connection exists for it), so
     * under AudioModePolicy.TELECOM nobody would set MODE_IN_COMMUNICATION:
     * fall back to the AUTOMATIC handling for this call.  [activateNow] for
     * an outgoing call, whose media is about to start; an incoming one is
     * activated when it is answered.
     */
    fun useAutomaticAudio(call: Call, activateNow: Boolean) {
        if (options.audioMode != AudioModePolicy.TELECOM || call.isEnded) return
        fallbackCalls.add(call)
        if (activateNow) activate()
    }

    private fun automaticFor(call: Call?): Boolean =
        options.audioMode == AudioModePolicy.AUTOMATIC || (call != null && call in fallbackCalls)

    override fun onCallPlacing() {
        telecom?.checkOutgoingPermitted()
    }

    override fun onCallPlaced(call: Call) {
        telecom?.onCallPlaced(call)
    }

    override fun onCallLive(call: Call) {
        if (automaticFor(call)) main.post { activate() }
    }

    override fun onCallEnded(call: Call, noCallsLeft: Boolean) {
        if (options.audioMode != AudioModePolicy.AUTOMATIC) {
            if (fallbackCalls.remove(call) && fallbackCalls.isEmpty()) {
                main.post { if (fallbackCalls.isEmpty()) deactivateFallback() }
            }
            return
        }
        if (!noCallsLeft) return
        // Re-checked on the main thread: a new call may have started since.
        main.post { if (Medut.calls.isEmpty()) deactivate() }
    }

    override fun onShutdown() {
        fallbackCalls.clear()
        if (options.audioMode == AudioModePolicy.AUTOMATIC) main.post { deactivate() }
        else main.post { deactivateFallback() }
    }

    // ── Audio mode and focus ─────────────────────────────────────────────

    private fun activate() {
        synchronized(lock) {
            if (audioActive) return
            activateLocked()
            if (options.audioMode != AudioModePolicy.AUTOMATIC) fallbackAudio = true
        }
    }

    private fun deactivateFallback() {
        synchronized(lock) {
            if (!fallbackAudio) return
            fallbackAudio = false
            deactivateLocked()
        }
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
        logPlatformEffects()
    }

    // Field data, nothing acts on it yet.  The SDK reads back whether the
    // VOICE_COMMUNICATION preset took (sles_vc.c), which cannot tell a phone
    // with no hardware AEC from one with it; whether the phone exposes the
    // separate effects is a hint, logged as each call's audio comes up.  Not
    // proof either way: many vendors cancel inside the HAL under the preset
    // and report no effect.
    private fun logPlatformEffects() {
        val aec = runCatching { AcousticEchoCanceler.isAvailable() }.getOrDefault(false)
        val ns = runCatching { NoiseSuppressor.isAvailable() }.getOrDefault(false)
        Log.i(TAG, "platform effects: AcousticEchoCanceler.isAvailable=$aec " +
            "NoiseSuppressor.isAvailable=$ns " +
            "(${Build.MANUFACTURER} ${Build.MODEL}, API ${Build.VERSION.SDK_INT})")
    }

    private companion object {
        const val TAG = "Medut"
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
