/// Typed Dart enums mirroring the EchoSDK C enums.
///
/// Every enum carries its C integer as [raw] and can be decoded with
/// `fromRaw()`. Unknown raw values map to a defined fallback rather than
/// throwing, so newer native libraries never crash older Dart code.
library;

import 'ffi_bindings.dart' as c;

/// SIP transport protocol.
enum Transport {
  udp(c.echosdk_transport_t.ECHOSDK_TRANSPORT_UDP),
  tcp(c.echosdk_transport_t.ECHOSDK_TRANSPORT_TCP),
  tls(c.echosdk_transport_t.ECHOSDK_TRANSPORT_TLS),
  ws(c.echosdk_transport_t.ECHOSDK_TRANSPORT_WS),
  wss(c.echosdk_transport_t.ECHOSDK_TRANSPORT_WSS);

  final int raw;
  const Transport(this.raw);
  static Transport fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => udp);
}

/// Media (SRTP) encryption mode.
enum MediaEncryption {
  none(c.echosdk_media_enc_t.ECHOSDK_MEDIA_ENC_NONE),
  sdes(c.echosdk_media_enc_t.ECHOSDK_MEDIA_ENC_SDES),
  dtlsSrtp(c.echosdk_media_enc_t.ECHOSDK_MEDIA_ENC_DTLS_SRTP);

  final int raw;
  const MediaEncryption(this.raw);
  static MediaEncryption fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => none);
}

/// Which candidates a call may use (WebRTC's `iceTransportPolicy`).
enum IceTransportPolicy {
  /// Host, server-reflexive and relayed candidates.
  all(c.echosdk_ice_transport_policy_t.ECHOSDK_ICE_TRANSPORT_ALL),

  /// Relayed only: nothing but TURN candidates is offered or accepted, and a
  /// call with no usable TURN server fails fast instead of leaking a direct
  /// path.
  relay(c.echosdk_ice_transport_policy_t.ECHOSDK_ICE_TRANSPORT_RELAY);

  final int raw;
  const IceTransportPolicy(this.raw);
  static IceTransportPolicy fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => all);
}

/// Registration lifecycle state.
enum RegState {
  unregistered(c.echosdk_reg_state_t.ECHOSDK_REG_UNREGISTERED),
  registering(c.echosdk_reg_state_t.ECHOSDK_REG_REGISTERING),
  registered(c.echosdk_reg_state_t.ECHOSDK_REG_REGISTERED),

  /// Terminal: the SDK has stopped trying — wrong credentials, the retry
  /// budget ran out, or the app cancelled the retry.  Needs the app or the
  /// user: [Account.retryNow], or new credentials.
  failed(c.echosdk_reg_state_t.ECHOSDK_REG_FAILED),
  unregistering(c.echosdk_reg_state_t.ECHOSDK_REG_UNREGISTERING),

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
  reconnecting(c.echosdk_reg_state_t.ECHOSDK_REG_RECONNECTING);

  final int raw;
  const RegState(this.raw);
  static RegState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => unregistered);
}

/// Call lifecycle state.
enum CallState {
  calling(c.echosdk_call_state_t.ECHOSDK_CALL_CALLING),
  ringing(c.echosdk_call_state_t.ECHOSDK_CALL_RINGING),
  established(c.echosdk_call_state_t.ECHOSDK_CALL_ESTABLISHED),
  held(c.echosdk_call_state_t.ECHOSDK_CALL_HELD),
  ended(c.echosdk_call_state_t.ECHOSDK_CALL_ENDED),
  cancelled(c.echosdk_call_state_t.ECHOSDK_CALL_CANCELLED),
  failed(c.echosdk_call_state_t.ECHOSDK_CALL_FAILED);

  final int raw;
  const CallState(this.raw);

  /// True for states after which the call object is dead.
  bool get isTerminal => this == ended || this == cancelled || this == failed;

  static CallState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => failed);
}

/// Buddy presence status.
enum PresenceStatus {
  unknown(c.echosdk_presence_status_t.ECHOSDK_PRESENCE_UNKNOWN),
  open(c.echosdk_presence_status_t.ECHOSDK_PRESENCE_OPEN),
  closed(c.echosdk_presence_status_t.ECHOSDK_PRESENCE_CLOSED),
  busy(c.echosdk_presence_status_t.ECHOSDK_PRESENCE_BUSY);

  final int raw;
  const PresenceStatus(this.raw);
  static PresenceStatus fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => unknown);
}

/// DTMF signalling mode.
enum DtmfMode {
  rfc4733(c.echosdk_dtmf_mode_t.ECHOSDK_DTMF_RFC4733),
  sipInfo(c.echosdk_dtmf_mode_t.ECHOSDK_DTMF_SIP_INFO),
  auto(c.echosdk_dtmf_mode_t.ECHOSDK_DTMF_AUTO);

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
  bestEffort(c.echosdk_ice_handover_t.ECHOSDK_ICE_HANDOVER_BEST_EFFORT),

  /// Try once, then report `callMigrationFailed`.  Preferred when calls are
  /// ICE+TURN over cellular: repeating an offer built from the wrong
  /// candidates only lengthens the silence before the app can redial.
  ///
  /// A call whose ICE was restarted keeps the full retry budget regardless.
  failFast(c.echosdk_ice_handover_t.ECHOSDK_ICE_HANDOVER_FAIL_FAST);

  final int raw;
  const IceHandover(this.raw);
  static IceHandover fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => bestEffort);
}

/// Jitter buffer type.
enum JitterBufferType {
  adaptive(c.echosdk_jbuf_type_t.ECHOSDK_JBUF_ADAPTIVE),
  fixed(c.echosdk_jbuf_type_t.ECHOSDK_JBUF_FIXED);

  final int raw;
  const JitterBufferType(this.raw);
}

/// Push notification provider (RFC 8599 pn-provider).
enum PushProvider {
  none(c.echosdk_push_provider_t.ECHOSDK_PUSH_PROVIDER_NONE),
  apns(c.echosdk_push_provider_t.ECHOSDK_PUSH_PROVIDER_APNS),
  apnsSandbox(c.echosdk_push_provider_t.ECHOSDK_PUSH_PROVIDER_APNS_SANDBOX),
  fcm(c.echosdk_push_provider_t.ECHOSDK_PUSH_PROVIDER_FCM);

  final int raw;
  const PushProvider(this.raw);
}

/// Which metric crossed its threshold in a [QualityAlertEvent].
enum QualityIssue {
  mos(c.echosdk_quality_issue_t.ECHOSDK_QUALITY_MOS),
  loss(c.echosdk_quality_issue_t.ECHOSDK_QUALITY_LOSS),
  jitter(c.echosdk_quality_issue_t.ECHOSDK_QUALITY_JITTER),
  rtt(c.echosdk_quality_issue_t.ECHOSDK_QUALITY_RTT),

  /// No inbound RTP for `EchoSDKConfig.mediaStallMs` while the call is not on
  /// hold — the link is up and the dialog is healthy, but no audio is
  /// arriving.  `value` is the stall duration in ms.  Non-fatal: it fires
  /// again with `recovering = true` when packets resume.  Use
  /// `EchoSDKConfig.rtpTimeoutSeconds` to end such a call instead.
  mediaStall(c.echosdk_quality_issue_t.ECHOSDK_QUALITY_MEDIA_STALL);

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
  newState(c.echosdk_ice_state_t.ECHOSDK_ICE_NEW),
  gathering(c.echosdk_ice_state_t.ECHOSDK_ICE_GATHERING),
  checking(c.echosdk_ice_state_t.ECHOSDK_ICE_CHECKING),
  /// A candidate pair is writable.
  connected(c.echosdk_ice_state_t.ECHOSDK_ICE_CONNECTED),
  /// The writable pair is also nominated — media path settled.
  completed(c.echosdk_ice_state_t.ECHOSDK_ICE_COMPLETED),
  /// The selected pair stopped answering or receiving; checks continue and
  /// the path may recover on its own.
  disconnected(c.echosdk_ice_state_t.ECHOSDK_ICE_DISCONNECTED),
  /// Every pair timed out and nothing is left to gather.  The SDK does not
  /// end the call — redialling is the app's decision.
  failed(c.echosdk_ice_state_t.ECHOSDK_ICE_FAILED),
  closed(c.echosdk_ice_state_t.ECHOSDK_ICE_CLOSED);

  final int raw;
  const IceState(this.raw);
  static IceState fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => newState);
}

/// Candidate type of one side of the selected ICE pair.
enum IceCandType {
  host(c.echosdk_ice_cand_type_t.ECHOSDK_ICE_CAND_HOST),
  srflx(c.echosdk_ice_cand_type_t.ECHOSDK_ICE_CAND_SRFLX),
  prflx(c.echosdk_ice_cand_type_t.ECHOSDK_ICE_CAND_PRFLX),
  relay(c.echosdk_ice_cand_type_t.ECHOSDK_ICE_CAND_RELAY),
  none(c.echosdk_ice_cand_type_t.ECHOSDK_ICE_CAND_NONE);

  final int raw;
  const IceCandType(this.raw);
  static IceCandType fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => none);
}

/// Media direction for SIP trace / media tap.
enum MediaDirection {
  rx(c.echosdk_media_dir_t.ECHOSDK_MEDIA_DIR_RX),
  tx(c.echosdk_media_dir_t.ECHOSDK_MEDIA_DIR_TX);

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
  changeDetected(c.echosdk_net_event_t.ECHOSDK_NET_CHANGE_DETECTED),
  down(c.echosdk_net_event_t.ECHOSDK_NET_DOWN),
  up(c.echosdk_net_event_t.ECHOSDK_NET_UP),
  transportReset(c.echosdk_net_event_t.ECHOSDK_NET_TRANSPORT_RESET),
  reregistering(c.echosdk_net_event_t.ECHOSDK_NET_REREGISTERING),
  callMigrating(c.echosdk_net_event_t.ECHOSDK_NET_CALL_MIGRATING),
  callMigrateAccepted(c.echosdk_net_event_t.ECHOSDK_NET_CALL_MIGRATE_ACCEPTED),
  callMigrated(c.echosdk_net_event_t.ECHOSDK_NET_CALL_MIGRATED),
  callMigrationFailed(c.echosdk_net_event_t.ECHOSDK_NET_CALL_MIGRATION_FAILED),
  callDeferred(c.echosdk_net_event_t.ECHOSDK_NET_CALL_DEFERRED),
  handoverFailed(c.echosdk_net_event_t.ECHOSDK_NET_HANDOVER_FAILED),

  /// This call has ICE that could not be re-gathered — the ICE restart the
  /// SDK normally performs on handover was not possible for it — so the
  /// re-INVITE carries the pre-handover candidates.  That recovers a direct or
  /// still-valid TURN-relayed path; if media does not resume, the remedy is to
  /// re-place the call.  `EchoSDKConfig.netIceHandover` decides whether to keep
  /// retrying.  An ICE call that was restarted does not emit this.
  callIceStale(c.echosdk_net_event_t.ECHOSDK_NET_CALL_ICE_STALE),

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
  callIceFailed(c.echosdk_net_event_t.ECHOSDK_NET_CALL_ICE_FAILED);

  final int raw;
  const NetworkStage(this.raw);
  static NetworkStage fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => changeDetected);
}

/// EchoSDK error codes (negative ints from the C API).
enum EchoSDKError {
  ok(c.echosdk_error_t.ECHOSDK_OK),
  invalidArgument(c.echosdk_error_t.ECHOSDK_ERR_INVAL),
  outOfMemory(c.echosdk_error_t.ECHOSDK_ERR_NOMEM),
  wrongState(c.echosdk_error_t.ECHOSDK_ERR_STATE),
  dns(c.echosdk_error_t.ECHOSDK_ERR_DNS),
  transport(c.echosdk_error_t.ECHOSDK_ERR_TRANSPORT),
  auth(c.echosdk_error_t.ECHOSDK_ERR_AUTH),
  server5xx(c.echosdk_error_t.ECHOSDK_ERR_SERVER_5XX),
  wsProtocolRejected(c.echosdk_error_t.ECHOSDK_ERR_WS_PROTOCOL_REJECTED),
  timeout(c.echosdk_error_t.ECHOSDK_ERR_TIMEOUT),
  already(c.echosdk_error_t.ECHOSDK_ERR_ALREADY),

  /// 403 Forbidden — the server refused this request, not the credentials.
  /// On a call: a dialplan or policy refusal ("you may not dial that").
  forbidden(c.echosdk_error_t.ECHOSDK_ERR_FORBIDDEN);

  final int raw;
  const EchoSDKError(this.raw);
  static EchoSDKError fromRaw(int raw) =>
      values.firstWhere((v) => v.raw == raw, orElse: () => invalidArgument);
}
