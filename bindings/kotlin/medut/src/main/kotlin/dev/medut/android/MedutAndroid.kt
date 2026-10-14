package dev.medut.android

import android.content.Context
import android.os.Handler
import android.os.Looper
import dev.medut.Medut
import dev.medut.MedutConfig

/** Who owns the audio mode and audio focus around calls. */
enum class AudioModePolicy {
    /** The SDK sets MODE_IN_COMMUNICATION and takes voice-call focus when a
     *  call is placed or answered, and restores them when the last call ends.
     *  For apps without a ConnectionService. */
    AUTOMATIC,
    /** Telecom owns it: a self-managed ConnectionService (or Core-Telecom)
     *  sets the mode and routes audio.  The SDK touches neither, and route
     *  selection becomes report-only so the two owners never fight. */
    TELECOM,
    /** The app manages mode and focus itself. */
    MANUAL,
}

data class AndroidOptions(
    /** Drive network handover from a ConnectivityManager default-network
     *  callback.  Sets `netMonitorIntervalSeconds` to 0: the built-in poller
     *  is only a fallback for platforms without a connectivity callback. */
    val monitorNetwork: Boolean = true,
    val audioMode: AudioModePolicy = AudioModePolicy.AUTOMATIC,
    /** Deliver listener callbacks on the main thread by default. */
    val listenOnMainThread: Boolean = true,
)

/**
 * Android entry point.  Starts [Medut] with the app's cache dir as its state
 * directory and wires it to the platform: audio mode and focus, network
 * changes, audio routes.
 *
 * Call it from `Application.onCreate`, or from the FCM service when a push
 * wakes the process, so the stack is up without any UI (or Flutter engine)
 * involved.  Everything else goes through [Medut].
 */
object MedutAndroid {
    @Volatile private var platform: AndroidPlatform? = null

    /** Call-audio routes (earpiece, speaker, Bluetooth, wired).  Available
     *  after [start]. */
    @JvmStatic
    val audioRoutes: AudioRoutes
        get() = platform?.routes ?: throw IllegalStateException("MedutAndroid.start() first")

    @JvmStatic
    @JvmOverloads
    fun start(context: Context, config: MedutConfig = MedutConfig(),
              options: AndroidOptions = AndroidOptions()) {
        val app = context.applicationContext
        if (options.listenOnMainThread) {
            val main = Handler(Looper.getMainLooper())
            Medut.defaultExecutor = java.util.concurrent.Executor { main.post(it) }
        }
        val effective = config.copy(
            tmpDir = config.tmpDir ?: app.cacheDir.absolutePath,
            netMonitorIntervalSeconds =
                if (options.monitorNetwork) 0 else config.netMonitorIntervalSeconds,
        )

        val p = AndroidPlatform(app, options)
        Medut.platform = p
        try {
            Medut.start(effective)
        } catch (t: Throwable) {
            Medut.platform = null
            p.release()
            throw t
        }
        platform = p
        p.started(reattached = Medut.reattached)
    }

    /**
     * Take the microphone and speaker over from the SDK: an AudioRecord /
     * AudioTrack pair capturing through VOICE_COMMUNICATION feeds the stack
     * instead of its OpenSL driver.  The streams open only once the audio
     * mode is in force, which turns routing races (late Bluetooth SCO,
     * Telecom owning the route) into program order.
     */
    @JvmStatic
    fun useAppOwnedAudio(enable: Boolean) {
        val p = platform ?: throw IllegalStateException("MedutAndroid.start() first")
        Medut.audio.useExternalDevice(enable)
        p.setAppOwnedAudio(enable)
    }

    /** Diagnostics for the app-owned device: armed, running, format, frame
     *  counters, last error. */
    @JvmStatic
    val appOwnedAudioStatus: Map<String, Any?>
        get() = platform?.engine?.status() ?: emptyMap()

    /** Failures of the app-owned device, which the SDK cannot see.  Main
     *  thread. */
    @JvmStatic
    @Volatile
    var onAppOwnedAudioError: ((code: String, message: String) -> Unit)? = null

    /**
     * Under AudioModePolicy.TELECOM or MANUAL the SDK does not activate call
     * audio, so it cannot know when the app-owned device may open.  Report it:
     * true once the audio mode is in force (Telecom made the call active),
     * false when the call audio ends.
     */
    @JvmStatic
    fun setCallAudioActive(active: Boolean) {
        platform?.externalAudioActive(active)
    }

    /** Stop the stack and release the platform hooks. */
    @JvmStatic
    fun shutdown() {
        Medut.shutdown()
        platform?.release()
        platform = null
        Medut.platform = null
    }
}
