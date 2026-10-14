package dev.medut.internal

import dev.medut.MedutError
import dev.medut.MedutException
import java.lang.ref.PhantomReference
import java.lang.ref.ReferenceQueue
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

/**
 * Keeps native handles from being used across a shutdown.
 *
 * medut_shutdown() frees every account and live call, so a handle from before
 * it dangles.  Every call that passes a handle to C runs under the read lock
 * and checks the handle's generation; shutdown and account destruction take
 * the write lock, so they wait for in-flight calls and nothing starts against
 * a stack that is going away.
 *
 * The SDK's event thread must never take this lock: shutdown holds the write
 * side while medut_shutdown() joins that thread.  It reads [currentGeneration]
 * instead.
 */
internal class Gate {
    private val lock = ReentrantReadWriteLock()
    // Written only under the write lock.
    private var running = false
    private val generation = AtomicLong(0)

    val isRunning: Boolean get() = lock.read { running }

    /** The generation new handles belong to.  Safe from the event thread. */
    val currentGeneration: Long get() = generation.get()

    /** Run [body] with the stack up, on generation [expected] (any when null). */
    fun <T> read(expected: Long?, operation: String, body: () -> T): T = lock.read {
        if (!running || (expected != null && expected != generation.get())) {
            throw MedutException(MedutError(MedutError.Code.NOT_RUNNING), operation)
        }
        body()
    }

    fun <T> write(body: (Exclusive) -> T): T = lock.write { body(Exclusive()) }

    /** State access while holding the write lock. */
    inner class Exclusive {
        var running: Boolean
            get() = this@Gate.running
            set(value) { this@Gate.running = value }

        /** Start a new generation; handles from earlier ones stop working. */
        fun advanceGeneration(): Long = generation.incrementAndGet()

        fun isCurrent(g: Long): Boolean = this@Gate.running && generation.get() == g
    }
}

/**
 * Releases owned events when the object holding them becomes unreachable.
 * An ended Call holds its terminal event this way, because that event owns the
 * native call's last reference.  A daemon thread drains the reference queue;
 * java.lang.ref.Cleaner would do the same but needs API 33 on Android.
 */
internal object EventReleaser {
    private val queue = ReferenceQueue<Any>()
    // Strong refs to the phantoms themselves until they are enqueued.
    private val pending = java.util.Collections.newSetFromMap(
        java.util.concurrent.ConcurrentHashMap<Ref, Boolean>())

    private class Ref(owner: Any, val event: Long) : PhantomReference<Any>(owner, queue)

    init {
        Thread({
            while (true) {
                try {
                    val ref = queue.remove() as Ref
                    pending.remove(ref)
                    Native.eventRelease(ref.event)
                } catch (_: InterruptedException) {
                    // Keep draining; the thread lives as long as the process.
                }
            }
        }, "medut-event-releaser").apply {
            isDaemon = true
            start()
        }
    }

    fun releaseWhenUnreachable(owner: Any, event: Long) {
        pending.add(Ref(owner, event))
    }
}
