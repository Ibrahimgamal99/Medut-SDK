package dev.medut.android

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.concurrent.CopyOnWriteArrayList

/** A place call audio can go. */
data class AudioRoute(
    /** Stable id: "earpiece", "speaker", "wired-headset", "bluetooth" for the
     *  first Bluetooth device and "bt:<deviceId>" for any others. */
    val id: String,
    /** The device name; empty for the built-ins, so the app can localise them. */
    val name: String,
    val kind: Kind,
    val isActive: Boolean,
) {
    enum class Kind { EARPIECE, SPEAKER, BLUETOOTH, WIRED_HEADSET }
}

/**
 * Call-audio route listing and selection: [MedutAndroid.audioRoutes].
 * Ported from the Flutter plugin's AudioRouter.
 *
 * API 31+ uses the communication-device API, which moves the open voice stream
 * live and reports system-initiated moves.  API 24-30 uses the speakerphone
 * flag and startBluetoothSco(); SCO connects asynchronously (0.5-2 s), so
 * Bluetooth is never reported active until the platform says CONNECTED.
 * Bluetooth means HFP/SCO: A2DP cannot carry the microphone and is not offered.
 */
class AudioRoutes internal constructor(context: Context) {
    private val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val appContext = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    /** Report-only mode: routes are listed and changes reported, but
     *  selection does nothing.  On under AudioModePolicy.TELECOM, where
     *  Telecom owns the route and overrides anything set here. */
    @Volatile var externalRouting = false

    // Pre-31 SCO bookkeeping: `scoRequested` is our intent, `scoConnected`
    // the platform's confirmation.  Only the latter makes Bluetooth active.
    @Volatile private var scoRequested = false
    @Volatile private var scoConnected = false
    private var deviceCallback: AudioDeviceCallback? = null
    private var scoReceiver: BroadcastReceiver? = null
    private var commDeviceListener: Any? = null

    /** Pre-31 only: SCO finished connecting, or dropped.  API 24-30 fixes a
     *  stream's route when it is created, so streams opened in between are on
     *  the wrong device; their owner rebuilds them here. */
    internal var onRouteSettled: (() -> Unit)? = null

    /** Called on the main thread when routes or the active route change. */
    fun addListener(listener: () -> Unit) { listeners.add(listener) }
    fun removeListener(listener: () -> Unit) { listeners.remove(listener) }

    private fun notifyChanged() {
        main.post { listeners.forEach { it() } }
    }

    internal fun start() {
        val cb = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<AudioDeviceInfo>) = notifyChanged()
            override fun onAudioDevicesRemoved(removed: Array<AudioDeviceInfo>) = notifyChanged()
        }
        deviceCallback = cb
        am.registerAudioDeviceCallback(cb, main)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val listener = AudioManager.OnCommunicationDeviceChangedListener { notifyChanged() }
            try {
                am.addOnCommunicationDeviceChangedListener({ main.post(it) }, listener)
                commDeviceListener = listener
            } catch (_: Exception) {
            }
        } else {
            val receiver = object : BroadcastReceiver() {
                override fun onReceive(context: Context?, intent: Intent?) {
                    val state = intent?.getIntExtra(AudioManager.EXTRA_SCO_AUDIO_STATE,
                        AudioManager.SCO_AUDIO_STATE_DISCONNECTED) ?: return
                    val was = scoConnected
                    scoConnected = state == AudioManager.SCO_AUDIO_STATE_CONNECTED
                    if (state == AudioManager.SCO_AUDIO_STATE_ERROR) scoRequested = false
                    if (was != scoConnected) {
                        notifyChanged()
                        main.post { onRouteSettled?.invoke() }
                    }
                }
            }
            scoReceiver = receiver
            @Suppress("UnspecifiedRegisterReceiverFlag") // system broadcast, API < 31 only
            appContext.registerReceiver(receiver, IntentFilter(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED))
        }
    }

    internal fun stop() {
        deviceCallback?.let { am.unregisterAudioDeviceCallback(it) }
        deviceCallback = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            (commDeviceListener as? AudioManager.OnCommunicationDeviceChangedListener)?.let {
                runCatching { am.removeOnCommunicationDeviceChangedListener(it) }
            }
        }
        commDeviceListener = null
        scoReceiver?.let { runCatching { appContext.unregisterReceiver(it) } }
        scoReceiver = null
    }

    // ── Listing ──────────────────────────────────────────────────────────

    fun list(): List<AudioRoute> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) list31() else listLegacy()

    private fun kindOf(type: Int): AudioRoute.Kind? = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> AudioRoute.Kind.EARPIECE
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> AudioRoute.Kind.SPEAKER
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> AudioRoute.Kind.BLUETOOTH
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> AudioRoute.Kind.WIRED_HEADSET
        else -> when {
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
                type == AudioDeviceInfo.TYPE_HEARING_AID -> AudioRoute.Kind.BLUETOOTH
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                (type == AudioDeviceInfo.TYPE_BLE_HEADSET ||
                    type == AudioDeviceInfo.TYPE_BLE_SPEAKER) -> AudioRoute.Kind.BLUETOOTH
            else -> null // media-only outputs (HDMI, A2DP, ...) are not call routes
        }
    }

    private fun idOf(kind: AudioRoute.Kind, dev: AudioDeviceInfo, firstBluetooth: Boolean) = when (kind) {
        AudioRoute.Kind.EARPIECE -> "earpiece"
        AudioRoute.Kind.SPEAKER -> "speaker"
        AudioRoute.Kind.WIRED_HEADSET -> "wired-headset"
        AudioRoute.Kind.BLUETOOTH -> if (firstBluetooth) "bluetooth" else "bt:${dev.id}"
    }

    private fun nameOf(kind: AudioRoute.Kind, dev: AudioDeviceInfo) = when (kind) {
        AudioRoute.Kind.EARPIECE, AudioRoute.Kind.SPEAKER -> ""
        else -> dev.productName?.toString().orEmpty()
    }

    @Suppress("NewApi") // guarded by the caller
    private fun list31(): List<AudioRoute> {
        val activeId = am.communicationDevice?.id
        val routes = ArrayList<AudioRoute>()
        var sawBluetooth = false
        for (dev in am.availableCommunicationDevices) {
            val kind = kindOf(dev.type) ?: continue
            val first = kind != AudioRoute.Kind.BLUETOOTH || !sawBluetooth
            if (kind == AudioRoute.Kind.BLUETOOTH) sawBluetooth = true
            val id = idOf(kind, dev, first)
            if (routes.any { it.id == id }) continue
            routes.add(AudioRoute(id, nameOf(kind, dev), kind, dev.id == activeId))
        }
        return routes
    }

    @Suppress("DEPRECATION")
    private fun listLegacy(): List<AudioRoute> {
        var hasEarpiece = false
        var wired: AudioDeviceInfo? = null
        var bluetooth: AudioDeviceInfo? = null
        for (dev in am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            when (kindOf(dev.type)) {
                AudioRoute.Kind.EARPIECE -> hasEarpiece = true
                AudioRoute.Kind.WIRED_HEADSET -> wired = wired ?: dev
                AudioRoute.Kind.BLUETOOTH -> bluetooth = bluetooth ?: dev
                else -> {}
            }
        }
        // The platform's own precedence: the speakerphone flag beats
        // everything, a CONNECTED SCO link beats wired, a plugged wired
        // headset beats the earpiece.
        val active = when {
            am.isSpeakerphoneOn -> AudioRoute.Kind.SPEAKER
            scoConnected -> AudioRoute.Kind.BLUETOOTH
            wired != null -> AudioRoute.Kind.WIRED_HEADSET
            else -> AudioRoute.Kind.EARPIECE
        }
        val routes = ArrayList<AudioRoute>(4)
        if (hasEarpiece) {
            routes.add(AudioRoute("earpiece", "", AudioRoute.Kind.EARPIECE, active == AudioRoute.Kind.EARPIECE))
        }
        routes.add(AudioRoute("speaker", "", AudioRoute.Kind.SPEAKER, active == AudioRoute.Kind.SPEAKER))
        wired?.let {
            routes.add(AudioRoute("wired-headset", nameOf(AudioRoute.Kind.WIRED_HEADSET, it),
                AudioRoute.Kind.WIRED_HEADSET, active == AudioRoute.Kind.WIRED_HEADSET))
        }
        bluetooth?.let {
            routes.add(AudioRoute("bluetooth", nameOf(AudioRoute.Kind.BLUETOOTH, it),
                AudioRoute.Kind.BLUETOOTH, active == AudioRoute.Kind.BLUETOOTH))
        }
        return routes
    }

    // ── Selection ────────────────────────────────────────────────────────

    /** Route call audio to [id] (from [list]).  An unknown id selects the
     *  platform default.  Ignored while [externalRouting].  Returns the
     *  refreshed list: selection can fail, or complete asynchronously. */
    fun select(id: String): List<AudioRoute> {
        if (!externalRouting) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) select31(id) else selectLegacy(id)
        }
        return list()
    }

    /** Speaker on, or back to the best other route (Bluetooth, wired,
     *  earpiece) rather than blindly to the earpiece. */
    fun setSpeakerphone(on: Boolean) {
        if (externalRouting) return
        if (on) {
            select("speaker")
            return
        }
        val routes = list()
        val target = routes.firstOrNull { it.kind == AudioRoute.Kind.BLUETOOTH }
            ?: routes.firstOrNull { it.kind == AudioRoute.Kind.WIRED_HEADSET }
            ?: routes.firstOrNull { it.kind == AudioRoute.Kind.EARPIECE }
        select(target?.id ?: "earpiece")
    }

    /** Undo any selection, as the last call ends. */
    internal fun resetToDefault() {
        if (externalRouting) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            am.clearCommunicationDevice()
        } else {
            stopSco()
            @Suppress("DEPRECATION")
            am.isSpeakerphoneOn = false
        }
    }

    @Suppress("NewApi") // guarded by the caller
    private fun select31(id: String) {
        var sawBluetooth = false
        for (dev in am.availableCommunicationDevices) {
            val kind = kindOf(dev.type) ?: continue
            val first = kind != AudioRoute.Kind.BLUETOOTH || !sawBluetooth
            if (kind == AudioRoute.Kind.BLUETOOTH) sawBluetooth = true
            if (idOf(kind, dev, first) == id) {
                am.setCommunicationDevice(dev)
                return
            }
        }
        am.clearCommunicationDevice()
    }

    @Suppress("DEPRECATION")
    private fun selectLegacy(id: String) {
        when {
            id == "speaker" -> {
                stopSco()
                am.isSpeakerphoneOn = true
            }
            id == "bluetooth" || id.startsWith("bt:") -> {
                am.isSpeakerphoneOn = false
                if (!scoRequested) {
                    scoRequested = true
                    am.startBluetoothSco()
                    am.isBluetoothScoOn = true
                }
            }
            else -> { // earpiece / wired: a plugged headset wins on its own
                stopSco()
                am.isSpeakerphoneOn = false
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun stopSco() {
        if (scoRequested || scoConnected) {
            am.stopBluetoothSco()
            am.isBluetoothScoOn = false
            scoRequested = false
            scoConnected = false
        }
    }
}
