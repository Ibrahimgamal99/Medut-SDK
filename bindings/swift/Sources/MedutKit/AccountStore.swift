import Foundation
import Security

/// Accounts that outlive the process: `Medut.shared.accountStore`.
///
/// A push can start the process with no UI, and the stack must register
/// before the INVITE arrives, so the account configs have to be on the native
/// side.  `save` keeps them in the Keychain, and `Medut.start` re-creates and
/// registers every saved account (`StartOptions.restoreAccounts`).  Apps that
/// keep their own account storage can do the same from their launch code
/// with `restore`, or never touch this.
///
/// Storage is one generic-password item holding a JSON array, accessible
/// after the first unlock so a push that lands on a locked phone can still
/// register.  Entries are keyed by the account URI; saving the same URI again
/// replaces it.  The Kotlin SDK's AccountStore is the same design.
public final class AccountStore: @unchecked Sendable {
    private let service: String
    private let accessGroup: String?
    private let lock = NSLock()

    public init(service: String = "dev.medut.accounts", accessGroup: String? = nil) {
        self.service = service
        self.accessGroup = accessGroup
    }

    /// Every saved account, in save order.
    public func list() -> [AccountConfig] { lock.locked { load() } }

    /// Save (or replace) `config`, keyed by its URI.
    public func save(_ config: AccountConfig) {
        lock.locked {
            let key = AccountStore.key(config.uri)
            store(load().filter { AccountStore.key($0.uri) != key } + [config])
        }
    }

    /// Forget the account with this URI or AOR.  Returns true if one was saved.
    @discardableResult
    public func remove(_ uri: String) -> Bool {
        lock.locked {
            let key = AccountStore.key(uri)
            let before = load()
            let after = before.filter { AccountStore.key($0.uri) != key }
            guard after.count != before.count else { return false }
            store(after)
            return true
        }
    }

    public func clear() {
        lock.locked { delete() }
    }

    /// Create every saved account on the running stack, skipping any whose
    /// AOR is already live (a reattached stack has them), and start
    /// registering them when `register` is set.  Returns the accounts created.
    @discardableResult
    public func restore(register: Bool = true) -> [Account] {
        let live = Set(Medut.shared.accounts.map { AccountStore.key($0.aor) })
        var created: [Account] = []
        for config in list() where !live.contains(AccountStore.key(config.uri)) {
            do {
                let account = try Medut.shared.createAccount(config)
                created.append(account)
                if register { try account.register() }
            } catch {
                NSLog("Medut: restore %@: %@", config.uri, String(describing: error))
            }
        }
        return created
    }

    // ── Keys ─────────────────────────────────────────────────────────────

    /// "sip:Alice@PBX.example.com:5060" and "alice@pbx.example.com" are the
    /// same account.
    static func key(_ uri: String) -> String {
        var s = uri.trimmingCharacters(in: .whitespaces).lowercased()
        for prefix in ["sips:", "sip:"] where s.hasPrefix(prefix) {
            s = String(s.dropFirst(prefix.count))
        }
        if let at = s.lastIndex(of: "@") {
            var host = String(s[s.index(after: at)...])
            if let semi = host.firstIndex(of: ";") { host = String(host[..<semi]) }
            if let colon = host.firstIndex(of: ":") { host = String(host[..<colon]) }
            s = String(s[...at]) + host
        }
        return s
    }

    // ── Keychain ─────────────────────────────────────────────────────────

    private var baseQuery: [String: Any] {
        var q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: "accounts",
        ]
        if let group = accessGroup { q[kSecAttrAccessGroup as String] = group }
        return q
    }

    private func load() -> [AccountConfig] {
        var query = baseQuery
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess, let data = item as? Data else { return [] }
        do {
            return try JSONDecoder().decode([AccountConfig].self, from: data)
        } catch {
            NSLog("Medut: accounts item unreadable, starting empty: %@", String(describing: error))
            return []
        }
    }

    private func store(_ accounts: [AccountConfig]) {
        guard let data = try? JSONEncoder().encode(accounts) else { return }
        let attributes: [String: Any] = [
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
        ]
        let status = SecItemUpdate(baseQuery as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            var add = baseQuery
            attributes.forEach { add[$0.key] = $0.value }
            let rc = SecItemAdd(add as CFDictionary, nil)
            if rc != errSecSuccess { NSLog("Medut: keychain add failed: %d", rc) }
        } else if status != errSecSuccess {
            NSLog("Medut: keychain update failed: %d", status)
        }
    }

    private func delete() {
        _ = SecItemDelete(baseQuery as CFDictionary)
    }
}

// ── Codable ───────────────────────────────────────────────────────────────
//
// Explicit keys and defaults, so a stored item written by an older build
// still loads when a field is added later.

extension ICEServer: Codable {}
extension Transport: Codable {}
extension MediaEncryption: Codable {}
extension ICETransportPolicy: Codable {}
extension PushProvider: Codable {}
extension DTMFMode: Codable {}

extension AccountConfig: Codable {
    private enum CodingKeys: String, CodingKey {
        case uri, password, transport, serverURL, serverHost, serverPort, outboundProxy
        case authUser, displayName, mediaEncryption, iceEnabled, rtcpMux
        case stunServer, turnServer, turnUser, turnPass, iceServers, iceTransportPolicy
        case verifyTLS, pushProvider, pushToken, pushParam, audioCodecs, dtmfMode
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.init(uri: try c.decode(String.self, forKey: .uri),
                  password: try c.decodeIfPresent(String.self, forKey: .password) ?? "")
        transport = try c.decodeIfPresent(Transport.self, forKey: .transport) ?? transport
        serverURL = try c.decodeIfPresent(String.self, forKey: .serverURL)
        serverHost = try c.decodeIfPresent(String.self, forKey: .serverHost)
        serverPort = try c.decodeIfPresent(UInt16.self, forKey: .serverPort) ?? 0
        outboundProxy = try c.decodeIfPresent(String.self, forKey: .outboundProxy)
        authUser = try c.decodeIfPresent(String.self, forKey: .authUser)
        displayName = try c.decodeIfPresent(String.self, forKey: .displayName)
        mediaEncryption = try c.decodeIfPresent(MediaEncryption.self, forKey: .mediaEncryption)
            ?? mediaEncryption
        iceEnabled = try c.decodeIfPresent(Bool.self, forKey: .iceEnabled) ?? false
        rtcpMux = try c.decodeIfPresent(Bool.self, forKey: .rtcpMux)
        stunServer = try c.decodeIfPresent(String.self, forKey: .stunServer)
        turnServer = try c.decodeIfPresent(String.self, forKey: .turnServer)
        turnUser = try c.decodeIfPresent(String.self, forKey: .turnUser)
        turnPass = try c.decodeIfPresent(String.self, forKey: .turnPass)
        iceServers = try c.decodeIfPresent([ICEServer].self, forKey: .iceServers) ?? []
        iceTransportPolicy = try c.decodeIfPresent(ICETransportPolicy.self, forKey: .iceTransportPolicy)
            ?? .all
        verifyTLS = try c.decodeIfPresent(Bool.self, forKey: .verifyTLS) ?? true
        pushProvider = try c.decodeIfPresent(PushProvider.self, forKey: .pushProvider) ?? .none
        pushToken = try c.decodeIfPresent(String.self, forKey: .pushToken)
        pushParam = try c.decodeIfPresent(String.self, forKey: .pushParam)
        audioCodecs = try c.decodeIfPresent([String].self, forKey: .audioCodecs) ?? []
        dtmfMode = try c.decodeIfPresent(DTMFMode.self, forKey: .dtmfMode) ?? dtmfMode
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(uri, forKey: .uri)
        try c.encode(password, forKey: .password)
        try c.encode(transport, forKey: .transport)
        try c.encodeIfPresent(serverURL, forKey: .serverURL)
        try c.encodeIfPresent(serverHost, forKey: .serverHost)
        try c.encode(serverPort, forKey: .serverPort)
        try c.encodeIfPresent(outboundProxy, forKey: .outboundProxy)
        try c.encodeIfPresent(authUser, forKey: .authUser)
        try c.encodeIfPresent(displayName, forKey: .displayName)
        try c.encode(mediaEncryption, forKey: .mediaEncryption)
        try c.encode(iceEnabled, forKey: .iceEnabled)
        try c.encodeIfPresent(rtcpMux, forKey: .rtcpMux)
        try c.encodeIfPresent(stunServer, forKey: .stunServer)
        try c.encodeIfPresent(turnServer, forKey: .turnServer)
        try c.encodeIfPresent(turnUser, forKey: .turnUser)
        try c.encodeIfPresent(turnPass, forKey: .turnPass)
        try c.encode(iceServers, forKey: .iceServers)
        try c.encode(iceTransportPolicy, forKey: .iceTransportPolicy)
        try c.encode(verifyTLS, forKey: .verifyTLS)
        try c.encode(pushProvider, forKey: .pushProvider)
        try c.encodeIfPresent(pushToken, forKey: .pushToken)
        try c.encodeIfPresent(pushParam, forKey: .pushParam)
        try c.encode(audioCodecs, forKey: .audioCodecs)
        try c.encode(dtmfMode, forKey: .dtmfMode)
    }
}
