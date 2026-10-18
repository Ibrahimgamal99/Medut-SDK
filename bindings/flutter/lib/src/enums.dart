/// Typed Dart enums mirroring the Medut C enums (medut.h).
///
/// Every enum carries its C integer as [raw] and can be decoded with
/// `fromRaw()`. Unknown raw values map to a defined fallback rather than
/// throwing, so newer native libraries never crash older Dart code.
///
/// The values are the C ones, written out: they cross the Pigeon channel as
/// is on Android/iOS and go straight into the C structs on desktop, and
/// test/medut_smoke_test.dart checks them against the generated FFI
/// bindings.
library;

/// SIP transport protocol.
enum Transport {
  udp(0),
  tcp(1),
  tls(2),
  ws(3),
  wss(4);

  final int raw;
  const Transport(this.raw);
  static Transport fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => udp);
}

/// Media (SRTP) encryption mode.
enum MediaEncryption {
  none(0),
  sdes(1),
  dtlsSrtp(2);

  final int raw;
  const MediaEncryption(this.raw);
  static MediaEncryption fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => none);
}

/// Which candidates a call may use (WebRTC's `iceTransportPolicy`).
enum IceTransportPolicy {
  /// Host, server-reflexive and relayed candidates.
  all(0),

  /// Relayed only: nothing but TURN candidates is offered or accepted, and a
  /// call with no usable TURN server fails fast instead of leaking a direct
  /// path.
  relay(1);

  final int raw;
  const IceTransportPolicy(this.raw);
  static IceTransportPolicy fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => all);
}

/// Registration lifecycle state.
enum RegState {
  unregistered(0),
  registering(1),
  registered(2),

  /// Terminal: the SDK has stopped trying — wrong credentials, the retry
  /// budget ran out, or the app cancelled the retry.  Needs the app or the
  /// user: [Account.retryNow], or new credentials.
  failed(3),
  unregistering(4),

  /// Transient: the registration is down and the SDK is getting it back on
  /// its own — a retry armed after a timeout or 5xx, a keepalive probe the
  /// proxy stopped answering, or a network handover (Wi-Fi ↔ cellular, VPN,
  /// dock).  Show "Reconnecting…"; there is nothing for the app to do.
  ///
  /// It holds for the whole recovery — the state does not flip back to
  /// [registering] for each attempt — and ends at [registered] or [failed].
  /// One event per attempt: [RegStateEvent.retryAttempt] counts up 1, 2, 3…
  /// for as long as the outage lasts and [RegStateEvent.retryDelayMs] is the
  /// backoff armed for the next one, so the latest event always renders a
  /// complete status line.
  reconnecting(5);

  final int raw;
  const RegState(this.raw);
  static RegState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => unregistered);
}

/// Call lifecycle state.
enum CallState {
  calling(0),
  ringing(1),
  established(2),
  held(3),
  ended(4),
  cancelled(5),
  failed(6);

  final int raw;
  const CallState(this.raw);

  /// True for states after which the call object is dead.
  bool get isTerminal => this == ended || this == cancelled || this == failed;

  static CallState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => failed);
}

/// Buddy presence status.
enum PresenceStatus {
  unknown(0),
  open(1),
  closed(2),
  busy(3);

  final int raw;
  const PresenceStatus(this.raw);
  static PresenceStatus fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => unknown);
}

/// DTMF signalling mode.
enum DtmfMode {
  rfc4733(0),
  sipInfo(1),
  auto(2);

  final int raw;
  const DtmfMode(this.raw);
  static DtmfMode fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => rfc4733);
}

/// Echo cancellation backend.
enum AecMode {
  off(0),
  suppressor(1),
  webrtc(2);

  final int raw;
  const AecMode(this.raw);
  static AecMode fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => suppressor);
}

/// What to do with an ICE call whose candidates could not be re-gathered on
/// handover.
///
/// An ICE call is normally migrated with a full RFC 8445 §9 ICE restart — new
/// credentials, a fresh gather on the new interface — which needs nothing from
/// the app and is not affected by this setting.  It applies to the calls the
/// restart could not be performed for, which fall back to a re-INVITE carrying
/// the pre-handover candidates and are marked [NetworkStage.callIceStale].
enum IceHandover {
  /// Send the re-INVITE and let media verification decide (default).
  bestEffort(0),

  /// Try once, then report `callMigrationFailed`.  Preferred when calls are
  /// ICE+TURN over cellular: repeating an offer built from the wrong
  /// candidates only lengthens the silence before the app can redial.
  ///
  /// A call whose ICE was restarted keeps the full retry budget regardless.
  failFast(1);

  final int raw;
  const IceHandover(this.raw);
  static IceHandover fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => bestEffort);
}

/// Jitter buffer type.
enum JitterBufferType {
  adaptive(0),
  fixed(1);

  final int raw;
  const JitterBufferType(this.raw);
}

/// RFC 3262 reliable provisional responses (100rel / PRACK) for an account;
/// see `Account.setReliableProvisional`.
enum ReliableProvisionalMode {
  /// Never send or require 100rel.
  disabled(0),

  /// Support 100rel when the peer offers it.
  enabled(1),

  /// Require 100rel; reject a peer that does not support it.
  required(2);

  final int raw;
  const ReliableProvisionalMode(this.raw);
  static ReliableProvisionalMode fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => enabled);
}

/// Push notification provider (RFC 8599 pn-provider).
enum PushProvider {
  none(0),
  apns(1),
  apnsSandbox(2),
  fcm(3);

  final int raw;
  const PushProvider(this.raw);
}

/// Which metric crossed its threshold in a [QualityAlertEvent].
enum QualityIssue {
  mos(0),
  loss(1),
  jitter(2),
  rtt(3),

  /// No inbound RTP for `MedutConfig.mediaStallMs` while the call is not on
  /// hold — the link is up and the dialog is healthy, but no audio is
  /// arriving.  `value` is the stall duration in ms.  Non-fatal: it fires
  /// again with `recovering = true` when packets resume.  Use
  /// `MedutConfig.rtpTimeoutSeconds` to end such a call instead.
  mediaStall(4);

  final int raw;
  const QualityIssue(this.raw);
  static QualityIssue fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => mos);
}

/// ICE liveness of a call's media path — the agent's own verdict from its
/// connectivity checks (RFC 8445) and consent freshness (RFC 7675), mirroring
/// the browser's `RTCIceConnectionState`.  Delivered as [IceStateEvent] and
/// readable via `Call.iceState`.
enum IceState {
  newState(0),
  gathering(1),
  checking(2),
  /// A candidate pair is writable.
  connected(3),
  /// The writable pair is also nominated — media path settled.
  completed(4),
  /// The selected pair stopped answering or receiving; checks continue and
  /// the path may recover on its own.
  disconnected(5),
  /// Every pair timed out and nothing is left to gather.  The SDK does not
  /// end the call — redialling is the app's decision.
  failed(6),
  closed(7);

  final int raw;
  const IceState(this.raw);
  static IceState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => newState);
}

/// Candidate type of one side of the selected ICE pair.
enum IceCandType {
  host(0),
  srflx(1),
  prflx(2),
  relay(3),
  none(4);

  final int raw;
  const IceCandType(this.raw);
  static IceCandType fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => none);
}

/// Media direction for SIP trace / media tap.
enum MediaDirection {
  rx(0),
  tx(1);

  final int raw;
  const MediaDirection(this.raw);
  static MediaDirection fromRaw(int raw) => raw == tx.raw ? tx : rx;
}

/// Stages of a network handover (Wi-Fi <-> 4G/5G, VPN up/down).
///
/// A typical Wi-Fi -> cellular handover emits, in order:
/// [changeDetected] -> [transportReset] -> [reregistering]
/// -> [callMigrating] -> [callMigrated].
enum NetworkStage {
  changeDetected(0),
  down(1),
  up(2),
  transportReset(3),
  reregistering(4),
  callMigrating(5),
  callMigrateAccepted(6),
  callMigrated(7),
  callMigrationFailed(8),
  callDeferred(9),
  handoverFailed(10),

  /// This call has ICE that could not be re-gathered — the ICE restart the
  /// SDK normally performs on handover was not possible for it — so the
  /// re-INVITE carries the pre-handover candidates.  That recovers a direct or
  /// still-valid TURN-relayed path; if media does not resume, the remedy is to
  /// re-place the call.  `MedutConfig.netIceHandover` decides whether to keep
  /// retrying.  An ICE call that was restarted does not emit this.
  callIceStale(11),

  /// ICE finished its connectivity checks with no usable candidate pair, so
  /// nothing was nominated and the media path was never validated.
  ///
  /// Unlike [callIceStale] the candidates were gathered and offered and the
  /// peer answered — every pair in the check list then failed, because the peer
  /// rejected the checks (a STUN 401 after it rebuilt its own ICE session
  /// across our restart offer) or the path stopped carrying UDP.
  ///
  /// The call is left up: where a pair had been validated earlier the stream is
  /// held on it and audio usually survives. If it does not, the remedy is to
  /// re-place the call.
  callIceFailed(12);

  final int raw;
  const NetworkStage(this.raw);
  static NetworkStage fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => changeDetected);
}

/// Medut error codes (negative ints from the C API).
enum MedutError {
  ok(0),
  invalidArgument(-1),
  outOfMemory(-2),
  wrongState(-3),
  dns(-4),
  transport(-5),
  auth(-6),
  server5xx(-7),
  wsProtocolRejected(-8),
  timeout(-9),
  already(-10),

  /// 403 Forbidden — the server refused this request, not the credentials.
  /// On a call: a dialplan or policy refusal ("you may not dial that").
  forbidden(-11);

  final int raw;
  const MedutError(this.raw);
  static MedutError fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => invalidArgument);
}
