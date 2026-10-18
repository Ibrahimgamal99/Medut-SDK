package dev.medut.internal

import dev.medut.Account
import dev.medut.Call
import dev.medut.CallState
import dev.medut.Medut
import java.lang.ref.WeakReference

/**
 * Handle -> wrapper maps, so every event about a native object reaches the
 * same Kotlin object.  Used from the event thread, so it never touches
 * [Gate].
 *
 * Only an event that starts a call (INCOMING_CALL, a live CALL_STATE) or a C
 * call that returns a new handle creates a wrapper.  Every other event looks
 * the call up: after a call's terminal state, stats, ICE, quality and network
 * events about it can still be queued, and re-creating a wrapper from one of
 * them would track a dead call forever (no terminal event would ever come to
 * untrack it, so "no calls left" would never be reported again).
 *
 * Ended calls stay findable while their wrapper is reachable: it holds the
 * terminal event, which owns the native call's last reference, so as long as
 * the weak reference below is set the handle cannot have been reused by a new
 * call.
 */
internal class Registry {
    private val accounts = HashMap<Long, Account>()
    private val calls = HashMap<Long, Call>()
    private val ended = HashMap<Long, WeakReference<Call>>()

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

    /** The wrapper for a call handle, live or recently ended.  Lookup only:
     *  never creates one. */
    @Synchronized
    fun call(handle: Long): Call? {
        if (handle == 0L) return null
        return calls[handle] ?: endedCall(handle)
    }

    /**
     * The wrapper for a call handle, creating and tracking it on first sight.
     * The handle must be valid now (being delivered, or just returned by C).
     * A call that already ended (its terminal event beat the caller here)
     * is returned as it is and not tracked again.
     *
     * [fresh] is for INCOMING_CALL, which always starts a new call: any ended
     * wrapper under the same handle is a stale entry and is dropped.
     */
    @Synchronized
    fun trackCall(handle: Long, account: Account?, state: CallState, isOutgoing: Boolean,
                  remoteUri: String, remoteDisplayName: String?, fresh: Boolean = false): Call {
        calls[handle]?.let { return it }
        if (fresh) ended.remove(handle)
        else endedCall(handle)?.let { return it }
        return Call(handle, Medut.gate.currentGeneration, account, state, isOutgoing,
            remoteUri, remoteDisplayName).also { calls[handle] = it }
    }

    /**
     * For CALL_STATE: the call's wrapper, or for a handle never seen before
     * (a call another consumer of the native stack created), a new one.  A
     * live state tracks it; a terminal one does not, since nothing would
     * ever untrack it.  Reads the call's account and state directly (no
     * SIP-thread round trip), so it is safe on the event thread.
     */
    fun callForState(handle: Long, state: CallState): Call {
        synchronized(this) { call(handle)?.let { return it } }
        val account = account(Native.callGetAccount(handle))
        val current = CallState.of(Native.callGetState(handle))
        if (!state.isTerminal) {
            return trackCall(handle, account, current, isOutgoing = false,
                remoteUri = "", remoteDisplayName = null)
        }
        return Call(handle, Medut.gate.currentGeneration, account, current,
            isOutgoing = false, remoteUri = "", remoteDisplayName = null)
    }

    /**
     * Drop a call that ended.  [retained] is whether its wrapper took the
     * terminal event (only then does the handle stay valid while the wrapper
     * is reachable, so only then is it remembered as ended).  Returns
     * whether it was tracked, and whether no live calls remain.
     */
    @Synchronized
    fun untrackCall(handle: Long, call: Call, retained: Boolean): Pair<Boolean, Boolean> {
        val wasTracked = calls[handle] === call
        if (wasTracked) calls.remove(handle)
        if (retained) {
            // Forget wrappers already collected; their handles may be reused.
            ended.values.removeAll { it.get() == null }
            ended[handle] = WeakReference(call)
        }
        return wasTracked to calls.isEmpty()
    }

    @Synchronized
    fun clear() {
        accounts.clear()
        calls.clear()
        ended.clear()
    }

    // Caller holds the monitor.
    private fun endedCall(handle: Long): Call? {
        val ref = ended[handle] ?: return null
        return ref.get() ?: run { ended.remove(handle); null }
    }
}
