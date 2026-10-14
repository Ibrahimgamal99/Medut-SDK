package dev.medut.android

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.os.Handler
import android.os.Looper
import android.os.SystemClock

/**
 * Feeds real default-network changes to the stack.  Ported from the Flutter
 * plugin, where each rule fixed a measured failure (every report can cost a
 * transport reset, a re-REGISTER and a re-INVITE per live call):
 *
 * - Only the default network's identity counts.  Capability and link-property
 *   callbacks fire constantly on a healthy network and are not overridden.
 * - One Wi-Fi <-> cellular switch delivers onLost and onAvailable in either
 *   order; a 400 ms debounce turns the burst into one handover.
 * - On a fresh stack, reports in the first 2 s describe the network the
 *   transports were just built on, and forwarding them reset the transports
 *   as the first INVITE arrived.  They are dropped (a window, not "the first
 *   report", so a real change is never the one swallowed).  A stack that was
 *   already running keeps them: its connections may have died meanwhile.
 */
internal class NetworkMonitor(context: Context, private val onChange: () -> Unit) {
    private val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var defaultNetwork: Network? = null
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var startedAt = 0L
    private var reportInitialPath = false

    private val notify = Runnable {
        if (!reportInitialPath &&
            SystemClock.elapsedRealtime() - startedAt < INITIAL_REPORT_WINDOW_MS) return@Runnable
        onChange()
    }

    fun start(reportInitialPath: Boolean) {
        stop()
        this.reportInitialPath = reportInitialPath
        startedAt = SystemClock.elapsedRealtime()
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                if (defaultNetwork == network) return // re-reported, not new
                defaultNetwork = network
                schedule()
            }

            override fun onLost(network: Network) {
                // A replacement already arrived: the tail of a switch that
                // onAvailable reported.
                if (defaultNetwork != network) return
                defaultNetwork = null
                schedule()
            }
        }
        try {
            cm.registerDefaultNetworkCallback(cb)
            callback = cb
        } catch (_: Exception) {
            // No ACCESS_NETWORK_STATE: handover relies on the SDK's own probes.
        }
    }

    fun stop() {
        main.removeCallbacks(notify)
        defaultNetwork = null
        callback?.let { runCatching { cm.unregisterNetworkCallback(it) } }
        callback = null
    }

    private fun schedule() {
        main.removeCallbacks(notify)
        main.postDelayed(notify, COALESCE_MS)
    }

    private companion object {
        const val COALESCE_MS = 400L
        const val INITIAL_REPORT_WINDOW_MS = 2000L
    }
}
