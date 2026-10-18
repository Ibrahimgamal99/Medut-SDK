import Foundation
import Medut

/// A SIP account.  Create one with `Medut.createAccount(_:)`.
///
/// Every method throws `.accountDestroyed` after `destroy()` and
/// `.notRunning` after `Medut.shutdown()`.
public final class Account: @unchecked Sendable, Hashable, Identifiable, CustomStringConvertible {
    let handle: OpaquePointer
    let generation: UInt64

    /// Stable identity, unique even when two accounts share an AOR.
    public let id = UUID()

    /// Address of record, "sip:user@host".
    public let aor: String

    // Read under the gate's read side, written under its write side.
    private var destroyed = false

    init(handle: OpaquePointer, generation: UInt64) {
        self.handle = handle
        self.generation = generation
        let size = 512
        var buf = [CChar](repeating: 0, count: size)
        self.aor = medut_account_get_aor(handle, &buf, size) == 0 ? String(cString: buf) : ""
    }

    public var description: String { "Account(\(aor))" }

    public static func == (a: Account, b: Account) -> Bool { a === b }
    public func hash(into h: inout Hasher) { h.combine(ObjectIdentifier(self)) }

    // ── Registration ─────────────────────────────────────────────────────

    /// Current registration state, as last reported by a registration event.
    public var registrationState: RegistrationState {
        (try? withHandle("medut_account_get_reg_state") {
            RegistrationState(c: medut_account_get_reg_state($0), fallback: .unregistered)
        }) ?? .unregistered
    }

    /// Start registering.  Progress arrives as `.registration` events.
    public func register() throws {
        try perform("medut_account_register") { medut_account_register($0) }
    }

    /// Refresh the registration in place: what to call when a push wakes the
    /// app.  On a registered account the REGISTER goes out from the client
    /// that holds the binding (same Call-ID, next CSeq, same Contact), so the
    /// registrar sees the binding refreshed, which is what RFC 8599 servers
    /// wait for before they release a held INVITE (s4.1.3).  `register()`
    /// starts a new client instead, and over UDP/TCP/TLS un-REGISTERs the old
    /// binding first, which some servers (OpenSIPS) see as a binding removed
    /// and forward nothing.  On an account that is not registered this is
    /// `register()`.  Progress arrives as `.registration` events.
    public func refreshRegistration() throws {
        try perform("medut_account_refresh_registration") {
            medut_account_refresh_registration($0)
        }
    }

    /// Unregister (REGISTER with Expires: 0).
    public func unregister() throws {
        try perform("medut_account_unregister") { medut_account_unregister($0) }
    }

    /// Override the stack-wide retry policy for this account.
    /// `maxAttempts` 0 retries forever.
    public func setRetryPolicy(initialMs: UInt32, maxMs: UInt32, backoff: Float,
                               maxAttempts: UInt32) throws {
        try perform("medut_account_set_retry_policy") {
            medut_account_set_retry_policy($0, initialMs, maxMs, backoff, maxAttempts)
        }
    }

    /// Stop retrying.  An account that was reconnecting reports `.failed`.
    public func cancelRetry() throws {
        try perform("medut_account_cancel_retry") { medut_account_cancel_retry($0) }
    }

    /// Skip the backoff and re-register now.
    public func retryNow() throws {
        try perform("medut_account_retry_now") { medut_account_retry_now($0) }
    }

    /// Probe the registration (SIP OPTIONS) now, e.g. on a push wake before
    /// placing a call.  A failure arrives as a registration event.
    public func keepaliveNow() throws {
        try perform("medut_account_keepalive_now") { medut_account_keepalive_now($0) }
    }

    /// Update the push token, e.g. when PushKit rotates it.  nil (or an empty
    /// string) clears the push parameters.  The account must have been
    /// created with a `pushProvider`.
    public func setPushToken(_ token: String?) throws {
        let token = (token?.isEmpty ?? true) ? nil : token
        try perform("medut_account_set_push_token") { medut_account_set_push_token($0, token) }
    }

    /// Replace this account's STUN/TURN servers; reaches the next gather
    /// (a new call, or an ICE restart).  Empty falls back to the stack-wide
    /// list.
    public func setICEServers(_ servers: [ICEServer], policy: ICETransportPolicy = .all) throws {
        let pool = CStringPool()
        let list = pool.iceServers(servers)
        try withExtendedLifetime(pool) {
            try perform("medut_account_set_ice_servers") {
                medut_account_set_ice_servers($0, list, UInt32(servers.count), policy.cValue())
            }
        }
    }

    // ── Headers ──────────────────────────────────────────────────────────

    /// Add a header to every request this account sends.
    public func addHeader(name: String, value: String) throws {
        try perform("medut_account_add_header") { medut_account_add_header($0, name, value) }
    }

    /// Add a header to REGISTER requests only (vendor push schemes).
    public func addRegisterHeader(name: String, value: String) throws {
        try perform("medut_account_add_register_header") {
            medut_account_add_register_header($0, name, value)
        }
    }

    /// RFC 3262 100rel mode.  Set before placing or answering calls.
    public func setReliableProvisional(_ mode: ReliableProvisionalMode) throws {
        try perform("medut_account_set_100rel") { medut_account_set_100rel($0, mode.cValue()) }
    }

    // ── Calls ────────────────────────────────────────────────────────────

    /// Place a call.  Progress arrives as `.callState` events on the
    /// returned call.
    public func call(_ uri: String) throws -> Call {
        #if os(iOS)
        if MedutCallKit.shared.isEnabled { try MedutCallKit.shared.checkOutgoingPermitted() }
        if Medut.shared.options.audioSession == .automatic {
            MedutAudioSession.shared.callStarting()
        }
        #endif
        let call = try withHandle("medut_call_invite") { h -> Call in
            var out: OpaquePointer?
            try check(medut_call_invite(h, uri, &out), "medut_call_invite")
            guard let ch = out else {
                throw MedutError(rawValue: MEDUT_ERR_NOMEM.rawValue, operation: "medut_call_invite")
            }
            return Medut.shared.track(call: ch, account: self, state: .calling, isOutgoing: true,
                                      remoteURI: uri, remoteDisplayName: nil)
        }
        #if os(iOS)
        if MedutCallKit.shared.isEnabled { MedutCallKit.shared.reportOutgoing(call) }
        #endif
        return call
    }

    // ── Messaging and presence ───────────────────────────────────────────

    /// Send a SIP MESSAGE.  `contentType` nil means "text/plain".
    public func sendMessage(to uri: String, body: String, contentType: String? = nil) throws {
        try perform("medut_message_send") { medut_message_send($0, uri, body, contentType) }
    }

    public func publishPresence(_ status: PresenceStatus) throws {
        try perform("medut_account_publish_presence") {
            medut_account_publish_presence($0, status.cValue())
        }
    }

    public func subscribePresence(_ uri: String) throws {
        try perform("medut_account_subscribe_presence") {
            medut_account_subscribe_presence($0, uri)
        }
    }

    public func unsubscribePresence(_ uri: String) throws {
        try perform("medut_account_unsubscribe_presence") {
            medut_account_unsubscribe_presence($0, uri)
        }
    }

    // ── Teardown ─────────────────────────────────────────────────────────

    /// Unregister and free the account, ending its calls.  Idempotent.
    public func destroy() {
        Medut.shared.gate.write { state in
            guard !destroyed, state.isCurrent(generation) else { return }
            destroyed = true
            medut_account_destroy(handle)
        }
        Medut.shared.untrack(account: self)
    }

    // ── Internal ─────────────────────────────────────────────────────────

    func withHandle<T>(_ operation: String, _ body: (OpaquePointer) throws -> T) throws -> T {
        try Medut.shared.gate.read(generation, operation) {
            if destroyed { throw MedutError(.accountDestroyed, operation: operation) }
            return try body(handle)
        }
    }

    private func perform(_ operation: String, _ fn: (OpaquePointer) -> Int32) throws {
        try withHandle(operation) { try check(fn($0), operation) }
    }
}
