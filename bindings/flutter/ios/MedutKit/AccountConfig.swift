import Foundation
import Medut

/// Configuration for one SIP account (medut_account_config_t).
public struct AccountConfig: Sendable {
    /// Login URI: "user@host", "user@host:port" or "sip:user@host".
    public var uri: String
    /// Digest auth password.
    public var password: String

    /// Ignored when `serverURL` is set (the URL scheme decides).
    public var transport: Transport = .udp
    /// Full server URL for WS/WSS, e.g. "wss://pbx.example.com/ws".
    public var serverURL: String?
    /// nil uses the host from `uri`.
    public var serverHost: String?
    /// 0 uses the port from `uri`, or the transport default.
    public var serverPort: UInt16 = 0
    public var outboundProxy: String?

    /// nil uses the user part of `uri`.
    public var authUser: String?
    public var displayName: String?

    public var mediaEncryption: MediaEncryption = .none
    public var iceEnabled = false
    /// nil inherits the stack-wide setting.
    public var rtcpMux: Bool?
    public var stunServer: String?
    public var turnServer: String?
    public var turnUser: String?
    public var turnPass: String?
    /// Empty inherits the stack-wide list.
    public var iceServers: [ICEServer] = []
    public var iceTransportPolicy: ICETransportPolicy = .all
    /// Check the server's TLS certificate for this account (SIP over TLS
    /// and WSS); nil follows `MedutConfig.verifyServer`.  Accounts that reach
    /// the same server and disagree get it verified.
    public var verifyTLS: Bool?

    /// RFC 8599 push parameters for the REGISTER Contact.  For APNs the
    /// token is the hex-encoded VoIP push token and `pushParam`
    /// "<Team ID>.<bundle ID>.voip" (docs/guides/push.md).
    /// `Account.setPushToken(_:)` updates the token later.
    public var pushProvider: PushProvider = .none
    public var pushToken: String?
    public var pushParam: String?

    /// Codec preference by name; empty inherits the stack-wide list.
    public var audioCodecs: [String] = []
    public var dtmfMode: DTMFMode = .rfc4733

    /// Headers on every request this account sends (`Account.addHeader`),
    /// added by `createAccount`, so a saved account keeps them.
    public var headers: [String: String] = [:]
    /// Headers on REGISTER only (`Account.addRegisterHeader`), e.g. a
    /// vendor's push token header.
    public var registerHeaders: [String: String] = [:]

    public init(uri: String, password: String) {
        self.uri = uri
        self.password = password
    }

    func withCConfig<T>(_ body: (UnsafePointer<medut_account_config_t>) throws -> T) rethrows -> T {
        let pool = CStringPool()
        var c = medut_account_config_t()

        c.uri = pool.string(uri)
        c.password = pool.string(password)
        c.transport = transport.cValue()
        c.server_url = pool.string(serverURL)
        c.server_host = pool.string(serverHost)
        c.server_port = serverPort
        c.outbound_proxy = pool.string(outboundProxy)
        c.auth_user = pool.string(authUser)
        c.display_name = pool.string(displayName)
        c.media_enc = mediaEncryption.cValue()
        c.ice_enabled = iceEnabled
        c.rtcp_mux_set = rtcpMux != nil
        c.rtcp_mux = rtcpMux ?? false
        c.stun_server = pool.string(stunServer)
        c.turn_server = pool.string(turnServer)
        c.turn_user = pool.string(turnUser)
        c.turn_pass = pool.string(turnPass)
        c.ice_servers = pool.iceServers(iceServers)
        c.ice_server_count = UInt32(iceServers.count)
        c.ice_transport_policy = iceTransportPolicy.cValue()
        c.verify_tls_set = verifyTLS != nil
        c.verify_tls = verifyTLS ?? false
        c.push_provider = pushProvider.cValue()
        c.push_token = pool.string(pushToken)
        c.push_param = pool.string(pushParam)
        c.audio_codec_name_count = writeFixedCStrings(audioCodecs, into: &c.audio_codec_names,
                                                      rows: 8, width: 32)
        c.dtmf_mode = dtmfMode.cValue()

        return try withExtendedLifetime(pool) { try withUnsafePointer(to: &c) { try body($0) } }
    }
}
