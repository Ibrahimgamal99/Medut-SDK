package dev.medut

import dev.medut.internal.EventReleaser
import dev.medut.internal.Native
import dev.medut.internal.RawCallInfo
import dev.medut.internal.RawStats
import java.util.UUID

/**
 * A call, incoming or outgoing.
 *
 * Safe to keep for as long as you hold it, including after it has ended: an
 * ended call keeps its terminal state event, which owns the native call's last
 * reference, until this object becomes unreachable.  Don't keep ended calls
 * around in bulk; keep a [CallInfo].
 *
 * Operations on an ended call throw [MedutError.Code.CALL_ENDED], except
 * [hangup], which is a no-op then.  After [Medut.shutdown] everything throws
 * [MedutError.Code.NOT_RUNNING].
 */
class Call internal constructor(
    internal val handle: Long,
    internal val generation: Long,
    val account: Account?,
    initialState: CallState,
    val isOutgoing: Boolean,
    /** The far end: the dialled URI for an outgoing call, the From URI for an
     *  incoming one. */
    val remoteUri: String,
    val remoteDisplayName: String?,
) {
    /** Stable identity, e.g. for a Telecom connection or a notification. */
    val id: UUID = UUID.randomUUID()

    /** The state from the latest call-state event. */
    @Volatile var state: CallState = initialState
        private set

    val isEnded: Boolean get() = state.isTerminal

    /** Why the call ended, once it has: the error the terminal event carried
     *  (null for a normal hangup) and the reason text. */
    @Volatile var endError: MedutError? = null
        private set
    @Volatile var endReason: String? = null
        private set

    private var holdsTerminalEvent = false

    /** The installed media tap; read on the audio thread (MediaTapSink). */
    @Volatile internal var mediaTap: MediaTapListener? = null

    /** This side asked to end the call ([hangup] / [reject]), so a terminal
     *  state is local, not the far end's doing.  Telecom reports it so. */
    @Volatile internal var endRequestedLocally = false

    override fun toString() = "Call($remoteUri, $state)"

    internal fun update(newState: CallState) {
        state = newState
    }

    /** Keep the terminal event until this object is unreachable.  Returns
     *  false if one is already held (the caller then releases it). */
    @Synchronized
    internal fun takeTerminalEvent(ev: Long, newState: CallState, error: MedutError?,
                                   reason: String?): Boolean {
        state = newState
        endError = error
        endReason = reason
        if (holdsTerminalEvent) return false
        holdsTerminalEvent = true
        EventReleaser.releaseWhenUnreachable(this, ev)
        return true
    }

    // ── Control ──────────────────────────────────────────────────────────

    /** Answer an incoming call.  With Telecom, call it from
     *  `Connection.onAnswer`, once Telecom has set the audio mode. */
    fun answer() {
        Medut.platform?.onCallStarting(this)
        perform("medut_call_answer") { Native.callAnswer(it) }
    }

    /** End the call (BYE, or CANCEL while it is being set up).  A no-op on a
     *  call that already ended. */
    fun hangup() {
        if (isEnded) return
        endRequestedLocally = true
        perform("medut_call_hangup") { Native.callHangup(it) }
    }

    /** End with a SIP status: for an unanswered incoming call this is the
     *  final response, e.g. 486 Busy Here or 603 Decline. */
    @JvmOverloads
    fun reject(statusCode: Int = 486, reason: String? = null) {
        endRequestedLocally = true
        perform("medut_call_reject") { Native.callReject(it, statusCode, reason) }
    }

    fun hold() = perform("medut_call_hold") { Native.callHold(it) }

    fun resume() = perform("medut_call_resume") { Native.callResume(it) }

    /** True while the call is on hold, either side. */
    val isHeld: Boolean
        get() = runCatching { read("medut_call_is_held") { Native.callIsHeld(it) } }.getOrDefault(false)

    /** Send one DTMF digit: 0-9, *, #, A-D. */
    fun sendDtmf(digit: Char) {
        if (digit !in "0123456789*#ABCDabcd") {
            throw MedutException(MedutError.fromRaw(-1)!!, "medut_call_send_dtmf")
        }
        perform("medut_call_send_dtmf") { Native.callSendDtmf(it, digit) }
    }

    /** Add a header to later requests in this dialog (re-INVITE, BYE, REFER). */
    fun addHeader(name: String, value: String) =
        perform("medut_call_add_header") { Native.callAddHeader(it, name, value) }

    // ── Transfer ─────────────────────────────────────────────────────────

    /** Blind transfer (REFER).  If refused, a [MedutEvent.TransferFailed]
     *  arrives and this call stays up. */
    fun transfer(uri: String) = perform("medut_call_transfer") { Native.callTransfer(it, uri) }

    /** Attended transfer: REFER with Replaces, bridging this call to
     *  [consultation], an established call to the transfer target. */
    fun attendedTransfer(consultation: Call) {
        if (consultation.generation != generation) {
            throw MedutException(MedutError(MedutError.Code.NOT_RUNNING), "medut_call_attended_transfer")
        }
        perform("medut_call_attended_transfer") {
            Native.callAttendedTransfer(it, consultation.handle)
        }
    }

    /** Follow an incoming REFER ([MedutEvent.TransferRequest]): place the
     *  call to the target, linked so the transferor learns the outcome.  This
     *  call stays up; end it when the new one connects. */
    fun acceptTransfer(): Call = read("medut_call_transfer_accept") { h ->
        if (isEnded) throw MedutException(MedutError(MedutError.Code.CALL_ENDED), "medut_call_transfer_accept")
        val out = LongArray(1)
        check(Native.callTransferAccept(h, out), "medut_call_transfer_accept")
        if (out[0] == 0L) throw MedutException(MedutError.fromRaw(-2)!!, "medut_call_transfer_accept")
        Medut.registry.trackCall(out[0], account, CallState.CALLING, isOutgoing = true,
            remoteUri = "", remoteDisplayName = null)
    }

    /** Refuse an incoming REFER; this call stays up.  603 is "declined",
     *  486 "busy". */
    @JvmOverloads
    fun rejectTransfer(statusCode: Int = 603, reason: String? = null) =
        perform("medut_call_transfer_reject") { Native.callTransferReject(it, statusCode, reason) }

    // ── Audio ────────────────────────────────────────────────────────────

    /** Mute the microphone for this call. */
    fun setMuted(muted: Boolean) = perform("medut_audio_mute") { Native.audioMute(it, muted) }

    val isMuted: Boolean
        get() = runCatching { read("medut_audio_is_muted") { Native.audioIsMuted(it) } }.getOrDefault(false)

    /** Silence the far end's audio for this call. */
    fun setReceiveMuted(muted: Boolean) =
        perform("medut_audio_mute_rx") { Native.audioMuteRx(it, muted) }

    /**
     * Tap this call's PCM in both directions, or remove the tap with null.
     * See [MediaTapListener] for the threading rules.  The tap goes away
     * with the call.
     */
    fun setMediaTap(listener: MediaTapListener?) {
        mediaTap = listener
        try {
            perform("medut_call_set_media_tap") { Native.callSetMediaTap(it, listener != null) }
        } catch (e: MedutException) {
            mediaTap = null
            throw e
        }
    }

    /** Record both directions, mixed, to a WAV file. */
    fun startRecording(path: String) =
        perform("medut_call_record_start") { Native.callRecordStart(it, path) }

    fun stopRecording() =
        perform("medut_call_record_stop", allowEnded = true) { Native.callRecordStop(it) }

    // ── Media tuning ─────────────────────────────────────────────────────

    /** DSCP for this call's RTP, e.g. 46 (EF). */
    fun setDscp(dscp: Int) = perform("medut_call_set_dscp_rtp") { Native.callSetDscpRtp(it, dscp) }

    /** End the call after this long without inbound RTP; 0 disables. */
    fun setRtpTimeout(seconds: Int) =
        perform("medut_call_set_rtp_timeout") { Native.callSetRtpTimeout(it, seconds) }

    /** Encoder bit rate in bps; 0 returns to the negotiated rate. */
    fun setBitrate(bps: Int) = perform("medut_call_set_bitrate") { Native.callSetBitrate(it, bps) }

    // ── Observability ────────────────────────────────────────────────────

    val iceState: IceState
        get() = runCatching { read("medut_call_ice_state") { IceState.of(Native.callIceState(it)) } }
            .getOrDefault(IceState.CLOSED)

    /** Facts about the call: peer, Call-ID, direction, duration.  After the
     *  call ends only state and duration are still filled in. */
    fun info(): CallInfo = read("medut_call_get_info") { h ->
        val raw = RawCallInfo()
        check(Native.callGetInfo(h, raw), "medut_call_get_info")
        CallInfo.from(raw)
    }

    /** Current media statistics. */
    fun stats(): MediaStats = read("medut_call_get_stats") { h ->
        val raw = RawStats()
        check(Native.callGetStats(h, raw), "medut_call_get_stats")
        MediaStats.from(raw)
    }

    // ── Internal ─────────────────────────────────────────────────────────

    private fun <T> read(operation: String, body: (Long) -> T): T =
        Medut.gate.read(generation, operation) { body(handle) }

    private fun perform(operation: String, allowEnded: Boolean = false, fn: (Long) -> Int) {
        read(operation) { h ->
            if (!allowEnded && isEnded) {
                throw MedutException(MedutError(MedutError.Code.CALL_ENDED), operation)
            }
            check(fn(h), operation)
        }
    }
}
