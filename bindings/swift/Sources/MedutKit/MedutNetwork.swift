import Foundation
import Medut

/// Network handover controls: `Medut.shared.network`.
///
/// With `StartOptions.monitorNetwork` (the default) the SDK reports network
/// changes itself from NWPathMonitor, so most apps never call `changed()`.
public final class MedutNetwork: @unchecked Sendable {
    init() {}

    private var gate: StackGate { Medut.shared.gate }

    /// Tell the stack the network may have changed.  Coalesced and cheap
    /// when nothing changed; progress arrives as `.network` events.
    public func changed() throws {
        try gate.read(nil, "medut_network_changed") {
            try check(medut_network_changed(), "medut_network_changed")
        }
    }

    /// Built-in interface poll period in seconds; 0 disables it.
    public func setMonitorInterval(seconds: UInt32) throws {
        try gate.read(nil, "medut_network_set_monitor_interval") {
            try check(medut_network_set_monitor_interval(seconds),
                      "medut_network_set_monitor_interval")
        }
    }

    public func setHandoverPolicy(reinviteCalls: Bool, hangupOnFailure: Bool) throws {
        try gate.read(nil, "medut_network_set_handover_policy") {
            try check(medut_network_set_handover_policy(reinviteCalls, hangupOnFailure),
                      "medut_network_set_handover_policy")
        }
    }

    /// The local IP the stack is using; empty when there is none.
    public func localAddress() throws -> String {
        try gate.read(nil, "medut_network_local_addr") {
            let size = 64
            var buf = [CChar](repeating: 0, count: size)
            try check(medut_network_local_addr(&buf, size), "medut_network_local_addr")
            return String(cString: buf)
        }
    }

    /// False while the device has no usable local address.
    public var isUp: Bool {
        (try? gate.read(nil, "medut_network_is_up") { medut_network_is_up() }) ?? false
    }

    /// Android pins multipath sockets by network handle; a no-op on iOS, which
    /// pins by interface name.  Here for API parity.
    public func setInterfaceHandle(_ interface: String, handle: UInt64) throws {
        try gate.read(nil, "medut_network_set_interface_handle") {
            try check(medut_network_set_interface_handle(interface, handle),
                      "medut_network_set_interface_handle")
        }
    }

    public func clearInterfaceHandle(_ interface: String) throws {
        try gate.read(nil, "medut_network_clear_interface_handle") {
            try check(medut_network_clear_interface_handle(interface),
                      "medut_network_clear_interface_handle")
        }
    }
}
