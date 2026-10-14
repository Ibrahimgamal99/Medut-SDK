package dev.medut.android

import android.content.Context
import android.os.PowerManager
import android.util.Log
import dev.medut.Call
import dev.medut.CallState
import dev.medut.Medut
import dev.medut.MedutEvent
import dev.medut.RegistrationState
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * Push wake-up (FCM).  The SDK carries no Firebase dependency: the app's
 * `FirebaseMessagingService` calls [wake] from `onMessageReceived`, and
 * gives the SDK's RFC 8599 push parameters to the server through
 * `AccountConfig.pushProvider` / `pushToken` / `pushParam` and
 * `Account.setPushToken` on token refresh.
 *
 * The flow this supports:
 *
 *  1. The server gets an INVITE for a device it cannot reach and sends a
 *     high-priority FCM data message.
 *  2. Android starts the process; `Application.onCreate` runs
 *     [MedutAndroid.start] and re-creates the accounts, as on any launch.
 *  3. `onMessageReceived` calls [wake], which holds a wake lock, makes every
 *     account reachable again (a keepalive probe on a registered account, a
 *     retry on one that is reconnecting or failed, a REGISTER on one that is
 *     not registered), and waits for the incoming call.
 *  4. The call arrives as [MedutEvent.IncomingCall].  With Telecom enabled
 *     ([AndroidOptions.telecom]) the system shows it; otherwise the app does,
 *     from the returned [Call].
 *
 * None of this needs a Flutter engine or any UI to be up.
 */
object MedutPush {
    private const val TAG = "MedutPush"

    /**
     * Handle a push that announces an incoming call.  Blocks the calling
     * thread (FCM's own background thread) for up to [waitForCallMs] while
     * the registration recovers and the INVITE arrives.
     *
     * @return the incoming call, or null when none arrived in time (the
     *         caller gave up, or the push was about something else).
     * @throws IllegalStateException when the stack is not running: start it
     *         from `Application.onCreate` so it is up before any push.
     */
    @JvmStatic
    @JvmOverloads
    fun wake(context: Context, waitForCallMs: Long = 10_000): Call? {
        check(Medut.isRunning) {
            "the Medut stack is not running; start it from Application.onCreate " +
                "so a push finds it up"
        }
        val pm = context.applicationContext.getSystemService(Context.POWER_SERVICE) as PowerManager
        val lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "medut:push")
        lock.acquire(waitForCallMs + 5_000)
        try {
            Medut.calls.firstOrNull { !it.isOutgoing && it.state == CallState.RINGING }?.let { return it }

            val arrived = CountDownLatch(1)
            val found = AtomicReference<Call?>()
            val subscription = Medut.addListener({ it.run() }) { event ->
                if (event is MedutEvent.IncomingCall) {
                    found.set(event.call)
                    arrived.countDown()
                }
            }
            try {
                reachAccounts()
                arrived.await(waitForCallMs, TimeUnit.MILLISECONDS)
            } finally {
                subscription.cancel()
            }
            return found.get()
        } finally {
            if (lock.isHeld) lock.release()
        }
    }

    /**
     * Make every account reachable again without waiting for a call: the
     * part of [wake] an app wants on a token refresh or a foreground return.
     */
    @JvmStatic
    fun reachAccounts() {
        // The process may have been dozing on a network that is gone;
        // coalesced and cheap when nothing changed.
        runCatching { Medut.network.changed() }
        for (account in Medut.accounts) {
            val result = runCatching {
                when (account.registrationState) {
                    RegistrationState.REGISTERED -> account.keepaliveNow()
                    RegistrationState.RECONNECTING, RegistrationState.FAILED -> account.retryNow()
                    RegistrationState.UNREGISTERED -> account.register()
                    RegistrationState.REGISTERING, RegistrationState.UNREGISTERING -> {}
                }
            }
            result.exceptionOrNull()?.let { Log.w(TAG, "${account.aor}: $it") }
        }
    }
}
