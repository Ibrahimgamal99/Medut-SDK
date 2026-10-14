/// Configuration classes mirroring `medut_config_t` and
/// `medut_account_config_t`.
///
/// Fields left null keep the C library's default (medut_config_init()), so
/// the Dart side carries no copy of the defaults.
part of '../medut.dart';

/// Opus encoder tuning (`medut_opus_config_t`).
class OpusConfig {
  /// Target bits per second (VBR); default 32000, where Opus speech
  /// quality levels off.  0 lets Opus pick (~50 kbps mono).
  final int? bitrate;

  /// 0-10 CPU trade-off; -1 = opus default (9).
  final int? complexity;

  /// Constant bitrate; false = VBR (default).
  final bool? cbr;

  /// Discontinuous transmission (silence suppression).
  final bool? dtx;

  /// In-band forward error correction; default true.  Asks the peer for
  /// FEC, and lets the SDK size our own from the loss it reports (see
  /// [MedutConfig.opusExpectedLossPct]).  False turns FEC off both ways.
  final bool? fec;

  /// Stereo output; false = mono (default).
  final bool? stereo;

  const OpusConfig({
    this.bitrate,
    this.complexity,
    this.cbr,
    this.dtx,
    this.fec,
    this.stereo,
  });
}

/// One STUN or TURN server in the URL grammar browsers accept (RFC 7064 /
/// RFC 7065): `stun:host[:port]`, `stuns:…`, `turn:host[:port][?transport=udp|tcp]`,
/// `turns:…`.  [user] / [pass] are the TURN long-term credentials.
class IceServer {
  final String url;
  final String? user;
  final String? pass;
  const IceServer(this.url, {this.user, this.pass});

  IceServerMessage _toMessage() =>
      IceServerMessage(url: url, username: user, password: pass);

  @override
  String toString() => 'IceServer($url${user != null ? ', user: $user' : ''})';
}

class MedutConfig {
  // ── Transport ──────────────────────────────────────────────────────────
  final Transport? transport;
  final String? localIp;
  final int? localPort;
  final String? bindInterface;
  final bool? preferIpv6;
  final String? sipDomain;

  /// Full server URL — required for WS/WSS and custom paths, e.g.
  /// `wss://pbx.example.com:8089/ws`. Overrides [transport]/[serverHost]/
  /// [serverPort] when set.
  final String? serverUrl;
  final String? serverHost;
  final int? serverPort;
  final String? outboundProxy;

  // ── TLS / WSS ──────────────────────────────────────────────────────────
  final String? caCertPath;
  final String? clientCert;
  final String? clientKey;
  final bool? verifyServer;

  /// SNI hostname override when the proxy cert differs from the SIP domain.
  final String? sniHostname;
  final String? userAgent;

  // ── WebSocket ──────────────────────────────────────────────────────────
  final String? wsOrigin;

  /// Extra WebSocket handshake headers, each as `"Header: value"`.
  final List<String> wsExtraHeaders;

  /// WS ping interval in ms; 0 = libre default (15 s); 20000-30000 advised.
  final int? wsKeepaliveMs;

  // ── NAT ────────────────────────────────────────────────────────────────
  final String? stunServer;
  final String? turnServer;
  final String? turnUser;
  final String? turnPass;
  final bool? iceEnabled;
  final bool? rtcpMux;

  /// Deadline for ICE candidate gathering on an outgoing call, in ms.
  /// null keeps the SDK default of 2000; `0` disables the deadline
  /// and waits indefinitely.
  ///
  /// With [iceEnabled] the INVITE is not sent when you place the call — it is
  /// sent once the ICE stack finishes gathering candidates. Nothing in that
  /// stack bounds how long that takes, and one path never reports back at
  /// all, so without a deadline an outgoing call can sit in
  /// [CallState.calling] forever with no SIP message on the wire and no event
  /// to react to.
  ///
  /// On expiry the offer goes out with whatever candidates were gathered,
  /// which is what browser SIP stacks (JsSIP, SIP.js, dart-sip-ua) and pjsua
  /// do. Gathering continues: a later completion re-offers the fuller set in
  /// a re-INVITE, and a later failure is dropped rather than ending the call.
  ///
  /// The same bound applies to the re-gather of the ICE restart that migrates a
  /// call on network handover — there it is how long the call stays without
  /// audio before an offer goes out, and `0` means 3 s rather than "for ever".
  final int? iceGatheringTimeoutMs;

  /// Every STUN/TURN server to gather from, in preference order.  Empty =
  /// fold in [stunServer] / [turnServer] (TURN first).  Each server is
  /// resolved and asked on every gather, including the re-gather of an ICE
  /// restart, so credentials refreshed with [Account.setIceServers] reach the
  /// next handover.
  final List<IceServer> iceServers;

  /// Which candidates calls may use; see [IceTransportPolicy].
  final IceTransportPolicy? iceTransportPolicy;

  /// Gather on every interface the platform can pin a socket to — cellular
  /// next to Wi-Fi on a phone — so a handover offers candidates that already
  /// exist.  On Android the plugin keeps a cellular network requested while
  /// the SDK is started and hands its interface to the SDK; the modem stays
  /// attached next to Wi-Fi, at some battery and data cost.  Default false.
  final bool? iceMultipath;

  // ── Media ──────────────────────────────────────────────────────────────
  final MediaEncryption? mediaEnc;

  /// Global codec preference, most preferred first, by name: `opus`,
  /// `ulaw`/`g711u`/`pcmu`, `alaw`/`g711a`/`pcma` — the full set compiled
  /// into the library, on every platform. Matched case-insensitively; an
  /// unrecognized name is passed through to baresip, so a codec registered
  /// by a module added to the build works too.
  ///
  /// Applies to every account that does not set [AccountConfig.audioCodecs].
  /// Only the first 8 entries are used. Leaving this empty offers the
  /// default `['opus', 'ulaw', 'alaw']`. A list where nothing resolves logs
  /// a warning and leaves all codecs on offer.
  final List<String> audioCodecs;
  final int? dscpSip;
  final int? dscpRtp;

  // ── Audio processing ───────────────────────────────────────────────────

  /// Echo-cancellation backend.
  ///
  /// Desktop-only in effect: on Android and iOS the platform audio driver
  /// captures through the OS voice path (`VOICE_COMMUNICATION` /
  /// `VoiceProcessingIO`) and the echo is gone in hardware before the SDK
  /// sees a sample, so [AecMode.suppressor] is not applied there — ducking
  /// the mic on top of a working canceller only costs full duplex.
  final AecMode? aecMode;

  /// Noise suppression on the TX path. Off by default.
  ///
  /// Never stacked on the phone's own: while Android captures through
  /// `VOICE_COMMUNICATION` (or iOS through `VoiceProcessingIO`) the OS already
  /// suppresses noise, and the SDK keeps this off there even when it is true.
  /// It applies while the app owns the device (app-owned audio), on an
  /// Android device that refuses the voice preset, and on desktop.
  final bool? noiseSuppression;

  /// Automatic gain control on the TX path. Off by default. Like
  /// [noiseSuppression], never stacked on the phone's own voice path.
  final bool? autoGainControl;

  /// 0 = none .. 1 = maximum; SUPPRESSOR mode only, desktop only.
  final double? aecSuppressionLevel;

  /// Mic gain dB, clamped [-20, +20]; 0 = unity.
  final double? micGainDb;

  /// Speaker gain dB, clamped [-20, +20]; 0 = unity.
  final double? speakerGainDb;
  final OpusConfig opus;

  /// Whether the SDK activates the platform audio session while starting.
  /// iOS only; ignored everywhere else.
  ///
  /// Set `false` in a CallKit app: `CXProvider` owns activation and Apple
  /// requires the AVAudioSession be activated only from
  /// `provider(_:didActivate:)`. Left `true`, simply starting the SDK — at
  /// launch, or on a PushKit wake while CallKit is still reporting the call —
  /// takes the exclusive PlayAndRecord route out from under CallKit. The
  /// category and mode are still configured either way, so audio works as soon
  /// as CallKit activates the session.
  ///
  /// Such apps start with `audioSession: AudioSessionMode.callFramework`,
  /// which sets this to false and stops the SDK toggling activation around
  /// calls.
  final bool? platformAudioActivate;

  /// Hand the microphone and speaker to the app instead of letting the SDK
  /// open them.
  ///
  /// Applied after `medut_init()` — there is no native config field for it,
  /// the switch is the runtime call [Medut.useAppOwnedAudio]. On mobile this
  /// also starts the plugin's realtime capture/playback loops around each
  /// call; on desktop the loops are the app's own business.
  ///
  /// Use it when the platform fights the SDK's own audio drivers — Bluetooth
  /// routing that will not follow, CallKit owning the session, one-way audio.
  /// The app then owns echo cancellation too. Default `false`: the SDK-owned
  /// device is the tested path.
  final bool appOwnedAudio;

  // ── Jitter buffer ──────────────────────────────────────────────────────
  final JitterBufferType? jitterBufferType;

  /// 0 = native default (40 ms).
  final int? jitterBufferMinMs;

  /// 0 = native default (400 ms).
  final int? jitterBufferMaxMs;

  // ── Registration & retry ───────────────────────────────────────────────
  /// REGISTER expiry in seconds; null = native default (3600 s).
  final int? regExpires;

  /// Refresh at N% of expires; null = native default (75).
  final int? regRefreshPct;

  /// SIP keepalive / reachability probe interval in ms; null = SDK default
  /// (30000), 0 disables it.  Every interval of registered idle time the SDK sends an OPTIONS
  /// request to the proxy: the request refreshes the UDP NAT binding — which
  /// carrier NAT drops long before [regExpires] elapses — and its answer, or
  /// absence, is a reachability test.  Suppressed while a call is up on the
  /// account.  See [keepaliveReregister] and `Account.keepaliveNow()`.
  final int? keepaliveIntervalMs;
  final int? regRetryInitialMs;
  final int? regRetryMaxMs;
  final double? regRetryBackoff;

  /// 0 = retry forever.
  final int? regRetryMaxAttempts;

  /// Randomise each retry delay by this fraction of itself, in [0, 1].
  /// null = SDK default (0.2), 0 disables it.  Without it every device that lost the same
  /// network re-registers on the same schedule and arrives at the registrar
  /// as one burst.
  final double? regRetryJitter;

  /// Fail an outgoing INVITE that gets no response at all after this many ms
  /// (RFC 3261 Timer B).  null = SDK default (32000), 0 disables the watchdog;
  /// 8000-12000 fails fast on
  /// mobile.  Only the `calling` state is watched — once the call is
  /// `ringing` the far end is demonstrably reachable.
  final int? sipTimerBMs;

  /// Fail a REGISTER that gets no response after this many ms (Timer F).
  /// null = SDK default (32000), 0 disables the watchdog.
  final int? sipTimerFMs;

  // ── Degraded links ─────────────────────────────────────────────────────
  // Handover (netMonitorIntervalSeconds and Medut.networkChanged) covers a
  // changed local address.  These cover the other failure: the address stays
  // put and the link goes bad — one bar of signal, a saturated uplink, a cell
  // that stops forwarding packets without dropping the PDP context.  Nothing
  // in SIP notices that on its own.

  /// End a call after this many seconds with no inbound RTP; 0 = never
  /// (default).  The fatal bound.  Only sendrecv streams are checked, so a
  /// held call is never torn down.  Prefer [mediaStallMs] for a warning that
  /// keeps the call up.
  final int? rtpTimeoutSeconds;

  /// Warn after this many ms with no inbound RTP, as a [QualityAlertEvent]
  /// with `issue == QualityIssue.mediaStall`; 0 = off.  Non-fatal, and the
  /// only way a stall with no address change becomes visible at all.
  /// Requires [statsIntervalMs] > 0.
  final int? mediaStallMs;

  /// Lower the audio encoder bitrate under packet loss and raise it again on
  /// recovery, driven by the loss the peer reports over RTCP.  Applied
  /// through the codec's encoder-update path, so there is no re-INVITE and no
  /// audio gap — and no effect for a fixed-rate codec such as G.711.
  /// Default true.
  final bool? adaptiveBitrate;

  /// Adaptation floor in bps; 0 = SDK default (12000).  Raised to 20000
  /// while Opus FEC is in use (below it libopus cannot code FEC), unless the
  /// loss turns out to be congestion.
  final int? adaptMinBitrate;

  /// Adaptation ceiling in bps; 0 = SDK default (32000).
  final int? adaptMaxBitrate;

  /// Step the bitrate down above this loss percentage; 0 = default (5.0).
  final double? adaptLossDownPct;

  /// Step the bitrate up below this loss percentage; 0 = default (1.0).
  final double? adaptLossUpPct;

  /// Consecutive clean stats ticks required before a step up; 0 = default (5).
  final int? adaptRecoverTicks;

  /// Expected packet loss handed to the Opus encoder, percent; 0 (default)
  /// = switched with the link.  Sizes in-band FEC (LBRR): the encoder spends
  /// part of its budget on a redundant copy of the previous frame, and the
  /// peer rebuilds a lost frame from the packet after it.  Left at 0 with
  /// `opus.fec` on, the SDK turns our FEC on at an RTCP report of 4%+ loss
  /// (or two in a row at 2%+) and off after 10 s under 1%; while on it
  /// follows the loss (5–25%).
  /// See [MediaStats.fecActive].  A non-zero value pins it.
  final int? opusExpectedLossPct;

  /// Re-REGISTER immediately when a keepalive probe fails; default true.
  /// Without it the account stays nominally registered until the next
  /// refresh — up to [regExpires] of missed inbound calls.
  final bool? keepaliveReregister;

  /// Walk the RFC 3263 SRV target list on registration retry; default true.
  /// Ignored when [outboundProxy] is pinned, or when the server is an IP
  /// literal or a WS/WSS URL, none of which has an ordered list to walk.
  final bool? dnsSrvFailover;

  /// How to treat an ICE call whose candidates could not be re-gathered on
  /// handover; see [IceHandover].  ICE calls are normally migrated with a full
  /// ICE restart, which this does not affect.
  final IceHandover? netIceHandover;

  // ── Quality / observability ────────────────────────────────────────────
  /// Emit [MediaStatsEvent] every N ms; 0 = disabled.
  final int? statsIntervalMs;

  /// Fire [QualityAlertEvent] when MOS-LQ drops below this (3.5 advised);
  /// 0 disables.
  final double? mosAlertThreshold;

  /// Fire when TX loss % exceeds this (5.0 advised); 0 disables.
  final double? lossAlertThreshold;

  /// Fire when RX jitter ms exceeds this (40.0 advised); 0 disables.
  final double? jitterAlertThreshold;

  // ── Platform ───────────────────────────────────────────────────────────
  /// Writable temp dir for SDK state. REQUIRED on Android (use
  /// [Medut.start], which fills it with the app cache dir automatically).
  final String? tmpDir;

  // ── Tracing / logging ──────────────────────────────────────────────────
  /// Emit [SipTraceEvent] for every SIP message.
  final bool? traceSip;

  /// Emit [SdpNegotiationEvent] per negotiation.
  final bool? traceSdpDiff;

  /// Live SIP+RTP capture to this pcap file; null = off.
  final String? pcapPath;

  /// 0=err, 1=warn, 2=info, 3=debug.
  final int? logLevel;

  // ── Network handover ───────────────────────────────────────────────────
  /// Interface poll period seconds; set 0 on mobile — the plugin drives
  /// [Medut.networkChanged] from the OS connectivity callback instead.
  final int? netMonitorIntervalSeconds;

  /// Debounce before a reported network change is acted on, in ms: how
  /// long the address set must stay stable before the handover runs
  /// (transport flush, re-REGISTER, re-INVITE per call).  The SDK default
  /// is 1500; a lower value shortens both the handover gap and the time
  /// spent "reconnecting" on every return to the app, at the risk of acting
  /// on an interface that is still coming up.  null keeps the SDK default.
  final int? netSettleMs;

  /// Re-INVITE live calls onto the new address after a handover; null = SDK
  /// default (true).
  final bool? netReinviteCalls;

  /// Hang up a call whose media could not be migrated instead of leaving it
  /// up with (probably) dead audio; null = SDK default (false).
  final bool? netHangupOnMigrationFailure;

  /// How long to wait for RTP on the new path before retrying the migration,
  /// in ms; null = SDK default (4000), 0 skips the check.
  final int? netVerifyMs;

  /// Handover / re-INVITE attempts before giving up; null = SDK default (6).
  final int? netMaxAttempts;

  // ── Session timers (RFC 4028) ──────────────────────────────────────────
  /// null = SDK default (on).
  final bool? sessionTimerEnabled;

  /// null = SDK default (1800 s).
  final int? sessionExpiresSeconds;

  /// null = SDK default (90 s).
  final int? sessionMinSeSeconds;

  /// MOS model: 0 = E-model (default), 1 = simplified.
  final int? mosMethod;

  const MedutConfig({
    this.transport,
    this.localIp,
    this.localPort,
    this.bindInterface,
    this.preferIpv6,
    this.sipDomain,
    this.serverUrl,
    this.serverHost,
    this.serverPort,
    this.outboundProxy,
    this.caCertPath,
    this.clientCert,
    this.clientKey,
    this.verifyServer,
    this.sniHostname,
    this.userAgent,
    this.wsOrigin,
    this.wsExtraHeaders = const [],
    this.wsKeepaliveMs,
    this.stunServer,
    this.turnServer,
    this.turnUser,
    this.turnPass,
    this.iceEnabled,
    this.iceGatheringTimeoutMs,
    this.iceServers = const [],
    this.iceTransportPolicy,
    this.iceMultipath,
    this.rtcpMux,
    this.mediaEnc,
    this.audioCodecs = const [],
    this.dscpSip,
    this.dscpRtp,
    this.aecMode,
    this.noiseSuppression,
    this.autoGainControl,
    this.aecSuppressionLevel,
    this.micGainDb,
    this.speakerGainDb,
    this.opus = const OpusConfig(),
    this.platformAudioActivate,
    this.appOwnedAudio = false,
    this.jitterBufferType,
    this.jitterBufferMinMs,
    this.jitterBufferMaxMs,
    this.regExpires,
    this.regRefreshPct,
    this.keepaliveIntervalMs,
    this.regRetryInitialMs,
    this.regRetryMaxMs,
    this.regRetryBackoff,
    this.regRetryMaxAttempts,
    this.statsIntervalMs,
    this.mosAlertThreshold,
    this.lossAlertThreshold,
    this.jitterAlertThreshold,
    this.regRetryJitter,
    this.sipTimerBMs,
    this.sipTimerFMs,
    this.rtpTimeoutSeconds,
    this.mediaStallMs,
    this.adaptiveBitrate,
    this.adaptMinBitrate,
    this.adaptMaxBitrate,
    this.adaptLossDownPct,
    this.adaptLossUpPct,
    this.adaptRecoverTicks,
    this.opusExpectedLossPct,
    this.keepaliveReregister,
    this.dnsSrvFailover,
    this.netIceHandover,
    this.tmpDir,
    this.traceSip,
    this.traceSdpDiff,
    this.pcapPath,
    this.logLevel,
    this.netMonitorIntervalSeconds,
    this.netSettleMs,
    this.netReinviteCalls,
    this.netHangupOnMigrationFailure,
    this.netVerifyMs,
    this.netMaxAttempts,
    this.sessionTimerEnabled,
    this.sessionExpiresSeconds,
    this.sessionMinSeSeconds,
    this.mosMethod,
  });

  ConfigMessage _toMessage() => ConfigMessage(
        transport: transport?.raw,
        localIp: localIp,
        localPort: localPort,
        bindInterface: bindInterface,
        preferIpv6: preferIpv6,
        sipDomain: sipDomain,
        serverUrl: serverUrl,
        serverHost: serverHost,
        serverPort: serverPort,
        outboundProxy: outboundProxy,
        caCertPath: caCertPath,
        clientCert: clientCert,
        clientKey: clientKey,
        verifyServer: verifyServer,
        sniHostname: sniHostname,
        userAgent: userAgent,
        wsOrigin: wsOrigin,
        wsExtraHeaders: wsExtraHeaders.isEmpty ? null : wsExtraHeaders,
        wsKeepaliveMs: wsKeepaliveMs,
        stunServer: stunServer,
        turnServer: turnServer,
        turnUser: turnUser,
        turnPass: turnPass,
        iceEnabled: iceEnabled,
        rtcpMux: rtcpMux,
        iceServers: iceServers.isEmpty
            ? null
            : iceServers.map((s) => s._toMessage()).toList(),
        iceTransportPolicy: iceTransportPolicy?.raw,
        iceGatheringTimeoutMs: iceGatheringTimeoutMs,
        iceMultipath: iceMultipath,
        mediaEncryption: mediaEnc?.raw,
        audioCodecs: audioCodecs.isEmpty ? null : audioCodecs,
        dscpSip: dscpSip,
        dscpRtp: dscpRtp,
        aecMode: aecMode?.raw,
        noiseSuppression: noiseSuppression,
        autoGainControl: autoGainControl,
        aecSuppressionLevel: aecSuppressionLevel,
        micGainDb: micGainDb,
        speakerGainDb: speakerGainDb,
        opusBitrate: opus.bitrate,
        opusComplexity: opus.complexity,
        opusConstantBitrate: opus.cbr,
        opusDtx: opus.dtx,
        opusFec: opus.fec,
        opusStereo: opus.stereo,
        platformAudioActivate: platformAudioActivate,
        jitterBufferType: jitterBufferType?.raw,
        jitterBufferMinMs: jitterBufferMinMs,
        jitterBufferMaxMs: jitterBufferMaxMs,
        regExpires: regExpires,
        regRefreshPct: regRefreshPct,
        keepaliveIntervalMs: keepaliveIntervalMs,
        keepaliveReregister: keepaliveReregister,
        regRetryInitialMs: regRetryInitialMs,
        regRetryMaxMs: regRetryMaxMs,
        regRetryBackoff: regRetryBackoff,
        regRetryMaxAttempts: regRetryMaxAttempts,
        regRetryJitter: regRetryJitter,
        dnsSrvFailover: dnsSrvFailover,
        sipTimerBMs: sipTimerBMs,
        sipTimerFMs: sipTimerFMs,
        sessionTimerEnabled: sessionTimerEnabled,
        sessionExpiresSeconds: sessionExpiresSeconds,
        sessionMinSeSeconds: sessionMinSeSeconds,
        statsIntervalMs: statsIntervalMs,
        mosMethod: mosMethod,
        mosAlertThreshold: mosAlertThreshold,
        lossAlertThreshold: lossAlertThreshold,
        jitterAlertThreshold: jitterAlertThreshold,
        rtpTimeoutSeconds: rtpTimeoutSeconds,
        mediaStallMs: mediaStallMs,
        adaptiveBitrate: adaptiveBitrate,
        adaptMinBitrate: adaptMinBitrate,
        adaptMaxBitrate: adaptMaxBitrate,
        adaptLossDownPct: adaptLossDownPct,
        adaptLossUpPct: adaptLossUpPct,
        adaptRecoverTicks: adaptRecoverTicks,
        opusExpectedLossPct: opusExpectedLossPct,
        netMonitorIntervalSeconds: netMonitorIntervalSeconds,
        netSettleMs: netSettleMs,
        netReinviteCalls: netReinviteCalls,
        netHangupOnMigrationFailure: netHangupOnMigrationFailure,
        netVerifyMs: netVerifyMs,
        netMaxAttempts: netMaxAttempts,
        netIceHandover: netIceHandover?.raw,
        tmpDir: tmpDir,
        traceSip: traceSip,
        traceSdpDiff: traceSdpDiff,
        pcapPath: pcapPath,
        logLevel: logLevel,
      );
}

/// Per-account configuration (`medut_account_config_t`).
class AccountConfig {
  /// SIP transport; ignored when [serverUrl] is set (derived from scheme).
  final Transport transport;

  /// Full server URL for WS/WSS or non-standard paths:
  /// `wss://pbx.example.com:8089/ws`. Overrides transport/host/port.
  final String? serverUrl;

  /// Override the physical server address; null = host from the AOR uri.
  final String? serverHost;
  final int serverPort;

  /// Auth username; null = user part of the AOR uri.
  final String? authUser;
  final String? displayName;

  /// Outbound proxy; null = auto-derived from server.
  final String? outboundProxy;

  final MediaEncryption mediaEnc;
  final bool iceEnabled;

  /// null = inherit the global rtcp_mux setting.
  final bool? rtcpMux;
  final String? stunServer;
  final String? turnServer;
  final String? turnUser;
  final String? turnPass;

  /// Per-account STUN/TURN list; empty = the global [MedutConfig.iceServers],
  /// then the legacy single-server fields.  Replaceable at runtime with
  /// [Account.setIceServers].
  final List<IceServer> iceServers;
  final IceTransportPolicy iceTransportPolicy;
  final bool verifyTls;

  final PushProvider pushProvider;
  final String? pushToken;
  final String? pushParam;

  /// Ordered codec preference by name — `opus`, `ulaw`/`pcmu`, `alaw`/`pcma`.
  /// Empty = global config codecs, falling back to `['opus', 'ulaw', 'alaw']`
  /// when those are empty too.
  final List<String> audioCodecs;
  final DtmfMode dtmfMode;

  const AccountConfig({
    this.transport = Transport.udp,
    this.serverUrl,
    this.serverHost,
    this.serverPort = 0,
    this.authUser,
    this.displayName,
    this.outboundProxy,
    this.mediaEnc = MediaEncryption.none,
    this.iceEnabled = false,
    this.rtcpMux,
    this.stunServer,
    this.turnServer,
    this.turnUser,
    this.turnPass,
    this.iceServers = const [],
    this.iceTransportPolicy = IceTransportPolicy.all,
    this.verifyTls = true,
    this.pushProvider = PushProvider.none,
    this.pushToken,
    this.pushParam,
    this.audioCodecs = const [],
    this.dtmfMode = DtmfMode.rfc4733,
  });

  AccountConfigMessage _toMessage(String uri, String password) =>
      AccountConfigMessage(
        uri: uri,
        password: password,
        transport: transport.raw,
        serverUrl: serverUrl,
        serverHost: serverHost,
        serverPort: serverPort,
        outboundProxy: outboundProxy,
        authUser: authUser,
        displayName: displayName,
        mediaEncryption: mediaEnc.raw,
        iceEnabled: iceEnabled,
        rtcpMux: rtcpMux,
        stunServer: stunServer,
        turnServer: turnServer,
        turnUser: turnUser,
        turnPass: turnPass,
        iceServers: iceServers.map((s) => s._toMessage()).toList(),
        iceTransportPolicy: iceTransportPolicy.raw,
        verifyTls: verifyTls,
        pushProvider: pushProvider.raw,
        pushToken: pushToken,
        pushParam: pushParam,
        audioCodecs: audioCodecs,
        dtmfMode: dtmfMode.raw,
      );
}
