package dev.medut.android.telecom

import android.os.Build
import android.telecom.CallAudioState
import android.telecom.Connection
import android.telecom.DisconnectCause
import android.telecom.TelecomManager
import androidx.annotation.RequiresApi
import dev.medut.Call
import dev.medut.CallState
import dev.medut.MedutError

/**
 * One SIP call as Telecom sees it.  SDK call-state events drive the Telecom
 * state through [sync]; Telecom's requests (answer from a Bluetooth headset,
 * hold for a cellular call, disconnect from the system UI) drive the call.
 *
 * A connection made for a push ([PushRing]) starts with no call: it rings
 * with the caller from the push payload, and [bind] hands it the INVITE when
 * that arrives.  An Answer before then is carried out on [bind]; a Decline
 * ends the ring, and the bridge rejects the INVITE when it comes.
 */
@RequiresApi(Build.VERSION_CODES.O)
internal class MedutConnection private constructor(
    call: Call?,
    private val bridge: TelecomBridge,
    val ring: PushRing?,
) : Connection() {
    constructor(call: Call, bridge: TelecomBridge) : this(call, bridge, null)
    constructor(ring: PushRing, bridge: TelecomBridge) : this(null, bridge, ring)

    /** The SIP call; null on a push ring until its INVITE is bound. */
    @Volatile
    var call: Call? = call
        private set

    /** Notification and intent key: the push ring's for a ring, so it stays
     *  the same across [bind]. */
    val key: String = ring?.key ?: call!!.id.toString()

    /** The last CallAudioState Telecom reported; main thread. */
    var lastAudioState: CallAudioState? = null

    private var everActive = false
    private var ended = false
    private var answerWanted = false

    init {
        connectionProperties = PROPERTY_SELF_MANAGED
        connectionCapabilities = CAPABILITY_HOLD or CAPABILITY_SUPPORT_HOLD or CAPABILITY_MUTE
        audioModeIsVoip = true
        if (call != null) {
            showCaller(call)
        } else {
            setAddress(bridge.addressOf(ring!!.payload.callerUri), TelecomManager.PRESENTATION_ALLOWED)
            ring.payload.callerName?.let {
                setCallerDisplayName(it, TelecomManager.PRESENTATION_ALLOWED)
            }
            setRinging()
        }
    }

    /** What the incoming-call UI shows. */
    val title: String
        get() = call?.let { c -> c.remoteDisplayName?.takeIf { it.isNotBlank() } ?: c.remoteUri }
            ?: ring?.payload?.let { it.callerName ?: it.callerUri }
            ?: "Incoming call"

    private fun showCaller(call: Call) {
        setAddress(bridge.addressOf(call), TelecomManager.PRESENTATION_ALLOWED)
        call.remoteDisplayName?.takeIf { it.isNotBlank() }?.let {
            setCallerDisplayName(it, TelecomManager.PRESENTATION_ALLOWED)
        }
    }

    /** A push ring's INVITE has arrived.  Main thread. */
    fun bind(call: Call) {
        if (ended || this.call != null) return
        this.call = call
        showCaller(call)
        if (answerWanted) {
            runCatching { call.answer() }.onFailure { sync(CallState.FAILED, null, it.message) }
        }
        sync(call.state, call.endError, call.endReason)
    }

    fun sync(state: CallState, error: MedutError?, reason: String?) {
        val call = call ?: return
        if (ended) return
        when (state) {
            CallState.CALLING -> setDialing()
            CallState.RINGING -> if (call.isOutgoing) setDialing() else setRinging()
            CallState.ESTABLISHED -> {
                everActive = true
                setActive()
            }
            CallState.HELD -> {
                everActive = true
                setOnHold()
            }
            CallState.ENDED, CallState.CANCELLED, CallState.FAILED -> end(call, state, error, reason)
        }
    }

    private fun end(call: Call, state: CallState, error: MedutError?, reason: String?) {
        ended = true
        val code = when {
            call.endRequestedLocally ->
                if (!call.isOutgoing && !everActive) DisconnectCause.REJECTED else DisconnectCause.LOCAL
            state == CallState.CANCELLED ->
                if (call.isOutgoing) DisconnectCause.CANCELED else DisconnectCause.MISSED
            state == CallState.FAILED ->
                if (reason?.contains("486") == true || reason?.contains("Busy", ignoreCase = true) == true)
                    DisconnectCause.BUSY
                else DisconnectCause.ERROR
            else -> DisconnectCause.REMOTE
        }
        val text = reason ?: error?.message
        setDisconnected(DisconnectCause(code, text, text, text ?: ""))
        destroy()
        bridge.detach(this)
    }

    /** End a push ring that never got its INVITE: [code] is MISSED for a
     *  timeout or a cancel push, REJECTED for a Decline.  Main thread. */
    fun endUnbound(code: Int) {
        if (ended || call != null) return
        ended = true
        setDisconnected(DisconnectCause(code))
        destroy()
        bridge.detach(this)
    }

    // ── Telecom -> call ──────────────────────────────────────────────────

    override fun onAnswer() {
        val call = call
        if (call == null) {
            answerWanted = true
            return
        }
        runCatching { call.answer() }.onFailure {
            sync(CallState.FAILED, null, it.message)
        }
    }

    override fun onAnswer(videoState: Int) = onAnswer()

    override fun onReject() {
        val call = call
        if (call == null) {
            bridge.ringDeclined(this)
            endUnbound(DisconnectCause.REJECTED)
            return
        }
        runCatching { call.reject(603, "Decline") }
    }

    override fun onReject(rejectReason: Int) = onReject()

    override fun onReject(replyMessage: String?) = onReject()

    override fun onDisconnect() {
        val call = call ?: return onReject()
        runCatching { call.hangup() }
    }

    override fun onAbort() = onDisconnect()

    override fun onHold() {
        runCatching { call?.hold() }
    }

    override fun onUnhold() {
        runCatching { call?.resume() }
    }

    override fun onPlayDtmfTone(c: Char) {
        runCatching { call?.sendDtmf(c) }
    }

    @Deprecated("Telecom deprecated it in API 34; still the delivery path for mute and route")
    override fun onCallAudioStateChanged(state: CallAudioState) {
        bridge.audioStateChanged(this, state)
    }

    override fun onShowIncomingCallUi() {
        bridge.showIncomingUi(this)
    }

    override fun onStateChanged(state: Int) {
        bridge.connectionStateChanged(this, state)
    }
}
