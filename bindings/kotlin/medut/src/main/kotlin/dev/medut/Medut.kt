package dev.medut

import dev.medut.internal.Gate
import dev.medut.internal.Native
import dev.medut.internal.Registry
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executor
import java.util.concurrent.Executors

/** Receives every [MedutEvent]. */
fun interface MedutListener {
    fun onEvent(event: MedutEvent)
}

/** A live listener registration.  Cancel it to stop receiving events. */
class MedutSubscription internal constructor(private val onCancel: () -> Unit) {
    @Volatile private var cancelled = false

    fun cancel() {
        if (cancelled) return
        cancelled = true
        onCancel()
    }
}

/**
 * Platform side effects of call state (Android audio mode and focus).  The
 * event-thread callbacks must not block.
 */
internal interface PlatformHooks {
    /** A call is about to be placed.  May throw to refuse it (the platform
     *  call framework has another call up). */
    fun onCallPlacing() {}
    /** A call is being placed ([call] null: it does not exist yet) or
     *  answered: make audio ready.  May block. */
    fun onCallStarting(call: Call?)
    /** An outgoing call was created. */
    fun onCallPlaced(call: Call) {}
    /** A call reported a live state.  Event thread. */
    fun onCallLive(call: Call)
    /** A call ended; [noCallsLeft] when it was the last.  Event thread. */
    fun onCallEnded(call: Call, noCallsLeft: Boolean)
    /** The stack is shutting down. */
    fun onShutdown()
    /** A monotonic clock, ns, that keeps counting while the device sleeps. */
    fun elapsedNanos(): Long = System.nanoTime()
}

/**
 * The Medut stack.  One per process, owned by native code for the process
 * lifetime: start it from `Application.onCreate` or the FCM service, and
 * leave it running.  On Android start it with `MedutAndroid.start(context)`,
 * which also wires audio focus and network monitoring.
 *
 * Thread-safe: every method can be called from any thread.  Listeners run on
 * the executor they were added with, in order for a serial executor.
 */
object Medut {
    /** Library version string. */
    @JvmStatic val version: String get() = Native.version()

    @JvmField val audio = MedutAudio()
    @JvmField val network = MedutNetwork()

    internal val gate = Gate()
    internal val registry = Registry()
    @Volatile internal var platform: PlatformHooks? = null

    private val lifecycle = Any()
    private val listeners = CopyOnWriteArrayList<Pair<Executor, MedutListener>>()

    /** Where listeners run when none is given.  The Android start sets the
     *  main thread; elsewhere it is one dedicated thread. */
    @Volatile internal var defaultExecutor: Executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "medut-listener").apply { isDaemon = true }
    }

    /** True when [start] found the stack already running in this process and
     *  took it over instead of starting it. */
    @Volatile var reattached = false
        private set

    val isRunning: Boolean get() = gate.isRunning

    /** When [start] brought the native stack up ([PlatformHooks.elapsedNanos]);
     *  0 while stopped, or when [start] took over a stack that was already
     *  running. */
    @Volatile private var startedAtNanos = 0L

    /** The stack came up less than [ms] ago, in this process: its transports
     *  are bound to the network the device is on now. */
    internal fun startedWithin(ms: Long): Boolean {
        val t = startedAtNanos
        return t != 0L && elapsedNanos() - t < ms * 1_000_000
    }

    // System.nanoTime() stops in deep sleep on Android: a stack started just
    // before the device dozed would look fresh to a push hours later.
    private fun elapsedNanos(): Long = platform?.elapsedNanos() ?: System.nanoTime()

    /**
     * Start the stack.  Throws [MedutError.Code.ALREADY] if it is running.
     *
     * If something else in the process already started the native stack,
     * this takes over its event delivery and adopts its live accounts and
     * calls; [reattached] is then true.
     */
    @JvmStatic
    @JvmOverloads
    fun start(config: MedutConfig = MedutConfig()) {
        if (config.tmpDir == null && isAndroid) {
            throw IllegalStateException(
                "On Android start the stack with MedutAndroid.start(context), " +
                    "or set MedutConfig.tmpDir to a writable directory")
        }
        synchronized(lifecycle) {
            val wasReattached = gate.write { state ->
                if (state.running) throw MedutException(MedutError.fromRaw(-10)!!, "start")
                state.advanceGeneration()
                val rc = Native.stackInit(config)
                val reattach = rc == ALREADY
                if (reattach) check(Native.setEventHandler(), "medut_set_event_handler")
                else check(rc, "medut_init")
                state.running = true
                reattach
            }
            reattached = wasReattached
            startedAtNanos = if (wasReattached) 0L else elapsedNanos()
            if (wasReattached) adoptLiveState()
        }
    }

    /**
     * Stop the stack: ends every call, unregisters and frees every account,
     * and joins the SDK's threads.  Accounts and calls from before this call
     * stop working; their methods throw [MedutError.Code.NOT_RUNNING].
     */
    @JvmStatic
    fun shutdown() {
        synchronized(lifecycle) {
            val wasRunning = gate.write { state ->
                if (!state.running) return@write false
                state.running = false
                state.advanceGeneration()
                Native.shutdown()
                true
            }
            if (!wasRunning) return
            registry.clear()
            reattached = false
            startedAtNanos = 0L
            platform?.onShutdown()
        }
    }

    /** Live accounts. */
    val accounts: List<Account> get() = registry.accounts()

    /** Live calls (not yet ended). */
    val calls: List<Call> get() = registry.calls()

    /** Create an account.  It does not register until [Account.register]. */
    @JvmStatic
    fun createAccount(config: AccountConfig): Account {
        val account = gate.read(null, "medut_account_create") {
            val out = LongArray(1)
            check(Native.accountCreate(config, out), "medut_account_create")
            if (out[0] == 0L) throw MedutException(MedutError.fromRaw(-2)!!, "medut_account_create")
            registry.trackAccount(out[0], gate.currentGeneration)
        }
        try {
            config.headers.forEach { (name, value) -> account.addHeader(name, value) }
            config.registerHeaders.forEach { (name, value) -> account.addRegisterHeader(name, value) }
        } catch (t: Throwable) {
            account.destroy()
            throw t
        }
        return account
    }

    /** The live account with this AOR ("sip:user@host"), if any. */
    @JvmStatic
    fun account(aor: String): Account? = registry.accounts().firstOrNull { it.aor == aor }

    /** Capture SIP to a Wireshark-readable pcap file.  RTP, STUN and DTLS
     *  are not captured; for those, capture on the network. */
    @JvmStatic
    fun startPcap(path: String) = gate.read(null, "medut_pcap_start") {
        check(Native.pcapStart(path), "medut_pcap_start")
    }

    @JvmStatic
    fun stopPcap() = gate.read(null, "medut_pcap_stop") {
        check(Native.pcapStop(), "medut_pcap_stop")
    }

    /** Call [listener] with every event, on [executor].  Keep the returned
     *  subscription; cancel it to stop. */
    @JvmStatic
    @JvmOverloads
    fun addListener(executor: Executor = defaultExecutor, listener: MedutListener): MedutSubscription {
        val entry = executor to listener
        listeners.add(entry)
        return MedutSubscription { listeners.remove(entry) }
    }

    /** Event thread: fan out to listeners.  A listener that throws (or an
     *  executor that refuses the task) is logged and skipped: it must not
     *  crash the app from the main looper, nor stop the other listeners or
     *  the event thread. */
    internal fun publish(event: MedutEvent) {
        for ((executor, listener) in listeners) {
            try {
                executor.execute {
                    try {
                        listener.onEvent(event)
                    } catch (t: Throwable) {
                        System.err.println("Medut: listener threw on ${event::class.simpleName}: $t")
                        t.printStackTrace()
                    }
                }
            } catch (t: Throwable) {
                System.err.println("Medut: listener executor refused ${event::class.simpleName}: $t")
            }
        }
    }

    /** Re-derive wrappers for accounts and calls another consumer created. */
    private fun adoptLiveState() {
        val generation = gate.currentGeneration
        for (h in Native.accountForeach()) registry.trackAccount(h, generation)
        for (h in Native.callForeach()) {
            val info = dev.medut.internal.RawCallInfo()
            val haveInfo = Native.callGetInfo(h, info) == 0
            registry.trackCall(
                h, registry.account(Native.callGetAccount(h)),
                CallState.of(Native.callGetState(h)),
                isOutgoing = haveInfo && info.is_outgoing,
                remoteUri = if (haveInfo) info.peer_uri.orEmpty() else "",
                remoteDisplayName = if (haveInfo) info.peer_display_name else null,
            )
        }
    }

    private const val ALREADY = -10

    private val isAndroid: Boolean =
        System.getProperty("java.vendor").orEmpty().contains("Android", ignoreCase = true)
}
