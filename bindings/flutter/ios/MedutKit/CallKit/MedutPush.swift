#if os(iOS)
import Foundation

/// Which payload fields carry the caller, for `MedutPush.wake`.
public struct PushPayloadKeys: Sendable {
    public var callerName = "caller_name"
    public var callerURI = "caller_uri"
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
///  4. The INVITE arrives and takes over the CallKit call: the caller shown
///     is updated from the SIP From header, and an Answer the user already
///     tapped is carried out.  No INVITE in time ends the call as unanswered.
///
/// None of this needs a Flutter engine or any UI to be up.
public enum MedutPush {
    public static var payloadKeys = PushPayloadKeys()

    /// Handle a VoIP push that announces an incoming call.  Call it from
    /// `pushRegistry(_:didReceiveIncomingPushWith:for:completion:)` and pass
    /// that method's `completion`: it runs once CallKit has the call.
    ///
    /// Needs the stack started with `StartOptions.callKit`; without it the
    /// accounts are still made reachable, but iOS will end the app for not
    /// reporting a call.
    public static func wake(payload: [AnyHashable: Any],
                            waitForCallSeconds: TimeInterval = 15,
                            completion: @escaping () -> Void) {
        guard Medut.shared.isRunning else {
            NSLog("Medut: push arrived but the stack is not running; start it in didFinishLaunching")
            completion()
            return
        }
        let keys = payloadKeys
        let name = string(payload, keys.callerName, container: keys.container)
        let uri = string(payload, keys.callerURI, container: keys.container)

        guard MedutCallKit.shared.isEnabled else {
            NSLog("Medut: push without CallKit enabled; re-registering only")
            reachAccounts()
            completion()
            return
        }
        MedutCallKit.shared.reportPushIncoming(callerName: name, callerURI: uri,
                                               timeout: waitForCallSeconds) { error in
            if let error = error {
                NSLog("Medut: CallKit refused the push call: %@", error.localizedDescription)
            }
            completion()
        }
        reachAccounts()
    }

    /// Make every account reachable again without waiting for a call: the
    /// part of `wake` an app wants on a token change or a foreground return.
    public static func reachAccounts() {
        // The process may have been suspended on a network that is gone;
        // coalesced and cheap when nothing changed.
        try? Medut.shared.network.changed()
        for account in Medut.shared.accounts {
            do {
                switch account.registrationState {
                case .registered: try account.keepaliveNow()
                case .reconnecting, .failed: try account.retryNow()
                case .unregistered: try account.register()
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

    public static func updatePushToken(hex: String) {
        for account in Medut.shared.accounts {
            try? account.setPushToken(hex)
        }
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
