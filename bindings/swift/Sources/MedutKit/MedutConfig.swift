import Foundation
import Medut

/// One STUN or TURN server, in the URL grammar browsers accept:
/// `stun:host[:port]`, `stuns:`, `turn:host[:port][?transport=udp|tcp]`,
/// `turns:`.  `username`/`password` are TURN long-term credentials.
public struct ICEServer: Equatable, Sendable {
    public var url: String
    public var username: String?
    public var password: String?

    public init(url: String, username: String? = nil, password: String? = nil) {
        self.url = url
        self.username = username
        self.password = password
    }
}

/// Opus encoder settings (medut_opus_config_t).
public struct OpusConfig: Equatable, Sendable {
    /// Target bit rate in bps; 0 lets Opus pick.
    public var bitrate: Int32
    /// 0-10 CPU trade-off; -1 is the Opus default.
    public var complexity: Int32
    public var constantBitrate: Bool
    public var discontinuousTransmission: Bool
    public var forwardErrorCorrection: Bool
    public var stereo: Bool

    /// The SDK defaults (from medut_config_init()).
    public init() {
        self.init(MedutConfig.cDefaults.opus)
    }

    init(_ c: medut_opus_config_t) {
        bitrate = c.bitrate
        complexity = c.complexity
        constantBitrate = c.cbr
        discontinuousTransmission = c.dtx
        forwardErrorCorrection = c.fec
        stereo = c.stereo
    }

    var c: medut_opus_config_t {
        medut_opus_config_t(bitrate: bitrate, complexity: complexity,
                            cbr: constantBitrate, dtx: discontinuousTransmission,
                            fec: forwardErrorCorrection, stereo: stereo)
    }
}

/// Stack-wide configuration, applied once by `Medut.start(_:)`.
///
/// Field for field this is medut_config_t; see medut.h for what each one does.
/// A fresh value holds the C library's own defaults (read from
/// medut_config_init()), so this binding never carries a second copy of them.
public struct MedutConfig: Sendable {
    // ── Transport ────────────────────────────────────────────────────────
    public var transport: Transport
    public var localIP: String?
    public var localPort: UInt16
    public var bindInterface: String?
    public var preferIPv6: Bool
    public var sipDomain: String?
    /// Full server URL; required for WS/WSS.  Overrides transport/host/port.
    public var serverURL: String?
    public var serverHost: String?
    public var serverPort: UInt16
    public var outboundProxy: String?

    // ── TLS / WSS ────────────────────────────────────────────────────────
    /// CA bundle path.  nil uses the root bundle shipped inside the SDK, which
    /// cannot see MDM-installed roots on iOS.
    public var caCertPath: String?
    public var clientCert: String?
    public var clientKey: String?
    public var verifyServer: Bool
    public var sniHostname: String?
    public var userAgent: String?

    // ── WebSocket ────────────────────────────────────────────────────────
    public var wsOrigin: String?
    /// Extra "Header: value" lines for the WebSocket upgrade.
    public var wsExtraHeaders: [String]
    public var wsKeepaliveMs: UInt32

    // ── NAT ──────────────────────────────────────────────────────────────
    public var stunServer: String?
    public var turnServer: String?
    public var turnUser: String?
    public var turnPass: String?
    public var iceEnabled: Bool
    public var rtcpMux: Bool
    public var iceServers: [ICEServer]
    public var iceTransportPolicy: ICETransportPolicy
    public var iceGatheringTimeoutMs: UInt32
    public var iceMultipath: Bool

    // ── Media ────────────────────────────────────────────────────────────
    public var mediaEncryption: MediaEncryption
    /// Codec preference by name ("opus", "pcmu"/"ulaw", "pcma"/"alaw"); at
    /// most 8.  Empty uses the SDK default: Opus, PCMU, PCMA.
    public var audioCodecs: [String]
    public var dscpSIP: UInt8
    public var dscpRTP: UInt8

    // ── Audio processing ─────────────────────────────────────────────────
    public var aecMode: AECMode
    public var noiseSuppression: Bool
    public var autoGainControl: Bool
    public var aecSuppressionLevel: Float
    public var micGainDB: Float
    public var speakerGainDB: Float
    public var opus: OpusConfig
    /// Activate the AVAudioSession during start.  Must be false with CallKit,
    /// which owns activation; `Medut.start` forces that for
    /// `AudioSessionPolicy.callKit`.
    public var platformAudioActivate: Bool

    // ── Jitter buffer ────────────────────────────────────────────────────
    public var jitterBufferType: JitterBufferType
    public var jitterBufferMinMs: UInt32
    public var jitterBufferMaxMs: UInt32

    // ── Registration ─────────────────────────────────────────────────────
    public var regExpires: UInt32
    public var regRefreshPct: UInt32
    public var keepaliveIntervalMs: UInt32
    public var keepaliveReregister: Bool
    public var regRetryInitialMs: UInt32
    public var regRetryMaxMs: UInt32
    public var regRetryBackoff: Float
    public var regRetryMaxAttempts: UInt32
    public var regRetryJitter: Float
    public var dnsSRVFailover: Bool

    // ── SIP timers ───────────────────────────────────────────────────────
    public var sipTimerBMs: UInt32
    public var sipTimerFMs: UInt32
    public var sessionTimerEnabled: Bool
    public var sessionExpiresSeconds: UInt32
    public var sessionMinSESeconds: UInt32

    // ── Quality / observability ──────────────────────────────────────────
    public var statsIntervalMs: UInt32
    public var mosMethod: MOSMethod
    public var mosAlertThreshold: Float
    public var lossAlertThreshold: Float
    public var jitterAlertThreshold: Float

    // ── Degraded links ───────────────────────────────────────────────────
    public var rtpTimeoutSeconds: UInt32
    public var mediaStallMs: UInt32
    public var adaptiveBitrate: Bool
    public var adaptMinBitrate: UInt32
    public var adaptMaxBitrate: UInt32
    public var adaptLossDownPct: Float
    public var adaptLossUpPct: Float
    public var adaptRecoverTicks: UInt32
    public var opusExpectedLossPct: UInt32

    // ── Network handover ─────────────────────────────────────────────────
    /// Built-in interface poll.  `Medut.start` sets 0 while its own
    /// NWPathMonitor drives handover (`StartOptions.monitorNetwork`).
    public var netMonitorIntervalSeconds: UInt32
    public var netSettleMs: UInt32
    public var netReinviteCalls: Bool
    public var netHangupOnMigrationFailure: Bool
    public var netVerifyMs: UInt32
    public var netMaxAttempts: UInt32
    public var netICEHandover: ICEHandoverPolicy

    // ── Platform, tracing, logging ───────────────────────────────────────
    /// Writable directory for SDK state.  nil uses NSTemporaryDirectory().
    public var tmpDir: String?
    public var traceSIP: Bool
    public var traceSDPDiff: Bool
    public var pcapPath: String?
    public var logLevel: LogLevel

    /// The SDK defaults.
    public init() {
        let c = MedutConfig.cDefaults
        transport = Transport(c: c.transport, fallback: .udp)
        localPort = c.local_port
        preferIPv6 = c.prefer_ipv6
        serverPort = c.server_port
        verifyServer = c.verify_server
        wsExtraHeaders = []
        wsKeepaliveMs = c.ws_keepalive_ms
        iceEnabled = c.ice_enabled
        rtcpMux = c.rtcp_mux
        iceServers = []
        iceTransportPolicy = ICETransportPolicy(c: c.ice_transport_policy, fallback: .all)
        iceGatheringTimeoutMs = c.ice_gathering_timeout_ms
        iceMultipath = c.ice_multipath
        mediaEncryption = MediaEncryption(c: c.media_enc, fallback: .none)
        audioCodecs = []
        dscpSIP = c.dscp_sip
        dscpRTP = c.dscp_rtp
        aecMode = AECMode(c: c.aec_mode, fallback: .suppressor)
        noiseSuppression = c.ns
        autoGainControl = c.agc
        aecSuppressionLevel = c.aec_suppression_level
        micGainDB = c.mic_gain_db
        speakerGainDB = c.speaker_gain_db
        opus = OpusConfig(c.opus)
        platformAudioActivate = c.platform_audio_activate
        jitterBufferType = JitterBufferType(c: c.jbuf_type, fallback: .adaptive)
        jitterBufferMinMs = c.jitter_buffer_min_ms
        jitterBufferMaxMs = c.jitter_buffer_max_ms
        regExpires = c.reg_expires
        regRefreshPct = c.reg_refresh_pct
        keepaliveIntervalMs = c.keepalive_interval
        keepaliveReregister = c.keepalive_reregister
        regRetryInitialMs = c.reg_retry_initial_ms
        regRetryMaxMs = c.reg_retry_max_ms
        regRetryBackoff = c.reg_retry_backoff
        regRetryMaxAttempts = c.reg_retry_max_attempts
        regRetryJitter = c.reg_retry_jitter
        dnsSRVFailover = c.dns_srv_failover
        sipTimerBMs = c.sip_timer_b_ms
        sipTimerFMs = c.sip_timer_f_ms
        sessionTimerEnabled = c.session_timer_enabled
        sessionExpiresSeconds = c.session_expires_s
        sessionMinSESeconds = c.session_min_se_s
        statsIntervalMs = c.stats_interval_ms
        mosMethod = MOSMethod(c: c.mos_method, fallback: .eModel)
        mosAlertThreshold = c.mos_alert_threshold
        lossAlertThreshold = c.loss_alert_threshold
        jitterAlertThreshold = c.jitter_alert_threshold
        rtpTimeoutSeconds = c.rtp_timeout_s
        mediaStallMs = c.media_stall_ms
        adaptiveBitrate = c.adaptive_bitrate
        adaptMinBitrate = c.adapt_min_bitrate
        adaptMaxBitrate = c.adapt_max_bitrate
        adaptLossDownPct = c.adapt_loss_down_pct
        adaptLossUpPct = c.adapt_loss_up_pct
        adaptRecoverTicks = c.adapt_recover_ticks
        opusExpectedLossPct = c.opus_expected_loss_pct
        netMonitorIntervalSeconds = c.net_monitor_interval_s
        netSettleMs = c.net_settle_ms
        netReinviteCalls = c.net_reinvite_calls
        netHangupOnMigrationFailure = c.net_hangup_on_migration_failure
        netVerifyMs = c.net_verify_ms
        netMaxAttempts = c.net_max_attempts
        netICEHandover = ICEHandoverPolicy(c: c.net_ice_handover, fallback: .bestEffort)
        traceSIP = c.trace_sip
        traceSDPDiff = c.trace_sdp_diff
        logLevel = LogLevel(rawValue: Int(c.log_level)) ?? .warning
    }

    static var cDefaults: medut_config_t {
        var c = medut_config_t()
        medut_config_init(&c)
        return c
    }

    /// Build the C struct (strings owned by `pool`) and hand it to `body`.
    /// The event sink is filled in by the caller.
    func withCConfig<T>(_ body: (inout medut_config_t) throws -> T) rethrows -> T {
        let pool = CStringPool()
        var c = MedutConfig.cDefaults

        c.transport = transport.cValue()
        c.local_ip = pool.string(localIP)
        c.local_port = localPort
        c.bind_interface = pool.string(bindInterface)
        c.prefer_ipv6 = preferIPv6
        c.sip_domain = pool.string(sipDomain)
        c.server_url = pool.string(serverURL)
        c.server_host = pool.string(serverHost)
        c.server_port = serverPort
        c.outbound_proxy = pool.string(outboundProxy)

        c.ca_cert_path = pool.string(caCertPath)
        c.client_cert = pool.string(clientCert)
        c.client_key = pool.string(clientKey)
        c.verify_server = verifyServer
        c.sni_hostname = pool.string(sniHostname)
        c.user_agent = pool.string(userAgent)

        c.ws_origin = pool.string(wsOrigin)
        c.ws_extra_headers = pool.stringArray(wsExtraHeaders)
        c.ws_keepalive_ms = wsKeepaliveMs

        c.stun_server = pool.string(stunServer)
        c.turn_server = pool.string(turnServer)
        c.turn_user = pool.string(turnUser)
        c.turn_pass = pool.string(turnPass)
        c.ice_enabled = iceEnabled
        c.rtcp_mux = rtcpMux
        c.ice_servers = pool.iceServers(iceServers)
        c.ice_server_count = UInt32(iceServers.count)
        c.ice_transport_policy = iceTransportPolicy.cValue()
        c.ice_gathering_timeout_ms = iceGatheringTimeoutMs
        c.ice_multipath = iceMultipath

        c.media_enc = mediaEncryption.cValue()
        c.audio_codec_name_count = writeFixedCStrings(audioCodecs, into: &c.audio_codec_names,
                                                      rows: 8, width: 32)
        c.dscp_sip = dscpSIP
        c.dscp_rtp = dscpRTP

        c.aec_mode = aecMode.cValue()
        c.ns = noiseSuppression
        c.agc = autoGainControl
        c.aec_suppression_level = aecSuppressionLevel
        c.mic_gain_db = micGainDB
        c.speaker_gain_db = speakerGainDB
        c.opus = opus.c
        c.platform_audio_activate = platformAudioActivate

        c.jbuf_type = jitterBufferType.cValue()
        c.jitter_buffer_min_ms = jitterBufferMinMs
        c.jitter_buffer_max_ms = jitterBufferMaxMs

        c.reg_expires = regExpires
        c.reg_refresh_pct = regRefreshPct
        c.keepalive_interval = keepaliveIntervalMs
        c.keepalive_reregister = keepaliveReregister
        c.reg_retry_initial_ms = regRetryInitialMs
        c.reg_retry_max_ms = regRetryMaxMs
        c.reg_retry_backoff = regRetryBackoff
        c.reg_retry_max_attempts = regRetryMaxAttempts
        c.reg_retry_jitter = regRetryJitter
        c.dns_srv_failover = dnsSRVFailover

        c.sip_timer_b_ms = sipTimerBMs
        c.sip_timer_f_ms = sipTimerFMs
        c.session_timer_enabled = sessionTimerEnabled
        c.session_expires_s = sessionExpiresSeconds
        c.session_min_se_s = sessionMinSESeconds

        c.stats_interval_ms = statsIntervalMs
        c.mos_method = mosMethod.cValue()
        c.mos_alert_threshold = mosAlertThreshold
        c.loss_alert_threshold = lossAlertThreshold
        c.jitter_alert_threshold = jitterAlertThreshold

        c.rtp_timeout_s = rtpTimeoutSeconds
        c.media_stall_ms = mediaStallMs
        c.adaptive_bitrate = adaptiveBitrate
        c.adapt_min_bitrate = adaptMinBitrate
        c.adapt_max_bitrate = adaptMaxBitrate
        c.adapt_loss_down_pct = adaptLossDownPct
        c.adapt_loss_up_pct = adaptLossUpPct
        c.adapt_recover_ticks = adaptRecoverTicks
        c.opus_expected_loss_pct = opusExpectedLossPct

        c.net_monitor_interval_s = netMonitorIntervalSeconds
        c.net_settle_ms = netSettleMs
        c.net_reinvite_calls = netReinviteCalls
        c.net_hangup_on_migration_failure = netHangupOnMigrationFailure
        c.net_verify_ms = netVerifyMs
        c.net_max_attempts = netMaxAttempts
        c.net_ice_handover = netICEHandover.cValue()

        c.tmp_dir = pool.string(tmpDir ?? NSTemporaryDirectory())
        c.trace_sip = traceSIP
        c.trace_sdp_diff = traceSDPDiff
        c.pcap_path = pool.string(pcapPath)
        c.log_level = Int32(logLevel.rawValue)

        return try withExtendedLifetime(pool) { try body(&c) }
    }
}
