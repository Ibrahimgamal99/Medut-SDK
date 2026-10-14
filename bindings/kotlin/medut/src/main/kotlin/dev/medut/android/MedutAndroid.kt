package dev.medut.android

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import dev.medut.Medut
import dev.medut.MedutConfig
import dev.medut.MedutEvent
import dev.medut.MedutSubscription
import dev.medut.android.telecom.MedutTelecom
import dev.medut.android.telecom.TelecomOptions

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
    /** Register every call with Telecom through the SDK's own self-managed
     *  ConnectionService ([dev.medut.android.telecom.MedutTelecom]).  Forces
     *  [audioMode] to TELECOM.  Needs API 26; ignored below. */
    val telecom: TelecomOptions? = null,
    /** On a fresh start, re-create and register every account in
     *  [MedutAndroid.accountStore], so a process started by a push is
     *  registered before any UI exists. */
    val restoreAccounts: Boolean = true,
    /** Write the stack's log events to logcat (tag "Medut"), so a process
     *  running with no UI attached still leaves a trace. */
    val logToLogcat: Boolean = true,
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
    private const val TAG = "MedutAndroid"

    private const val LOG_TAG = "Medut"

    @Volatile private var platform: AndroidPlatform? = null
    @Volatile private var accounts: AccountStore? = null
    @Volatile private var logSubscription: MedutSubscription? = null

    /** Call-audio routes (earpiece, speaker, Bluetooth, wired).  Available
     *  after [start]. */
    @JvmStatic
    val audioRoutes: AudioRoutes
        get() = platform?.routes ?: throw IllegalStateException("MedutAndroid.start() first")

    /** Accounts kept across process restarts.  Available after [start]; an
     *  app that needs it earlier can construct its own [AccountStore]. */
    @JvmStatic
    val accountStore: AccountStore
        get() = accounts ?: throw IllegalStateException("MedutAndroid.start() first")

    @JvmStatic
    @JvmOverloads
    fun start(context: Context, config: MedutConfig = MedutConfig(),
              options: AndroidOptions = AndroidOptions()) {
        val app = context.applicationContext
        if (options.listenOnMainThread) {
            val main = Handler(Looper.getMainLooper())
            Medut.defaultExecutor = java.util.concurrent.Executor { main.post(it) }
        }
        val tmpDir = config.tmpDir ?: app.cacheDir.absolutePath
        val effective = config.copy(
            tmpDir = tmpDir,
            netMonitorIntervalSeconds =
                if (options.monitorNetwork) 0 else config.netMonitorIntervalSeconds,
            // A relative capture path lands in the state directory, where the
            // app can write without any permission.
            pcapPath = config.pcapPath?.let { if (it.startsWith("/")) it else "$tmpDir/$it" },
        )
        val effectiveOptions = when {
            options.telecom == null -> options
            MedutTelecom.isSupported -> options.copy(audioMode = AudioModePolicy.TELECOM)
            else -> {
                Log.w(TAG, "Telecom integration needs API 26; using ${options.audioMode}")
                options.copy(telecom = null)
            }
        }

        val p = AndroidPlatform(app, effectiveOptions)
        Medut.platform = p
        if (options.logToLogcat && logSubscription == null) {
            logSubscription = Medut.addListener({ it.run() }) { event ->
                when (event) {
                    is MedutEvent.Log -> Log.i(LOG_TAG, event.message.trimEnd())
                    is MedutEvent.Registration ->
                        Log.i(LOG_TAG, "registration ${event.account?.aor}: ${event.state}" +
                            (event.errorText?.let { " ($it)" } ?: ""))
                    is MedutEvent.IncomingCall -> Log.i(LOG_TAG, "incoming call from ${event.fromUri}")
                    is MedutEvent.CallStateChanged ->
                        Log.i(LOG_TAG, "call ${event.call.remoteUri}: ${event.state}" +
                            (event.reason?.let { " ($it)" } ?: ""))
                    else -> {}
                }
            }
        }
        try {
            Medut.start(effective)
        } catch (t: Throwable) {
            Medut.platform = null
            p.release()
            throw t
        }
        platform = p
        p.started(reattached = Medut.reattached, iceMultipath = effective.iceMultipath)

        val store = accounts ?: AccountStore(app).also { accounts = it }
        if (options.restoreAccounts && !Medut.reattached) store.restore()
    }

    /** True when the SDK's Telecom integration is on for this stack. */
    @JvmStatic
    val isTelecomEnabled: Boolean get() = platform?.telecom != null

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

    /**
     * Register the accounts after a reboot ([MedutBootReceiver]), instead of
     * waiting for the first push or launch.  The choice is kept by the
     * system across reboots and updates; the app must start the stack from
     * `Application.onCreate` for it to do anything.
     */
    @JvmStatic
    fun setStartOnBoot(context: Context, enabled: Boolean) {
        val app = context.applicationContext
        app.packageManager.setComponentEnabledSetting(
            ComponentName(app, MedutBootReceiver::class.java),
            if (enabled) PackageManager.COMPONENT_ENABLED_STATE_ENABLED
            else PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
            PackageManager.DONT_KILL_APP)
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
