package dev.medut.flutter

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

/**
 * Audio-output route enumeration + selection for calls.
 *
 * Routes are the four things a softphone user can point call audio at:
 * earpiece, loudspeaker, Bluetooth (HFP/SCO — A2DP alone cannot carry the
 * mic and is deliberately NOT offered), and wired/USB headset.
 *
 * Two implementations behind one surface:
 *  - API 31+: AudioManager communication-device API (the platform routes the
 *    open voice stream live, and reports system-initiated moves through
 *    addOnCommunicationDeviceChangedListener).
 *  - API 24-30: speakerphone flag + startBluetoothSco(). SCO connect is
 *    ASYNC (0.5-2s): a Bluetooth route is never reported active until
 *    ACTION_SCO_AUDIO_STATE_UPDATED says CONNECTED, so a picker built on this
 *    list cannot lie about where audio is.
 *
 * Route ids are stable, human-stable strings ('earpiece' | 'speaker' |
 * 'wired-headset' | 'bluetooth', plus 'bt:<deviceId>' only when several BT
 * devices are present) so host apps can persist / compare them across calls.
 *
 * [externalRouting]: when the host app integrates with a self-managed
 * ConnectionService (Android Telecom owns CallAudioState while its call is
 * active and overrides anything set here), it can flip this on — enumeration
 * and change events keep flowing, but selection becomes report-only, so the
 * two route owners never fight.
 */
internal class AudioRouter(
    context: Context,
    private val onRoutesChanged: () -> Unit,
    /**
     * Pre-31 only: SCO has finished connecting, or has dropped.
     *
     * This edge matters because API 24-30 fixes a stream's routing when the
     * stream is *created*, and startBluetoothSco() is async — so anything
     * opened between "select Bluetooth" and this callback is on the wrong
     * device for the rest of its life. Whoever owns the streams can rebuild
     * them here; when the SDK owns them there is nothing to do.
     */
    private val onRouteSettled: (() -> Unit)? = null,
) {
    private val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val appContext = context.applicationContext
    private val mainHandler = Handler(Looper.getMainLooper())

    /** Report-only mode: list + notify, never apply (Telecom owns the route). */
    var externalRouting = false

    // Pre-31 SCO bookkeeping. `requested` is our intent; `connected` is the
    // platform's confirmation — only the latter makes the BT route `active`.
    private var scoRequested = false
    private var scoConnected = false

    private var deviceCallback: AudioDeviceCallback? = null
    private var scoReceiver: BroadcastReceiver? = null
    private var commDeviceListener: AudioManager.OnCommunicationDeviceChangedListener? = null

    fun start() {
        val cb = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<AudioDeviceInfo>) {
                notifyChanged()
            }

            override fun onAudioDevicesRemoved(removed: Array<AudioDeviceInfo>) {
                notifyChanged()
            }
        }
        deviceCallback = cb
        am.registerAudioDeviceCallback(cb, mainHandler)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val listener = AudioManager.OnCommunicationDeviceChangedListener { notifyChanged() }
            commDeviceListener = listener
            try {
                am.addOnCommunicationDeviceChangedListener(
                    { r -> mainHandler.post(r) }, listener)
            } catch (_: Exception) {
                commDeviceListener = null
            }
        } else {
            val receiver = object : BroadcastReceiver() {
                override fun onReceive(context: Context?, intent: Intent?) {
                    val state = intent?.getIntExtra(
                        AudioManager.EXTRA_SCO_AUDIO_STATE,
                        AudioManager.SCO_AUDIO_STATE_DISCONNECTED) ?: return
                    val was = scoConnected
                    scoConnected = state == AudioManager.SCO_AUDIO_STATE_CONNECTED
                    if (state == AudioManager.SCO_AUDIO_STATE_ERROR) {
                        // Fall back honestly: stop pretending BT is coming.
                        scoRequested = false
                    }
                    if (was != scoConnected) {
                        notifyChanged()
                        mainHandler.post { onRouteSettled?.invoke() }
                    }
                }
            }
            scoReceiver = receiver
            appContext.registerReceiver(
                receiver, IntentFilter(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED))
        }
    }

    fun stop() {
        deviceCallback?.let { am.unregisterAudioDeviceCallback(it) }
        deviceCallback = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            commDeviceListener?.let {
                try {
                    am.removeOnCommunicationDeviceChangedListener(it)
                } catch (_: Exception) {
                }
            }
            commDeviceListener = null
        }
        scoReceiver?.let {
            try {
                appContext.unregisterReceiver(it)
            } catch (_: Exception) {
            }
        }
        scoReceiver = null
    }

    private fun notifyChanged() {
        mainHandler.post { onRoutesChanged() }
    }

    // ── Enumeration ─────────────────────────────────────────────────────

    fun listRoutes(): List<Map<String, Any>> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) listRoutes31()
        else listRoutesLegacy()

    private fun route(
        id: String, name: String, kind: String, active: Boolean,
    ): Map<String, Any> =
        mapOf("id" to id, "name" to name, "kind" to kind, "isActive" to active)

    private fun kindOf(type: Int): String? = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "earpiece"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "speaker"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "bluetooth"
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> "wired-headset"
        else -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
            type == AudioDeviceInfo.TYPE_HEARING_AID) "bluetooth"
        else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            (type == AudioDeviceInfo.TYPE_BLE_HEADSET ||
                type == AudioDeviceInfo.TYPE_BLE_SPEAKER)) "bluetooth"
        else null // media-only outputs (HDMI, A2DP, remote submix…) — not call routes
    }

    /** Stable route id. Semantic for the singletons; per-device for extra BT. */
    private fun idOf(kind: String, dev: AudioDeviceInfo, firstBt: Boolean): String =
        when (kind) {
            "earpiece" -> "earpiece"
            "speaker" -> "speaker"
            "wired-headset" -> "wired-headset"
            else -> if (firstBt) "bluetooth" else "bt:${dev.id}"
        }

    private fun labelOf(kind: String, dev: AudioDeviceInfo): String = when (kind) {
        // Built-ins get '' so the host app can localize ("Earpiece"/"Speaker").
        "earpiece", "speaker" -> ""
        else -> dev.productName?.toString() ?: ""
    }

    @Suppress("NewApi") // guarded by caller
    private fun listRoutes31(): List<Map<String, Any>> {
        val devices = am.availableCommunicationDevices
        val activeId = am.communicationDevice?.id
        val routes = ArrayList<Map<String, Any>>(devices.size)
        var sawBt = false
        for (dev in devices) {
            val kind = kindOf(dev.type) ?: continue
            val first = kind != "bluetooth" || !sawBt
            if (kind == "bluetooth") sawBt = true
            val id = idOf(kind, dev, first)
            if (routes.any { it["id"] == id }) continue
            routes.add(route(id, labelOf(kind, dev), kind, dev.id == activeId))
        }
        return routes
    }

    private fun listRoutesLegacy(): List<Map<String, Any>> {
        val devices = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        var hasEarpiece = false
        var wired: AudioDeviceInfo? = null
        var bt: AudioDeviceInfo? = null
        for (dev in devices) {
            when (kindOf(dev.type)) {
                "earpiece" -> hasEarpiece = true
                "wired-headset" -> wired = wired ?: dev
                "bluetooth" -> bt = bt ?: dev
            }
        }
        // The active route, in the platform's own precedence: speakerphone flag
        // beats everything; a CONNECTED SCO link beats wired; a plugged wired
        // headset otherwise owns the audio; else the earpiece.
        val active = when {
            am.isSpeakerphoneOn -> "speaker"
            scoConnected -> "bluetooth"
            wired != null -> "wired-headset"
            else -> "earpiece"
        }
        val routes = ArrayList<Map<String, Any>>(4)
        if (hasEarpiece) {
            routes.add(route("earpiece", "", "earpiece", active == "earpiece"))
        }
        routes.add(route("speaker", "", "speaker", active == "speaker"))
        wired?.let {
            routes.add(route(
                "wired-headset", labelOf("wired-headset", it), "wired-headset",
                active == "wired-headset"))
        }
        bt?.let {
            routes.add(route(
                "bluetooth", labelOf("bluetooth", it), "bluetooth",
                active == "bluetooth"))
        }
        return routes
    }

    // ── Selection ───────────────────────────────────────────────────────

    /** Apply [id]; returns the refreshed route list (the caller reports the
     *  route actually in force — selection can fail or complete async). */
    fun selectRoute(id: String): List<Map<String, Any>> {
        if (!externalRouting) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) select31(id)
            else selectLegacy(id)
        }
        return listRoutes()
    }

    fun setSpeakerphone(on: Boolean) {
        if (externalRouting) return
        if (on) {
            selectRoute("speaker")
        } else {
            // Off-target: the best non-speaker route (BT, wired, earpiece) —
            // forcing the earpiece with a headset connected is the classic
            // "speaker toggle strands the audio" bug.
            val routes = listRoutes()
            val target = routes.firstOrNull { it["kind"] == "bluetooth" }
                ?: routes.firstOrNull { it["kind"] == "wired-headset" }
                ?: routes.firstOrNull { it["kind"] == "earpiece" }
            selectRoute((target?.get("id") as? String) ?: "earpiece")
        }
    }

    @Suppress("NewApi") // guarded by caller
    private fun select31(id: String) {
        val devices = am.availableCommunicationDevices
        var sawBt = false
        for (dev in devices) {
            val kind = kindOf(dev.type) ?: continue
            val first = kind != "bluetooth" || !sawBt
            if (kind == "bluetooth") sawBt = true
            if (idOf(kind, dev, first) == id) {
                am.setCommunicationDevice(dev)
                return
            }
        }
        // Unknown id → platform default.
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
            else -> { // earpiece / wired-headset (wired wins automatically when plugged)
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
