package dev.medut.flutter

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.NoiseSuppressor
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

/**
 * Platform shim for the Medut FFI plugin.
 *
 * The SIP/media work happens entirely in native code driven over Dart FFI;
 * this class only covers what FFI cannot reach:
 *  - the app cache dir (Medut's required tmp_dir on Android),
 *  - voice-call audio focus + MODE_IN_COMMUNICATION,
 *  - speakerphone routing,
 *  - ConnectivityManager default-network callbacks, forwarded to Dart so
 *    the SDK can re-register / migrate calls on network handover.
 */
class MedutPlugin : FlutterPlugin, MethodChannel.MethodCallHandler {
    private lateinit var channel: MethodChannel
    private lateinit var context: Context
    private val mainHandler = Handler(Looper.getMainLooper())

    private var audioManager: AudioManager? = null
    private var focusRequest: AudioFocusRequest? = null
    private var connectivityManager: ConnectivityManager? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null
    private var cellularCallback: ConnectivityManager.NetworkCallback? = null
    private var cellularIfname: String? = null

    // The network that is currently the default, as last reported by the
    // callback below. Identity, not capabilities — see onAvailable/onLost.
    // Volatile: the callbacks arrive on ConnectivityManager's thread, the
    // register/unregister path writes it from the main thread.
    @Volatile
    private var defaultNetwork: Network? = null
    private var audioRouter: AudioRouter? = null
    private var appAudio: AppOwnedAudioEngine? = null

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        context = binding.applicationContext
        channel = MethodChannel(binding.binaryMessenger, "medut")
        channel.setMethodCallHandler(this)
        val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager = am
        appAudio = AppOwnedAudioEngine(am) { code, message ->
            mainHandler.post {
                channel.invokeMethod(
                    "onAppOwnedAudioError",
                    mapOf("code" to code, "message" to message))
            }
        }
        audioRouter = AudioRouter(
            context,
            onRoutesChanged = {
                // Already posted to the main thread by AudioRouter.
                channel.invokeMethod("onAudioRoutesChanged", null)
            },
            // Pre-31 only: SCO has just connected or dropped, so any stream
            // created before it is stuck on the old device for its lifetime.
            // If the app owns the streams we can rebuild them; if the SDK owns
            // them there is nothing to do and this is a no-op.
            onRouteSettled = { appAudio?.restartStreams() },
        ).also { it.start() }
        registerNetworkCallback()
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        setCellularRequested(false)
        unregisterNetworkCallback()
        audioRouter?.stop()
        audioRouter = null
        // Streams die before the mode reverts to MODE_NORMAL — the reverse of
        // the startup order, for the same routing reason.
        appAudio?.disarm()
        appAudio = null
        abandonAudioFocus()
        channel.setMethodCallHandler(null)
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "getCacheDir" -> result.success(context.cacheDir.absolutePath)
            "setCellularRequested" -> {
                setCellularRequested(call.argument<Boolean>("on") ?: false)
                result.success(null)
            }
            "configureAudioSession" -> {
                val active = call.argument<Boolean>("active") ?: false
                if (active) {
                    requestAudioFocus()
                    logPlatformEffects()
                } else {
                    abandonAudioFocus()
                }
                result.success(null)
            }
            "setSpeakerphone" -> {
                val on = call.argument<Boolean>("on") ?: false
                val router = audioRouter
                if (router != null) {
                    // Route-aware: OFF returns to the best non-speaker route
                    // (BT > wired > earpiece), not blindly to the earpiece.
                    router.setSpeakerphone(on)
                } else {
                    audioManager?.isSpeakerphoneOn = on
                }
                result.success(null)
            }
            "listAudioRoutes" ->
                result.success(audioRouter?.listRoutes() ?: emptyList<Map<String, Any>>())
            "setAudioRoute" -> {
                val id = call.argument<String>("id")
                if (id == null) {
                    result.error("bad-args", "setAudioRoute needs an id", null)
                } else {
                    result.success(audioRouter?.selectRoute(id)
                        ?: emptyList<Map<String, Any>>())
                }
            }
            "setExternalRouting" -> {
                audioRouter?.externalRouting = call.argument<Boolean>("on") ?: false
                result.success(null)
            }
            "startAppOwnedAudio" -> {
                appAudio?.arm()
                result.success(null)
            }
            "stopAppOwnedAudio" -> {
                appAudio?.disarm()
                result.success(null)
            }
            "appOwnedAudioStatus" ->
                result.success(appAudio?.status() ?: emptyMap<String, Any?>())
            // iOS-only: CallKit activates and deactivates the AVAudioSession, and
            // the host forwards that here. Android has no such broker — audio focus
            // is requested by configureAudioSession above — so this is a genuine
            // no-op. It still needs a case: the Dart guard is `_isMobile`, which is
            // true for Android too, so falling through to notImplemented() below
            // raised MissingPluginException on a call three doc comments promise is
            // harmless (platform_channel.dart, medut.dart, docs/api/media.md).
            "notifyCallKitAudioActive" -> result.success(null)
            else -> result.notImplemented()
        }
    }

    // Field data, nothing acts on it yet. The SDK reads back whether the
    // VOICE_COMMUNICATION preset took (sles_vc.c), which cannot tell a phone
    // with no hardware AEC from one with it; whether the phone exposes the
    // separate effects is a hint, collected here before any behaviour leans on
    // it. Not proof either way: many vendors cancel inside the HAL under the
    // preset and report no effect. Logged as each call's audio comes up, just
    // ahead of the SDK's "TX chain" line for that call.
    private fun logPlatformEffects() {
        val aec = runCatching { AcousticEchoCanceler.isAvailable() }.getOrDefault(false)
        val ns = runCatching { NoiseSuppressor.isAvailable() }.getOrDefault(false)
        Log.i(TAG, "platform effects: AcousticEchoCanceler.isAvailable=$aec " +
                   "NoiseSuppressor.isAvailable=$ns " +
                   "(${Build.MANUFACTURER} ${Build.MODEL}, API ${Build.VERSION.SDK_INT})")
    }

    // ── Audio focus ─────────────────────────────────────────────────────
    //
    // Both of these must finish their work before the method-channel result is
    // sent: the Dart side awaits `configureAudioSession(true)` before it starts
    // a call, precisely so MODE_IN_COMMUNICATION is in force by the time the
    // native core opens the OpenSL streams. Android fixes a stream's routing
    // when it is created, so a mode set that lands late does not move the
    // audio — it produces a call the user cannot hear. Keep this synchronous.

    // When the SDK owns the device, a transient focus loss is survivable —
    // OpenSL keeps running and the far end hears the interruption at worst.
    // When the app owns it, nothing else would tell our capture and playback
    // threads to stand down, so the listener is not optional.
    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> appAudio?.pause()
            AudioManager.AUDIOFOCUS_GAIN -> appAudio?.resume()
        }
    }

    private fun requestAudioFocus() {
        val am = audioManager ?: return
        // Mode first, then focus: the mode is what routes the voice streams,
        // and it must be set even if the focus request is refused.
        am.mode = AudioManager.MODE_IN_COMMUNICATION
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (focusRequest == null) {
                focusRequest = AudioFocusRequest.Builder(
                    AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
                )
                    .setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                            .build()
                    )
                    .setOnAudioFocusChangeListener(focusListener, mainHandler)
                    .build()
            }
            focusRequest?.let { am.requestAudioFocus(it) }
        } else {
            @Suppress("DEPRECATION")
            am.requestAudioFocus(
                focusListener,
                AudioManager.STREAM_VOICE_CALL,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            )
        }
    }

    private fun abandonAudioFocus() {
        val am = audioManager ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let { am.abandonAudioFocusRequest(it) }
        } else {
            @Suppress("DEPRECATION")
            am.abandonAudioFocus(focusListener)
        }
        am.isSpeakerphoneOn = false
        am.mode = AudioManager.MODE_NORMAL
    }

    // ── Network change → Dart → medut_network_changed() ──────────────

    private fun registerNetworkCallback() {
        // Idempotent. Android's guidance is that a NetworkCallback is
        // registered at most once at a time, and overwriting the field would
        // leak the previous one: it stays registered and keeps delivering, so
        // every network change would arrive twice and cost two handovers
        // (transport flush + re-REGISTER + a re-INVITE per live call). Not
        // hypothetical for a push-woken app whose engine can be re-attached
        // without onDetachedFromEngine having run.
        unregisterNetworkCallback()

        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE)
                as ConnectivityManager
        connectivityManager = cm
        val cb = object : ConnectivityManager.NetworkCallback() {
            // Only the default-network *identity* matters here. onCapabilities-
            // Changed / onLinkPropertiesChanged fire constantly on a healthy
            // network (signal strength, validation, DNS) and are deliberately
            // not overridden: each one would be an unnecessary handover.
            //
            // One Wi-Fi<->cellular switch delivers both of the callbacks below
            // — for a default-network callback onLost() means "no longer the
            // default", not "gone" — and the ordering between them is not
            // guaranteed. Reporting both means two handovers for one switch,
            // and the second one lands while the first is still re-REGISTERing.
            // Observed on-device (2026-09-02, Android, mid-call over WSS): two
            // changeDetected 3.5 s apart, two transport resets, two re-INVITEs
            // of the same call. So key on the network that is default now.
            override fun onAvailable(network: Network) {
                if (defaultNetwork == network) return   // re-reported, not new
                defaultNetwork = network
                notifyDart()
            }

            override fun onLost(network: Network) {
                // A replacement already arrived: this is the tail of the switch
                // that onAvailable reported, not a second one. When it is the
                // current default that went away, there is no replacement yet
                // and the SDK does need to know — it has no address to bind.
                if (defaultNetwork != network) return
                defaultNetwork = null
                notifyDart()
            }
        }
        networkCallback = cb
        try {
            cm.registerDefaultNetworkCallback(cb)
        } catch (_: Exception) {
            // Missing ACCESS_NETWORK_STATE or too many callbacks — the SDK
            // still works, handover just relies on its own detection.
            networkCallback = null
        }
    }

    // ── Second path for ICE: keep cellular up next to Wi-Fi during a call ──
    //
    // Android tears the cellular data path down while Wi-Fi is the default,
    // so there is nothing to gather candidates on until the switch has
    // already happened. requestNetwork(TRANSPORT_CELLULAR) keeps the radio
    // attached for as long as the request is held; the SDK then binds its
    // extra sockets to that Network (android_setsocknetwork) through the
    // handle handed over here. Held only while a call is up — Dart drives it.
    private fun setCellularRequested(on: Boolean) {
        val cm = connectivityManager
            ?: (context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager)
                .also { connectivityManager = it }
        if (!on) {
            val cb = cellularCallback ?: return
            cellularCallback = null
            try { cm.unregisterNetworkCallback(cb) } catch (_: Exception) {}
            cellularIfname?.let { name ->
                cellularIfname = null
                mainHandler.post {
                    channel.invokeMethod("onInterfaceHandle",
                        mapOf("ifname" to name, "handle" to null))
                }
            }
            return
        }
        if (cellularCallback != null) return
        val req = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        val cb = object : ConnectivityManager.NetworkCallback() {
            private fun report(network: Network, lp: LinkProperties?) {
                val name = lp?.interfaceName ?: return
                cellularIfname = name
                val handle = network.networkHandle
                mainHandler.post {
                    channel.invokeMethod("onInterfaceHandle",
                        mapOf("ifname" to name, "handle" to handle))
                }
            }
            override fun onAvailable(network: Network) {
                report(network, cm.getLinkProperties(network))
            }
            override fun onLinkPropertiesChanged(network: Network, lp: LinkProperties) {
                report(network, lp)
            }
            override fun onLost(network: Network) {
                val name = cellularIfname ?: return
                cellularIfname = null
                mainHandler.post {
                    channel.invokeMethod("onInterfaceHandle",
                        mapOf("ifname" to name, "handle" to null))
                }
            }
        }
        cellularCallback = cb
        try {
            cm.requestNetwork(req, cb)
        } catch (e: Exception) {
            // No CHANGE_NETWORK_STATE / no modem: multipath stays off.
            Log.w(TAG, "requestNetwork(cellular) failed: $e")
            cellularCallback = null
        }
    }

    private fun unregisterNetworkCallback() {
        // A debounced notify must not outlive the callback that scheduled it:
        // onDetachedFromEngine clears the method-call handler right after this,
        // and invoking into a detached channel throws.
        mainHandler.removeCallbacks(notifyRunnable)
        defaultNetwork = null
        val cb = networkCallback ?: return
        try {
            connectivityManager?.unregisterNetworkCallback(cb)
        } catch (_: Exception) {
        }
        networkCallback = null
    }

    /**
     * Coalesce a burst of default-network callbacks into ONE handover.
     *
     * onAvailable's and onLost's own dedupes each assume they see the other
     * half of a switch first, and the ordering is not guaranteed (see the
     * comment on the callback above). When onLost(old) lands FIRST,
     * `defaultNetwork == network` still holds, so it notifies and clears the
     * field — and the onAvailable(new) that follows sees a different network
     * and notifies again. That is two handovers for one switch in half of all
     * orderings: exactly the doubled transport reset + doubled re-INVITE the
     * dedupes exist to prevent.
     *
     * Debouncing on the main thread makes the result ordering-independent: the
     * last state to settle within the window is the one the SDK is told about.
     * The window is short enough to stay imperceptible against the 1/2/4/8s
     * handover ladder in netmon.c, and it costs nothing when only one callback
     * arrives.
     */
    private fun notifyDart() {
        // MethodChannel calls must run on the platform (main) thread anyway, so
        // the debounce and the invoke share one handler and need no locking.
        mainHandler.removeCallbacks(notifyRunnable)
        mainHandler.postDelayed(notifyRunnable, NETWORK_COALESCE_MS)
    }

    private val notifyRunnable = Runnable {
        channel.invokeMethod("onNetworkChanged", null)
    }

    private companion object {
        /**
         * How long [notifyDart] waits for a default-network burst to settle.
         *
         * A Wi-Fi<->cellular switch delivers onLost + onAvailable within a few
         * hundred ms; 400 ms swallows the pair without adding a delay the SDK's
         * own handover ladder (1/2/4/8s, netmon.c) would notice.
         */
        const val NETWORK_COALESCE_MS = 400L

        const val TAG = "Medut"
    }
}
