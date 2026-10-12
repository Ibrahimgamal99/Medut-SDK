/// Typed Dart enums mirroring the Medut C enums.
///
/// Every enum carries its C integer as [raw] and can be decoded with
/// `fromRaw()`. Unknown raw values map to a defined fallback rather than
/// throwing, so newer native libraries never crash older Dart code.
library;

import 'ffi_bindings.dart' as c;

/// SIP transport protocol.
enum Transport {
  udp(c.medut_transport_t.MEDUT_TRANSPORT_UDP),
  tcp(c.medut_transport_t.MEDUT_TRANSPORT_TCP),
  tls(c.medut_transport_t.MEDUT_TRANSPORT_TLS),
  ws(c.medut_transport_t.MEDUT_TRANSPORT_WS),
  wss(c.medut_transport_t.MEDUT_TRANSPORT_WSS);

  final int raw;
  const Transport(this.raw);
  static Transport fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => udp);
}

/// Media (SRTP) encryption mode.
enum MediaEncryption {
  none(c.medut_media_enc_t.MEDUT_MEDIA_ENC_NONE),
  sdes(c.medut_media_enc_t.MEDUT_MEDIA_ENC_SDES),
  dtlsSrtp(c.medut_media_enc_t.MEDUT_MEDIA_ENC_DTLS_SRTP);

  final int raw;
  const MediaEncryption(this.raw);
  static MediaEncryption fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => none);
}

/// Which candidates a call may use (WebRTC's `iceTransportPolicy`).
enum IceTransportPolicy {
  /// Host, server-reflexive and relayed candidates.
  all(c.medut_ice_transport_policy_t.MEDUT_ICE_TRANSPORT_ALL),

  /// Relayed only: nothing but TURN candidates is offered or accepted, and a
  /// call with no usable TURN server fails fast instead of leaking a direct
  /// path.
  relay(c.medut_ice_transport_policy_t.MEDUT_ICE_TRANSPORT_RELAY);

  final int raw;
  const IceTransportPolicy(this.raw);
  static IceTransportPolicy fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => all);
}

/// Registration lifecycle state.
enum RegState {
  unregistered(c.medut_reg_state_t.MEDUT_REG_UNREGISTERED),
  registering(c.medut_reg_state_t.MEDUT_REG_REGISTERING),
  registered(c.medut_reg_state_t.MEDUT_REG_REGISTERED),

  /// Terminal: the SDK has stopped trying — wrong credentials, the retry
  /// budget ran out, or the app cancelled the retry.  Needs the app or the
  /// user: [Account.retryNow], or new credentials.
  failed(c.medut_reg_state_t.MEDUT_REG_FAILED),
  unregistering(c.medut_reg_state_t.MEDUT_REG_UNREGISTERING),

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
  reconnecting(c.medut_reg_state_t.MEDUT_REG_RECONNECTING);

  final int raw;
  const RegState(this.raw);
  static RegState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => unregistered);
}

/// Call lifecycle state.
enum CallState {
  calling(c.medut_call_state_t.MEDUT_CALL_CALLING),
  ringing(c.medut_call_state_t.MEDUT_CALL_RINGING),
  established(c.medut_call_state_t.MEDUT_CALL_ESTABLISHED),
  held(c.medut_call_state_t.MEDUT_CALL_HELD),
  ended(c.medut_call_state_t.MEDUT_CALL_ENDED),
  cancelled(c.medut_call_state_t.MEDUT_CALL_CANCELLED),
  failed(c.medut_call_state_t.MEDUT_CALL_FAILED);

  final int raw;
  const CallState(this.raw);

  /// True for states after which the call object is dead.
  bool get isTerminal => this == ended || this == cancelled || this == failed;

  static CallState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => failed);
}

/// Buddy presence status.
enum PresenceStatus {
  unknown(c.medut_presence_status_t.MEDUT_PRESENCE_UNKNOWN),
  open(c.medut_presence_status_t.MEDUT_PRESENCE_OPEN),
  closed(c.medut_presence_status_t.MEDUT_PRESENCE_CLOSED),
  busy(c.medut_presence_status_t.MEDUT_PRESENCE_BUSY);

  final int raw;
  const PresenceStatus(this.raw);
  static PresenceStatus fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => unknown);
}

/// DTMF signalling mode.
enum DtmfMode {
  rfc4733(c.medut_dtmf_mode_t.MEDUT_DTMF_RFC4733),
  sipInfo(c.medut_dtmf_mode_t.MEDUT_DTMF_SIP_INFO),
  auto(c.medut_dtmf_mode_t.MEDUT_DTMF_AUTO);

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
  bestEffort(c.medut_ice_handover_t.MEDUT_ICE_HANDOVER_BEST_EFFORT),

  /// Try once, then report `callMigrationFailed`.  Preferred when calls are
  /// ICE+TURN over cellular: repeating an offer built from the wrong
  /// candidates only lengthens the silence before the app can redial.
  ///
  /// A call whose ICE was restarted keeps the full retry budget regardless.
  failFast(c.medut_ice_handover_t.MEDUT_ICE_HANDOVER_FAIL_FAST);

  final int raw;
  const IceHandover(this.raw);
  static IceHandover fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => bestEffort);
}

/// Jitter buffer type.
enum JitterBufferType {
  adaptive(c.medut_jbuf_type_t.MEDUT_JBUF_ADAPTIVE),
  fixed(c.medut_jbuf_type_t.MEDUT_JBUF_FIXED);

  final int raw;
  const JitterBufferType(this.raw);
}

/// Push notification provider (RFC 8599 pn-provider).
enum PushProvider {
  none(c.medut_push_provider_t.MEDUT_PUSH_PROVIDER_NONE),
  apns(c.medut_push_provider_t.MEDUT_PUSH_PROVIDER_APNS),
  apnsSandbox(c.medut_push_provider_t.MEDUT_PUSH_PROVIDER_APNS_SANDBOX),
  fcm(c.medut_push_provider_t.MEDUT_PUSH_PROVIDER_FCM);

  final int raw;
  const PushProvider(this.raw);
}

/// Which metric crossed its threshold in a [QualityAlertEvent].
enum QualityIssue {
  mos(c.medut_quality_issue_t.MEDUT_QUALITY_MOS),
  loss(c.medut_quality_issue_t.MEDUT_QUALITY_LOSS),
  jitter(c.medut_quality_issue_t.MEDUT_QUALITY_JITTER),
  rtt(c.medut_quality_issue_t.MEDUT_QUALITY_RTT),

  /// No inbound RTP for `MedutConfig.mediaStallMs` while the call is not on
  /// hold — the link is up and the dialog is healthy, but no audio is
  /// arriving.  `value` is the stall duration in ms.  Non-fatal: it fires
  /// again with `recovering = true` when packets resume.  Use
  /// `MedutConfig.rtpTimeoutSeconds` to end such a call instead.
  mediaStall(c.medut_quality_issue_t.MEDUT_QUALITY_MEDIA_STALL);

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
  newState(c.medut_ice_state_t.MEDUT_ICE_NEW),
  gathering(c.medut_ice_state_t.MEDUT_ICE_GATHERING),
  checking(c.medut_ice_state_t.MEDUT_ICE_CHECKING),
  /// A candidate pair is writable.
  connected(c.medut_ice_state_t.MEDUT_ICE_CONNECTED),
  /// The writable pair is also nominated — media path settled.
  completed(c.medut_ice_state_t.MEDUT_ICE_COMPLETED),
  /// The selected pair stopped answering or receiving; checks continue and
  /// the path may recover on its own.
  disconnected(c.medut_ice_state_t.MEDUT_ICE_DISCONNECTED),
  /// Every pair timed out and nothing is left to gather.  The SDK does not
  /// end the call — redialling is the app's decision.
  failed(c.medut_ice_state_t.MEDUT_ICE_FAILED),
  closed(c.medut_ice_state_t.MEDUT_ICE_CLOSED);

  final int raw;
  const IceState(this.raw);
  static IceState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => newState);
}

/// Candidate type of one side of the selected ICE pair.
enum IceCandType {
  host(c.medut_ice_cand_type_t.MEDUT_ICE_CAND_HOST),
  srflx(c.medut_ice_cand_type_t.MEDUT_ICE_CAND_SRFLX),
  prflx(c.medut_ice_cand_type_t.MEDUT_ICE_CAND_PRFLX),
  relay(c.medut_ice_cand_type_t.MEDUT_ICE_CAND_RELAY),
  none(c.medut_ice_cand_type_t.MEDUT_ICE_CAND_NONE);

  final int raw;
  const IceCandType(this.raw);
  static IceCandType fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => none);
}

/// Media direction for SIP trace / media tap.
enum MediaDirection {
  rx(c.medut_media_dir_t.MEDUT_MEDIA_DIR_RX),
  tx(c.medut_media_dir_t.MEDUT_MEDIA_DIR_TX);

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
  changeDetected(c.medut_net_event_t.MEDUT_NET_CHANGE_DETECTED),
  down(c.medut_net_event_t.MEDUT_NET_DOWN),
  up(c.medut_net_event_t.MEDUT_NET_UP),
  transportReset(c.medut_net_event_t.MEDUT_NET_TRANSPORT_RESET),
  reregistering(c.medut_net_event_t.MEDUT_NET_REREGISTERING),
  callMigrating(c.medut_net_event_t.MEDUT_NET_CALL_MIGRATING),
  callMigrateAccepted(c.medut_net_event_t.MEDUT_NET_CALL_MIGRATE_ACCEPTED),
  callMigrated(c.medut_net_event_t.MEDUT_NET_CALL_MIGRATED),
  callMigrationFailed(c.medut_net_event_t.MEDUT_NET_CALL_MIGRATION_FAILED),
  callDeferred(c.medut_net_event_t.MEDUT_NET_CALL_DEFERRED),
  handoverFailed(c.medut_net_event_t.MEDUT_NET_HANDOVER_FAILED),

  /// This call has ICE that could not be re-gathered — the ICE restart the
  /// SDK normally performs on handover was not possible for it — so the
  /// re-INVITE carries the pre-handover candidates.  That recovers a direct or
  /// still-valid TURN-relayed path; if media does not resume, the remedy is to
  /// re-place the call.  `MedutConfig.netIceHandover` decides whether to keep
  /// retrying.  An ICE call that was restarted does not emit this.
  callIceStale(c.medut_net_event_t.MEDUT_NET_CALL_ICE_STALE),

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
  callIceFailed(c.medut_net_event_t.MEDUT_NET_CALL_ICE_FAILED);

  final int raw;
  const NetworkStage(this.raw);
  static NetworkStage fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => changeDetected);
}

/// Medut error codes (negative ints from the C API).
enum MedutError {
  ok(c.medut_error_t.MEDUT_OK),
  invalidArgument(c.medut_error_t.MEDUT_ERR_INVAL),
  outOfMemory(c.medut_error_t.MEDUT_ERR_NOMEM),
  wrongState(c.medut_error_t.MEDUT_ERR_STATE),
  dns(c.medut_error_t.MEDUT_ERR_DNS),
  transport(c.medut_error_t.MEDUT_ERR_TRANSPORT),
  auth(c.medut_error_t.MEDUT_ERR_AUTH),
  server5xx(c.medut_error_t.MEDUT_ERR_SERVER_5XX),
  wsProtocolRejected(c.medut_error_t.MEDUT_ERR_WS_PROTOCOL_REJECTED),
  timeout(c.medut_error_t.MEDUT_ERR_TIMEOUT),
  already(c.medut_error_t.MEDUT_ERR_ALREADY),

  /// 403 Forbidden — the server refused this request, not the credentials.
  /// On a call: a dialplan or policy refusal ("you may not dial that").
  forbidden(c.medut_error_t.MEDUT_ERR_FORBIDDEN);

  final int raw;
  const MedutError(this.raw);
  static MedutError fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => invalidArgument);
}
