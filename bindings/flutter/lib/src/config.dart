/// Configuration classes mirroring `medut_config_t` and
/// `medut_account_config_t`.
library;

import 'dart:ffi';
import 'package:ffi/ffi.dart';

import 'enums.dart';
import 'ffi_bindings.dart' as c;

/// Opus encoder tuning (`medut_opus_config_t`).
class OpusConfig {
  /// Target bits per second (VBR); default 32000, where Opus speech
  /// quality levels off.  0 lets Opus pick (~50 kbps mono).
  final int bitrate;

  /// 0-10 CPU trade-off; -1 = opus default (9).
  final int complexity;

  /// Constant bitrate; false = VBR (default).
  final bool cbr;

  /// Discontinuous transmission (silence suppression).
  final bool dtx;

  /// In-band forward error correction; default true.  Asks the peer for
  /// FEC, and lets the SDK size our own from the loss it reports (see
  /// [MedutConfig.opusExpectedLossPct]).  False turns FEC off both ways.
  final bool fec;

  /// Stereo output; false = mono (default).
  final bool stereo;

  const OpusConfig({
    this.bitrate = 32000,
    this.complexity = -1,
    this.cbr = false,
    this.dtx = false,
    this.fec = true,
    this.stereo = false,
  });
}

/// Global stack configuration, applied once at [Medut] startup.
///
/// Everything is optional; native defaults (see medut.h) apply for any
/// field left at its Dart default.
/// One STUN or TURN server in the URL grammar browsers accept (RFC 7064 /
/// RFC 7065): `stun:host[:port]`, `stuns:…`, `turn:host[:port][?transport=udp|tcp]`,
/// `turns:…`.  [user] / [pass] are the TURN long-term credentials.
class IceServer {
  final String url;
  final String? user;
  final String? pass;
  const IceServer(this.url, {this.user, this.pass});

  @override
  String toString() => 'IceServer($url${user != null ? ', user: $user' : ''})';
}

class MedutConfig {
  // ── Transport ──────────────────────────────────────────────────────────
  final Transport transport;
  final String? localIp;
  final int localPort;
  final String? bindInterface;
  final bool preferIpv6;
  final String? sipDomain;

  /// Full server URL — required for WS/WSS and custom paths, e.g.
  /// `wss://pbx.example.com:8089/ws`. Overrides [transport]/[serverHost]/
  /// [serverPort] when set.
  final String? serverUrl;
  final String? serverHost;
  final int serverPort;
  final String? outboundProxy;

  // ── TLS / WSS ──────────────────────────────────────────────────────────
  final String? caCertPath;
  final String? clientCert;
  final String? clientKey;
  final bool verifyServer;

  /// SNI hostname override when the proxy cert differs from the SIP domain.
  final String? sniHostname;
  final String? userAgent;

  // ── WebSocket ──────────────────────────────────────────────────────────
  final String? wsOrigin;

  /// Extra WebSocket handshake headers, each as `"Header: value"`.
  final List<String> wsExtraHeaders;

  /// WS ping interval in ms; 0 = libre default (15 s); 20000-30000 advised.
  final int wsKeepaliveMs;

  // ── NAT ────────────────────────────────────────────────────────────────
  final String? stunServer;
  final String? turnServer;
  final String? turnUser;
  final String? turnPass;
  final bool iceEnabled;
  final bool rtcpMux;

  /// Deadline for ICE candidate gathering on an outgoing call, in ms.
  /// `-1` (default) keeps the SDK default of 2000; `0` disables the deadline
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
  final int iceGatheringTimeoutMs;

  /// Every STUN/TURN server to gather from, in preference order.  Empty =
  /// fold in [stunServer] / [turnServer] (TURN first).  Each server is
  /// resolved and asked on every gather, including the re-gather of an ICE
  /// restart, so credentials refreshed with [Account.setIceServers] reach the
  /// next handover.
  final List<IceServer> iceServers;

  /// Which candidates calls may use; see [IceTransportPolicy].
  final IceTransportPolicy iceTransportPolicy;

  /// Gather on every interface the platform can pin a socket to — cellular
  /// next to Wi-Fi on a phone — so a handover offers candidates that already
  /// exist.  On Android the plugin keeps a cellular network requested while
  /// the SDK is started and hands its interface to the SDK; the modem stays
  /// attached next to Wi-Fi, at some battery and data cost.  Default false.
  final bool iceMultipath;

  // ── Media ──────────────────────────────────────────────────────────────
  final MediaEncryption mediaEnc;

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
  final int dscpSip;
  final int dscpRtp;

  // ── Audio processing ───────────────────────────────────────────────────

  /// Echo-cancellation backend.
  ///
  /// Desktop-only in effect: on Android and iOS the platform audio driver
  /// captures through the OS voice path (`VOICE_COMMUNICATION` /
  /// `VoiceProcessingIO`) and the echo is gone in hardware before the SDK
  /// sees a sample, so [AecMode.suppressor] is not applied there — ducking
  /// the mic on top of a working canceller only costs full duplex.
  final AecMode aecMode;

  /// Noise suppression on the TX path. Off by default.
  ///
  /// Never stacked on the phone's own: while Android captures through
  /// `VOICE_COMMUNICATION` (or iOS through `VoiceProcessingIO`) the OS already
  /// suppresses noise, and the SDK keeps this off there even when it is true.
  /// It applies while the app owns the device (app-owned audio), on an
  /// Android device that refuses the voice preset, and on desktop.
  final bool noiseSuppression;

  /// Automatic gain control on the TX path. Off by default. Like
  /// [noiseSuppression], never stacked on the phone's own voice path.
  final bool autoGainControl;

  /// 0 = none .. 1 = maximum; SUPPRESSOR mode only, desktop only.
  final double aecSuppressionLevel;

  /// Mic gain dB, clamped [-20, +20]; 0 = unity.
  final double micGainDb;

  /// Speaker gain dB, clamped [-20, +20]; 0 = unity.
  final double speakerGainDb;
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
  /// Such apps normally also pass `manageAudioSession: false` to
  /// [Medut.start], which stops the SDK toggling activation around calls.
  final bool platformAudioActivate;

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
  final JitterBufferType jitterBufferType;

  /// 0 = native default (40 ms).
  final int jitterBufferMinMs;

  /// 0 = native default (400 ms).
  final int jitterBufferMaxMs;

  // ── Registration & retry ───────────────────────────────────────────────
  /// 0 = native default (3600 s).
  final int regExpires;

  /// Refresh at N% of expires; 0 = native default (75).
  final int regRefreshPct;

  /// SIP keepalive / reachability probe interval in ms; 0 = SDK default
  /// (30000).  Every interval of registered idle time the SDK sends an OPTIONS
  /// request to the proxy: the request refreshes the UDP NAT binding — which
  /// carrier NAT drops long before [regExpires] elapses — and its answer, or
  /// absence, is a reachability test.  Suppressed while a call is up on the
  /// account.  See [keepaliveReregister] and `Account.keepaliveNow()`.
  final int keepaliveIntervalMs;
  final int regRetryInitialMs;
  final int regRetryMaxMs;
  final double regRetryBackoff;

  /// 0 = retry forever.
  final int regRetryMaxAttempts;

  /// Randomise each retry delay by this fraction of itself, in [0, 1].
  /// 0 = SDK default (0.2).  Without it every device that lost the same
  /// network re-registers on the same schedule and arrives at the registrar
  /// as one burst.
  final double regRetryJitter;

  /// Fail an outgoing INVITE that gets no response at all after this many ms
  /// (RFC 3261 Timer B).  0 = SDK default (32000); 8000-12000 fails fast on
  /// mobile.  Only the `calling` state is watched — once the call is
  /// `ringing` the far end is demonstrably reachable.
  final int sipTimerBMs;

  /// Fail a REGISTER that gets no response after this many ms (Timer F).
  /// 0 = SDK default (32000).
  final int sipTimerFMs;

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
  final int rtpTimeoutSeconds;

  /// Warn after this many ms with no inbound RTP, as a [QualityAlertEvent]
  /// with `issue == QualityIssue.mediaStall`; 0 = off.  Non-fatal, and the
  /// only way a stall with no address change becomes visible at all.
  /// Requires [statsIntervalMs] > 0.
  final int mediaStallMs;

  /// Lower the audio encoder bitrate under packet loss and raise it again on
  /// recovery, driven by the loss the peer reports over RTCP.  Applied
  /// through the codec's encoder-update path, so there is no re-INVITE and no
  /// audio gap — and no effect for a fixed-rate codec such as G.711.
  /// Default true.
  final bool adaptiveBitrate;

  /// Adaptation floor in bps; 0 = SDK default (12000).  Raised to 20000
  /// while Opus FEC is in use (below it libopus cannot code FEC), unless the
  /// loss turns out to be congestion.
  final int adaptMinBitrate;

  /// Adaptation ceiling in bps; 0 = SDK default (32000).
  final int adaptMaxBitrate;

  /// Step the bitrate down above this loss percentage; 0 = default (5.0).
  final double adaptLossDownPct;

  /// Step the bitrate up below this loss percentage; 0 = default (1.0).
  final double adaptLossUpPct;

  /// Consecutive clean stats ticks required before a step up; 0 = default (5).
  final int adaptRecoverTicks;

  /// Expected packet loss handed to the Opus encoder, percent; 0 (default)
  /// = switched with the link.  Sizes in-band FEC (LBRR): the encoder spends
  /// part of its budget on a redundant copy of the previous frame, and the
  /// peer rebuilds a lost frame from the packet after it.  Left at 0 with
  /// `opus.fec` on, the SDK turns our FEC on at an RTCP report of 4%+ loss
  /// (or two in a row at 2%+) and off after 10 s under 1%; while on it
  /// follows the loss (5–25%).
  /// See [MediaStats.fecActive].  A non-zero value pins it.
  final int opusExpectedLossPct;

  /// Re-REGISTER immediately when a keepalive probe fails; default true.
  /// Without it the account stays nominally registered until the next
  /// refresh — up to [regExpires] of missed inbound calls.
  final bool keepaliveReregister;

  /// Walk the RFC 3263 SRV target list on registration retry; default true.
  /// Ignored when [outboundProxy] is pinned, or when the server is an IP
  /// literal or a WS/WSS URL, none of which has an ordered list to walk.
  final bool dnsSrvFailover;

  /// How to treat an ICE call whose candidates could not be re-gathered on
  /// handover; see [IceHandover].  ICE calls are normally migrated with a full
  /// ICE restart, which this does not affect.
  final IceHandover netIceHandover;

  // ── Quality / observability ────────────────────────────────────────────
  /// Emit [MediaStatsEvent] every N ms; 0 = disabled.
  final int statsIntervalMs;

  /// Fire [QualityAlertEvent] when MOS-LQ drops below this (3.5 advised);
  /// 0 disables.
  final double mosAlertThreshold;

  /// Fire when TX loss % exceeds this (5.0 advised); 0 disables.
  final double lossAlertThreshold;

  /// Fire when RX jitter ms exceeds this (40.0 advised); 0 disables.
  final double jitterAlertThreshold;

  // ── Platform ───────────────────────────────────────────────────────────
  /// Writable temp dir for SDK state. REQUIRED on Android (use
  /// [Medut.start], which fills it with the app cache dir automatically).
  final String? tmpDir;

  // ── Tracing / logging ──────────────────────────────────────────────────
  /// Emit [SipTraceEvent] for every SIP message.
  final bool traceSip;

  /// Emit [SdpNegotiationEvent] per negotiation.
  final bool traceSdpDiff;

  /// Live SIP+RTP capture to this pcap file; null = off.
  final String? pcapPath;

  /// 0=err, 1=warn, 2=info, 3=debug.
  final int logLevel;

  // ── Network handover ───────────────────────────────────────────────────
  /// Interface poll period seconds; set 0 on mobile — the plugin drives
  /// [Medut.networkChanged] from the OS connectivity callback instead.
  final int netMonitorIntervalSeconds;

  /// Debounce before a reported network change is acted on, in ms: how
  /// long the address set must stay stable before the handover runs
  /// (transport flush, re-REGISTER, re-INVITE per call).  The SDK default
  /// is 1500; a lower value shortens both the handover gap and the time
  /// spent "reconnecting" on every return to the app, at the risk of acting
  /// on an interface that is still coming up.  -1 keeps the SDK default.
  final int netSettleMs;

  const MedutConfig({
    this.transport = Transport.udp,
    this.localIp,
    this.localPort = 0,
    this.bindInterface,
    this.preferIpv6 = false,
    this.sipDomain,
    this.serverUrl,
    this.serverHost,
    this.serverPort = 0,
    this.outboundProxy,
    this.caCertPath,
    this.clientCert,
    this.clientKey,
    this.verifyServer = true,
    this.sniHostname,
    this.userAgent,
    this.wsOrigin,
    this.wsExtraHeaders = const [],
    this.wsKeepaliveMs = 0,
    this.stunServer,
    this.turnServer,
    this.turnUser,
    this.turnPass,
    this.iceEnabled = false,
    this.iceGatheringTimeoutMs = -1,
    this.iceServers = const [],
    this.iceTransportPolicy = IceTransportPolicy.all,
    this.iceMultipath = false,
    this.rtcpMux = true,
    this.mediaEnc = MediaEncryption.none,
    this.audioCodecs = const [],
    this.dscpSip = 0,
    this.dscpRtp = 0,
    this.aecMode = AecMode.suppressor,
    this.noiseSuppression = false,
    this.autoGainControl = false,
    this.aecSuppressionLevel = 1.0,
    this.micGainDb = 0,
    this.speakerGainDb = 0,
    this.opus = const OpusConfig(),
    this.platformAudioActivate = true,
    this.appOwnedAudio = false,
    this.jitterBufferType = JitterBufferType.adaptive,
    this.jitterBufferMinMs = 0,
    this.jitterBufferMaxMs = 0,
    this.regExpires = 0,
    this.regRefreshPct = 0,
    this.keepaliveIntervalMs = 0,
    this.regRetryInitialMs = 0,
    this.regRetryMaxMs = 0,
    this.regRetryBackoff = 0,
    this.regRetryMaxAttempts = 0,
    this.statsIntervalMs = 2000,
    this.mosAlertThreshold = 3.5,
    this.lossAlertThreshold = 5.0,
    this.jitterAlertThreshold = 40.0,
    this.regRetryJitter = 0,
    this.sipTimerBMs = 0,
    this.sipTimerFMs = 0,
    this.rtpTimeoutSeconds = 0,
    this.mediaStallMs = 4000,
    this.adaptiveBitrate = true,
    this.adaptMinBitrate = 0,
    this.adaptMaxBitrate = 0,
    this.adaptLossDownPct = 0,
    this.adaptLossUpPct = 0,
    this.adaptRecoverTicks = 0,
    this.opusExpectedLossPct = 0,
    this.keepaliveReregister = true,
    this.dnsSrvFailover = true,
    this.netIceHandover = IceHandover.bestEffort,
    this.tmpDir,
    this.traceSip = false,
    this.traceSdpDiff = false,
    this.pcapPath,
    this.logLevel = 1,
    this.netMonitorIntervalSeconds = 10,
    this.netSettleMs = -1,
  });

  MedutConfig copyWith(
      {String? tmpDir, int? netMonitorIntervalSeconds, int? netSettleMs}) {
    return MedutConfig(
      transport: transport,
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
      wsExtraHeaders: wsExtraHeaders,
      wsKeepaliveMs: wsKeepaliveMs,
      stunServer: stunServer,
      turnServer: turnServer,
      turnUser: turnUser,
      turnPass: turnPass,
      iceEnabled: iceEnabled,
      iceGatheringTimeoutMs: iceGatheringTimeoutMs,
      iceServers: iceServers,
      iceTransportPolicy: iceTransportPolicy,
      iceMultipath: iceMultipath,
      rtcpMux: rtcpMux,
      mediaEnc: mediaEnc,
      audioCodecs: audioCodecs,
      dscpSip: dscpSip,
      dscpRtp: dscpRtp,
      aecMode: aecMode,
      noiseSuppression: noiseSuppression,
      autoGainControl: autoGainControl,
      aecSuppressionLevel: aecSuppressionLevel,
      micGainDb: micGainDb,
      speakerGainDb: speakerGainDb,
      opus: opus,
      platformAudioActivate: platformAudioActivate,
      appOwnedAudio: appOwnedAudio,
      jitterBufferType: jitterBufferType,
      jitterBufferMinMs: jitterBufferMinMs,
      jitterBufferMaxMs: jitterBufferMaxMs,
      regExpires: regExpires,
      regRefreshPct: regRefreshPct,
      keepaliveIntervalMs: keepaliveIntervalMs,
      regRetryInitialMs: regRetryInitialMs,
      regRetryMaxMs: regRetryMaxMs,
      regRetryBackoff: regRetryBackoff,
      regRetryMaxAttempts: regRetryMaxAttempts,
      statsIntervalMs: statsIntervalMs,
      mosAlertThreshold: mosAlertThreshold,
      lossAlertThreshold: lossAlertThreshold,
      jitterAlertThreshold: jitterAlertThreshold,
      regRetryJitter: regRetryJitter,
      sipTimerBMs: sipTimerBMs,
      sipTimerFMs: sipTimerFMs,
      rtpTimeoutSeconds: rtpTimeoutSeconds,
      mediaStallMs: mediaStallMs,
      adaptiveBitrate: adaptiveBitrate,
      adaptMinBitrate: adaptMinBitrate,
      adaptMaxBitrate: adaptMaxBitrate,
      adaptLossDownPct: adaptLossDownPct,
      adaptLossUpPct: adaptLossUpPct,
      adaptRecoverTicks: adaptRecoverTicks,
      opusExpectedLossPct: opusExpectedLossPct,
      keepaliveReregister: keepaliveReregister,
      dnsSrvFailover: dnsSrvFailover,
      netIceHandover: netIceHandover,
      tmpDir: tmpDir ?? this.tmpDir,
      traceSip: traceSip,
      traceSdpDiff: traceSdpDiff,
      pcapPath: pcapPath,
      logLevel: logLevel,
      netMonitorIntervalSeconds:
          netMonitorIntervalSeconds ?? this.netMonitorIntervalSeconds,
      netSettleMs: netSettleMs ?? this.netSettleMs,
    );
  }
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
}

// ── Native marshalling helpers (internal) ─────────────────────────────────

/// Tracks native allocations so they can be freed after the C call.
class NativeScope {
  final List<Pointer<NativeType>> _allocs = [];

  Pointer<Char> str(String? s) {
    if (s == null) return nullptr;
    final p = s.toNativeUtf8().cast<Char>();
    _allocs.add(p);
    return p;
  }

  /// NULL-terminated `const char **` from a string list; nullptr if empty.
  Pointer<Pointer<Char>> strv(List<String> list) {
    if (list.isEmpty) return nullptr;
    final arr = calloc<Pointer<Char>>(list.length + 1);
    for (var i = 0; i < list.length; i++) {
      arr[i] = str(list[i]);
    }
    arr[list.length] = nullptr;
    _allocs.add(arr);
    return arr;
  }

  /// `medut_ice_server_t[]` from a server list; nullptr if empty.
  Pointer<c.medut_ice_server_t> iceServers(List<IceServer> list) {
    if (list.isEmpty) return nullptr;
    final arr = calloc<c.medut_ice_server_t>(list.length);
    for (var i = 0; i < list.length; i++) {
      arr[i].url = str(list[i].url);
      arr[i].user = str(list[i].user);
      arr[i].pass = str(list[i].pass);
    }
    _allocs.add(arr);
    return arr;
  }

  void free() {
    for (final p in _allocs) {
      calloc.free(p);
    }
    _allocs.clear();
  }
}

/// Copy codec names into a fixed `char[8][32]` native array, returning how
/// many entries were written. Extra codecs past the 8th, and characters past
/// the 31st of a name, are dropped — the native array cannot hold them.
int writeCodecNamesInto(Array<Array<Char>> names, List<String> codecs) {
  final count = codecs.length > 8 ? 8 : codecs.length;
  for (var i = 0; i < count; i++) {
    final bytes = codecs[i].codeUnits;
    final len = bytes.length > 31 ? 31 : bytes.length;
    for (var j = 0; j < len; j++) {
      names[i][j] = bytes[j];
    }
    names[i][len] = 0;
  }
  return count;
}

/// Copy codec names into the fixed `char[8][32]` array of an account config.
void writeCodecNames(c.medut_account_config_t cfg, List<String> codecs) {
  cfg.audio_codec_name_count =
      writeCodecNamesInto(cfg.audio_codec_names, codecs);
}

/// Populate a native `medut_config_t` from [MedutConfig].
/// Returns a [NativeScope] the caller must free after `medut_init`
/// (the SDK deep-copies all config strings).
NativeScope fillNativeConfig(
    Pointer<c.medut_config_t> cfg, MedutConfig conf, c.MedutBindings b) {
  final scope = NativeScope();
  b.medut_config_init(cfg);
  final r = cfg.ref;

  r.transport = conf.transport.raw;
  r.local_ip = scope.str(conf.localIp);
  r.local_port = conf.localPort;
  r.bind_interface = scope.str(conf.bindInterface);
  r.prefer_ipv6 = conf.preferIpv6;
  r.sip_domain = scope.str(conf.sipDomain);
  r.server_url = scope.str(conf.serverUrl);
  r.server_host = scope.str(conf.serverHost);
  r.server_port = conf.serverPort;
  r.outbound_proxy = scope.str(conf.outboundProxy);

  r.ca_cert_path = scope.str(conf.caCertPath);
  r.client_cert = scope.str(conf.clientCert);
  r.client_key = scope.str(conf.clientKey);
  r.verify_server = conf.verifyServer;
  r.sni_hostname = scope.str(conf.sniHostname);
  r.user_agent = scope.str(conf.userAgent);

  r.ws_origin = scope.str(conf.wsOrigin);
  r.ws_extra_headers = scope.strv(conf.wsExtraHeaders);
  r.ws_keepalive_ms = conf.wsKeepaliveMs;

  r.stun_server = scope.str(conf.stunServer);
  r.turn_server = scope.str(conf.turnServer);
  r.turn_user = scope.str(conf.turnUser);
  r.turn_pass = scope.str(conf.turnPass);
  r.ice_enabled = conf.iceEnabled;
  // -1 means "leave medut_config_init()'s default"; 0 is a real value here
  // (disable the deadline), so the usual `> 0` guard would swallow it.
  if (conf.iceGatheringTimeoutMs >= 0) {
    r.ice_gathering_timeout_ms = conf.iceGatheringTimeoutMs;
  }
  r.ice_servers = scope.iceServers(conf.iceServers);
  r.ice_server_count = conf.iceServers.length;
  r.ice_transport_policy = conf.iceTransportPolicy.raw;
  r.ice_multipath = conf.iceMultipath;
  r.rtcp_mux = conf.rtcpMux;

  r.media_enc = conf.mediaEnc.raw;
  if (conf.audioCodecs.isNotEmpty) {
    r.audio_codec_name_count =
        writeCodecNamesInto(r.audio_codec_names, conf.audioCodecs);
  }
  r.dscp_sip = conf.dscpSip;
  r.dscp_rtp = conf.dscpRtp;

  r.aec_mode = conf.aecMode.raw;
  r.ns = conf.noiseSuppression;
  r.agc = conf.autoGainControl;
  r.aec_suppression_level = conf.aecSuppressionLevel;
  r.mic_gain_db = conf.micGainDb;
  r.speaker_gain_db = conf.speakerGainDb;
  r.platform_audio_activate = conf.platformAudioActivate;

  r.opus.bitrate = conf.opus.bitrate;
  r.opus.complexity = conf.opus.complexity;
  r.opus.cbr = conf.opus.cbr;
  r.opus.dtx = conf.opus.dtx;
  r.opus.fec = conf.opus.fec;
  r.opus.stereo = conf.opus.stereo;

  r.jbuf_type = conf.jitterBufferType.raw;
  if (conf.jitterBufferMinMs > 0) r.jitter_buffer_min_ms = conf.jitterBufferMinMs;
  if (conf.jitterBufferMaxMs > 0) r.jitter_buffer_max_ms = conf.jitterBufferMaxMs;

  if (conf.regExpires > 0) r.reg_expires = conf.regExpires;
  if (conf.regRefreshPct > 0) r.reg_refresh_pct = conf.regRefreshPct;
  if (conf.keepaliveIntervalMs > 0) r.keepalive_interval = conf.keepaliveIntervalMs;
  if (conf.regRetryInitialMs > 0) r.reg_retry_initial_ms = conf.regRetryInitialMs;
  if (conf.regRetryMaxMs > 0) r.reg_retry_max_ms = conf.regRetryMaxMs;
  if (conf.regRetryBackoff > 0) r.reg_retry_backoff = conf.regRetryBackoff;
  if (conf.regRetryMaxAttempts > 0) {
    r.reg_retry_max_attempts = conf.regRetryMaxAttempts;
  }

  r.stats_interval_ms = conf.statsIntervalMs;
  r.mos_alert_threshold = conf.mosAlertThreshold;
  r.loss_alert_threshold = conf.lossAlertThreshold;
  r.jitter_alert_threshold = conf.jitterAlertThreshold;

  // Degraded-link handling.  Fields whose Dart default is 0 are assigned only
  // when set, so medut_config_init()'s value survives — the same convention
  // the registration block above uses.  mediaStallMs and the two booleans
  // carry meaningful non-zero defaults and are always written.
  if (conf.regRetryJitter > 0) r.reg_retry_jitter = conf.regRetryJitter;
  if (conf.sipTimerBMs > 0) r.sip_timer_b_ms = conf.sipTimerBMs;
  if (conf.sipTimerFMs > 0) r.sip_timer_f_ms = conf.sipTimerFMs;
  r.rtp_timeout_s = conf.rtpTimeoutSeconds;
  r.media_stall_ms = conf.mediaStallMs;
  r.adaptive_bitrate = conf.adaptiveBitrate;
  if (conf.adaptMinBitrate > 0) r.adapt_min_bitrate = conf.adaptMinBitrate;
  if (conf.adaptMaxBitrate > 0) r.adapt_max_bitrate = conf.adaptMaxBitrate;
  if (conf.adaptLossDownPct > 0) {
    r.adapt_loss_down_pct = conf.adaptLossDownPct;
  }
  if (conf.adaptLossUpPct > 0) r.adapt_loss_up_pct = conf.adaptLossUpPct;
  if (conf.adaptRecoverTicks > 0) {
    r.adapt_recover_ticks = conf.adaptRecoverTicks;
  }
  r.opus_expected_loss_pct = conf.opusExpectedLossPct;
  r.keepalive_reregister = conf.keepaliveReregister;
  r.dns_srv_failover = conf.dnsSrvFailover;
  r.net_ice_handover = conf.netIceHandover.raw;

  r.tmp_dir = scope.str(conf.tmpDir);
  r.trace_sip = conf.traceSip;
  r.trace_sdp_diff = conf.traceSdpDiff;
  r.pcap_path = scope.str(conf.pcapPath);
  r.log_level = conf.logLevel;
  r.net_monitor_interval_s = conf.netMonitorIntervalSeconds;
  if (conf.netSettleMs >= 0) r.net_settle_ms = conf.netSettleMs;

  return scope;
}

/// Populate a native `medut_account_config_t` from [AccountConfig].
/// Returns a [NativeScope] the caller must free after `medut_account_create`.
NativeScope fillNativeAccountConfig(Pointer<c.medut_account_config_t> cfg,
    String uri, String password, AccountConfig conf) {
  final scope = NativeScope();
  final r = cfg.ref;

  r.uri = scope.str(uri);
  r.password = scope.str(password);
  r.transport = conf.transport.raw;
  r.server_url = scope.str(conf.serverUrl);
  r.server_host = scope.str(conf.serverHost);
  r.server_port = conf.serverPort;
  r.auth_user = scope.str(conf.authUser);
  r.display_name = scope.str(conf.displayName);
  r.outbound_proxy = scope.str(conf.outboundProxy);

  r.media_enc = conf.mediaEnc.raw;
  r.ice_enabled = conf.iceEnabled;
  if (conf.rtcpMux != null) {
    r.rtcp_mux = conf.rtcpMux!;
    r.rtcp_mux_set = true;
  }
  r.stun_server = scope.str(conf.stunServer);
  r.turn_server = scope.str(conf.turnServer);
  r.turn_user = scope.str(conf.turnUser);
  r.turn_pass = scope.str(conf.turnPass);
  r.ice_servers = scope.iceServers(conf.iceServers);
  r.ice_server_count = conf.iceServers.length;
  r.ice_transport_policy = conf.iceTransportPolicy.raw;
  r.verify_tls = conf.verifyTls;

  r.push_provider = conf.pushProvider.raw;
  r.push_token = scope.str(conf.pushToken);
  r.push_param = scope.str(conf.pushParam);

  if (conf.audioCodecs.isNotEmpty) {
    writeCodecNames(r, conf.audioCodecs);
  }
  r.dtmf_mode = conf.dtmfMode.raw;

  return scope;
}
