package dev.medut

import dev.medut.internal.Native
import dev.medut.internal.RawIceServers

/**
 * A SIP account.  Create one with [Medut.createAccount].
 *
 * Every method throws [MedutError.Code.ACCOUNT_DESTROYED] after [destroy] and
 * [MedutError.Code.NOT_RUNNING] after [Medut.shutdown].
 */
class Account internal constructor(
    internal val handle: Long,
    internal val generation: Long,
) {
    /** Stable identity, unique even when two accounts share an AOR. */
    val id: java.util.UUID = java.util.UUID.randomUUID()

    /** Address of record, "sip:user@host".  Read once, while the handle is
     *  known to be live. */
    val aor: String = Native.accountGetAor(handle).orEmpty()

    // Read under the gate's read lock, written under its write lock.
    private var destroyed = false

    override fun toString() = "Account($aor)"

    /** Current registration state, as last reported by a registration event. */
    val registrationState: RegistrationState
        get() = runCatching {
            withHandle("medut_account_get_reg_state") {
                RegistrationState.of(Native.accountGetRegState(it))
            }
        }.getOrDefault(RegistrationState.UNREGISTERED)

    /** Start registering.  Progress arrives as [MedutEvent.Registration]. */
    fun register() = perform("medut_account_register") { Native.accountRegister(it) }

    /** Unregister (REGISTER with Expires: 0). */
    fun unregister() = perform("medut_account_unregister") { Native.accountUnregister(it) }

    /** Override the stack-wide retry policy.  [maxAttempts] 0 retries forever. */
    fun setRetryPolicy(initialMs: Int, maxMs: Int, backoff: Float, maxAttempts: Int = 0) =
        perform("medut_account_set_retry_policy") {
            Native.accountSetRetryPolicy(it, initialMs, maxMs, backoff, maxAttempts)
        }

    /** Stop retrying.  An account that was reconnecting reports FAILED. */
    fun cancelRetry() = perform("medut_account_cancel_retry") { Native.accountCancelRetry(it) }

    /** Skip the backoff and re-register now. */
    fun retryNow() = perform("medut_account_retry_now") { Native.accountRetryNow(it) }

    /** Probe the registration (SIP OPTIONS) now, e.g. on a push wake before
     *  placing a call.  A failure arrives as a registration event. */
    fun keepaliveNow() = perform("medut_account_keepalive_now") { Native.accountKeepaliveNow(it) }

    /** Update the push token (FCM token refresh).  null clears the push
     *  parameters.  The account must have been created with a pushProvider. */
    fun setPushToken(token: String?) =
        perform("medut_account_set_push_token") { Native.accountSetPushToken(it, token) }

    /** Replace this account's STUN/TURN servers; reaches the next gather.
     *  Empty falls back to the stack-wide list. */
    @JvmOverloads
    fun setIceServers(servers: List<IceServer>, policy: IceTransportPolicy = IceTransportPolicy.ALL) =
        perform("medut_account_set_ice_servers") {
            Native.accountSetIceServers(it, RawIceServers(servers), policy.value)
        }

    /** Add a header to every request this account sends. */
    fun addHeader(name: String, value: String) =
        perform("medut_account_add_header") { Native.accountAddHeader(it, name, value) }

    /** Add a header to REGISTER requests only (vendor push schemes). */
    fun addRegisterHeader(name: String, value: String) =
        perform("medut_account_add_register_header") {
            Native.accountAddRegisterHeader(it, name, value)
        }

    /** RFC 3262 100rel mode.  Set before placing or answering calls. */
    fun setReliableProvisional(mode: ReliableProvisionalMode) =
        perform("medut_account_set_100rel") { Native.accountSet100rel(it, mode.value) }

    /** Place a call.  Progress arrives as [MedutEvent.CallStateChanged]. */
    fun call(uri: String): Call {
        Medut.platform?.onCallStarting()
        return withHandle("medut_call_invite") { h ->
            val out = LongArray(1)
            check(Native.callInvite(h, uri, out), "medut_call_invite")
            if (out[0] == 0L) throw MedutException(MedutError.fromRaw(-2)!!, "medut_call_invite")
            Medut.registry.trackCall(out[0], this, CallState.CALLING, isOutgoing = true,
                remoteUri = uri, remoteDisplayName = null)
        }
    }

    /** Send a SIP MESSAGE.  [contentType] null means "text/plain". */
    @JvmOverloads
    fun sendMessage(to: String, body: String, contentType: String? = null) =
        perform("medut_message_send") { Native.messageSend(it, to, body, contentType) }

    fun publishPresence(status: PresenceStatus) =
        perform("medut_account_publish_presence") { Native.accountPublishPresence(it, status.value) }

    fun subscribePresence(uri: String) =
        perform("medut_account_subscribe_presence") { Native.accountSubscribePresence(it, uri) }

    fun unsubscribePresence(uri: String) =
        perform("medut_account_unsubscribe_presence") { Native.accountUnsubscribePresence(it, uri) }

    /** Unregister and free the account, ending its calls.  Idempotent. */
    fun destroy() {
        Medut.gate.write { state ->
            if (!destroyed && state.isCurrent(generation)) {
                destroyed = true
                Native.accountDestroy(handle)
            }
        }
        Medut.registry.untrackAccount(this)
    }

    internal fun <T> withHandle(operation: String, body: (Long) -> T): T =
        Medut.gate.read(generation, operation) {
            if (destroyed) {
                throw MedutException(MedutError(MedutError.Code.ACCOUNT_DESTROYED), operation)
            }
            body(handle)
        }

    private fun perform(operation: String, fn: (Long) -> Int) {
        withHandle(operation) { check(fn(it), operation) }
    }
}
