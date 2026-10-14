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
 */
@RequiresApi(Build.VERSION_CODES.O)
internal class MedutConnection(val call: Call, private val bridge: TelecomBridge) : Connection() {
    private var everActive = false
    private var ended = false

    init {
        connectionProperties = PROPERTY_SELF_MANAGED
        connectionCapabilities = CAPABILITY_HOLD or CAPABILITY_SUPPORT_HOLD or CAPABILITY_MUTE
        audioModeIsVoip = true
        setAddress(bridge.addressOf(call), TelecomManager.PRESENTATION_ALLOWED)
        call.remoteDisplayName?.takeIf { it.isNotBlank() }?.let {
            setCallerDisplayName(it, TelecomManager.PRESENTATION_ALLOWED)
        }
    }

    fun sync(state: CallState, error: MedutError?, reason: String?) {
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
            CallState.ENDED, CallState.CANCELLED, CallState.FAILED -> end(state, error, reason)
        }
    }

    private fun end(state: CallState, error: MedutError?, reason: String?) {
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
        bridge.detach(call, this)
    }

    // ── Telecom -> call ──────────────────────────────────────────────────

    override fun onAnswer() {
        runCatching { call.answer() }.onFailure {
            sync(CallState.FAILED, null, it.message)
        }
    }

    override fun onAnswer(videoState: Int) = onAnswer()

    override fun onReject() {
        runCatching { call.reject(603, "Decline") }
    }

    override fun onReject(rejectReason: Int) = onReject()

    override fun onReject(replyMessage: String?) = onReject()

    override fun onDisconnect() {
        runCatching { call.hangup() }
    }

    override fun onAbort() = onDisconnect()

    override fun onHold() {
        runCatching { call.hold() }
    }

    override fun onUnhold() {
        runCatching { call.resume() }
    }

    override fun onPlayDtmfTone(c: Char) {
        runCatching { call.sendDtmf(c) }
    }

    @Deprecated("Telecom deprecated it in API 34; still the delivery path for mute and route")
    override fun onCallAudioStateChanged(state: CallAudioState) {
        bridge.audioStateChanged(this, state)
    }

    override fun onShowIncomingCallUi() {
        bridge.showIncomingUi(call)
    }

    override fun onStateChanged(state: Int) {
        bridge.connectionStateChanged(this, state)
    }
}
