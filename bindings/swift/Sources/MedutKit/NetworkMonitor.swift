#if canImport(Network)
import Foundation
import Network

/// Feeds real network changes to the stack (medut_network_changed()).
///
/// Ported from the Flutter plugin (bindings/flutter/ios/Classes/MedutPlugin.swift),
/// where each filter below fixed a measured failure:
///
/// - Every forwarded report can cost a handover: a transport reset, a
///   re-REGISTER, and over WebSocket a re-INVITE of every live call.
///   NWPathMonitor fires on any path property change, repeats `.satisfied`
///   for one transition and fires again on foreground, which is exactly when a
///   push for an incoming call lands.  So only a change in the path signature
///   (reachability, primary interface, usable interfaces, interface types in
///   use) is forwarded.  The primary interface and types matter: with Wi-Fi
///   and cellular both up, a Wi-Fi -> cellular switch leaves the set of
///   available interfaces unchanged.
/// - On a freshly started stack the first report describes the network the
///   transports were just built on; forwarding it reset the transports right
///   as the first INVITE arrived.  Reports in the first 2 s are dropped (a
///   window, not "the first report", so a real change is never the one
///   swallowed).  A stack that was already running keeps them: its cached
///   connections may have died while nobody was listening.
final class NetworkMonitor {
    private let monitor = NWPathMonitor()
    private let queue = DispatchQueue(label: "dev.medut.network-monitor")
    private let reportInitialPath: Bool
    private let onChange: () -> Void
    // Touched only on `queue`.
    private var lastSignature: String?
    private var startedAt = Date()

    private static let initialReportWindow: TimeInterval = 2

    init(reportInitialPath: Bool, onChange: @escaping () -> Void) {
        self.reportInitialPath = reportInitialPath
        self.onChange = onChange
    }

    func start() {
        queue.sync { startedAt = Date() }
        monitor.pathUpdateHandler = { [weak self] path in self?.handle(path) }
        monitor.start(queue: queue)
    }

    func stop() {
        monitor.cancel()
    }

    private func handle(_ path: NWPath) {
        let signature = NetworkMonitor.signature(path)
        guard signature != lastSignature else { return }
        lastSignature = signature
        if !reportInitialPath,
           Date().timeIntervalSince(startedAt) < NetworkMonitor.initialReportWindow {
            return
        }
        onChange()
    }

    /// Everything about a path that decides whether a handover is real.  Two
    /// paths with equal signatures need no handover.
    static func signature(_ path: NWPath) -> String {
        let names = path.availableInterfaces.map { $0.name }
        // availableInterfaces is ordered best-first.
        let primary = names.first ?? "-"
        let types: [(String, NWInterface.InterfaceType)] = [
            ("wifi", .wifi), ("cell", .cellular), ("wired", .wiredEthernet),
            ("loop", .loopback), ("other", .other),
        ]
        let used = types.filter { path.usesInterfaceType($0.1) }.map { $0.0 }
        return [
            String(describing: path.status),
            primary,
            names.sorted().joined(separator: ","),
            used.joined(separator: ","),
        ].joined(separator: "|")
    }
}
#endif
