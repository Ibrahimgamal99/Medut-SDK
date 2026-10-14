import Foundation
import os.log

/// Writes the stack's log and call events to the unified log (subsystem
/// `dev.medut`), so a process woken by a push with no UI attached still
/// leaves a trace in Console.  `StartOptions.logToSystemLog`.
enum SystemLog {
    private static let log = OSLog(subsystem: "dev.medut", category: "stack")
    private static let queue = DispatchQueue(label: "dev.medut.syslog")
    private static let lock = NSLock()
    private static var subscription: MedutSubscription?

    static func attach() {
        lock.locked {
            guard subscription == nil else { return }
            subscription = Medut.shared.addEventHandler(queue: queue) { event in
                switch event {
                case .log(let message):
                    os_log("%{public}@", log: log, type: .info,
                           message.trimmingCharacters(in: .newlines))
                case .registration(let e):
                    os_log("registration %{public}@: %{public}@%{public}@", log: log, type: .info,
                           e.account?.aor ?? "?", String(describing: e.state),
                           e.errorText.map { " (\($0))" } ?? "")
                case .incomingCall(let e):
                    os_log("incoming call from %{public}@", log: log, type: .info, e.fromURI)
                case .callState(let e):
                    os_log("call %{public}@: %{public}@%{public}@", log: log, type: .info,
                           e.call.remoteURI, String(describing: e.state),
                           e.reason.map { " (\($0))" } ?? "")
                default:
                    break
                }
            }
        }
    }
}
