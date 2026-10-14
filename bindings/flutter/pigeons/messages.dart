// Pigeon schema for the Flutter plugin: Dart talks to the native SDKs
// (bindings/kotlin, bindings/swift), which own the stack.
//
// Regenerate after editing:
//   dart run pigeon --input pigeons/messages.dart
//
// Conventions:
//  - Enums cross as their C values (medut.h), the numbering every binding
//    shares, so no layer keeps a mapping table.
//  - Config fields are nullable: null means "the C default"
//    (medut_config_init()), so Dart carries no copy of the defaults.
//  - Accounts and calls are referenced by the native objects' `id` (UUID).
//  - Failures arrive as PlatformException with `code` = the MedutError code
//    name (e.g. NOT_RUNNING, CALL_ENDED, AUTHENTICATION) and `details` = the
//    raw C return value.
import 'package:pigeon/pigeon.dart';

@ConfigurePigeon(PigeonOptions(
  dartOut: 'lib/src/messages.g.dart',
  dartPackageName: 'medut',
  kotlinOut: 'android/src/main/kotlin/dev/medut/flutter/Messages.g.kt',
  kotlinOptions: KotlinOptions(package: 'dev.medut.flutter'),
  swiftOut: 'ios/Classes/Messages.g.swift',
))
class IceServerMessage {
  IceServerMessage({required this.url, this.username, this.password});
  String url;
  String? username;
  String? password;
}

/// medut_config_t.  Every field optional; null keeps the C default.
class ConfigMessage {
  int? transport;
  String? localIp;
  int? localPort;
  String? bindInterface;
  bool? preferIpv6;
  String? sipDomain;
  String? serverUrl;
  String? serverHost;
  int? serverPort;
  String? outboundProxy;
  String? caCertPath;
  String? clientCert;
  String? clientKey;
  bool? verifyServer;
  String? sniHostname;
  String? userAgent;
  String? wsOrigin;
  List<String>? wsExtraHeaders;
  int? wsKeepaliveMs;
  String? stunServer;
  String? turnServer;
  String? turnUser;
  String? turnPass;
  bool? iceEnabled;
  bool? rtcpMux;
  List<IceServerMessage>? iceServers;
  int? iceTransportPolicy;
  int? iceGatheringTimeoutMs;
  bool? iceMultipath;
  int? mediaEncryption;
  List<String>? audioCodecs;
  int? dscpSip;
  int? dscpRtp;
  int? aecMode;
  bool? noiseSuppression;
  bool? autoGainControl;
  double? aecSuppressionLevel;
  double? micGainDb;
  double? speakerGainDb;
  int? opusBitrate;
  int? opusComplexity;
  bool? opusConstantBitrate;
  bool? opusDtx;
  bool? opusFec;
  bool? opusStereo;
  bool? platformAudioActivate;
  int? jitterBufferType;
  int? jitterBufferMinMs;
  int? jitterBufferMaxMs;
  int? regExpires;
  int? regRefreshPct;
  int? keepaliveIntervalMs;
  bool? keepaliveReregister;
  int? regRetryInitialMs;
  int? regRetryMaxMs;
  double? regRetryBackoff;
  int? regRetryMaxAttempts;
  double? regRetryJitter;
  bool? dnsSrvFailover;
  int? sipTimerBMs;
  int? sipTimerFMs;
  bool? sessionTimerEnabled;
  int? sessionExpiresSeconds;
  int? sessionMinSeSeconds;
  int? statsIntervalMs;
  int? mosMethod;
  double? mosAlertThreshold;
  double? lossAlertThreshold;
  double? jitterAlertThreshold;
  int? rtpTimeoutSeconds;
  int? mediaStallMs;
  bool? adaptiveBitrate;
  int? adaptMinBitrate;
  int? adaptMaxBitrate;
  double? adaptLossDownPct;
  double? adaptLossUpPct;
  int? adaptRecoverTicks;
  int? opusExpectedLossPct;
  int? netMonitorIntervalSeconds;
  int? netSettleMs;
  bool? netReinviteCalls;
  bool? netHangupOnMigrationFailure;
  int? netVerifyMs;
  int? netMaxAttempts;
  int? netIceHandover;
  String? tmpDir;
  bool? traceSip;
  bool? traceSdpDiff;
  String? pcapPath;
  int? logLevel;
}

class StartOptionsMessage {
  StartOptionsMessage({
    required this.monitorNetwork,
    required this.audioMode,
    required this.appOwnedAudio,
    required this.telecom,
  });

  /// Drive handover from the OS connectivity callback.
  bool monitorNetwork;

  /// 0 = the SDK activates call audio itself; 1 = the platform call
  /// framework does (CallKit / Telecom); 2 = the app does.
  int audioMode;

  /// Hand the microphone and speaker to the SDK's app-owned engine.
  bool appOwnedAudio;

  /// Android: register every call with Telecom through the SDK's own
  /// self-managed ConnectionService (implies audioMode 1).  iOS: the SDK's
  /// CallKit integration.
  bool telecom;
}

/// medut_account_config_t.  Optional fields left null use the C default.
class AccountConfigMessage {
  AccountConfigMessage({required this.uri, required this.password});
  String uri;
  String password;
  int? transport;
  String? serverUrl;
  String? serverHost;
  int? serverPort;
  String? outboundProxy;
  String? authUser;
  String? displayName;
  int? mediaEncryption;
  bool? iceEnabled;
  bool? rtcpMux;
  String? stunServer;
  String? turnServer;
  String? turnUser;
  String? turnPass;
  List<IceServerMessage>? iceServers;
  int? iceTransportPolicy;
  bool? verifyTls;
  int? pushProvider;
  String? pushToken;
  String? pushParam;
  List<String>? audioCodecs;
  int? dtmfMode;
}

class AccountMessage {
  AccountMessage({
    required this.id,
    required this.aor,
    required this.registrationState,
  });
  String id;
  String aor;
  int registrationState;
}

class CallMessage {
  CallMessage({
    required this.id,
    this.accountId,
    required this.state,
    required this.isOutgoing,
    required this.remoteUri,
    this.remoteDisplayName,
  });
  String id;
  String? accountId;
  int state;
  bool isOutgoing;
  String remoteUri;
  String? remoteDisplayName;
}

class StartResultMessage {
  StartResultMessage({
    required this.reattached,
    required this.version,
    required this.accounts,
    required this.calls,
  });

  /// The native stack was already running (started by the app at launch or
  /// from a push); this is its live state.
  bool reattached;
  String version;
  List<AccountMessage> accounts;
  List<CallMessage> calls;
}

/// medut_call_info_t.
class CallInfoMessage {
  CallInfoMessage({
    required this.peerUri,
    required this.peerDisplayName,
    required this.localUri,
    required this.contactUri,
    required this.callId,
    required this.diverterUri,
    required this.isOutgoing,
    required this.isRemoteHold,
    required this.sipStatus,
    required this.durationMs,
    required this.setupDurationMs,
    required this.lineNumber,
    required this.transport,
    required this.state,
  });
  String peerUri;
  String peerDisplayName;
  String localUri;
  String contactUri;
  String callId;
  String diverterUri;
  bool isOutgoing;
  bool isRemoteHold;
  int sipStatus;
  int durationMs;
  int setupDurationMs;
  int lineNumber;
  int transport;
  int state;
}

/// medut_ev_media_stats_t.
class MediaStatsMessage {
  MediaStatsMessage({
    required this.packetsSent,
    required this.packetsReceived,
    required this.packetsLost,
    required this.packetsLostRx,
    required this.bytesSent,
    required this.bytesReceived,
    required this.txErrors,
    required this.rxErrors,
    required this.lossPct,
    required this.lossPctRx,
    required this.jitterMs,
    required this.txJitterMs,
    required this.rttMs,
    required this.jitterBufferMs,
    required this.jitterBufferLoad,
    required this.latePackets,
    required this.discardedPackets,
    required this.jitterBufferTargetMs,
    required this.jitterBufferAdaptive,
    required this.plcFrames,
    required this.plcRatio,
    required this.fecRecovered,
    required this.plcConcealed,
    required this.fecActive,
    required this.fecLevelPct,
    required this.bandwidthKbpsTx,
    required this.bandwidthKbpsRx,
    required this.avgBandwidthKbpsTx,
    required this.avgBandwidthKbpsRx,
    required this.mosLq,
    required this.mosCq,
    required this.mosLqRx,
    required this.mosCqRx,
    required this.mosLqMin,
    required this.mosLqAvg,
    required this.mosMethod,
    required this.codecName,
    required this.codecClockRate,
    required this.codecSampleRate,
    required this.codecChannels,
    required this.payloadType,
    required this.audioLevelDbov,
    required this.micLevelDbov,
    required this.ssrcTx,
    required this.ssrcRx,
    required this.remoteAddress,
    required this.statsTick,
    required this.callDurationMs,
    required this.isFinal,
    required this.iceState,
    required this.iceLocalType,
    required this.iceRemoteType,
    required this.iceRttMs,
    required this.iceGeneration,
    required this.iceLocalAddress,
    required this.iceRemoteAddress,
  });
  int packetsSent;
  int packetsReceived;
  int packetsLost;
  int packetsLostRx;
  int bytesSent;
  int bytesReceived;
  int txErrors;
  int rxErrors;
  double lossPct;
  double lossPctRx;
  double jitterMs;
  double txJitterMs;
  double rttMs;
  int jitterBufferMs;
  int jitterBufferLoad;
  int latePackets;
  int discardedPackets;
  int jitterBufferTargetMs;
  bool jitterBufferAdaptive;
  int plcFrames;
  double plcRatio;
  int fecRecovered;
  int plcConcealed;
  bool fecActive;
  int fecLevelPct;
  int bandwidthKbpsTx;
  int bandwidthKbpsRx;
  int avgBandwidthKbpsTx;
  int avgBandwidthKbpsRx;
  double mosLq;
  double mosCq;
  double mosLqRx;
  double mosCqRx;
  double mosLqMin;
  double mosLqAvg;
  int mosMethod;
  String codecName;
  int codecClockRate;
  int codecSampleRate;
  int codecChannels;
  int payloadType;
  double audioLevelDbov;
  double micLevelDbov;
  int ssrcTx;
  int ssrcRx;
  String remoteAddress;
  int statsTick;
  int callDurationMs;
  bool isFinal;
  int iceState;
  int iceLocalType;
  int iceRemoteType;
  double iceRttMs;
  int iceGeneration;
  String iceLocalAddress;
  String iceRemoteAddress;
}

class AudioDeviceMessage {
  AudioDeviceMessage({
    required this.name,
    required this.label,
    required this.isDefault,
  });
  String name;

  /// medut_audio_device_t.description (renamed: it clashes with Swift's
  /// CustomStringConvertible).
  String label;
  bool isDefault;
}

/// The format the current call negotiated, for the app-owned device.
class ExternalAudioFormatMessage {
  ExternalAudioFormatMessage({
    required this.sampleRate,
    required this.channels,
    required this.ptimeMs,
  });
  int sampleRate;
  int channels;
  int ptimeMs;
}

class AudioRouteMessage {
  AudioRouteMessage({
    required this.id,
    required this.name,
    required this.kind,
    required this.isActive,
  });

  /// "earpiece", "speaker", "wired-headset", "bluetooth", "bt:<id>".
  String id;

  /// Empty for the built-ins, so the app can localise them.
  String name;

  /// "earpiece", "speaker", "bluetooth" or "wired-headset".
  String kind;
  bool isActive;
}

/// Everything the stack reports, as one message: `type` is the C event type
/// (medut_event_type_t) or one of the plugin's own, and only the fields that
/// type carries are set.  Field names follow medut.h.
class EventMessage {
  EventMessage({required this.type});

  /// medut_event_type_t, or 100 (call-audio routes changed: [routes]) /
  /// 101 (the app-owned audio device failed: [errorCode], [message]).
  int type;

  String? accountId;
  CallMessage? call;
  int? state;
  int? previousState;
  int? error;
  String? message;
  int? retryAttempt;
  int? retryDelayMs;
  String? errorText;
  String? fromUri;
  String? displayName;
  String? reason;
  String? digit;
  String? localSdp;
  String? remoteSdp;
  String? negotiatedCodec;
  String? negotiatedCrypto;
  List<String>? rejectedCodecs;
  List<String>? warnings;
  int? direction;
  String? transport;
  String? remoteAddress;
  String? rawMessage;
  int? timestampUs;
  MediaStatsMessage? stats;
  String? referToUri;
  bool? isAttended;
  bool? autoFollowed;
  bool? messagesWaiting;
  int? newVoice;
  int? oldVoice;
  int? newUrgent;
  int? oldUrgent;
  String? rawBody;
  String? body;
  String? contentType;
  String? targetUri;
  int? status;
  int? issue;
  double? value;
  double? threshold;
  bool? recovering;
  int? stage;
  String? localAddress;
  int? attempt;
  int? maxAttempts;
  int? elapsedMs;
  bool? ice;
  int? generation;
  int? localType;
  int? remoteType;
  double? rttMs;
  List<AudioRouteMessage>? routes;
  String? errorCode;
}

@HostApi()
abstract class MedutHostApi {
  // Lifecycle
  /// Start the native stack, or attach to the one already running.
  StartResultMessage start(ConfigMessage config, StartOptionsMessage options);
  void shutdown();

  /// Let the native side release an ended call this side no longer holds.
  void releaseCall(String callId);

  // Accounts
  /// [remember] keeps the config in the native account store, so a process
  /// started by a push re-creates and registers the account with no Dart.
  AccountMessage createAccount(AccountConfigMessage config, bool remember);

  /// [forget] also drops the account from the native store.
  void accountDestroy(String accountId, bool forget);

  /// URIs of the accounts the native store holds.
  List<String> rememberedAccounts();
  void forgetAccount(String uri);
  void accountRegister(String accountId);
  void accountUnregister(String accountId);
  int accountRegistrationState(String accountId);
  void accountSetRetryPolicy(
      String accountId, int initialMs, int maxMs, double backoff, int maxAttempts);
  void accountCancelRetry(String accountId);
  void accountRetryNow(String accountId);
  void accountKeepaliveNow(String accountId);
  void accountSetPushToken(String accountId, String? token);
  void accountSetIceServers(
      String accountId, List<IceServerMessage> servers, int policy);
  void accountAddHeader(String accountId, String name, String value);
  void accountAddRegisterHeader(String accountId, String name, String value);
  void accountSetReliableProvisional(String accountId, int mode);
  void accountSendMessage(
      String accountId, String to, String body, String? contentType);
  void accountPublishPresence(String accountId, int status);
  void accountSubscribePresence(String accountId, String uri);
  void accountUnsubscribePresence(String accountId, String uri);
  CallMessage accountCall(String accountId, String uri);

  // Calls
  void callAnswer(String callId);
  void callHangup(String callId);
  void callReject(String callId, int statusCode, String? reason);
  void callHold(String callId);
  void callResume(String callId);
  bool callIsHeld(String callId);
  void callSendDtmf(String callId, String digit);
  void callTransfer(String callId, String uri);
  void callAttendedTransfer(String callId, String consultationCallId);
  CallMessage callAcceptTransfer(String callId);
  void callRejectTransfer(String callId, int statusCode, String? reason);
  void callAddHeader(String callId, String name, String value);
  void callSetMuted(String callId, bool muted);
  bool callIsMuted(String callId);
  void callSetReceiveMuted(String callId, bool muted);
  void callStartRecording(String callId, String path);
  void callStopRecording(String callId);
  void callSetDscp(String callId, int dscp);
  void callSetRtpTimeout(String callId, int seconds);
  void callSetBitrate(String callId, int bps);
  int callIceState(String callId);
  CallInfoMessage callInfo(String callId);
  MediaStatsMessage callStats(String callId);

  // Audio processing and devices
  void setEchoCancellation(bool enabled);
  void setAecMode(int mode);
  void setAecSuppressionLevel(double level);
  void setNoiseSuppression(bool enabled);
  void setAutoGainControl(bool enabled);
  void setMicGainDb(double db);
  void setSpeakerGainDb(double db);
  void setJitterBuffer(int minMs, int maxMs);
  void setJitterBufferType(int type);
  void setAdaptiveBitrate(bool enabled, int minBps, int maxBps);
  List<AudioDeviceMessage> inputDevices();
  List<AudioDeviceMessage> outputDevices();
  void setInputDevice(String? name);
  void setOutputDevice(String? name);

  // Call-audio routes
  List<AudioRouteMessage> audioRoutes();
  List<AudioRouteMessage> selectAudioRoute(String id);
  void setSpeakerphone(bool on);
  void setExternalRouting(bool on);

  // App-owned audio device
  void useAppOwnedAudio(bool enable);
  Map<String, Object?> appOwnedAudioStatus();

  /// True while a call is capturing or playing through the app-owned device.
  bool appOwnedAudioActive();

  /// The current call's negotiated format, or null before media is up.
  ExternalAudioFormatMessage? appOwnedAudioFormat();

  /// The platform call framework activated (true) or deactivated call
  /// audio: CallKit's didActivate / didDeactivate on iOS, Telecom on Android.
  void platformCallAudioActive(bool active);

  // Network and capture
  void networkChanged();
  void networkSetMonitorInterval(int seconds);
  void networkSetHandoverPolicy(bool reinviteCalls, bool hangupOnFailure);
  String networkLocalAddress();
  bool networkIsUp();
  void startPcap(String path);
  void stopPcap();
}

@FlutterApi()
abstract class MedutEventsApi {
  void onEvent(EventMessage event);
}
