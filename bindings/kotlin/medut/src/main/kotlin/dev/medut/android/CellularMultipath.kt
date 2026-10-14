package dev.medut.android

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.util.Log
import dev.medut.Medut

/**
 * A second path for ICE: keeps cellular up next to Wi-Fi while the stack
 * runs (`MedutConfig.iceMultipath`).
 *
 * Android tears the cellular data path down while Wi-Fi is the default, so
 * there is nothing to gather candidates on until the switch has already
 * happened.  `requestNetwork(TRANSPORT_CELLULAR)` keeps the radio attached
 * for as long as the request is held, and the SDK pins its extra sockets to
 * that network (`android_setsocknetwork`) through the handle reported here.
 * The modem stays attached at some battery and data cost, which is why this
 * is opt-in.  Needs `CHANGE_NETWORK_STATE` (declared by the SDK manifest).
 */
internal class CellularMultipath(context: Context) {
    private val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private var callback: ConnectivityManager.NetworkCallback? = null
    @Volatile private var ifname: String? = null

    fun start() {
        if (callback != null) return
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = report(network, cm.getLinkProperties(network))
            override fun onLinkPropertiesChanged(network: Network, lp: LinkProperties) = report(network, lp)
            override fun onLost(network: Network) = clear()
        }
        try {
            cm.requestNetwork(request, cb)
            callback = cb
        } catch (e: Exception) {
            // No CHANGE_NETWORK_STATE, or no modem: multipath stays off.
            Log.w(TAG, "requestNetwork(cellular) failed; ICE multipath off: $e")
        }
    }

    fun stop() {
        val cb = callback ?: return
        callback = null
        runCatching { cm.unregisterNetworkCallback(cb) }
        clear()
    }

    private fun report(network: Network, lp: LinkProperties?) {
        val name = lp?.interfaceName ?: return
        val previous = ifname
        if (previous != null && previous != name) forget(previous)
        ifname = name
        // The stack may be between generations (a restart): nothing to do then.
        runCatching { Medut.network.setInterfaceHandle(name, network.networkHandle) }
    }

    private fun clear() {
        val name = ifname ?: return
        ifname = null
        forget(name)
    }

    private fun forget(name: String) {
        runCatching { Medut.network.clearInterfaceHandle(name) }
    }

    private companion object {
        const val TAG = "MedutNetwork"
    }
}
