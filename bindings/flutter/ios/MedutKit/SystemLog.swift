import Foundation
import os.log

/// Writes the stack's log and call events to the unified log (subsystem
/// `dev.medut`), so a process woken by a push with no UI attached still
/// leaves a trace in Console.  `StartOptions.logToSystemLog`.
///
/// Registration and call lines are public; the stack's own log lines are
/// private and at debug level (they may hold SIP with credentials).
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
                    // The stack's log can carry whole SIP messages (with
                    // Authorization / digest headers) when SIP tracing is
                    // on: private, and at debug level so it is not
                    // persisted by default.
                    os_log("%{private}@", log: log, type: .debug,
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
