package dev.medut

import dev.medut.internal.Native
import dev.medut.internal.RawConfig

// The JNI layer reads these classes' fields by name (medut_jni.c,
// fill_config): renaming a property here means renaming it there.  Consumer
// ProGuard rules keep the names in minified apps.

/** The C library's own defaults (medut_config_init()), read once. */
internal object CDefaults {
    val c: RawConfig by lazy { RawConfig().also { Native.configDefaults(it) } }
}

/**
 * One STUN or TURN server, in the URL grammar browsers accept:
 * `stun:host[:port]`, `stuns:`, `turn:host[:port][?transport=udp|tcp]`,
 * `turns:`.  [username] / [password] are TURN long-term credentials.
 */
data class IceServer(
    val url: String,
    val username: String? = null,
    val password: String? = null,
)

/** Opus encoder settings (medut_opus_config_t). */
data class OpusConfig(
    /** Target bit rate in bps; 0 lets Opus pick. */
    val bitrate: Int = CDefaults.c.opus_bitrate,
    /** 0-10 CPU trade-off; -1 is the Opus default. */
    val complexity: Int = CDefaults.c.opus_complexity,
    val constantBitrate: Boolean = CDefaults.c.opus_cbr,
    val discontinuousTransmission: Boolean = CDefaults.c.opus_dtx,
    val forwardErrorCorrection: Boolean = CDefaults.c.opus_fec,
    val stereo: Boolean = CDefaults.c.opus_stereo,
)

/**
 * Stack-wide configuration, applied once by [Medut.start].
 *
 * Field for field this is medut_config_t; see medut.h for what each one does.
 * Every default comes from the C library (medut_config_init()), so this
 * binding never carries a second copy of them.  Strings left null and lists
 * left empty use the C default too.
 */
data class MedutConfig(
    // Transport
    val transport: Transport = Transport.of(CDefaults.c.transport),
    val localIp: String? = null,
    val localPort: Int = CDefaults.c.local_port,
    val bindInterface: String? = null,
    val preferIpv6: Boolean = CDefaults.c.prefer_ipv6,
    val sipDomain: String? = null,
    /** Full server URL; required for WS/WSS.  Overrides transport/host/port. */
    val serverUrl: String? = null,
    val serverHost: String? = null,
    val serverPort: Int = CDefaults.c.server_port,
    val outboundProxy: String? = null,

    // TLS / WSS
    /** CA bundle path; null uses the platform trust store. */
    val caCertPath: String? = null,
    val clientCert: String? = null,
    val clientKey: String? = null,
    val verifyServer: Boolean = CDefaults.c.verify_server,
    val sniHostname: String? = null,
    val userAgent: String? = null,

    // WebSocket
    val wsOrigin: String? = null,
    /** Extra "Header: value" lines for the WebSocket upgrade. */
    val wsExtraHeaders: List<String> = emptyList(),
    val wsKeepaliveMs: Int = CDefaults.c.ws_keepalive_ms,

    // NAT
    val stunServer: String? = null,
    val turnServer: String? = null,
    val turnUser: String? = null,
    val turnPass: String? = null,
    val iceEnabled: Boolean = CDefaults.c.ice_enabled,
    val rtcpMux: Boolean = CDefaults.c.rtcp_mux,
    val iceServers: List<IceServer> = emptyList(),
    val iceTransportPolicy: IceTransportPolicy = IceTransportPolicy.of(CDefaults.c.ice_transport_policy),
    val iceGatheringTimeoutMs: Int = CDefaults.c.ice_gathering_timeout_ms,
    val iceMultipath: Boolean = CDefaults.c.ice_multipath,

    // Media
    val mediaEncryption: MediaEncryption = MediaEncryption.of(CDefaults.c.media_enc),
    /** Codec preference by name ("opus", "pcmu"/"ulaw", "pcma"/"alaw"); at
     *  most 8.  Empty uses the SDK default: Opus, PCMU, PCMA. */
    val audioCodecs: List<String> = emptyList(),
    val dscpSip: Int = CDefaults.c.dscp_sip,
    val dscpRtp: Int = CDefaults.c.dscp_rtp,

    // Audio processing
    val aecMode: AecMode = AecMode.of(CDefaults.c.aec_mode),
    val noiseSuppression: Boolean = CDefaults.c.ns,
    val autoGainControl: Boolean = CDefaults.c.agc,
    val aecSuppressionLevel: Float = CDefaults.c.aec_suppression_level,
    val micGainDb: Float = CDefaults.c.mic_gain_db,
    val speakerGainDb: Float = CDefaults.c.speaker_gain_db,
    val opus: OpusConfig = OpusConfig(),
    /** iOS only; ignored on Android. */
    val platformAudioActivate: Boolean = CDefaults.c.platform_audio_activate,

    // Jitter buffer
    val jitterBufferType: JitterBufferType = JitterBufferType.of(CDefaults.c.jbuf_type),
    val jitterBufferMinMs: Int = CDefaults.c.jitter_buffer_min_ms,
    val jitterBufferMaxMs: Int = CDefaults.c.jitter_buffer_max_ms,

    // Registration
    val regExpires: Int = CDefaults.c.reg_expires,
    val regRefreshPct: Int = CDefaults.c.reg_refresh_pct,
    val keepaliveIntervalMs: Int = CDefaults.c.keepalive_interval,
    val keepaliveReregister: Boolean = CDefaults.c.keepalive_reregister,
    val regRetryInitialMs: Int = CDefaults.c.reg_retry_initial_ms,
    val regRetryMaxMs: Int = CDefaults.c.reg_retry_max_ms,
    val regRetryBackoff: Float = CDefaults.c.reg_retry_backoff,
    val regRetryMaxAttempts: Int = CDefaults.c.reg_retry_max_attempts,
    val regRetryJitter: Float = CDefaults.c.reg_retry_jitter,
    val dnsSrvFailover: Boolean = CDefaults.c.dns_srv_failover,

    // SIP timers
    val sipTimerBMs: Int = CDefaults.c.sip_timer_b_ms,
    val sipTimerFMs: Int = CDefaults.c.sip_timer_f_ms,
    val sessionTimerEnabled: Boolean = CDefaults.c.session_timer_enabled,
    val sessionExpiresSeconds: Int = CDefaults.c.session_expires_s,
    val sessionMinSeSeconds: Int = CDefaults.c.session_min_se_s,

    // Quality / observability
    val statsIntervalMs: Int = CDefaults.c.stats_interval_ms,
    val mosMethod: MosMethod = MosMethod.of(CDefaults.c.mos_method),
    val mosAlertThreshold: Float = CDefaults.c.mos_alert_threshold,
    val lossAlertThreshold: Float = CDefaults.c.loss_alert_threshold,
    val jitterAlertThreshold: Float = CDefaults.c.jitter_alert_threshold,

    // Degraded links
    val rtpTimeoutSeconds: Int = CDefaults.c.rtp_timeout_s,
    val mediaStallMs: Int = CDefaults.c.media_stall_ms,
    val adaptiveBitrate: Boolean = CDefaults.c.adaptive_bitrate,
    val adaptMinBitrate: Int = CDefaults.c.adapt_min_bitrate,
    val adaptMaxBitrate: Int = CDefaults.c.adapt_max_bitrate,
    val adaptLossDownPct: Float = CDefaults.c.adapt_loss_down_pct,
    val adaptLossUpPct: Float = CDefaults.c.adapt_loss_up_pct,
    val adaptRecoverTicks: Int = CDefaults.c.adapt_recover_ticks,
    val opusExpectedLossPct: Int = CDefaults.c.opus_expected_loss_pct,

    // Network handover
    /** Built-in interface poll.  The Android start sets 0, since its own
     *  ConnectivityManager callback drives handover. */
    val netMonitorIntervalSeconds: Int = CDefaults.c.net_monitor_interval_s,
    val netSettleMs: Int = CDefaults.c.net_settle_ms,
    val netReinviteCalls: Boolean = CDefaults.c.net_reinvite_calls,
    val netHangupOnMigrationFailure: Boolean = CDefaults.c.net_hangup_on_migration_failure,
    val netVerifyMs: Int = CDefaults.c.net_verify_ms,
    val netMaxAttempts: Int = CDefaults.c.net_max_attempts,
    val netIceHandover: IceHandoverPolicy = IceHandoverPolicy.of(CDefaults.c.net_ice_handover),

    // Platform, tracing, logging
    /** Writable directory for SDK state.  Required on Android; the Android
     *  start fills in the app cache dir. */
    val tmpDir: String? = null,
    val traceSip: Boolean = CDefaults.c.trace_sip,
    val traceSdpDiff: Boolean = CDefaults.c.trace_sdp_diff,
    val pcapPath: String? = null,
    val logLevel: LogLevel = LogLevel.of(CDefaults.c.log_level),
)

/** Configuration for one SIP account (medut_account_config_t). */
data class AccountConfig(
    /** Login URI: "user@host", "user@host:port" or "sip:user@host". */
    val uri: String,
    /** Digest auth password. */
    val password: String,
    /** Ignored when [serverUrl] is set (the URL scheme decides). */
    val transport: Transport = Transport.UDP,
    /** Full server URL for WS/WSS, e.g. "wss://pbx.example.com/ws". */
    val serverUrl: String? = null,
    /** null uses the host from [uri]. */
    val serverHost: String? = null,
    /** 0 uses the port from [uri], or the transport default. */
    val serverPort: Int = 0,
    val outboundProxy: String? = null,
    /** null uses the user part of [uri]. */
    val authUser: String? = null,
    val displayName: String? = null,
    val mediaEncryption: MediaEncryption = MediaEncryption.NONE,
    val iceEnabled: Boolean = false,
    /** null inherits the stack-wide setting. */
    val rtcpMux: Boolean? = null,
    val stunServer: String? = null,
    val turnServer: String? = null,
    val turnUser: String? = null,
    val turnPass: String? = null,
    /** Empty inherits the stack-wide list. */
    val iceServers: List<IceServer> = emptyList(),
    val iceTransportPolicy: IceTransportPolicy = IceTransportPolicy.ALL,
    val verifyTls: Boolean = true,
    /** RFC 8599 push parameters for the REGISTER Contact.  For FCM the token
     *  is the registration token and [pushParam] the Firebase project ID
     *  (docs/guides/push.md).  [Account.setPushToken] updates the token later. */
    val pushProvider: PushProvider = PushProvider.NONE,
    val pushToken: String? = null,
    val pushParam: String? = null,
    /** Codec preference by name; empty inherits the stack-wide list. */
    val audioCodecs: List<String> = emptyList(),
    val dtmfMode: DtmfMode = DtmfMode.RFC4733,
    /** Headers on every request this account sends ([Account.addHeader]),
     *  added by [Medut.createAccount], so a saved account keeps them. */
    val headers: Map<String, String> = emptyMap(),
    /** Headers on REGISTER only ([Account.addRegisterHeader]), e.g. a
     *  vendor's push token header. */
    val registerHeaders: Map<String, String> = emptyMap(),
)
