package dev.medut

import dev.medut.internal.Native

/**
 * Network handover controls: [Medut.network].
 *
 * `MedutAndroid.start` reports network changes itself from a
 * ConnectivityManager callback, so most apps never call [changed].
 */
class MedutNetwork internal constructor() {
    private val gate get() = Medut.gate

    /** Tell the stack the network may have changed.  Coalesced and cheap when
     *  nothing changed; progress arrives as [MedutEvent.Network]. */
    fun changed() = gate.read(null, "medut_network_changed") {
        check(Native.networkChanged(), "medut_network_changed")
    }

    /** Built-in interface poll period in seconds; 0 disables it. */
    fun setMonitorInterval(seconds: Int) = gate.read(null, "medut_network_set_monitor_interval") {
        check(Native.networkSetMonitorInterval(seconds), "medut_network_set_monitor_interval")
    }

    fun setHandoverPolicy(reinviteCalls: Boolean, hangupOnFailure: Boolean) =
        gate.read(null, "medut_network_set_handover_policy") {
            check(Native.networkSetHandoverPolicy(reinviteCalls, hangupOnFailure),
                "medut_network_set_handover_policy")
        }

    /** The local IP the stack is using; empty when there is none. */
    val localAddress: String
        get() = gate.read(null, "medut_network_local_addr") { Native.networkLocalAddr().orEmpty() }

    /** False while the device has no usable local address. */
    val isUp: Boolean
        get() = runCatching { gate.read(null, "medut_network_is_up") { Native.networkIsUp() } }
            .getOrDefault(false)

    /** Pin ICE multipath sockets for [ifname] to Android network [handle]
     *  (`Network.getNetworkHandle()`). */
    fun setInterfaceHandle(ifname: String, handle: Long) =
        gate.read(null, "medut_network_set_interface_handle") {
            check(Native.networkSetInterfaceHandle(ifname, handle), "medut_network_set_interface_handle")
        }

    fun clearInterfaceHandle(ifname: String) =
        gate.read(null, "medut_network_clear_interface_handle") {
            check(Native.networkClearInterfaceHandle(ifname), "medut_network_clear_interface_handle")
        }
}
