#if os(iOS)
import Foundation

/// Which payload fields carry what, for `MedutPush.wake`.  The defaults
/// match the Kotlin SDK's `PushPayloadKeys`; a server that already sends
/// other names is met by changing these, not the server.
public struct PushPayloadKeys: Sendable {
    public var callerName = "caller_name"
    public var callerURI = "caller_uri"
    /// "call" or "cancel" (`callValue` / `cancelValue`).  A push without it
    /// is a call: many servers send nothing but a wake-up.
    public var type = "type"
    public var callValue = "call"
    public var cancelValue = "cancel"
    /// SIP Call-ID of the INVITE the push announces.  When present, the
    /// INVITE with that Call-ID takes over the CallKit call the push
    /// reported, and a cancel push ends that one.
    public var callID = "call_id"
    /// A nested dictionary the keys are looked up in as well as the top
    /// level, e.g. `{"medut": {"caller_uri": ...}}`.
    public var container = "medut"

    public init() {}
}

/// Push wake-up (PushKit / APNs VoIP).  The SDK carries no PushKit
/// dependency: the app's `PKPushRegistryDelegate` calls [wake] from
/// `didReceiveIncomingPushWith`, and gives the SDK's RFC 8599 push
/// parameters to the server through `AccountConfig.pushProvider` /
/// `pushToken` / `pushParam` and `updatePushToken` on credential changes.
///
/// The flow this supports:
///
///  1. The server gets an INVITE for a device it cannot reach and sends a
///     VoIP push with the caller in the payload.
///  2. iOS starts the process; `application(_:didFinishLaunchingWithOptions:)`
///     runs `Medut.shared.start` with `StartOptions.callKit` and re-creates
///     the accounts (the account store does it), as on any launch.
///  3. The push delegate calls `wake`, which reports the call to CallKit at
///     once (iOS ends the app otherwise), makes every account reachable
///     again, and waits for the INVITE.
///  4. The INVITE arrives and takes over the CallKit call (matched by Call-ID
///     when the payload has one): the caller shown is updated from the SIP
///     From header, and an Answer the user already tapped is carried out.
///     No INVITE in time ends the call as unanswered; a cancel push ends it
///     at once.
///
/// None of this needs a Flutter engine or any UI to be up.
public enum MedutPush {
    /// Field names `wake` reads.  Set once, before the first push; safe from
    /// any thread.
    public static var payloadKeys: PushPayloadKeys {
        get { keysLock.locked { _payloadKeys } }
        set { keysLock.locked { _payloadKeys = newValue } }
    }

    private static let keysLock = NSLock()

    /// A stack younger than this has nothing a network change could have made stale.
    private static let freshStartSeconds: TimeInterval = 10
    private static var _payloadKeys = PushPayloadKeys()

    private enum Kind { case call, cancel, other }

    /// Handle a VoIP push.  Call it from
    /// `pushRegistry(_:didReceiveIncomingPushWith:for:completion:)` and pass
    /// that method's `completion`: it runs once CallKit has been told.
    ///
    /// Every push reports a call to CallKit before this returns, because
    /// iOS 13+ ends an app that does not, and stops delivering VoIP pushes
    /// to one that keeps missing.  When no call can follow (the stack is not
    /// running, the stack runs without `StartOptions.callKit`, a cancel with
    /// nothing to cancel, an unknown `type`) that call is ended at once.  An
    /// app with its own CallKit integration should not call this: report the
    /// call itself and call `reachAccounts()`.
    public static func wake(payload: [AnyHashable: Any],
                            waitForCallSeconds: TimeInterval = 15,
                            completion: @escaping () -> Void) {
        let keys = payloadKeys
        let name = string(payload, keys.callerName, container: keys.container)
        let uri = string(payload, keys.callerURI, container: keys.container)
        let callID = string(payload, keys.callID, container: keys.container)
        let kind = pushKind(string(payload, keys.type, container: keys.container), keys)
        let callKit = MedutCallKit.shared

        if kind == .cancel {
            callKit.cancelPushIncoming(callID: callID, callerName: name, callerURI: uri,
                                       completion: completion)
            return
        }
        guard Medut.shared.isRunning else {
            // The configuration is the app's, so there is nothing to start
            // the stack with here.
            NSLog("Medut: push arrived but the stack is not running; start it in "
                  + "didFinishLaunching.  Reporting and ending a placeholder call.")
            callKit.reportAndEndPushCall(callerName: name, callerURI: uri,
                                         remoteEnded: false) { _ in completion() }
            return
        }
        guard kind == .call else {
            NSLog("Medut: push of unknown type; re-registering only")
            reachAccounts()
            callKit.reportAndEndPushCall(callerName: name, callerURI: uri,
                                         remoteEnded: false) { _ in completion() }
            return
        }
        guard callKit.isEnabled else {
            NSLog("Medut: push without CallKit enabled; re-registering, and reporting "
                  + "a placeholder call that ends at once")
            reachAccounts()
            callKit.reportAndEndPushCall(callerName: name, callerURI: uri,
                                         remoteEnded: false) { _ in completion() }
            return
        }
        callKit.reportPushIncoming(callerName: name, callerURI: uri, callID: callID,
                                   timeout: waitForCallSeconds) { error in
            if let error = error {
                NSLog("Medut: CallKit refused the push call: %@", error.localizedDescription)
            }
            completion()
        }
        reachAccounts()
    }

    /// Make every account reachable again without waiting for a call: the
    /// part of `wake` an app wants on a foreground return.  Each account
    /// refreshes its registration in place (`Account.refreshRegistration()`):
    /// RFC 8599 servers release a held INVITE on the binding refresh
    /// (s4.1.3), not on an OPTIONS probe, and not on a binding removed and
    /// added again.  Accounts in a retry loop retry now.
    public static func reachAccounts() {
        // The process may have been suspended on a network that is gone;
        // coalesced and cheap when nothing changed.  Not when the stack came
        // up moments ago (the push launched the app): its transports were
        // bound to this network seconds ago, and the path check that skips a
        // needless handover only runs once every account is REGISTERED
        // (src/netmon.c), so the report would flush the transports and re-do
        // the first REGISTER while the INVITE is on its way.
        if !Medut.shared.startedWithin(freshStartSeconds) {
            try? Medut.shared.network.changed()
        }
        for account in Medut.shared.accounts {
            do {
                switch account.registrationState {
                case .registered, .unregistered: try account.refreshRegistration()
                case .reconnecting, .failed: try account.retryNow()
                case .registering, .unregistering: break
                }
            } catch {
                NSLog("Medut: %@: %@", account.aor, String(describing: error))
            }
        }
    }

    /// Apply a new PushKit VoIP token (`pushRegistry(_:didUpdate:for:)`) to
    /// every account, hex-encoded as APNs expects.
    public static func updatePushToken(_ token: Data) {
        updatePushToken(hex: token.map { String(format: "%02x", $0) }.joined())
    }

    /// Every account with a push provider sends it (the others keep it and
    /// send nothing), and the saved accounts keep it, so a process a later
    /// push starts registers with this token and not the one it replaced.
    /// An empty string clears the token, as `clearPushToken()`.
    public static func updatePushToken(hex: String) {
        apply(token: hex.isEmpty ? nil : hex)
    }

    /// The token is gone (`pushRegistry(_:didInvalidatePushTokenFor:)`, or
    /// the user turned calls off): every account drops its push parameters,
    /// and the saved accounts forget the token.
    public static func clearPushToken() {
        apply(token: nil)
    }

    private static func apply(token: String?) {
        for account in Medut.shared.accounts {
            do {
                try account.setPushToken(token)
            } catch {
                NSLog("Medut: %@: %@", account.aor, String(describing: error))
            }
        }
        Medut.shared.accountStore.updatePushToken(token)
    }

    private static func pushKind(_ value: String?, _ keys: PushPayloadKeys) -> Kind {
        guard let value = value?.lowercased() else { return .call }
        if value == keys.callValue.lowercased() { return .call }
        if value == keys.cancelValue.lowercased() { return .cancel }
        return .other
    }

    private static func string(_ payload: [AnyHashable: Any], _ key: String,
                               container: String) -> String? {
        if let s = payload[key] as? String, !s.isEmpty { return s }
        if let nested = payload[container] as? [AnyHashable: Any],
           let s = nested[key] as? String, !s.isEmpty {
            return s
        }
        return nil
    }
}
#endif
