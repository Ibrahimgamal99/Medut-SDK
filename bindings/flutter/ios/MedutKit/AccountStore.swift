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
/// register.  Entries are keyed by the account URI (scheme, case and URI
/// parameters ignored, port kept); saving the same URI again replaces it.
/// The Kotlin SDK's AccountStore is the same design.
public final class AccountStore: @unchecked Sendable {
    private let service: String
    private let accessGroup: String?
    private let lock = NSLock()

    public init(service: String = "dev.medut.accounts", accessGroup: String? = nil) {
        self.service = service
        self.accessGroup = accessGroup
    }

    /// Every saved account, in save order.  Empty when the Keychain cannot
    /// be read (before the first unlock after a reboot).
    public func list() -> [AccountConfig] { lock.locked { load() ?? [] } }

    /// Save (or replace) `config`, keyed by its URI.  Not saved while the
    /// Keychain cannot be read, so an unreadable item is never overwritten
    /// with a partial list.
    public func save(_ config: AccountConfig) {
        lock.locked {
            guard let saved = load() else {
                NSLog("Medut: keychain unreadable, not saving %@", config.uri)
                return
            }
            let key = AccountStore.key(config.uri)
            store(saved.filter { AccountStore.key($0.uri) != key } + [config])
        }
    }

    /// Replace the push token of the saved account with this URI or AOR, or
    /// of every saved account when `uri` is nil, skipping any without a push
    /// provider, so a process a push starts registers with the current token.
    /// Returns how many were updated.  `MedutPush.updatePushToken` calls this.
    /// nil or an empty token clears the saved token.
    @discardableResult
    public func updatePushToken(_ token: String?, uri: String? = nil) -> Int {
        let token = (token?.isEmpty ?? true) ? nil : token
        return lock.locked {
            guard let saved = load() else { return 0 }
            let targets: Set<Int>? = uri.map { AccountStore.matching($0, in: saved) }
            var changed = 0
            var updated = saved
            for (i, config) in saved.enumerated() {
                guard config.pushProvider != .none, config.pushToken != token,
                      targets?.contains(i) ?? true else { continue }
                updated[i].pushToken = token
                changed += 1
            }
            if changed > 0 { store(updated) }
            return changed
        }
    }

    /// Forget the account with this URI or AOR.  Returns true if one was saved.
    @discardableResult
    public func remove(_ uri: String) -> Bool {
        lock.locked {
            guard let before = load() else { return false }
            let doomed = AccountStore.matching(uri, in: before)
            guard !doomed.isEmpty else { return false }
            store(before.enumerated().filter { !doomed.contains($0.offset) }.map { $0.element })
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
        // A live account's AOR carries no port, so live accounts are counted
        // per port-less key: two saved accounts on one host (different
        // ports) are both skipped only when two such accounts are live.
        var live: [String: Int] = [:]
        for account in Medut.shared.accounts {
            live[AccountStore.hostKey(account.aor), default: 0] += 1
        }
        var created: [Account] = []
        for config in list() {
            let hostKey = AccountStore.hostKey(config.uri)
            if let n = live[hostKey], n > 0 {
                live[hostKey] = n - 1
                continue
            }
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

    /// "sip:Alice@PBX.example.com:5060;transport=tls" and
    /// "alice@pbx.example.com:5060" are the same account.  The port is part
    /// of the key: two accounts for one user on one host but different ports
    /// are different accounts.
    static func key(_ uri: String) -> String {
        var s = uri.trimmingCharacters(in: .whitespaces).lowercased()
        if s.hasPrefix("<"), s.hasSuffix(">") { s = String(s.dropFirst().dropLast()) }
        for prefix in ["sips:", "sip:"] where s.hasPrefix(prefix) {
            s = String(s.dropFirst(prefix.count))
        }
        // URI parameters (";transport=tls") follow the host part.
        let hostStart = s.lastIndex(of: "@").map { s.index(after: $0) } ?? s.startIndex
        if let semi = s[hostStart...].firstIndex(of: ";") { s = String(s[..<semi]) }
        return s
    }

    /// `key` without the port, for matching an AOR ("sip:user@host", which
    /// never carries one) against saved URIs.
    static func hostKey(_ uri: String) -> String {
        let k = key(uri)
        let hostStart = k.lastIndex(of: "@").map { k.index(after: $0) } ?? k.startIndex
        let host = k[hostStart...]
        if host.hasPrefix("[") {
            // IPv6 literal: "[::1]:5060" -> "[::1]".
            guard let close = host.firstIndex(of: "]") else { return k }
            return String(k[..<k.index(after: close)])
        }
        if let colon = host.lastIndex(of: ":") { return String(k[..<colon]) }
        return k
    }

    /// Indices of the saved entries `query` (a URI or an AOR) names: the
    /// exact key, or, for a query without a port that matches nothing
    /// exactly, every entry on that user and host.
    static func matching(_ query: String, in list: [AccountConfig]) -> Set<Int> {
        let k = key(query)
        let exact = Set(list.indices.filter { key(list[$0].uri) == k })
        guard exact.isEmpty, hostKey(query) == k else { return exact }
        return Set(list.indices.filter { hostKey(list[$0].uri) == k })
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

    /// The saved list; nil when the Keychain cannot be read now, which is
    /// not the same as nothing saved (callers must not write then).
    private func load() -> [AccountConfig]? {
        var query = baseQuery
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        switch status {
        case errSecSuccess:
            break
        case errSecItemNotFound:
            return []
        case errSecInteractionNotAllowed:
            // Before the first unlock after a reboot: a push that starts the
            // process then cannot restore its accounts.
            NSLog("Medut: keychain locked (errSecInteractionNotAllowed): saved accounts "
                  + "unavailable until the device is first unlocked")
            return nil
        default:
            NSLog("Medut: keychain read failed: %d", status)
            return nil
        }
        guard let data = item as? Data else { return [] }
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

// Synthesis is only available in the declaring file, so this one is spelled
// out.
extension ICEServer: Codable {
    private enum CodingKeys: String, CodingKey { case url, username, password }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.init(url: try c.decode(String.self, forKey: .url),
                  username: try c.decodeIfPresent(String.self, forKey: .username),
                  password: try c.decodeIfPresent(String.self, forKey: .password))
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(url, forKey: .url)
        try c.encodeIfPresent(username, forKey: .username)
        try c.encodeIfPresent(password, forKey: .password)
    }
}

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
        case headers, registerHeaders
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
        headers = try c.decodeIfPresent([String: String].self, forKey: .headers) ?? [:]
        registerHeaders = try c.decodeIfPresent([String: String].self, forKey: .registerHeaders) ?? [:]
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
        try c.encode(headers, forKey: .headers)
        try c.encode(registerHeaders, forKey: .registerHeaders)
    }
}
