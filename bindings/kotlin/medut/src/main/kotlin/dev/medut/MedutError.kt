package dev.medut

import dev.medut.internal.Native

/**
 * An error from the SDK: a negative `MEDUT_ERR_*` value from the C core, a
 * positive errno from the layers under it, or one raised by this binding
 * before reaching C (the stack is not running, the call has ended, the
 * account was destroyed).
 */
data class MedutError(val code: Code, val rawValue: Int = STATE) {
    enum class Code {
        INVALID_ARGUMENT, OUT_OF_MEMORY, INVALID_STATE, DNS, TRANSPORT, AUTHENTICATION,
        SERVER_ERROR, WEBSOCKET_REJECTED, TIMEOUT, ALREADY, FORBIDDEN,
        /** The server's TLS certificate was rejected (wrong host name, expired,
         *  untrusted).  Registration stops retrying. */
        TLS_CERTIFICATE,
        /** [Medut.start] was not called, or [Medut.shutdown] already ran. */
        NOT_RUNNING,
        /** The call has ended; nothing more can be done with it. */
        CALL_ENDED,
        /** The account was destroyed. */
        ACCOUNT_DESTROYED,
        /** A positive errno from below the SDK (ENOENT, EALREADY, ...). */
        SYSTEM,
        /** A negative code this binding does not know. */
        UNKNOWN,
    }

    val message: String
        get() = when (code) {
            Code.NOT_RUNNING -> "the Medut stack is not running"
            Code.CALL_ENDED -> "the call has ended"
            Code.ACCOUNT_DESTROYED -> "the account was destroyed"
            Code.SYSTEM -> "system error $rawValue"
            else -> Native.strerror(rawValue)
        }

    companion object {
        private const val STATE = -3

        /** The error for a C return code, or null for 0. */
        fun fromRaw(rc: Int): MedutError? = if (rc == 0) null else MedutError(codeOf(rc), rc)

        private fun codeOf(rc: Int): Code = when {
            rc > 0 -> Code.SYSTEM
            else -> when (rc) {
                -1 -> Code.INVALID_ARGUMENT
                -2 -> Code.OUT_OF_MEMORY
                -3 -> Code.INVALID_STATE
                -4 -> Code.DNS
                -5 -> Code.TRANSPORT
                -6 -> Code.AUTHENTICATION
                -7 -> Code.SERVER_ERROR
                -8 -> Code.WEBSOCKET_REJECTED
                -9 -> Code.TIMEOUT
                -10 -> Code.ALREADY
                -11 -> Code.FORBIDDEN
                -12 -> Code.TLS_CERTIFICATE
                else -> Code.UNKNOWN
            }
        }
    }
}

/** Thrown by SDK methods; [operation] is the C function that failed. */
class MedutException(val error: MedutError, val operation: String) :
    RuntimeException("$operation: ${error.message} (${error.rawValue})") {
    val code: MedutError.Code get() = error.code
}

/** Throw for a non-zero C return code. */
internal fun check(rc: Int, operation: String) {
    if (rc != 0) throw MedutException(MedutError.fromRaw(rc)!!, operation)
}
