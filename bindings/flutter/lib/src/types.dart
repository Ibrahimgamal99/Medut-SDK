part of '../medut.dart';

// ── Event types ─────────────────────────────────────────────────────────────

abstract class MedutEvent {}

class RegStateEvent extends MedutEvent {
  final RegState state;
  final MedutError error;
  final String? errorStr;
  final int retryAttempt;
  final int retryDelayMs;
  RegStateEvent(this.state, this.error, this.errorStr,
      {this.retryAttempt = 0, this.retryDelayMs = 0});
}

class IncomingCallEvent extends MedutEvent {
  final Call call;
  final String fromUri;
  final String? displayName;
  IncomingCallEvent(this.call, this.fromUri, this.displayName);
}

class CallStateEvent extends MedutEvent {
  final Call call;
  final CallState state;
  final MedutError error;
  final String? reason;

  /// On a terminal state: this side ended the call ([Call.hangup] /
  /// [Call.reject]), from the app or from the system: a Decline in the
  /// system call UI (Telecom, CallKit), a Bluetooth headset or a watch.  An
  /// incoming call that ends unanswered with this set was declined, not
  /// missed.  False on non-terminal states.
  final bool endedLocally;
  CallStateEvent(this.call, this.state, this.error, this.reason,
      {this.endedLocally = false});
}

class CallDtmfEvent extends MedutEvent {
  final Call call;
  final String digit;
  CallDtmfEvent(this.call, this.digit);
}

/// Static and slow-moving facts about a call, as opposed to the per-tick
/// media numbers in [Call.getStats].
class CallInfo {
  final String peerUri;
  final String peerDisplayName;
  final String localUri;
  final String contactUri;
  final String callId;

  /// Diversion / History-Info URI when the call was forwarded to us; empty
  /// otherwise. This is not Referred-By: a call that reached us by transfer
  /// carries no diverter.
  final String diverterUri;
  final bool isOutgoing;

  /// True when the PEER has put us on hold. Local hold — the hold this app
  /// asked for — is [Call.isHeld]; the two are independent.
  final bool isRemoteHold;
  final int sipStatus;
  final Duration duration;
  final Duration setupDuration;
  final int lineNumber;
  final Transport transport;
  final CallState state;

  CallInfo._fromMessage(CallInfoMessage m)
      : peerUri = m.peerUri,
        peerDisplayName = m.peerDisplayName,
        localUri = m.localUri,
        contactUri = m.contactUri,
        callId = m.callId,
        diverterUri = m.diverterUri,
        isOutgoing = m.isOutgoing,
        isRemoteHold = m.isRemoteHold,
        sipStatus = m.sipStatus,
        duration = Duration(milliseconds: m.durationMs),
        setupDuration = Duration(milliseconds: m.setupDurationMs),
        lineNumber = m.lineNumber,
        transport = Transport.fromRaw(m.transport),
        state = CallState.fromRaw(m.state);
}

class AudioDevice {
  final String name;
  final String description;
  final bool isDefault;
  AudioDevice(this.name, this.description, this.isDefault);
}

/// A failure inside the app-owned audio engine.
///
/// These have no other reporting path: once the app owns the device, the SDK
/// is not holding it and cannot see it fail. Left unhandled they surface as a
/// call that connects with silence in one or both directions.
///
/// Codes: `mic-permission` (RECORD_AUDIO not granted), `unsupported-rate` (the
/// device will not open the negotiated rate), `device-open`, `capture-dead`,
/// `playback-dead`, `unavailable` (the native library did not load).
class AppOwnedAudioError {
  final String code;
  final String message;

  const AppOwnedAudioError(this.code, this.message);

  @override
  String toString() => 'AppOwnedAudioError($code): $message';
}

/// What the current call negotiated, for sizing the app's own audio device.
/// PCM across the app-owned boundary is always S16LE interleaved.
/// See [Medut.appOwnedAudioFormat].
class ExternalAudioFormat {
  final int sampleRate;
  final int channels;
  final int ptimeMs;

  const ExternalAudioFormat(this.sampleRate, this.channels, this.ptimeMs);

  /// Total samples (frames x channels) in one ptime — the natural buffer size
  /// for the app's capture and playback loops.
  int get samplesPerFrame => sampleRate * channels * ptimeMs ~/ 1000;

  @override
  bool operator ==(Object other) =>
      other is ExternalAudioFormat &&
      other.sampleRate == sampleRate &&
      other.channels == channels &&
      other.ptimeMs == ptimeMs;

  @override
  int get hashCode => Object.hash(sampleRate, channels, ptimeMs);

  @override
  String toString() => '${sampleRate}Hz ${channels}ch ${ptimeMs}ms';
}

/// Periodic per-call media statistics (see `medut_ev_media_stats_t`).
class MediaStats {
  final Call call;
  // Packet counters
  final int packetsSent;
  final int packetsReceived;
  final int packetsLost;
  final int packetsLostRx;
  final int bytesSent;
  final int bytesReceived;
  final int txErrors;
  final int rxErrors;
  // Loss
  final double lossPct;
  final double lossPctRx;
  // Delay / jitter
  final double jitterMs;
  final double txJitterMs;
  final double rttMs;
  // Jitter buffer
  final int jitterBufferMs;
  final int jitterBufferLoad;
  final int latePackets;
  final int discardedPackets;
  final int jitterBufferTargetMs;
  final bool jitterBufferAdaptive;
  // PLC
  final int plcFrames;
  final double plcRatio;
  // Bandwidth
  final int bandwidthTx;
  final int bandwidthRx;
  final int avgBandwidthTx;
  final int avgBandwidthRx;
  // MOS
  final double mosLq;
  final double mosCq;
  final double mosLqRx;
  final double mosCqRx;
  final int mosMethod;
  // Codec
  final String codec;
  final int codecClockRate;
  final int codecSampleRate;
  final int codecChannels;
  final int payloadType;
  // Audio level
  final double audioLevelDbov;
  final double micLevelDbov;
  // Stream identity
  final int ssrcTx;
  final int ssrcRx;
  final String remoteAddr;
  // Session history
  final double mosLqMin;
  final double mosLqAvg;
  final int statsTick;
  final int callDurationMs;

  /// True on the last stats event before call teardown.
  final bool isFinal;

  // ICE — the media path as the agent sees it.  [IceState.newState] and
  // zeroes on a call without ICE.  On an ICE call [remoteAddr] above is the
  // same address as [iceRemoteAddr].
  final IceState iceState;
  final IceCandType iceLocalType;
  final IceCandType iceRemoteType;

  /// STUN round trip on the selected pair, ms.
  final double iceRttMs;

  /// ICE restarts so far; 1 is the initial session.
  final int iceGeneration;
  final String iceLocalAddr;
  final String iceRemoteAddr;

  /// Whether our encoder is sending Opus in-band FEC.  Switched from the
  /// peer's receiver reports: on at 4% loss (2% in two reports running), off
  /// after 10 s under 1%.  False
  /// for G.711, or a peer that did not ask for FEC.
  final bool fecActive;

  /// Expected loss % the FEC is sized for (5–25); 0 when off.
  final int fecLevelPct;

  /// Opus FEC we receive: how each frame the jitter buffer reported lost was
  /// filled in, cumulative for the call — rebuilt from the redundant copy the
  /// next packet carried ([fecRecovered]), or concealed by the decoder alone
  /// ([plcConcealed]).  [plcFrames] counts the gaps; these two say what became
  /// of them.  0 for a codec other than Opus.
  final int fecRecovered;
  final int plcConcealed;

  MediaStats({
    required this.call,
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
    required this.bandwidthTx,
    required this.bandwidthRx,
    required this.avgBandwidthTx,
    required this.avgBandwidthRx,
    required this.mosLq,
    required this.mosCq,
    required this.mosLqRx,
    required this.mosCqRx,
    required this.mosMethod,
    required this.codec,
    required this.codecClockRate,
    required this.codecSampleRate,
    required this.codecChannels,
    required this.payloadType,
    required this.audioLevelDbov,
    required this.micLevelDbov,
    required this.ssrcTx,
    required this.ssrcRx,
    required this.remoteAddr,
    required this.mosLqMin,
    required this.mosLqAvg,
    required this.statsTick,
    required this.callDurationMs,
    required this.isFinal,
    required this.iceState,
    required this.iceLocalType,
    required this.iceRemoteType,
    required this.iceRttMs,
    required this.iceGeneration,
    required this.iceLocalAddr,
    required this.iceRemoteAddr,
    required this.fecActive,
    required this.fecLevelPct,
    required this.fecRecovered,
    required this.plcConcealed,
  });

  MediaStats._fromMessage(this.call, MediaStatsMessage m)
      : packetsSent = m.packetsSent,
        packetsReceived = m.packetsReceived,
        packetsLost = m.packetsLost,
        packetsLostRx = m.packetsLostRx,
        bytesSent = m.bytesSent,
        bytesReceived = m.bytesReceived,
        txErrors = m.txErrors,
        rxErrors = m.rxErrors,
        lossPct = m.lossPct,
        lossPctRx = m.lossPctRx,
        jitterMs = m.jitterMs,
        txJitterMs = m.txJitterMs,
        rttMs = m.rttMs,
        jitterBufferMs = m.jitterBufferMs,
        jitterBufferLoad = m.jitterBufferLoad,
        latePackets = m.latePackets,
        discardedPackets = m.discardedPackets,
        jitterBufferTargetMs = m.jitterBufferTargetMs,
        jitterBufferAdaptive = m.jitterBufferAdaptive,
        plcFrames = m.plcFrames,
        plcRatio = m.plcRatio,
        bandwidthTx = m.bandwidthKbpsTx,
        bandwidthRx = m.bandwidthKbpsRx,
        avgBandwidthTx = m.avgBandwidthKbpsTx,
        avgBandwidthRx = m.avgBandwidthKbpsRx,
        mosLq = m.mosLq,
        mosCq = m.mosCq,
        mosLqRx = m.mosLqRx,
        mosCqRx = m.mosCqRx,
        mosMethod = m.mosMethod,
        codec = m.codecName,
        codecClockRate = m.codecClockRate,
        codecSampleRate = m.codecSampleRate,
        codecChannels = m.codecChannels,
        payloadType = m.payloadType,
        audioLevelDbov = m.audioLevelDbov,
        micLevelDbov = m.micLevelDbov,
        ssrcTx = m.ssrcTx,
        ssrcRx = m.ssrcRx,
        remoteAddr = m.remoteAddress,
        mosLqMin = m.mosLqMin,
        mosLqAvg = m.mosLqAvg,
        statsTick = m.statsTick,
        callDurationMs = m.callDurationMs,
        isFinal = m.isFinal,
        iceState = IceState.fromRaw(m.iceState),
        iceLocalType = IceCandType.fromRaw(m.iceLocalType),
        iceRemoteType = IceCandType.fromRaw(m.iceRemoteType),
        iceRttMs = m.iceRttMs,
        iceGeneration = m.iceGeneration,
        iceLocalAddr = m.iceLocalAddress,
        iceRemoteAddr = m.iceRemoteAddress,
        fecActive = m.fecActive,
        fecLevelPct = m.fecLevelPct,
        fecRecovered = m.fecRecovered,
        plcConcealed = m.plcConcealed;
}

class MediaStatsEvent extends MedutEvent {
  final MediaStats stats;
  MediaStatsEvent(this.stats);

  Call get call => stats.call;
}

/// A call-quality threshold was crossed (or recovered).
/// Thresholds are set in [MedutConfig] (`mosAlertThreshold`, ...).
class QualityAlertEvent extends MedutEvent {
  final Call call;
  final QualityIssue issue;
  final double value;
  final double threshold;

  /// True when the metric returned to the good side of the threshold.
  final bool recovering;
  QualityAlertEvent(
      this.call, this.issue, this.value, this.threshold, this.recovering);
}

/// Incoming REFER — the peer asks us to transfer this call.
///
/// Answer with [Call.transferAccept] or [Call.transferReject] — exactly one.
/// The SDK has already sent the provisional replies and the transferor is now
/// waiting for the final NOTIFY that only one of those two produces; ignoring
/// the event leaves it waiting out its subscription.
class TransferRequestEvent extends MedutEvent {
  final Call call;
  final String referToUri;

  /// True = attended transfer (REFER carries Replaces).
  final bool hasReplaces;

  /// True when the SDK already followed the transfer itself. Always false
  /// today — placing a call is the app's decision — but check it before
  /// calling [Call.transferAccept] so a future auto-follow policy cannot make
  /// you place a second call on top of the SDK's.
  final bool autoFollowed;
  TransferRequestEvent(this.call, this.referToUri, this.hasReplaces,
      {this.autoFollowed = false});
}

/// An outgoing REFER was refused — the transfer did not happen.
///
/// [call] is still established.  A failed transfer is not a call failure: the
/// far end simply would not take the call, and the user is still on the line
/// (usually on hold, if the app parked them for a warm transfer).  Resume them
/// and report [reason]; do not hang up.
///
/// There is no matching success event.  A REFER that is accepted moves the call
/// to the transfer target and closes our leg, which arrives as a
/// [CallStateEvent] with [CallState.ended].
class TransferFailedEvent extends MedutEvent {
  final Call call;

  /// Status line or cause text from the stack; empty when none was supplied.
  final String reason;
  TransferFailedEvent(this.call, this.reason);
}

/// Non-fatal registrar warning.
class RegistrarWarningEvent extends MedutEvent {
  final String message;
  RegistrarWarningEvent(this.message);
}

/// Result of an SDP offer/answer negotiation (enable with
/// [MedutConfig.traceSdpDiff]).
class SdpNegotiationEvent extends MedutEvent {
  final Call call;
  final String? localSdp;
  final String? remoteSdp;
  final String? negotiatedCodec;
  final String? negotiatedCrypto;
  SdpNegotiationEvent(this.call, this.localSdp, this.remoteSdp,
      this.negotiatedCodec, this.negotiatedCrypto);
}

/// Message-waiting indication (voicemail).
class MwiEvent extends MedutEvent {
  final bool messagesWaiting;
  final int newVoice;
  final int oldVoice;
  final int newUrgent;
  final int oldUrgent;
  final String? rawBody;
  MwiEvent(this.messagesWaiting, this.newVoice, this.oldVoice, this.newUrgent,
      this.oldUrgent, this.rawBody);
}

class LogEvent extends MedutEvent {
  final String message;
  LogEvent(this.message);
}

class MessageEvent extends MedutEvent {
  final String fromUri;
  final String body;
  final String contentType;
  MessageEvent(this.fromUri, this.body, this.contentType);
}

class PresenceStateEvent extends MedutEvent {
  final String targetUri;
  final PresenceStatus status;
  PresenceStateEvent(this.targetUri, this.status);
}

/// Progress of a network handover (Wi-Fi <-> 4G/5G, VPN, dock/undock).
///
/// Delivered on [Medut.events]. Most stages are not account-scoped, so
/// they do NOT appear on [Account.events].
class NetworkEvent extends MedutEvent {
  final NetworkStage stage;
  final Call? call;
  final String localAddr;
  final int attempt;
  final int maxAttempts;
  final int elapsedMs;

  /// True when the call uses ICE — media recovery is best-effort there.
  final bool ice;
  final MedutError error;

  NetworkEvent({
    required this.stage,
    required this.call,
    required this.localAddr,
    required this.attempt,
    required this.maxAttempts,
    required this.elapsedMs,
    required this.ice,
    required this.error,
  });
}

/// The ICE liveness of a call's media path changed.  See [IceState].
class IceStateEvent extends MedutEvent {
  final Call? call;
  final IceState state;
  final IceState prev;

  /// ICE restarts so far; 1 is the initial session.
  final int generation;
  final IceCandType localType;
  final IceCandType remoteType;

  /// The selected candidate pair as `ip:port`; empty until one is selected.
  final String localAddr;
  final String remoteAddr;

  /// STUN round trip on the selected pair, ms.
  final double rttMs;
  final MedutError error;

  IceStateEvent({
    required this.call,
    required this.state,
    required this.prev,
    required this.generation,
    required this.localType,
    required this.remoteType,
    required this.localAddr,
    required this.remoteAddr,
    required this.rttMs,
    required this.error,
  });
}

class SipTraceEvent extends MedutEvent {
  final MediaDirection direction;
  final String transport;
  final String remoteAddr;
  final String rawMessage;
  final int timestampUs;
  SipTraceEvent(this.direction, this.transport, this.remoteAddr,
      this.rawMessage, this.timestampUs);
}

/// An event type this binding does not know how to decode (newer native
/// library). Carries the raw type id so nothing is silently dropped.
class UnknownEvent extends MedutEvent {
  final int rawType;
  UnknownEvent(this.rawType);
}

// ── Call-audio routes ──────────────────────────────────────────────────────

/// Normalised category of an audio-output route, one enum across Android's
/// AudioDeviceInfo types and iOS's AVAudioSession ports.
enum AudioRouteKind { earpiece, speaker, bluetooth, wired, unknown }

/// A selectable audio-output route for a call.
class AudioRoute {
  const AudioRoute({
    required this.id,
    required this.name,
    required this.kind,
    required this.isActive,
  });

  /// Stable opaque id, handed back to [Medut.setAudioRoute]
  /// ('earpiece' | 'speaker' | 'wired-headset' | 'bluetooth' | 'bt:<dev>').
  final String id;

  /// Device name for headsets; '' for built-ins so the app can localize.
  final String name;

  final AudioRouteKind kind;

  /// Whether call audio currently flows through this route. On pre-31
  /// Android a just-selected Bluetooth route stays inactive until the SCO
  /// link actually connects (0.5–2s) — an `onAudioRoutesChanged` push
  /// follows when it does.
  final bool isActive;

  static AudioRoute _fromMessage(AudioRouteMessage m) => AudioRoute(
        id: m.id,
        name: m.name,
        kind: switch (m.kind) {
          'earpiece' => AudioRouteKind.earpiece,
          'speaker' => AudioRouteKind.speaker,
          'bluetooth' => AudioRouteKind.bluetooth,
          'wired-headset' => AudioRouteKind.wired,
          _ => AudioRouteKind.unknown,
        },
        isActive: m.isActive,
      );

  @override
  String toString() =>
      'AudioRoute($id, $kind${isActive ? ', active' : ''}'
      '${name.isNotEmpty ? ', "$name"' : ''})';
}
