package dev.medut

/** Everything the stack reports.  See medut.h for when each one fires. */
sealed interface MedutEvent {
    /** The call the event is about, if any. */
    val call: Call? get() = null
    /** The account the event is about, if any. */
    val account: Account? get() = call?.account

    data class Log(val message: String) : MedutEvent

    data class Registration(
        override val account: Account?,
        val state: RegistrationState,
        val error: MedutError?,
        val errorText: String?,
        /** On a RECONNECTING event with a retry armed: which attempt is next
         *  and how long until it goes out.  0 otherwise. */
        val retryAttempt: Long,
        val retryDelayMs: Long,
    ) : MedutEvent

    data class IncomingCall(
        override val account: Account?,
        override val call: Call,
        val fromUri: String,
        val displayName: String?,
    ) : MedutEvent

    data class CallStateChanged(
        override val account: Account?,
        override val call: Call,
        val state: CallState,
        val error: MedutError?,
        val reason: String?,
        /** On a terminal state: this side ended the call ([Call.hangup] /
         *  [Call.reject]), from the app or from the system: a Decline in
         *  Telecom's UI, a Bluetooth headset or a watch.  An incoming call
         *  that ends unanswered with this set was declined, not missed.
         *  False on non-terminal states. */
        val endedLocally: Boolean = false,
    ) : MedutEvent

    data class Dtmf(override val call: Call?, val digit: Char) : MedutEvent

    data class SdpNegotiation(
        override val call: Call?,
        val localSdp: String?,
        val remoteSdp: String?,
        val negotiatedCodec: String?,
        /** "NONE", "SDES" or "DTLS-SRTP". */
        val negotiatedCrypto: String?,
        val rejectedCodecs: List<String>,
        val warnings: List<String>,
    ) : MedutEvent

    data class SipTrace(
        /** RX for received, TX for sent. */
        val direction: MediaDirection,
        val transport: String,
        val remoteAddress: String,
        val message: String,
        val timestampMicros: Long,
    ) : MedutEvent

    data class MediaStatsUpdate(override val call: Call?, val stats: MediaStats) : MedutEvent

    data class RegistrarWarning(val message: String) : MedutEvent

    data class TransferRequest(
        override val account: Account?,
        /** The call that received the REFER.  Answer with
         *  [Call.acceptTransfer] or [Call.rejectTransfer] on it, exactly once. */
        override val call: Call?,
        val referToUri: String,
        /** Attended transfer (Refer-To carries Replaces). */
        val isAttended: Boolean,
        val autoFollowed: Boolean,
    ) : MedutEvent

    data class TransferFailed(
        override val account: Account?,
        /** The call we tried to transfer.  It is still up. */
        override val call: Call?,
        val reason: String?,
    ) : MedutEvent

    data class MessageWaiting(
        override val account: Account?,
        val messagesWaiting: Boolean,
        val newVoice: Long,
        val oldVoice: Long,
        val newUrgent: Long,
        val oldUrgent: Long,
        val rawBody: String?,
    ) : MedutEvent

    data class Message(
        override val account: Account?,
        val fromUri: String,
        val body: String,
        val contentType: String?,
    ) : MedutEvent

    data class Presence(
        override val account: Account?,
        val targetUri: String,
        val status: PresenceStatus,
    ) : MedutEvent

    data class QualityAlert(
        override val call: Call?,
        val issue: QualityIssue,
        val value: Float,
        val threshold: Float,
        /** The metric came back on the good side of the threshold. */
        val recovering: Boolean,
    ) : MedutEvent

    data class Network(
        val stage: NetworkStage,
        override val call: Call?,
        private val accountHandle: Account?,
        val localAddress: String,
        val attempt: Long,
        val maxAttempts: Long,
        val elapsedMs: Long,
        /** The call has live ICE and migrates with an ICE restart. */
        val ice: Boolean,
        val error: MedutError?,
    ) : MedutEvent {
        override val account: Account? get() = accountHandle ?: call?.account
    }

    data class IceStateChanged(
        override val call: Call?,
        val state: IceState,
        val previous: IceState,
        /** ICE restarts so far; 1 is the initial session. */
        val generation: Long,
        val localType: IceCandidateType,
        val remoteType: IceCandidateType,
        val localAddress: String,
        val remoteAddress: String,
        val rttMs: Float,
        val error: MedutError?,
    ) : MedutEvent

    /** An event type newer than this binding. */
    data class Unknown(val type: Int) : MedutEvent
}
