package dev.medut.internal

import dev.medut.Account
import dev.medut.Call
import dev.medut.CallState
import dev.medut.Medut

/**
 * Handle -> wrapper maps, so every event about a native object reaches the
 * same Kotlin object.  Used from the event thread, so it never touches
 * [Gate].
 */
internal class Registry {
    private val accounts = HashMap<Long, Account>()
    private val calls = HashMap<Long, Call>()

    @Synchronized fun accounts(): List<Account> = accounts.values.toList()
    @Synchronized fun calls(): List<Call> = calls.values.toList()

    @Synchronized
    fun trackAccount(handle: Long, generation: Long): Account =
        accounts.getOrPut(handle) { Account(handle, generation) }

    @Synchronized
    fun untrackAccount(account: Account) {
        if (accounts[account.handle] === account) accounts.remove(account.handle)
    }

    /**
     * Lookup only.  An account handle in an event can belong to an account
     * destroyed since the event was queued, so it is never adopted from there.
     */
    @Synchronized
    fun account(handle: Long): Account? = if (handle == 0L) null else accounts[handle]

    /** The wrapper for a live call handle, creating it on first sight.  The
     *  handle must be valid now (being delivered, or just returned by C). */
    @Synchronized
    fun trackCall(handle: Long, account: Account?, state: CallState, isOutgoing: Boolean,
                  remoteUri: String, remoteDisplayName: String?): Call =
        calls.getOrPut(handle) {
            Call(handle, Medut.gate.currentGeneration, account, state, isOutgoing,
                 remoteUri, remoteDisplayName)
        }

    /** Track a call first seen in an event that says nothing else about it.
     *  Reads the wrapper directly (no SIP-thread round trip), so it is safe on
     *  the event thread. */
    fun trackCall(handle: Long): Call {
        synchronized(this) { calls[handle]?.let { return it } }
        return trackCall(handle, account(Native.callGetAccount(handle)),
            CallState.of(Native.callGetState(handle)), isOutgoing = false,
            remoteUri = "", remoteDisplayName = null)
    }

    fun trackCallOrNull(handle: Long): Call? = if (handle == 0L) null else trackCall(handle)

    /** Drop a call that ended.  Returns true when no live calls remain. */
    @Synchronized
    fun untrackCall(handle: Long): Boolean {
        calls.remove(handle)
        return calls.isEmpty()
    }

    @Synchronized
    fun clear() {
        accounts.clear()
        calls.clear()
    }
}
