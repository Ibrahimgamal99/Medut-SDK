package dev.medut.android.telecom

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.telecom.CallAudioState
import android.telecom.Connection
import android.telecom.DisconnectCause
import android.telecom.PhoneAccount
import android.telecom.PhoneAccountHandle
import android.telecom.TelecomManager
import android.util.Log
import androidx.annotation.RequiresApi
import dev.medut.Call
import dev.medut.CallState
import dev.medut.Medut
import dev.medut.MedutError
import dev.medut.MedutEvent
import dev.medut.MedutException
import dev.medut.MedutSubscription
import dev.medut.android.AndroidPlatform
import dev.medut.android.AudioRoute
import dev.medut.android.AudioRoutes
import dev.medut.android.MedutPush
import dev.medut.android.PushPayload
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executor

/** Settings for the SDK's self-managed ConnectionService ([MedutTelecom]). */
data class TelecomOptions(
    /** The PhoneAccount label the system shows; null uses the app name. */
    val label: String? = null,
    /** PhoneAccount id, unique within the app. */
    val phoneAccountId: String = "medut",
    /** Show the built-in incoming-call notification (ringtone, full-screen
     *  intent to the app's launch activity, Answer / Decline actions) when
     *  Telecom asks for the incoming-call UI.  Off when the app shows its
     *  own from [MedutTelecom.onShowIncomingCallUi]. */
    val incomingCallNotification: Boolean = true,
    val notificationChannelId: String = "medut_calls",
    /** The notification channel's name, as the user sees it in settings. */
    val notificationChannelName: String = "Calls",
    /** The activity the built-in notification opens (tap, full-screen
     *  intent, and after Answer), with [MedutTelecom.EXTRA_CALL_ID]; null
     *  opens the app's launch activity. */
    val callActivity: Class<out Activity>? = null,
)

/**
 * Telecom integration: every SIP call is also a self-managed Telecom
 * connection, so the system knows about it.  Calls appear in the system call
 * UI (and on Bluetooth, Android Auto, Wear), a cellular call and a SIP call
 * never fight over audio, Telecom owns the audio mode and the route, and an
 * incoming call reaches the user with no app UI up.
 *
 * Turn it on with `AndroidOptions(telecom = TelecomOptions())` in
 * [dev.medut.android.MedutAndroid.start].  Needs API 26; below that the
 * option is ignored.  Nothing else changes: `account.call()` places a call
 * and `call.answer()` answers one, and the SDK tells Telecom.
 *
 * Call audio under Telecom: [dev.medut.android.AudioModePolicy.TELECOM] is
 * set for you, and `MedutAndroid.audioRoutes` selects routes through the
 * active connection.
 */
object MedutTelecom {
    private const val TAG = "MedutTelecom"

    /** Self-managed connection services need API 26. */
    @JvmStatic
    val isSupported: Boolean get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O

    @JvmStatic
    val isEnabled: Boolean get() = bridge != null

    /**
     * Telecom wants the incoming-call UI shown (a self-managed service draws
     * its own).  Main thread.  Null shows the built-in notification when
     * [TelecomOptions.incomingCallNotification] is on; set it to start your
     * own activity instead.
     */
    @JvmStatic
    @Volatile
    var onShowIncomingCallUi: ((Call) -> Unit)? = null

    /**
     * The same, for a call a push announced ([dev.medut.android.MedutPush.wake])
     * that is ringing before its INVITE has arrived: there is no [Call] yet,
     * only what the push said, and the ring's key.  Answer or decline it
     * with [answerPushRing] / [declinePushRing]; [callFor] resolves the key
     * to the call once the INVITE is in.  Main thread.  Null shows the
     * built-in notification when it is on.  When the INVITE arrives,
     * [onShowIncomingCallUi] is called with the call.
     */
    @JvmStatic
    @Volatile
    var onShowPushIncomingCallUi: ((key: String, payload: PushPayload) -> Unit)? = null

    /**
     * Answer the push ring with this key, as the system's Answer would: at
     * once if its INVITE is in, otherwise as soon as it arrives.  Returns
     * false when no such ring is up.  Any thread.
     */
    @JvmStatic
    fun answerPushRing(key: String): Boolean = onRing(key) { it.onAnswer() }

    /**
     * Decline the push ring with this key: the ring ends, and its INVITE is
     * rejected with 603, now or when it arrives.  Returns false when no such
     * ring is up.  Any thread.
     */
    @JvmStatic
    fun declinePushRing(key: String): Boolean = onRing(key) { it.onReject() }

    private fun onRing(key: String, action: (MedutConnection) -> Unit): Boolean {
        val connection = bridge?.ringConnection(key) ?: return false
        if (Looper.myLooper() == Looper.getMainLooper()) action(connection)
        else Handler(Looper.getMainLooper()).post { action(connection) }
        return true
    }

    /**
     * Decide on an incoming call before Telecom hears of it: return a SIP
     * status to reject it with (486 for "two calls already up", 603 for a
     * blocked number or Do Not Disturb), or null to ring.  A refused call
     * never reaches the system, so nothing flashes; a push ring it would have
     * taken over ends quietly (rejected).  Main thread, before the call is
     * handed to Telecom; other listeners still get the IncomingCall event.
     */
    @JvmStatic
    @Volatile
    var incomingCallFilter: ((Call) -> Int?)? = null

    /**
     * Telecom changed a call's audio: the route moved (a Bluetooth headset
     * connected, the system call UI picked the speaker) or Telecom muted it
     * (a headset button, a watch).  [muted] is the call's mute state after
     * the change, [route] the active route.  Main thread; for redrawing a
     * call screen.
     */
    @JvmStatic
    @Volatile
    var onCallAudioStateChanged: ((call: Call, muted: Boolean, route: AudioRoute?) -> Unit)? = null

    /**
     * Telecom refused to create the connection for an outgoing call (another
     * call has the line).  The SIP call has been hung up by the time this
     * runs.  Main thread.
     */
    @JvmStatic
    @Volatile
    var onOutgoingCallRefused: ((Call) -> Unit)? = null

    @Volatile
    internal var bridge: TelecomBridge? = null

    /** The action of the intent that opens [TelecomOptions.callActivity]
     *  after the built-in notification's Answer (already carried out). */
    const val ACTION_ANSWER = "dev.medut.action.ANSWER"
    const val ACTION_DECLINE = "dev.medut.action.DECLINE"
    /** On the intents the built-in notification sends: the call's key, for
     *  [callFor] (a push ring's key resolves once its INVITE is in). */
    const val EXTRA_CALL_ID = "dev.medut.extra.CALL_ID"

    /**
     * The call a notification action or launch intent refers to.  For a
     * call a push announced, null until its INVITE has arrived.
     */
    @JvmStatic
    fun callFor(intent: Intent?): Call? {
        val id = intent?.getStringExtra(EXTRA_CALL_ID) ?: return null
        return Medut.calls.firstOrNull { it.id.toString() == id } ?: bridge?.ringCall(id)
    }

    /** The built-in notification's Answer or Decline (MedutAnswerActivity,
     *  MedutCallActionReceiver).  Main thread. */
    internal fun handleAction(intent: Intent) {
        val key = intent.getStringExtra(EXTRA_CALL_ID) ?: return
        val bridge = bridge
        // A push ring answers and declines through its connection, which
        // carries the choice over to an INVITE still on its way.
        val ring = bridge?.ringConnection(key)
        val call = if (ring == null) {
            callFor(intent) ?: run {
                // The call is gone (ended, or the stack restarted): nothing to
                // answer or decline, but the notification must not linger.
                bridge?.notifier?.cancel(key)
                return
            }
        } else null
        when (intent.action) {
            ACTION_ANSWER -> {
                if (ring != null) ring.onAnswer()
                else runCatching { call!!.answer() }.onFailure { Log.w(TAG, "answer: $it") }
            }
            ACTION_DECLINE -> {
                if (ring != null) ring.onReject()
                else runCatching { call!!.reject(603, "Decline") }.onFailure { Log.w(TAG, "decline: $it") }
            }
        }
        bridge?.notifier?.cancel(key)
    }

    /** The intent that opens the app's call screen for [key]. */
    internal fun callScreenIntent(context: Context, key: String, action: String?): Intent? {
        val options = bridge?.options
        val intent = options?.callActivity?.let { Intent(context, it) }
            ?: context.packageManager.getLaunchIntentForPackage(context.packageName)
            ?: return null
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        action?.let { intent.action = it }
        return intent.putExtra(EXTRA_CALL_ID, key)
    }
}

/**
 * The live half of [MedutTelecom]: owns the PhoneAccount, turns SDK calls
 * into Telecom requests, and keeps each [MedutConnection] in step with its
 * call.  Everything runs on the main thread (SDK events are delivered there,
 * and Telecom calls the service there), so the maps need no further locking
 * than their own.
 */
@RequiresApi(Build.VERSION_CODES.O)
internal class TelecomBridge(
    context: Context,
    val options: TelecomOptions,
    private val platform: AndroidPlatform,
) : AudioRoutes.TelecomRouter {
    private val app = context.applicationContext
    private val tm = app.getSystemService(Context.TELECOM_SERVICE) as TelecomManager
    private val main = Handler(Looper.getMainLooper())
    private val mainExecutor = Executor { r ->
        if (Looper.myLooper() == Looper.getMainLooper()) r.run() else main.post(r)
    }

    val handle = PhoneAccountHandle(
        ComponentName(app, MedutConnectionService::class.java), options.phoneAccountId)
    val notifier = IncomingCallNotifier(app, options)

    /** Calls handed to Telecom and not yet wrapped in a connection. */
    private val pending = ConcurrentHashMap<String, Call>()
    private val connections = ConcurrentHashMap<Call, MedutConnection>()
    /** Push rings by key, from [ringForPush] until their connection ends
     *  (or, declined, until their INVITE is turned away or they expire).
     *  Changed on the main thread only; read from any. */
    private val rings = ConcurrentHashMap<String, PushRing>()
    private var subscription: MedutSubscription? = null
    private var audioActive = false

    @Volatile
    override var audioState: CallAudioState? = null
        private set

    fun start() {
        val label = options.label ?: app.applicationInfo.loadLabel(app.packageManager).toString()
        val account = PhoneAccount.builder(handle, label)
            .setCapabilities(PhoneAccount.CAPABILITY_SELF_MANAGED)
            .addSupportedUriScheme(PhoneAccount.SCHEME_SIP)
            .addSupportedUriScheme(PhoneAccount.SCHEME_TEL)
            .build()
        tm.registerPhoneAccount(account)
        subscription = Medut.addListener(mainExecutor) { onEvent(it) }
        MedutTelecom.bridge = this
        // A stack that was already running (reattached) may be ringing.
        for (call in Medut.calls) {
            if (!call.isOutgoing && call.state == CallState.RINGING) onIncoming(call)
        }
    }

    fun stop() {
        if (MedutTelecom.bridge === this) MedutTelecom.bridge = null
        subscription?.cancel()
        subscription = null
        for (connection in connections.values) {
            connection.setDisconnected(DisconnectCause(DisconnectCause.LOCAL))
            connection.destroy()
        }
        connections.clear()
        pending.clear()
        for (ring in rings.values) ring.connection?.endUnbound(DisconnectCause.LOCAL)
        rings.clear()
        notifier.cancelAll()
        runCatching { tm.unregisterPhoneAccount(handle) }
    }

    // ── SDK -> Telecom ───────────────────────────────────────────────────

    private fun onEvent(event: MedutEvent) {
        when (event) {
            is MedutEvent.IncomingCall -> onIncoming(event.call)
            is MedutEvent.CallStateChanged -> {
                val connection = connections[event.call]
                if (connection != null) {
                    connection.sync(event.state, event.error, event.reason)
                } else if (event.state != CallState.RINGING && event.state != CallState.CALLING) {
                    // A call Telecom refused rings through the notification
                    // alone (showIncomingUi(call)); nothing else takes it down.
                    notifier.cancel(event.call.id.toString())
                }
            }
            else -> {}
        }
    }

    /** [dev.medut.PlatformHooks.onCallPlacing]: refuse before the INVITE. */
    fun checkOutgoingPermitted() {
        if (!tm.isOutgoingCallPermitted(handle)) {
            throw MedutException(MedutError(MedutError.Code.INVALID_STATE),
                "Telecom refuses a new outgoing call (another call is up)")
        }
    }

    /** [dev.medut.PlatformHooks.onCallPlaced]: the INVITE is out; ask
     *  Telecom for the connection.  Any thread. */
    fun onCallPlaced(call: Call) {
        val id = call.id.toString()
        pending[id] = call
        val extras = Bundle().apply {
            putParcelable(TelecomManager.EXTRA_PHONE_ACCOUNT_HANDLE, handle)
            putBundle(TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS,
                Bundle().apply { putString(MedutTelecom.EXTRA_CALL_ID, id) })
        }
        try {
            tm.placeCall(addressOf(call), extras)
        } catch (e: SecurityException) {
            // MANAGE_OWN_CALLS missing or the account disabled: the call
            // goes on without Telecom, so nothing sets the audio mode for
            // it unless the SDK does.
            pending.remove(id)
            Log.w(TAG, "placeCall: $e; the call goes on without Telecom")
            platform.useAutomaticAudio(call, activateNow = true)
        }
    }

    private fun onIncoming(call: Call) {
        val id = call.id.toString()
        if (connections.containsKey(call) || pending.containsKey(id)) return
        MedutTelecom.incomingCallFilter?.invoke(call)?.let { code ->
            refuse(call, code)
            return
        }
        if (claimForRing(call)) return
        if (!tm.isIncomingCallPermitted(handle)) {
            runCatching { call.reject(486, "Busy Here") }
            return
        }
        pending[id] = call
        val extras = Bundle().apply {
            putString(MedutTelecom.EXTRA_CALL_ID, id)
            putParcelable(TelecomManager.EXTRA_INCOMING_CALL_ADDRESS, addressOf(call))
        }
        try {
            tm.addNewIncomingCall(handle, extras)
        } catch (e: SecurityException) {
            // No connection will exist for this call, so under
            // AudioModePolicy.TELECOM nobody would set MODE_IN_COMMUNICATION:
            // the SDK activates call audio itself when it is answered.
            pending.remove(id)
            Log.w(TAG, "addNewIncomingCall: $e; the call rings without Telecom")
            platform.useAutomaticAudio(call, activateNow = false)
            showIncomingUi(call)
        }
    }

    // ── Push rings ───────────────────────────────────────────────────────

    /**
     * Ring for a call [payload] announced, before its INVITE: Telecom gets an
     * incoming connection now, and [onIncoming] hands it the INVITE.  Any
     * thread; the work happens on the main thread.
     */
    fun ringForPush(payload: PushPayload, timeoutMs: Long) {
        main.post {
            // FCM can deliver a push twice, and a server may push again.
            val duplicate = rings.values.any {
                it.state == PushRing.State.RINGING &&
                    (it.payload.callId == null || it.payload.callId == payload.callId)
            }
            if (duplicate) return@post
            if (!tm.isIncomingCallPermitted(handle)) {
                // Another call has the line; the INVITE gets a 486 from
                // onIncoming when it comes.
                return@post
            }
            val ring = PushRing("push:${UUID.randomUUID()}", payload)
            rings[ring.key] = ring
            val extras = Bundle().apply {
                putString(MedutTelecom.EXTRA_CALL_ID, ring.key)
                putParcelable(TelecomManager.EXTRA_INCOMING_CALL_ADDRESS, addressOf(payload.callerUri))
            }
            try {
                tm.addNewIncomingCall(handle, extras)
            } catch (e: SecurityException) {
                rings.remove(ring.key)
                Log.w(TAG, "addNewIncomingCall (push): $e")
                return@post
            }
            main.postDelayed({ expire(ring) }, timeoutMs)
        }
    }

    /** A cancel push: end the rings it names (all unbound ones when it
     *  names no Call-ID).  A ring whose INVITE has arrived ends with it. */
    fun cancelPushRing(payload: PushPayload) {
        main.post {
            for (ring in rings.values.toList()) {
                if (ring.state != PushRing.State.RINGING) continue
                if (payload.callId != null && ring.payload.callId != null &&
                    payload.callId != ring.payload.callId) continue
                end(ring, DisconnectCause.MISSED)
            }
        }
    }

    /** The filter turned [call] away: reject it, and end the push ring it
     *  would have taken over, if any, as rejected.  Main thread. */
    private fun refuse(call: Call, code: Int) {
        runCatching { call.reject(code) }.onFailure { Log.w(TAG, "filter reject: $it") }
        val ring = rings.values.firstOrNull {
            (it.state == PushRing.State.RINGING || it.state == PushRing.State.DECLINED) &&
                MedutPush.matches(it.payload, call)
        } ?: return
        if (ring.state == PushRing.State.DECLINED) {
            rings.remove(ring.key)
            ring.state = PushRing.State.ENDED
        } else {
            end(ring, DisconnectCause.REJECTED)
        }
    }

    /** The INVITE a push ring is waiting for: bind it.  Main thread. */
    private fun claimForRing(call: Call): Boolean {
        val ring = rings.values.firstOrNull {
            (it.state == PushRing.State.RINGING || it.state == PushRing.State.DECLINED) &&
                MedutPush.matches(it.payload, call)
        } ?: return false
        if (ring.state == PushRing.State.DECLINED) {
            rings.remove(ring.key)
            ring.state = PushRing.State.ENDED
            runCatching { call.reject(603, "Decline") }
            return true
        }
        ring.state = PushRing.State.BOUND
        ring.call = call
        val connection = ring.connection ?: return true   // bound on creation
        connections[call] = connection
        connection.bind(call)
        MedutTelecom.onShowIncomingCallUi?.let { custom ->
            notifier.cancel(connection.key)
            custom(call)
        }
        return true
    }

    private fun expire(ring: PushRing) {
        when (ring.state) {
            PushRing.State.RINGING -> end(ring, DisconnectCause.MISSED)
            PushRing.State.DECLINED -> {
                rings.remove(ring.key)
                ring.state = PushRing.State.ENDED
            }
            PushRing.State.BOUND, PushRing.State.ENDED -> {}
        }
    }

    private fun end(ring: PushRing, code: Int) {
        ring.state = PushRing.State.ENDED
        rings.remove(ring.key)
        notifier.cancel(ring.key)
        ring.connection?.endUnbound(code)
    }

    /** Telecom created the connection for a push ring.  Main thread. */
    fun takeRing(key: String?): PushRing? {
        val ring = key?.let { rings[it] } ?: return null
        return ring.takeIf { it.connection == null && it.state != PushRing.State.ENDED }
    }

    fun attachRing(ring: PushRing, connection: MedutConnection) {
        ring.connection = connection
        ring.call?.let { call ->
            connections[call] = connection
            connection.bind(call)
        }
    }

    /** Declined before the INVITE: keep the ring so the INVITE is turned
     *  away when it comes, rather than ringing again. */
    fun ringDeclined(connection: MedutConnection) {
        val ring = connection.ring ?: return
        if (ring.state == PushRing.State.RINGING) ring.state = PushRing.State.DECLINED
    }

    /** Telecom would not take a push ring (another call has the line). */
    fun ringRefused(ring: PushRing) {
        ring.state = PushRing.State.ENDED
        rings.remove(ring.key)
        ring.call?.let { incomingRefused(it) }
    }

    fun ringConnection(key: String): MedutConnection? = rings[key]?.connection

    fun ringCall(key: String): Call? = rings[key]?.call

    // ── Telecom -> SDK (from the service and the connections) ────────────

    fun takePending(id: String?): Call? = id?.let { pending.remove(it) }

    fun attach(call: Call, connection: MedutConnection) {
        connections[call] = connection
        // The call may have moved on (or ended) while Telecom was asked.
        connection.sync(call.state, call.endError, call.endReason)
    }

    fun detach(connection: MedutConnection) {
        connection.call?.let { connections.remove(it, connection) }
        connection.ring?.let { ring ->
            // A declined ring stays to turn its INVITE away.
            if (ring.state != PushRing.State.DECLINED) {
                ring.state = PushRing.State.ENDED
                rings.remove(ring.key)
            }
        }
        notifier.cancel(connection.key)
        updateAudioActive()
    }

    fun outgoingRefused(call: Call) {
        runCatching { call.hangup() }
        main.post { MedutTelecom.onOutgoingCallRefused?.invoke(call) }
    }

    fun incomingRefused(call: Call) {
        runCatching { call.reject(486, "Busy Here") }
    }

    /** No connection (Telecom refused us): show the call ourselves. */
    private fun showIncomingUi(call: Call) {
        val custom = MedutTelecom.onShowIncomingCallUi
        val title = call.remoteDisplayName?.takeIf { it.isNotBlank() } ?: call.remoteUri
        when {
            custom != null -> custom(call)
            options.incomingCallNotification -> notifier.show(call.id.toString(), title)
        }
    }

    fun showIncomingUi(connection: MedutConnection) {
        val call = connection.call
        val custom = if (call != null) MedutTelecom.onShowIncomingCallUi?.let { { it(call) } }
            else MedutTelecom.onShowPushIncomingCallUi?.let { { it(connection.key, connection.ring!!.payload) } }
        when {
            custom != null -> custom()
            options.incomingCallNotification -> notifier.show(connection.key, connection.title)
        }
    }

    fun connectionStateChanged(connection: MedutConnection, state: Int) {
        if (state == Connection.STATE_ACTIVE || state == Connection.STATE_DISCONNECTED) {
            notifier.cancel(connection.key)
            updateAudioActive()
        }
    }

    fun audioStateChanged(connection: MedutConnection, state: CallAudioState) {
        val previous = connection.lastAudioState
        connection.lastAudioState = state
        audioState = state
        // Telecom reports its mute flag with every route change, and for a
        // call the app muted itself (Call.setMuted) that flag is still
        // false: only a change of it is Telecom muting (a Bluetooth headset,
        // a watch).
        if (previous != null && previous.isMuted != state.isMuted) {
            runCatching { connection.call?.setMuted(state.isMuted) }
        }
        platform.routes.notifyChanged()
        val call = connection.call ?: return
        MedutTelecom.onCallAudioStateChanged?.let { listener ->
            val muted = runCatching { call.isMuted }.getOrDefault(state.isMuted)
            listener(call, muted, platform.routes.list().firstOrNull { it.isActive })
        }
    }

    /** Telecom has set the audio mode for an active connection: the moment
     *  the app-owned audio device may open (AudioModePolicy.TELECOM). */
    private fun updateAudioActive() {
        val active = connections.values.any {
            it.state == Connection.STATE_ACTIVE || it.state == Connection.STATE_HOLDING
        }
        if (active == audioActive) return
        audioActive = active
        platform.externalAudioActive(active)
    }

    // ── Routes ───────────────────────────────────────────────────────────

    // setAudioRoute is deprecated in API 34 for requestCallEndpointChange,
    // which needs the CallEndpoint objects onAvailableCallEndpointsChanged
    // reports; setAudioRoute still works on every API level.
    @Suppress("DEPRECATION")
    override fun select(route: AudioRoute): Boolean {
        val connection = connections.values.firstOrNull { it.state == Connection.STATE_ACTIVE }
            ?: connections.values.firstOrNull { it.state != Connection.STATE_DISCONNECTED }
            ?: return false
        connection.setAudioRoute(when (route.kind) {
            AudioRoute.Kind.EARPIECE -> CallAudioState.ROUTE_EARPIECE
            AudioRoute.Kind.SPEAKER -> CallAudioState.ROUTE_SPEAKER
            AudioRoute.Kind.BLUETOOTH -> CallAudioState.ROUTE_BLUETOOTH
            AudioRoute.Kind.WIRED_HEADSET -> CallAudioState.ROUTE_WIRED_HEADSET
        })
        return true
    }

    fun addressOf(call: Call): Uri = addressOf(call.remoteUri)

    fun addressOf(uri: String?): Uri {
        if (uri.isNullOrBlank()) return Uri.fromParts(PhoneAccount.SCHEME_SIP, "unknown", null)
        return Uri.parse(if (uri.contains(':')) uri else "sip:$uri")
    }

    private companion object {
        const val TAG = "MedutTelecom"
    }
}

/**
 * A call a push announced, ringing in Telecom before its INVITE.  State
 * changes on the main thread.
 */
internal class PushRing(val key: String, val payload: PushPayload) {
    enum class State {
        /** Ringing; no INVITE yet. */
        RINGING,
        /** The INVITE arrived and the connection carries it. */
        BOUND,
        /** The user declined before the INVITE; it is rejected on arrival. */
        DECLINED,
        ENDED,
    }

    @Volatile var state = State.RINGING
    @Volatile var connection: MedutConnection? = null
    @Volatile var call: Call? = null
}
