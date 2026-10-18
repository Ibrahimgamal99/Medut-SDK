package dev.medut.android

import android.content.Context
import android.os.PowerManager
import android.util.Log
import dev.medut.Call
import dev.medut.CallState
import dev.medut.Medut
import dev.medut.MedutEvent
import dev.medut.RegistrationState
import dev.medut.android.telecom.MedutTelecom
import org.json.JSONObject
import android.os.SystemClock
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Which data-message fields carry what, for [PushPayload.parse].  The
 * defaults are the payload docs/guides/push.md describes; a server that
 * already sends other names is met by changing these, not the server.
 */
data class PushPayloadKeys(
    /** "call" or "cancel" ([callValue] / [cancelValue]).  A push without
     *  it is a call: many servers send nothing but a wake-up. */
    val type: String = "type",
    val callValue: String = "call",
    val cancelValue: String = "cancel",
    /** SIP Call-ID of the INVITE the push announces; matches the push to
     *  that INVITE when present. */
    val callId: String = "call_id",
    val callerUri: String = "caller_uri",
    val callerName: String = "caller_name",
    /** The callee's AOR, for a device with more than one account. */
    val aor: String = "aor",
    /** A field whose value is a JSON object holding the keys above, e.g.
     *  `{"medut": "{\"call_id\": ...}"}`; looked in after the top level. */
    val container: String = "medut",
)

/** What a push said, read with [PushPayloadKeys]. */
data class PushPayload(
    val type: Type,
    val callId: String?,
    val callerUri: String?,
    val callerName: String?,
    val aor: String?,
    /** Every field as it arrived. */
    val data: Map<String, String>,
) {
    enum class Type {
        /** An INVITE is on its way. */
        CALL,
        /** The caller gave up before the INVITE reached this device. */
        CANCEL,
        /** A type this SDK does not know; [MedutPush.wake] only makes the
         *  accounts reachable. */
        OTHER,
    }

    companion object {
        /** Read an FCM data message (`RemoteMessage.data`). */
        @JvmStatic
        @JvmOverloads
        fun parse(data: Map<String, String>, keys: PushPayloadKeys = MedutPush.payloadKeys): PushPayload {
            val nested = data[keys.container]?.let {
                runCatching { JSONObject(it) }.getOrNull()
            }
            fun field(key: String): String? =
                data[key]?.takeIf { it.isNotEmpty() }
                    ?: nested?.optString(key)?.takeIf { it.isNotEmpty() }
            val type = when (field(keys.type)?.lowercase()) {
                null, keys.callValue.lowercase() -> Type.CALL
                keys.cancelValue.lowercase() -> Type.CANCEL
                else -> Type.OTHER
            }
            return PushPayload(type, field(keys.callId), field(keys.callerUri),
                field(keys.callerName), field(keys.aor), data)
        }
    }
}

/**
 * Push wake-up (FCM).  The SDK carries no Firebase dependency: the app's
 * `FirebaseMessagingService` hands each data message to [wake] and each new
 * token to [onNewToken].  The SDK puts the RFC 8599 push binding
 * (`;pn-provider=fcm;pn-prid=<token>;pn-param=<…>`) in the Contact of every
 * REGISTER, from `AccountConfig.pushProvider` / `pushToken` / `pushParam`.
 *
 * The flow this supports, with any server that implements RFC 8599 (or a
 * push gateway that sends the same data message):
 *
 *  1. The server gets an INVITE for a device it cannot reach, holds it, and
 *     sends a high-priority FCM data message ([PushPayload]).
 *  2. Android starts the process; `Application.onCreate` runs
 *     [MedutAndroid.start] and re-creates the saved accounts, as on any launch.
 *  3. `onMessageReceived` calls [wake].  With Telecom on, the phone rings at
 *     once with the caller from the payload.  Every account sends a REGISTER
 *     (RFC 8599 s4.1.3: that is what tells the server the device is back).
 *  4. The server forwards the held INVITE.  It takes over the ringing call
 *     (matched by Call-ID when the payload has one), and an Answer the user
 *     already tapped is carried out.  No INVITE in time ends the ring as
 *     missed; a cancel push ends it at once.
 *
 * None of this needs a Flutter engine or any UI to be up.
 */
object MedutPush {
    private const val TAG = "MedutPush"

    /** Field names [wake] reads.  Set once, before the first push. */
    @JvmStatic
    @Volatile
    var payloadKeys = PushPayloadKeys()

    /**
     * Ring through Telecom as soon as a call push arrives, before the INVITE.
     * Off waits for the INVITE, as when Telecom is not enabled.
     */
    @JvmStatic
    @Volatile
    var ringOnPush = true

    /**
     * Handle a push from `FirebaseMessagingService.onMessageReceived`.
     * Blocks the calling thread (FCM's own background thread) for up to
     * [waitForCallMs] while the accounts re-register and the INVITE arrives.
     *
     * @param data `RemoteMessage.data`; empty is a plain wake-up.
     * @return the incoming call, or null for a cancel, an unknown type, or
     *         no INVITE in time.
     * @throws IllegalStateException when the stack is not running: start it
     *         from `Application.onCreate` so it is up before any push.
     */
    @JvmStatic
    @JvmOverloads
    fun wake(context: Context, data: Map<String, String>, waitForCallMs: Long = 10_000): Call? =
        wake(context, PushPayload.parse(data, payloadKeys), waitForCallMs)

    /** [wake] with a payload already read (or built by the app). */
    @JvmStatic
    @JvmOverloads
    fun wake(context: Context, payload: PushPayload, waitForCallMs: Long = 10_000): Call? {
        check(Medut.isRunning) {
            "the Medut stack is not running; start it from Application.onCreate " +
                "so a push finds it up"
        }
        if (payload.type == PushPayload.Type.CANCEL) {
            MedutTelecom.bridge?.cancelPushRing(payload)
            return null
        }
        if (payload.type == PushPayload.Type.OTHER) {
            reachAccounts()
            return null
        }
        return waitForCall(context, payload, waitForCallMs)
    }

    /** A wake-up with no payload: re-register and wait for any INVITE. */
    @JvmStatic
    @JvmOverloads
    fun wake(context: Context, waitForCallMs: Long = 10_000): Call? =
        waitForCall(context, null, waitForCallMs)

    private fun waitForCall(context: Context, payload: PushPayload?, waitForCallMs: Long): Call? {
        val pm = context.applicationContext.getSystemService(Context.POWER_SERVICE) as PowerManager
        val lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "medut:push")
        lock.acquire(waitForCallMs + 5_000)
        try {
            // The listener runs inline on the SDK's event thread, which must
            // never take the gate (shutdown holds it while joining that
            // thread), and matches() reads the call's Call-ID through it.  So
            // the listener only queues; the matching happens here, on the
            // caller's thread.
            val incoming = LinkedBlockingQueue<Call>()
            val subscription = Medut.addListener({ it.run() }) { event ->
                if (event is MedutEvent.IncomingCall) incoming.offer(event.call)
            }
            try {
                // Already here: the device was reachable after all, or the
                // INVITE beat the push.
                Medut.calls.firstOrNull {
                    !it.isOutgoing && it.state == CallState.RINGING && matches(payload, it)
                }?.let { return it }

                if (payload != null && ringOnPush) {
                    MedutTelecom.bridge?.ringForPush(payload, waitForCallMs)
                }
                reachAccounts()
                val deadline = SystemClock.elapsedRealtime() + waitForCallMs
                while (true) {
                    val left = deadline - SystemClock.elapsedRealtime()
                    if (left <= 0) return null
                    val call = incoming.poll(left, TimeUnit.MILLISECONDS) ?: return null
                    if (matches(payload, call)) return call
                }
            } finally {
                subscription.cancel()
            }
        } finally {
            if (lock.isHeld) lock.release()
        }
    }

    /** Is [call] the one [payload] announced?  Without a Call-ID or AOR in
     *  the payload, any incoming call is.  Takes the gate (Call.info): never
     *  call it on the SDK's event thread. */
    internal fun matches(payload: PushPayload?, call: Call): Boolean {
        if (payload == null) return true
        payload.aor?.let { aor ->
            val account = call.account?.aor ?: return@let
            if (!sameAor(aor, account)) return false
        }
        val callId = payload.callId ?: return true
        val sipCallId = runCatching { call.info().callId }.getOrDefault("")
        return sipCallId.isEmpty() || sipCallId == callId
    }

    private fun sameAor(a: String, b: String): Boolean {
        fun norm(s: String) = s.trim().lowercase().removePrefix("sips:").removePrefix("sip:")
            .substringBefore(';').substringBefore('>')
        return norm(a) == norm(b)
    }

    /**
     * Make every account reachable again without waiting for a call: the
     * part of [wake] an app wants on a foreground return.  Each account
     * refreshes its registration in place ([dev.medut.Account.refreshRegistration]):
     * RFC 8599 servers release a held INVITE on the binding refresh, not on
     * an OPTIONS probe, and not on a binding removed and added again.
     */
    @JvmStatic
    fun reachAccounts() {
        // The process may have been dozing on a network that is gone;
        // coalesced and cheap when nothing changed.
        runCatching { Medut.network.changed() }
        for (account in Medut.accounts) {
            val result = runCatching {
                when (account.registrationState) {
                    RegistrationState.REGISTERED, RegistrationState.UNREGISTERED ->
                        account.refreshRegistration()
                    RegistrationState.RECONNECTING, RegistrationState.FAILED -> account.retryNow()
                    RegistrationState.REGISTERING, RegistrationState.UNREGISTERING -> {}
                }
            }
            result.exceptionOrNull()?.let { Log.w(TAG, "${account.aor}: $it") }
        }
    }

    /**
     * A new FCM token (`FirebaseMessagingService.onNewToken`): every account
     * with a push provider sends it to the server (the others keep it and
     * send nothing), and the saved accounts keep it, so a process a later
     * push starts registers with this token and not the one it replaced.
     */
    @JvmStatic
    fun onNewToken(token: String) {
        for (account in Medut.accounts) {
            runCatching { account.setPushToken(token) }
                .onFailure { Log.w(TAG, "${account.aor}: $it") }
        }
        runCatching { MedutAndroid.accountStore.updatePushToken(token) }
            .onFailure { Log.w(TAG, "account store: $it") }
    }
}
