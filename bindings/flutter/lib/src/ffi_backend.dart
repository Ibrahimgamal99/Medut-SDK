/// Desktop backend: the [MedutHostApi] surface implemented in Dart over
/// `dart:ffi`, for Linux, macOS and Windows.
///
/// On Android and iOS the native SDKs implement this interface behind
/// Pigeon; here the same methods drive the C library directly, so the public
/// Dart API in `medut.dart` cannot tell the two apart:
///
///  - accounts and calls are referenced by generated UUID strings, mapped to
///    the C handles here;
///  - C events become [EventMessage]s, delivered to [FfiBackend.onEvent];
///  - failures throw [PlatformException] with the code names the native SDKs
///    use (`NOT_RUNNING`, `CALL_ENDED`, `AUTHENTICATION`, ...) and `details`
///    = the C return value.
///
/// Like the native SDKs, an ended call stays valid while Dart holds it: the
/// terminal call-state event (which owns the C call's last reference) is kept
/// unreleased until [releaseCall], or until 32 newer ended calls evict it.
///
/// Mobile-only features behave predictably here: `remember` is ignored and
/// the account store is empty, audio routes are an empty list and selecting
/// one does nothing, Telecom/CallKit options are ignored, and [pushWake]
/// throws `UNSUPPORTED`.
library;

import 'dart:ffi';
import 'dart:math';

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';

import 'ffi_bindings.dart' as c;
import 'messages.g.dart';
import 'sdk.dart' as native;

/// The MedutError code name the native SDKs use for a C return value.
String errorCodeName(int rc) {
  if (rc > 0) return 'SYSTEM';
  switch (rc) {
    case -1:
      return 'INVALID_ARGUMENT';
    case -2:
      return 'OUT_OF_MEMORY';
    case -3:
      return 'INVALID_STATE';
    case -4:
      return 'DNS';
    case -5:
      return 'TRANSPORT';
    case -6:
      return 'AUTHENTICATION';
    case -7:
      return 'SERVER_ERROR';
    case -8:
      return 'WEBSOCKET_REJECTED';
    case -9:
      return 'TIMEOUT';
    case -10:
      return 'ALREADY';
    case -11:
      return 'FORBIDDEN';
    case -12:
      return 'TLS_CERTIFICATE';
    default:
      return 'UNKNOWN';
  }
}

const int _errState = -3;
const int _maxEndedCalls = 32;

typedef _EventCb = Void Function(Pointer<c.medut_event_t>, Pointer<Void>);

class _CallEntry {
  _CallEntry(this.handle, this.accountId, this.state, this.isOutgoing,
      this.remoteUri, this.remoteDisplayName);

  final Pointer<c.medut_call> handle;
  final String? accountId;
  int state;
  final bool isOutgoing;
  final String remoteUri;
  final String? remoteDisplayName;

  /// This side asked to end the call ([FfiBackend.callHangup] /
  /// [FfiBackend.callReject]), so a terminal state is local.
  bool endRequestedLocally = false;

  /// The terminal call-state event, kept unreleased: it owns the C call's
  /// last reference, so [handle] stays valid while this entry exists.
  Pointer<c.medut_event_t>? terminalEvent;

  bool get isEnded => state >= 4; // ENDED, CANCELLED, FAILED
}

class FfiBackend implements MedutHostApi {
  FfiBackend({String? libPath}) {
    if (libPath != null) native.setLibPath(libPath);
  }

  /// Where decoded events go; null drops them (still releasing the C event).
  void Function(EventMessage event)? onEvent;

  // MedutHostApi's Pigeon plumbing, meaningless here.
  @override
  // ignore: non_constant_identifier_names
  BinaryMessenger? get pigeonVar_binaryMessenger => null;
  @override
  // ignore: non_constant_identifier_names
  String get pigeonVar_messageChannelSuffix => '';

  /// The backend C events are routed to (the C callback is process-wide).
  static FfiBackend? _active;

  NativeCallable<_EventCb>? _callback;
  bool _running = false;

  final Map<int, String> _accountIds = {};
  final Map<String, Pointer<c.medut_account>> _accounts = {};
  final Map<int, String> _callIds = {};
  final Map<String, _CallEntry> _calls = {};
  final List<String> _endedCalls = [];

  static final Random _random = Random.secure();

  c.MedutBindings get _b => native.nativeBindings;

  // ── Errors ───────────────────────────────────────────────────────────────

  PlatformException _error(int rc, String operation) => PlatformException(
      code: errorCodeName(rc),
      message: '$operation: ${_strerror(rc)} ($rc)',
      details: rc);

  void _check(int rc, String operation) {
    if (rc != 0) throw _error(rc, operation);
  }

  String _strerror(int rc) =>
      _b.medut_strerror(rc).cast<Utf8>().toDartString();

  void _requireRunning(String operation) {
    if (!_running) {
      throw PlatformException(
          code: 'NOT_RUNNING',
          message: '$operation: the Medut stack is not running',
          details: _errState);
    }
  }

  Pointer<c.medut_account> _account(String id, String operation) {
    _requireRunning(operation);
    final h = _accounts[id];
    if (h == null) {
      throw PlatformException(
          code: 'ACCOUNT_DESTROYED',
          message: '$operation: no account $id',
          details: _errState);
    }
    return h;
  }

  _CallEntry _call(String id, String operation) {
    _requireRunning(operation);
    final e = _calls[id];
    if (e == null) {
      throw PlatformException(
          code: 'CALL_ENDED',
          message: '$operation: no call $id (ended and released)',
          details: _errState);
    }
    return e;
  }

  /// A call that has not ended, for operations that make no sense after.
  _CallEntry _liveCall(String id, String operation) {
    final e = _call(id, operation);
    if (e.isEnded) {
      throw PlatformException(
          code: 'CALL_ENDED',
          message: '$operation: the call has ended',
          details: _errState);
    }
    return e;
  }

  static String _uuid() {
    final b = List<int>.generate(16, (_) => _random.nextInt(256));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    final h = b.map((x) => x.toRadixString(16).padLeft(2, '0')).join();
    return '${h.substring(0, 8)}-${h.substring(8, 12)}-${h.substring(12, 16)}-'
        '${h.substring(16, 20)}-${h.substring(20)}';
  }

  // ── Lifecycle ────────────────────────────────────────────────────────────

  @override
  Future<StartResultMessage> start(
      ConfigMessage config, StartOptionsMessage options) async {
    if (_running) return _result(false);
    final b = _b;
    final callback = NativeCallable<_EventCb>.listener(_onNativeEvent);
    _callback = callback;
    _active = this;

    var reattached = false;
    try {
      if (b.medut_is_initialized()) {
        // Started by an earlier instance that detached: take over delivery
        // and adopt what it left.
        _check(
            b.medut_set_event_handler(callback.nativeFunction, nullptr, true),
            'medut_set_event_handler');
        reattached = true;
        _running = true;
        _adopt();
      } else {
        _init(config, options, callback);
        _running = true;
      }
      if (options.appOwnedAudio) {
        _check(b.medut_audio_use_external(true), 'medut_audio_use_external');
      }
    } catch (_) {
      if (!_running) _closeCallback();
      rethrow;
    }
    return _result(reattached);
  }

  StartResultMessage _result(bool reattached) => StartResultMessage(
        reattached: reattached,
        version: _b.medut_version().cast<Utf8>().toDartString(),
        accounts: _accounts.keys.map(_accountMessage).toList(),
        calls: _calls.entries
            .where((e) => !e.value.isEnded)
            .map((e) => _callMessage(e.key, e.value))
            .toList(),
      );

  void _init(ConfigMessage config, StartOptionsMessage options,
      NativeCallable<_EventCb> callback) {
    final b = _b;
    final cfg = calloc<c.medut_config_t>();
    final arena = Arena();
    try {
      b.medut_config_init(cfg);
      // Layout guard: medut_config_init (C) wrote its compile-time sizeof.
      // If the ffigen struct disagrees, every field after the drift lands in
      // the wrong place.
      if (cfg.ref.struct_size != sizeOf<c.medut_config_t>()) {
        throw PlatformException(
            code: 'INVALID_STATE',
            message: 'medut_config_t layout mismatch: native '
                '${cfg.ref.struct_size} bytes vs Dart '
                '${sizeOf<c.medut_config_t>()}; regenerate '
                'ffi_bindings.dart (dart run ffigen) against the medut.h '
                'the library was built from',
            details: _errState);
      }
      _fillConfig(cfg.ref, config, arena);
      if (!options.monitorNetwork && config.netMonitorIntervalSeconds == null) {
        cfg.ref.net_monitor_interval_s = 0;
      }
      // NativeCallable.listener runs the callback after the C call returned,
      // so the C side must hand over event ownership; _dispatch releases.
      cfg.ref.deliver_owned_events = true;
      cfg.ref.event_cb = callback.nativeFunction;
      cfg.ref.event_userdata = nullptr;
      _check(b.medut_init(cfg), 'medut_init');
    } finally {
      arena.releaseAll();
      calloc.free(cfg);
    }
  }

  void _adopt() {
    final b = _b;
    final accounts = <Pointer<c.medut_account>>[];
    final acb = NativeCallable<
            Void Function(Pointer<c.medut_account>, Pointer<Void>)>.isolateLocal(
        (Pointer<c.medut_account> h, Pointer<Void> _) => accounts.add(h));
    try {
      b.medut_account_foreach(acb.nativeFunction, nullptr);
    } finally {
      acb.close();
    }
    for (final h in accounts) {
      _trackAccount(h);
    }

    final calls = <Pointer<c.medut_call>>[];
    final ccb = NativeCallable<
            Void Function(Pointer<c.medut_call>, Pointer<Void>)>.isolateLocal(
        (Pointer<c.medut_call> h, Pointer<Void> _) => calls.add(h));
    try {
      b.medut_call_foreach(ccb.nativeFunction, nullptr);
    } finally {
      ccb.close();
    }
    for (final h in calls) {
      _trackCall(h);
    }
  }

  @override
  Future<void> shutdown() async {
    if (!_running) return;
    _releaseHeldEvents();
    _b.medut_shutdown();
    _running = false;
    _closeCallback();
    _clear();
  }

  /// Stop receiving events, leaving the C stack running for a later
  /// [start] to reattach to.
  void detach() {
    if (!_running) return;
    _b.medut_set_event_handler(nullptr, nullptr, false);
    _releaseHeldEvents();
    _running = false;
    _closeCallback();
    _clear();
  }

  void _closeCallback() {
    _callback?.close();
    _callback = null;
    if (identical(_active, this)) _active = null;
  }

  void _releaseHeldEvents() {
    for (final e in _calls.values) {
      final ev = e.terminalEvent;
      if (ev != null) {
        e.terminalEvent = null;
        _b.medut_event_release(ev);
      }
    }
  }

  void _clear() {
    _accountIds.clear();
    _accounts.clear();
    _callIds.clear();
    _calls.clear();
    _endedCalls.clear();
  }

  @override
  Future<void> releaseCall(String callId) async {
    final e = _calls[callId];
    if (e == null || !e.isEnded) return;
    _forgetCall(callId);
  }

  void _forgetCall(String id) {
    final e = _calls.remove(id);
    _endedCalls.remove(id);
    if (e == null) return;
    if (_callIds[e.handle.address] == id) _callIds.remove(e.handle.address);
    final ev = e.terminalEvent;
    if (ev != null) {
      e.terminalEvent = null;
      _b.medut_event_release(ev);
    }
  }

  // ── Tracking ─────────────────────────────────────────────────────────────

  String _trackAccount(Pointer<c.medut_account> h) {
    final existing = _accountIds[h.address];
    if (existing != null) return existing;
    final id = _uuid();
    _accountIds[h.address] = id;
    _accounts[id] = h;
    return id;
  }

  /// An account handle from an event or a call: looked up, never adopted —
  /// it may belong to an account destroyed since the event was queued.
  String? _accountIdOf(Pointer<c.medut_account> h) =>
      h == nullptr ? null : _accountIds[h.address];

  String _trackCall(Pointer<c.medut_call> h,
      {Pointer<c.medut_account>? account,
      bool? isOutgoing,
      String? remoteUri,
      String? remoteDisplayName,
      int? state}) {
    final existing = _callIds[h.address];
    if (existing != null) return existing;
    final b = _b;
    var outgoing = isOutgoing;
    var uri = remoteUri;
    var display = remoteDisplayName;
    if (outgoing == null || uri == null) {
      final info = calloc<c.medut_call_info_t>();
      try {
        if (b.medut_call_get_info(h, info) == 0) {
          outgoing ??= info.ref.is_outgoing;
          uri ??= _chars(info.ref.peer_uri, 256);
          display ??= _chars(info.ref.peer_display_name, 128);
        }
      } finally {
        calloc.free(info);
      }
    }
    final id = _uuid();
    _callIds[h.address] = id;
    _calls[id] = _CallEntry(
      h,
      _accountIdOf(account ?? b.medut_call_get_account(h)),
      state ?? b.medut_call_get_state(h),
      outgoing ?? false,
      uri ?? '',
      (display == null || display.isEmpty) ? null : display,
    );
    return id;
  }

  AccountMessage _accountMessage(String id) {
    final h = _accounts[id]!;
    return AccountMessage(
        id: id,
        aor: _aor(h),
        registrationState: _b.medut_account_get_reg_state(h));
  }

  String _aor(Pointer<c.medut_account> h) => using((arena) {
        final buf = arena<Char>(320);
        if (_b.medut_account_get_aor(h, buf, 320) != 0) return '';
        return buf.cast<Utf8>().toDartString();
      });

  CallMessage _callMessage(String id, _CallEntry e) => CallMessage(
        id: id,
        accountId: e.accountId,
        state: e.state,
        isOutgoing: e.isOutgoing,
        remoteUri: e.remoteUri,
        remoteDisplayName: e.remoteDisplayName,
      );

  CallMessage? _callMessageFor(Pointer<c.medut_call> h) {
    if (h == nullptr) return null;
    final id = _trackCall(h);
    return _callMessage(id, _calls[id]!);
  }

  // ── Events ───────────────────────────────────────────────────────────────

  static void _onNativeEvent(Pointer<c.medut_event_t> ev, Pointer<Void> _) {
    final backend = _active;
    if (backend == null || !backend._running) {
      // Torn down while events were in flight: still ours to release.
      native.nativeBindings.medut_event_release(ev);
      return;
    }
    backend._dispatch(ev);
  }

  void _dispatch(Pointer<c.medut_event_t> ev) {
    EventMessage? message;
    var held = false;
    try {
      (message, held) = _decode(ev);
    } finally {
      if (!held) _b.medut_event_release(ev);
    }
    if (message != null) onEvent?.call(message);
  }

  /// The message for one event, and whether the event is now held (a
  /// terminal call state, see [_CallEntry.terminalEvent]).
  (EventMessage?, bool) _decode(Pointer<c.medut_event_t> ev) {
    final u = ev.ref.u;
    final type = ev.ref.type;
    switch (type) {
      case c.medut_event_type_t.MEDUT_EV_LOG:
        return (EventMessage(type: type, message: _str(u.log.message)), false);

      case c.medut_event_type_t.MEDUT_EV_REG_STATE:
        final r = u.reg;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(r.account),
            state: r.state,
            error: r.error == 0 ? null : r.error,
            errorText: _str(r.error_str),
            retryAttempt: r.retry_attempt,
            retryDelayMs: r.retry_delay_ms,
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_INCOMING_CALL:
        final i = u.incoming;
        final from = _str(i.from_uri) ?? '';
        final display = _str(i.display_name);
        final id = _trackCall(i.call,
            account: i.account,
            isOutgoing: false,
            remoteUri: from,
            remoteDisplayName: display,
            state: 1 /* RINGING */);
        final e = _calls[id]!;
        if (!e.isEnded) e.state = 1;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(i.account),
            call: _callMessage(id, e),
            fromUri: from,
            displayName: display,
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_CALL_STATE:
        final s = u.call_state;
        final id = _trackCall(s.call, account: s.account, state: s.state);
        final e = _calls[id]!;
        final wasEnded = e.isEnded;
        e.state = s.state;
        var held = false;
        if (e.isEnded && !wasEnded && e.terminalEvent == null) {
          e.terminalEvent = ev;
          held = true;
          _endedCalls.add(id);
          while (_endedCalls.length > _maxEndedCalls) {
            _forgetCall(_endedCalls.first);
          }
        }
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(s.account),
            call: _callMessage(id, e),
            state: s.state,
            error: s.error == 0 ? null : s.error,
            reason: _str(s.reason),
            endedLocally: e.isEnded && e.endRequestedLocally,
          ),
          held
        );

      case c.medut_event_type_t.MEDUT_EV_CALL_DTMF:
        final d = u.dtmf;
        return (
          EventMessage(
              type: type,
              call: _callMessageFor(d.call),
              digit: String.fromCharCode(d.digit)),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_SDP_NEGOTIATION:
        final s = u.sdp;
        return (
          EventMessage(
            type: type,
            call: _callMessageFor(s.call),
            localSdp: _str(s.local_sdp),
            remoteSdp: _str(s.remote_sdp),
            negotiatedCodec: _str(s.negotiated_codec),
            negotiatedCrypto: _str(s.negotiated_crypto),
            rejectedCodecs: _strv(s.rejected_codecs),
            warnings: _strv(s.warnings),
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_SIP_TRACE:
        final t = u.sip_trace;
        return (
          EventMessage(
            type: type,
            direction: t.dir,
            transport: _str(t.transport) ?? '',
            remoteAddress: _str(t.remote_addr) ?? '',
            rawMessage: _str(t.raw_message) ?? '',
            timestampUs: t.timestamp_us,
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_MEDIA_STATS:
        final s = u.stats;
        return (
          EventMessage(
              type: type, call: _callMessageFor(s.call), stats: _stats(s)),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_REGISTRAR_WARNING:
        return (
          EventMessage(type: type, message: _str(u.reg_warn.message)),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_TRANSFER_REQUEST:
        final t = u.transfer_req;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(t.account),
            call: _callMessageFor(t.call),
            referToUri: _str(t.refer_to_uri) ?? '',
            isAttended: t.has_replaces,
            autoFollowed: t.auto_followed,
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_TRANSFER_FAILED:
        final t = u.transfer_failed;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(t.account),
            call: _callMessageFor(t.call),
            reason: _str(t.reason) ?? '',
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_MWI:
        final m = u.mwi;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(m.account),
            messagesWaiting: m.messages_waiting,
            newVoice: m.new_voice,
            oldVoice: m.old_voice,
            newUrgent: m.new_urgent,
            oldUrgent: m.old_urgent,
            rawBody: _str(m.raw_body),
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_MESSAGE:
        final m = u.msg;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(m.account),
            fromUri: _str(m.from_uri) ?? '',
            body: _str(m.body) ?? '',
            contentType: _str(m.content_type) ?? 'text/plain',
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_PRESENCE_STATE:
        final p = u.presence;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(p.account),
            targetUri: _str(p.target_uri) ?? '',
            status: p.status,
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_QUALITY_ALERT:
        final q = u.quality_alert;
        return (
          EventMessage(
            type: type,
            call: _callMessageFor(q.call),
            issue: q.issue,
            value: q.value,
            threshold: q.threshold,
            recovering: q.recovering,
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_NETWORK:
        final n = u.network;
        return (
          EventMessage(
            type: type,
            accountId: _accountIdOf(n.account),
            call: _callMessageFor(n.call),
            stage: n.event,
            localAddress: _str(n.local_addr) ?? '',
            attempt: n.attempt,
            maxAttempts: n.max_attempts,
            elapsedMs: n.elapsed_ms,
            ice: n.ice,
            error: n.error == 0 ? null : n.error,
          ),
          false
        );

      case c.medut_event_type_t.MEDUT_EV_ICE_STATE:
        final i = u.ice;
        return (
          EventMessage(
            type: type,
            call: _callMessageFor(i.call),
            state: i.state,
            previousState: i.prev,
            generation: i.generation,
            localType: i.local_type,
            remoteType: i.remote_type,
            localAddress: _str(i.local_addr) ?? '',
            remoteAddress: _str(i.remote_addr) ?? '',
            rttMs: i.rtt_ms,
            error: i.error == 0 ? null : i.error,
          ),
          false
        );

      default:
        return (EventMessage(type: type), false);
    }
  }

  // ── Accounts ─────────────────────────────────────────────────────────────

  @override
  Future<AccountMessage> createAccount(
      AccountConfigMessage config, bool remember) async {
    _requireRunning('medut_account_create');
    // `remember` needs a native account store and a native start to replay
    // it; desktop has neither, so it is ignored (documented).
    final id = using((arena) {
      final cfg = arena<c.medut_account_config_t>();
      _fillAccountConfig(cfg.ref, config, arena);
      final out = arena<Pointer<c.medut_account>>();
      _check(_b.medut_account_create(cfg, out), 'medut_account_create');
      if (out.value == nullptr) throw _error(-2, 'medut_account_create');
      return _trackAccount(out.value);
    });
    return _accountMessage(id);
  }

  /// Idempotent: an account this backend does not hold is a no-op, as on
  /// Android and iOS.
  @override
  Future<void> accountDestroy(String accountId, bool forget) async {
    final h = _accounts.remove(accountId);
    if (h == null || !_running) return;
    _accountIds.remove(h.address);
    _b.medut_account_destroy(h);
  }

  @override
  Future<List<String>> rememberedAccounts() async => const [];

  @override
  Future<void> forgetAccount(String uri) async {}

  @override
  Future<void> accountRegister(String accountId) async => _check(
      _b.medut_account_register(
          _account(accountId, 'medut_account_register')),
      'medut_account_register');

  @override
  Future<void> accountRefreshRegistration(String accountId) async => _check(
      _b.medut_account_refresh_registration(
          _account(accountId, 'medut_account_refresh_registration')),
      'medut_account_refresh_registration');

  @override
  Future<void> accountUnregister(String accountId) async => _check(
      _b.medut_account_unregister(
          _account(accountId, 'medut_account_unregister')),
      'medut_account_unregister');

  @override
  Future<int> accountRegistrationState(String accountId) async =>
      _b.medut_account_get_reg_state(
          _account(accountId, 'medut_account_get_reg_state'));

  @override
  Future<void> accountSetRetryPolicy(String accountId, int initialMs,
          int maxMs, double backoff, int maxAttempts) async =>
      _check(
          _b.medut_account_set_retry_policy(
              _account(accountId, 'medut_account_set_retry_policy'),
              initialMs,
              maxMs,
              backoff,
              maxAttempts),
          'medut_account_set_retry_policy');

  @override
  Future<void> accountCancelRetry(String accountId) async => _check(
      _b.medut_account_cancel_retry(
          _account(accountId, 'medut_account_cancel_retry')),
      'medut_account_cancel_retry');

  @override
  Future<void> accountRetryNow(String accountId) async => _check(
      _b.medut_account_retry_now(
          _account(accountId, 'medut_account_retry_now')),
      'medut_account_retry_now');

  @override
  Future<void> accountKeepaliveNow(String accountId) async => _check(
      _b.medut_account_keepalive_now(
          _account(accountId, 'medut_account_keepalive_now')),
      'medut_account_keepalive_now');

  @override
  Future<void> accountSetPushToken(String accountId, String? token) async {
    final h = _account(accountId, 'medut_account_set_push_token');
    using((arena) => _check(
        _b.medut_account_set_push_token(h, _cstr(token, arena)),
        'medut_account_set_push_token'));
  }

  @override
  Future<void> accountSetIceServers(
      String accountId, List<IceServerMessage> servers, int policy) async {
    final h = _account(accountId, 'medut_account_set_ice_servers');
    using((arena) => _check(
        _b.medut_account_set_ice_servers(
            h, _iceServers(servers, arena), servers.length, policy),
        'medut_account_set_ice_servers'));
  }

  @override
  Future<void> accountAddHeader(
      String accountId, String name, String value) async {
    final h = _account(accountId, 'medut_account_add_header');
    using((arena) => _check(
        _b.medut_account_add_header(
            h, _cstr(name, arena), _cstr(value, arena)),
        'medut_account_add_header'));
  }

  @override
  Future<void> accountAddRegisterHeader(
      String accountId, String name, String value) async {
    final h = _account(accountId, 'medut_account_add_register_header');
    using((arena) => _check(
        _b.medut_account_add_register_header(
            h, _cstr(name, arena), _cstr(value, arena)),
        'medut_account_add_register_header'));
  }

  @override
  Future<void> accountSetReliableProvisional(
          String accountId, int mode) async =>
      _check(
          _b.medut_account_set_100rel(
              _account(accountId, 'medut_account_set_100rel'), mode),
          'medut_account_set_100rel');

  @override
  Future<void> accountSendMessage(
      String accountId, String to, String body, String? contentType) async {
    final h = _account(accountId, 'medut_message_send');
    using((arena) => _check(
        _b.medut_message_send(h, _cstr(to, arena), _cstr(body, arena),
            _cstr(contentType ?? 'text/plain', arena)),
        'medut_message_send'));
  }

  @override
  Future<void> accountPublishPresence(String accountId, int status) async =>
      _check(
          _b.medut_account_publish_presence(
              _account(accountId, 'medut_account_publish_presence'), status),
          'medut_account_publish_presence');

  @override
  Future<void> accountSubscribePresence(String accountId, String uri) async {
    final h = _account(accountId, 'medut_account_subscribe_presence');
    using((arena) => _check(
        _b.medut_account_subscribe_presence(h, _cstr(uri, arena)),
        'medut_account_subscribe_presence'));
  }

  @override
  Future<void> accountUnsubscribePresence(String accountId, String uri) async {
    final h = _account(accountId, 'medut_account_unsubscribe_presence');
    using((arena) => _check(
        _b.medut_account_unsubscribe_presence(h, _cstr(uri, arena)),
        'medut_account_unsubscribe_presence'));
  }

  @override
  Future<CallMessage> accountCall(String accountId, String uri) async {
    final h = _account(accountId, 'medut_call_invite');
    final id = using((arena) {
      final out = arena<Pointer<c.medut_call>>();
      _check(_b.medut_call_invite(h, _cstr(uri, arena), out),
          'medut_call_invite');
      if (out.value == nullptr) throw _error(-2, 'medut_call_invite');
      return _trackCall(out.value,
          account: h, isOutgoing: true, remoteUri: uri, state: 0);
    });
    return _callMessage(id, _calls[id]!);
  }

  // ── Calls ────────────────────────────────────────────────────────────────

  @override
  Future<void> callAnswer(String callId) async => _check(
      _b.medut_call_answer(_liveCall(callId, 'medut_call_answer').handle),
      'medut_call_answer');

  /// A no-op on a call that already ended, as on Android and iOS.
  @override
  Future<void> callHangup(String callId) async {
    final e = _call(callId, 'medut_call_hangup');
    if (e.isEnded) return;
    e.endRequestedLocally = true;
    _check(_b.medut_call_hangup(e.handle), 'medut_call_hangup');
  }

  @override
  Future<void> callReject(String callId, int statusCode, String? reason) async {
    final e = _liveCall(callId, 'medut_call_reject');
    e.endRequestedLocally = true;
    using((arena) => _check(
        _b.medut_call_reject(e.handle, statusCode, _cstr(reason, arena)),
        'medut_call_reject'));
  }

  @override
  Future<void> callHold(String callId) async => _check(
      _b.medut_call_hold(_liveCall(callId, 'medut_call_hold').handle),
      'medut_call_hold');

  @override
  Future<void> callResume(String callId) async => _check(
      _b.medut_call_resume(_liveCall(callId, 'medut_call_resume').handle),
      'medut_call_resume');

  @override
  Future<bool> callIsHeld(String callId) async {
    final e = _call(callId, 'medut_call_is_held');
    return !e.isEnded && _b.medut_call_is_held(e.handle);
  }

  @override
  Future<void> callSendDtmf(String callId, String digit) async {
    if (digit.length != 1 || !'0123456789*#ABCDabcd'.contains(digit)) {
      throw PlatformException(
          code: 'INVALID_ARGUMENT',
          message: 'sendDtmf takes one digit (0-9, *, #, A-D), got "$digit"',
          details: -1);
    }
    _check(
        _b.medut_call_send_dtmf(
            _liveCall(callId, 'medut_call_send_dtmf').handle,
            digit.codeUnitAt(0)),
        'medut_call_send_dtmf');
  }

  @override
  Future<void> callTransfer(String callId, String uri) async {
    final e = _liveCall(callId, 'medut_call_transfer');
    using((arena) => _check(
        _b.medut_call_transfer(e.handle, _cstr(uri, arena)),
        'medut_call_transfer'));
  }

  @override
  Future<void> callAttendedTransfer(
      String callId, String consultationCallId) async {
    final a = _liveCall(callId, 'medut_call_attended_transfer');
    final b = _liveCall(consultationCallId, 'medut_call_attended_transfer');
    _check(_b.medut_call_attended_transfer(a.handle, b.handle),
        'medut_call_attended_transfer');
  }

  @override
  Future<CallMessage> callAcceptTransfer(String callId) async {
    final e = _liveCall(callId, 'medut_call_transfer_accept');
    final id = using((arena) {
      final out = arena<Pointer<c.medut_call>>();
      _check(_b.medut_call_transfer_accept(e.handle, out),
          'medut_call_transfer_accept');
      if (out.value == nullptr) throw _error(-2, 'medut_call_transfer_accept');
      return _trackCall(out.value, isOutgoing: true, state: 0);
    });
    return _callMessage(id, _calls[id]!);
  }

  @override
  Future<void> callRejectTransfer(
      String callId, int statusCode, String? reason) async {
    final e = _liveCall(callId, 'medut_call_transfer_reject');
    using((arena) => _check(
        _b.medut_call_transfer_reject(
            e.handle, statusCode, _cstr(reason, arena)),
        'medut_call_transfer_reject'));
  }

  @override
  Future<void> callAddHeader(String callId, String name, String value) async {
    final e = _liveCall(callId, 'medut_call_add_header');
    using((arena) => _check(
        _b.medut_call_add_header(
            e.handle, _cstr(name, arena), _cstr(value, arena)),
        'medut_call_add_header'));
  }

  @override
  Future<void> callSetMuted(String callId, bool muted) async => _check(
      _b.medut_audio_mute(
          _liveCall(callId, 'medut_audio_mute').handle, muted),
      'medut_audio_mute');

  @override
  Future<bool> callIsMuted(String callId) async {
    final e = _call(callId, 'medut_audio_is_muted');
    return !e.isEnded && _b.medut_audio_is_muted(e.handle);
  }

  @override
  Future<void> callSetReceiveMuted(String callId, bool muted) async => _check(
      _b.medut_audio_mute_rx(
          _liveCall(callId, 'medut_audio_mute_rx').handle, muted),
      'medut_audio_mute_rx');

  @override
  Future<void> callStartRecording(String callId, String path) async {
    final e = _liveCall(callId, 'medut_call_record_start');
    using((arena) => _check(
        _b.medut_call_record_start(e.handle, _cstr(path, arena)),
        'medut_call_record_start'));
  }

  @override
  Future<void> callStopRecording(String callId) async => _check(
      _b.medut_call_record_stop(
          _call(callId, 'medut_call_record_stop').handle),
      'medut_call_record_stop');

  @override
  Future<void> callSetDscp(String callId, int dscp) async => _check(
      _b.medut_call_set_dscp_rtp(
          _liveCall(callId, 'medut_call_set_dscp_rtp').handle, dscp),
      'medut_call_set_dscp_rtp');

  @override
  Future<void> callSetRtpTimeout(String callId, int seconds) async => _check(
      _b.medut_call_set_rtp_timeout(
          _liveCall(callId, 'medut_call_set_rtp_timeout').handle, seconds),
      'medut_call_set_rtp_timeout');

  @override
  Future<void> callSetBitrate(String callId, int bps) async => _check(
      _b.medut_call_set_bitrate(
          _liveCall(callId, 'medut_call_set_bitrate').handle, bps),
      'medut_call_set_bitrate');

  @override
  Future<int> callIceState(String callId) async {
    final e = _call(callId, 'medut_call_ice_state');
    return e.isEnded ? 7 /* CLOSED */ : _b.medut_call_ice_state(e.handle);
  }

  @override
  Future<CallInfoMessage> callInfo(String callId) async {
    final e = _call(callId, 'medut_call_get_info');
    return using((arena) {
      final p = arena<c.medut_call_info_t>();
      _check(_b.medut_call_get_info(e.handle, p), 'medut_call_get_info');
      final r = p.ref;
      return CallInfoMessage(
        peerUri: _chars(r.peer_uri, 256),
        peerDisplayName: _chars(r.peer_display_name, 128),
        localUri: _chars(r.local_uri, 256),
        contactUri: _chars(r.contact_uri, 256),
        callId: _chars(r.call_id, 128),
        diverterUri: _chars(r.diverter_uri, 256),
        isOutgoing: r.is_outgoing,
        isRemoteHold: r.is_remote_hold,
        sipStatus: r.sip_status,
        durationMs: r.duration_ms,
        setupDurationMs: r.setup_duration_ms,
        lineNumber: r.line_number,
        transport: r.transport,
        state: r.state,
      );
    });
  }

  @override
  Future<MediaStatsMessage> callStats(String callId) async {
    final e = _call(callId, 'medut_call_get_stats');
    return using((arena) {
      final p = arena<c.medut_ev_media_stats_t>();
      _check(_b.medut_call_get_stats(e.handle, p), 'medut_call_get_stats');
      return _stats(p.ref);
    });
  }

  // ── Audio processing and devices ─────────────────────────────────────────

  @override
  Future<void> setEchoCancellation(bool enabled) async {
    _requireRunning('medut_set_aec');
    _b.medut_set_aec(enabled);
  }

  @override
  Future<void> setAecMode(int mode) async {
    _requireRunning('medut_set_aec_mode');
    _check(_b.medut_set_aec_mode(mode), 'medut_set_aec_mode');
  }

  @override
  Future<void> setAecSuppressionLevel(double level) async {
    _requireRunning('medut_set_aec_suppression_level');
    _b.medut_set_aec_suppression_level(level);
  }

  @override
  Future<void> setNoiseSuppression(bool enabled) async {
    _requireRunning('medut_set_ns');
    _b.medut_set_ns(enabled);
  }

  @override
  Future<void> setAutoGainControl(bool enabled) async {
    _requireRunning('medut_set_agc');
    _b.medut_set_agc(enabled);
  }

  @override
  Future<void> setMicGainDb(double db) async {
    _requireRunning('medut_set_mic_gain_db');
    _b.medut_set_mic_gain_db(db);
  }

  @override
  Future<void> setSpeakerGainDb(double db) async {
    _requireRunning('medut_set_speaker_gain_db');
    _b.medut_set_speaker_gain_db(db);
  }

  @override
  Future<void> setJitterBuffer(int minMs, int maxMs) async {
    _requireRunning('medut_set_jitter_buffer');
    _b.medut_set_jitter_buffer(minMs, maxMs);
  }

  @override
  Future<void> setJitterBufferType(int type) async {
    _requireRunning('medut_set_jitter_buffer_type');
    _b.medut_set_jitter_buffer_type(type);
  }

  @override
  Future<void> setAdaptiveBitrate(bool enabled, int minBps, int maxBps) async {
    _requireRunning('medut_set_adaptive_bitrate');
    _b.medut_set_adaptive_bitrate(enabled, minBps, maxBps);
  }

  @override
  Future<List<AudioDeviceMessage>> inputDevices() async {
    _requireRunning('medut_audio_list_input_devices');
    return _devices(_b.medut_audio_list_input_devices);
  }

  @override
  Future<List<AudioDeviceMessage>> outputDevices() async {
    _requireRunning('medut_audio_list_output_devices');
    return _devices(_b.medut_audio_list_output_devices);
  }

  List<AudioDeviceMessage> _devices(
          int Function(Pointer<c.medut_audio_device_t>, int) list) =>
      using((arena) {
        const max = 32;
        final buf = arena<c.medut_audio_device_t>(max);
        final n = list(buf, max);
        return [
          for (var i = 0; i < n && i < max; i++)
            AudioDeviceMessage(
              name: _chars(buf[i].name, 128),
              label: _chars(buf[i].description, 256),
              isDefault: buf[i].is_default,
            ),
        ];
      });

  @override
  Future<void> setInputDevice(String? name) async {
    _requireRunning('medut_audio_set_input_device');
    using((arena) => _check(
        _b.medut_audio_set_input_device(_cstr(name, arena)),
        'medut_audio_set_input_device'));
  }

  @override
  Future<void> setOutputDevice(String? name) async {
    _requireRunning('medut_audio_set_output_device');
    using((arena) => _check(
        _b.medut_audio_set_output_device(_cstr(name, arena)),
        'medut_audio_set_output_device'));
  }

  // ── Call-audio routes: none on desktop ───────────────────────────────────

  @override
  Future<List<AudioRouteMessage>> audioRoutes() async => const [];

  @override
  Future<List<AudioRouteMessage>> selectAudioRoute(String id) async =>
      const [];

  @override
  Future<void> setSpeakerphone(bool on) async {}

  @override
  Future<void> setExternalRouting(bool on) async {}

  // ── App-owned audio ──────────────────────────────────────────────────────

  @override
  Future<void> useAppOwnedAudio(bool enable) async {
    _requireRunning('medut_audio_use_external');
    _check(_b.medut_audio_use_external(enable), 'medut_audio_use_external');
  }

  /// The capture/playback loops are the app's own on desktop: nothing to
  /// report.
  @override
  Future<Map<String, Object?>> appOwnedAudioStatus() async => const {};

  @override
  Future<bool> appOwnedAudioActive() async =>
      _running && _b.medut_audio_external_is_active();

  @override
  Future<ExternalAudioFormatMessage?> appOwnedAudioFormat() async {
    if (!_running) return null;
    return using((arena) {
      final srate = arena<Uint32>();
      final ch = arena<Uint8>();
      final ptime = arena<Uint32>();
      if (_b.medut_audio_external_format(srate, ch, ptime) != 0) return null;
      return ExternalAudioFormatMessage(
          sampleRate: srate.value, channels: ch.value, ptimeMs: ptime.value);
    });
  }

  @override
  Future<void> platformCallAudioActive(bool active) async {}

  // ── Network and capture ──────────────────────────────────────────────────

  @override
  Future<void> networkChanged() async {
    _requireRunning('medut_network_changed');
    _check(_b.medut_network_changed(), 'medut_network_changed');
  }

  @override
  Future<void> networkSetMonitorInterval(int seconds) async {
    _requireRunning('medut_network_set_monitor_interval');
    _check(_b.medut_network_set_monitor_interval(seconds),
        'medut_network_set_monitor_interval');
  }

  @override
  Future<void> networkSetHandoverPolicy(
      bool reinviteCalls, bool hangupOnFailure) async {
    _requireRunning('medut_network_set_handover_policy');
    _check(
        _b.medut_network_set_handover_policy(reinviteCalls, hangupOnFailure),
        'medut_network_set_handover_policy');
  }

  @override
  Future<String> networkLocalAddress() async {
    _requireRunning('medut_network_local_addr');
    return using((arena) {
      final buf = arena<Char>(64);
      if (_b.medut_network_local_addr(buf, 64) != 0) return '';
      return buf.cast<Utf8>().toDartString();
    });
  }

  @override
  Future<bool> networkIsUp() async => _running && _b.medut_network_is_up();

  @override
  Future<void> startPcap(String path) async {
    _requireRunning('medut_pcap_start');
    using((arena) => _check(
        _b.medut_pcap_start(_cstr(path, arena)), 'medut_pcap_start'));
  }

  @override
  Future<void> stopPcap() async {
    _requireRunning('medut_pcap_stop');
    _check(_b.medut_pcap_stop(), 'medut_pcap_stop');
  }

  @override
  Future<String> strerror(int code) async => _strerror(code);

  // ── Push ─────────────────────────────────────────────────────────────────

  @override
  Future<bool> pushWake(Map<String, String> data, int waitForCallMs) async =>
      throw PlatformException(
          code: 'UNSUPPORTED',
          message: 'pushWake: push wake-up is Android-only; desktop has no push',
          details: 0);

  /// Every account takes the new token; failures are per account and do
  /// not stop the others, as with the native SDKs.
  @override
  Future<void> pushNewToken(String token) async {
    if (!_running) return;
    using((arena) {
      final p = _cstr(token, arena);
      for (final h in _accounts.values) {
        _b.medut_account_set_push_token(h, p);
      }
    });
  }

  // ── Marshalling ──────────────────────────────────────────────────────────

  static Pointer<Char> _cstr(String? s, Arena arena) =>
      s == null ? nullptr : s.toNativeUtf8(allocator: arena).cast<Char>();

  static String? _str(Pointer<Char> p) =>
      p == nullptr ? null : p.cast<Utf8>().toDartString();

  /// A NULL-terminated `const char *const *`; empty for NULL.
  static List<String> _strv(Pointer<Pointer<Char>> p) {
    final out = <String>[];
    if (p == nullptr) return out;
    for (var i = 0; p[i] != nullptr; i++) {
      out.add(p[i].cast<Utf8>().toDartString());
    }
    return out;
  }

  /// A fixed-size NUL-terminated char array out of a struct.
  static String _chars(Array<Char> a, int len) {
    final bytes = <int>[];
    for (var i = 0; i < len; i++) {
      final ch = a[i];
      if (ch == 0) break;
      bytes.add(ch & 0xff);
    }
    return String.fromCharCodes(bytes);
  }

  static Pointer<Pointer<Char>> _strvOf(List<String> list, Arena arena) {
    final arr = arena<Pointer<Char>>(list.length + 1);
    for (var i = 0; i < list.length; i++) {
      arr[i] = _cstr(list[i], arena);
    }
    arr[list.length] = nullptr;
    return arr;
  }

  static Pointer<c.medut_ice_server_t> _iceServers(
      List<IceServerMessage> list, Arena arena) {
    if (list.isEmpty) return nullptr;
    final arr = arena<c.medut_ice_server_t>(list.length);
    for (var i = 0; i < list.length; i++) {
      arr[i].url = _cstr(list[i].url, arena);
      arr[i].user = _cstr(list[i].username, arena);
      arr[i].pass = _cstr(list[i].password, arena);
    }
    return arr;
  }

  /// Copy codec names into a fixed `char[8][32]` array, returning how many
  /// were written.  Past the 8th codec and the 31st character of a name the
  /// array has no room; those are dropped.
  static int _codecNames(Array<Array<Char>> names, List<String> codecs) {
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

  /// Override the medut_config_init() defaults with what was set.
  static void _fillConfig(
      c.medut_config_t r, ConfigMessage m, Arena arena) {
    void str(String? v, void Function(Pointer<Char>) set) {
      if (v != null) set(_cstr(v, arena));
    }

    if (m.transport != null) r.transport = m.transport!;
    str(m.localIp, (p) => r.local_ip = p);
    if (m.localPort != null) r.local_port = m.localPort!;
    str(m.bindInterface, (p) => r.bind_interface = p);
    if (m.preferIpv6 != null) r.prefer_ipv6 = m.preferIpv6!;
    str(m.sipDomain, (p) => r.sip_domain = p);
    str(m.serverUrl, (p) => r.server_url = p);
    str(m.serverHost, (p) => r.server_host = p);
    if (m.serverPort != null) r.server_port = m.serverPort!;
    str(m.outboundProxy, (p) => r.outbound_proxy = p);
    str(m.caCertPath, (p) => r.ca_cert_path = p);
    str(m.clientCert, (p) => r.client_cert = p);
    str(m.clientKey, (p) => r.client_key = p);
    if (m.verifyServer != null) r.verify_server = m.verifyServer!;
    str(m.sniHostname, (p) => r.sni_hostname = p);
    str(m.userAgent, (p) => r.user_agent = p);
    str(m.wsOrigin, (p) => r.ws_origin = p);
    final headers = m.wsExtraHeaders;
    if (headers != null && headers.isNotEmpty) {
      r.ws_extra_headers = _strvOf(headers, arena);
    }
    if (m.wsKeepaliveMs != null) r.ws_keepalive_ms = m.wsKeepaliveMs!;
    str(m.stunServer, (p) => r.stun_server = p);
    str(m.turnServer, (p) => r.turn_server = p);
    str(m.turnUser, (p) => r.turn_user = p);
    str(m.turnPass, (p) => r.turn_pass = p);
    if (m.iceEnabled != null) r.ice_enabled = m.iceEnabled!;
    if (m.rtcpMux != null) r.rtcp_mux = m.rtcpMux!;
    final ice = m.iceServers;
    if (ice != null && ice.isNotEmpty) {
      r.ice_servers = _iceServers(ice, arena);
      r.ice_server_count = ice.length;
    }
    if (m.iceTransportPolicy != null) {
      r.ice_transport_policy = m.iceTransportPolicy!;
    }
    if (m.iceGatheringTimeoutMs != null) {
      r.ice_gathering_timeout_ms = m.iceGatheringTimeoutMs!;
    }
    if (m.iceMultipath != null) r.ice_multipath = m.iceMultipath!;
    if (m.mediaEncryption != null) r.media_enc = m.mediaEncryption!;
    final codecs = m.audioCodecs;
    if (codecs != null && codecs.isNotEmpty) {
      r.audio_codec_name_count = _codecNames(r.audio_codec_names, codecs);
    }
    if (m.dscpSip != null) r.dscp_sip = m.dscpSip!;
    if (m.dscpRtp != null) r.dscp_rtp = m.dscpRtp!;
    if (m.aecMode != null) r.aec_mode = m.aecMode!;
    if (m.noiseSuppression != null) r.ns = m.noiseSuppression!;
    if (m.autoGainControl != null) r.agc = m.autoGainControl!;
    if (m.aecSuppressionLevel != null) {
      r.aec_suppression_level = m.aecSuppressionLevel!;
    }
    if (m.micGainDb != null) r.mic_gain_db = m.micGainDb!;
    if (m.speakerGainDb != null) r.speaker_gain_db = m.speakerGainDb!;
    if (m.opusBitrate != null) r.opus.bitrate = m.opusBitrate!;
    if (m.opusComplexity != null) r.opus.complexity = m.opusComplexity!;
    if (m.opusConstantBitrate != null) r.opus.cbr = m.opusConstantBitrate!;
    if (m.opusDtx != null) r.opus.dtx = m.opusDtx!;
    if (m.opusFec != null) r.opus.fec = m.opusFec!;
    if (m.opusStereo != null) r.opus.stereo = m.opusStereo!;
    if (m.platformAudioActivate != null) {
      r.platform_audio_activate = m.platformAudioActivate!;
    }
    if (m.jitterBufferType != null) r.jbuf_type = m.jitterBufferType!;
    if (m.jitterBufferMinMs != null) {
      r.jitter_buffer_min_ms = m.jitterBufferMinMs!;
    }
    if (m.jitterBufferMaxMs != null) {
      r.jitter_buffer_max_ms = m.jitterBufferMaxMs!;
    }
    if (m.regExpires != null) r.reg_expires = m.regExpires!;
    if (m.regRefreshPct != null) r.reg_refresh_pct = m.regRefreshPct!;
    if (m.keepaliveIntervalMs != null) {
      r.keepalive_interval = m.keepaliveIntervalMs!;
    }
    if (m.keepaliveReregister != null) {
      r.keepalive_reregister = m.keepaliveReregister!;
    }
    if (m.regRetryInitialMs != null) {
      r.reg_retry_initial_ms = m.regRetryInitialMs!;
    }
    if (m.regRetryMaxMs != null) r.reg_retry_max_ms = m.regRetryMaxMs!;
    if (m.regRetryBackoff != null) r.reg_retry_backoff = m.regRetryBackoff!;
    if (m.regRetryMaxAttempts != null) {
      r.reg_retry_max_attempts = m.regRetryMaxAttempts!;
    }
    if (m.regRetryJitter != null) r.reg_retry_jitter = m.regRetryJitter!;
    if (m.dnsSrvFailover != null) r.dns_srv_failover = m.dnsSrvFailover!;
    if (m.sipTimerBMs != null) r.sip_timer_b_ms = m.sipTimerBMs!;
    if (m.sipTimerFMs != null) r.sip_timer_f_ms = m.sipTimerFMs!;
    if (m.sessionTimerEnabled != null) {
      r.session_timer_enabled = m.sessionTimerEnabled!;
    }
    if (m.sessionExpiresSeconds != null) {
      r.session_expires_s = m.sessionExpiresSeconds!;
    }
    if (m.sessionMinSeSeconds != null) {
      r.session_min_se_s = m.sessionMinSeSeconds!;
    }
    if (m.statsIntervalMs != null) r.stats_interval_ms = m.statsIntervalMs!;
    if (m.mosMethod != null) r.mos_method = m.mosMethod!;
    if (m.mosAlertThreshold != null) {
      r.mos_alert_threshold = m.mosAlertThreshold!;
    }
    if (m.lossAlertThreshold != null) {
      r.loss_alert_threshold = m.lossAlertThreshold!;
    }
    if (m.jitterAlertThreshold != null) {
      r.jitter_alert_threshold = m.jitterAlertThreshold!;
    }
    if (m.rtpTimeoutSeconds != null) r.rtp_timeout_s = m.rtpTimeoutSeconds!;
    if (m.mediaStallMs != null) r.media_stall_ms = m.mediaStallMs!;
    if (m.adaptiveBitrate != null) r.adaptive_bitrate = m.adaptiveBitrate!;
    if (m.adaptMinBitrate != null) r.adapt_min_bitrate = m.adaptMinBitrate!;
    if (m.adaptMaxBitrate != null) r.adapt_max_bitrate = m.adaptMaxBitrate!;
    if (m.adaptLossDownPct != null) {
      r.adapt_loss_down_pct = m.adaptLossDownPct!;
    }
    if (m.adaptLossUpPct != null) r.adapt_loss_up_pct = m.adaptLossUpPct!;
    if (m.adaptRecoverTicks != null) {
      r.adapt_recover_ticks = m.adaptRecoverTicks!;
    }
    if (m.opusExpectedLossPct != null) {
      r.opus_expected_loss_pct = m.opusExpectedLossPct!;
    }
    if (m.netMonitorIntervalSeconds != null) {
      r.net_monitor_interval_s = m.netMonitorIntervalSeconds!;
    }
    if (m.netSettleMs != null) r.net_settle_ms = m.netSettleMs!;
    if (m.netReinviteCalls != null) r.net_reinvite_calls = m.netReinviteCalls!;
    if (m.netHangupOnMigrationFailure != null) {
      r.net_hangup_on_migration_failure = m.netHangupOnMigrationFailure!;
    }
    if (m.netVerifyMs != null) r.net_verify_ms = m.netVerifyMs!;
    if (m.netMaxAttempts != null) r.net_max_attempts = m.netMaxAttempts!;
    if (m.netIceHandover != null) r.net_ice_handover = m.netIceHandover!;
    str(m.tmpDir, (p) => r.tmp_dir = p);
    if (m.traceSip != null) r.trace_sip = m.traceSip!;
    if (m.traceSdpDiff != null) r.trace_sdp_diff = m.traceSdpDiff!;
    str(m.pcapPath, (p) => r.pcap_path = p);
    // Any int reaches C as is: -1 = no log events, above 3 = like 3.
    if (m.logLevel != null) r.log_level = m.logLevel!;
  }

  /// medut_account_config_t has no init function: zero is its default,
  /// except verify_tls, which the Dart AccountConfig always sends.
  static void _fillAccountConfig(
      c.medut_account_config_t r, AccountConfigMessage m, Arena arena) {
    r.uri = _cstr(m.uri, arena);
    r.password = _cstr(m.password, arena);
    if (m.transport != null) r.transport = m.transport!;
    r.server_url = _cstr(m.serverUrl, arena);
    r.server_host = _cstr(m.serverHost, arena);
    if (m.serverPort != null) r.server_port = m.serverPort!;
    r.auth_user = _cstr(m.authUser, arena);
    r.display_name = _cstr(m.displayName, arena);
    r.outbound_proxy = _cstr(m.outboundProxy, arena);
    if (m.mediaEncryption != null) r.media_enc = m.mediaEncryption!;
    if (m.iceEnabled != null) r.ice_enabled = m.iceEnabled!;
    if (m.rtcpMux != null) {
      r.rtcp_mux = m.rtcpMux!;
      r.rtcp_mux_set = true;
    }
    r.stun_server = _cstr(m.stunServer, arena);
    r.turn_server = _cstr(m.turnServer, arena);
    r.turn_user = _cstr(m.turnUser, arena);
    r.turn_pass = _cstr(m.turnPass, arena);
    final ice = m.iceServers ?? const [];
    r.ice_servers = _iceServers(ice, arena);
    r.ice_server_count = ice.length;
    if (m.iceTransportPolicy != null) {
      r.ice_transport_policy = m.iceTransportPolicy!;
    }
    r.verify_tls = m.verifyTls ?? true;
    if (m.pushProvider != null) r.push_provider = m.pushProvider!;
    r.push_token = _cstr(m.pushToken, arena);
    r.push_param = _cstr(m.pushParam, arena);
    final codecs = m.audioCodecs;
    if (codecs != null && codecs.isNotEmpty) {
      r.audio_codec_name_count = _codecNames(r.audio_codec_names, codecs);
    }
    if (m.dtmfMode != null) r.dtmf_mode = m.dtmfMode!;
  }

  static MediaStatsMessage _stats(c.medut_ev_media_stats_t s) =>
      MediaStatsMessage(
        packetsSent: s.packets_sent,
        packetsReceived: s.packets_received,
        packetsLost: s.packets_lost,
        packetsLostRx: s.packets_lost_rx,
        bytesSent: s.bytes_sent,
        bytesReceived: s.bytes_received,
        txErrors: s.tx_errors,
        rxErrors: s.rx_errors,
        lossPct: s.loss_pct,
        lossPctRx: s.loss_pct_rx,
        jitterMs: s.jitter_ms,
        txJitterMs: s.tx_jitter_ms,
        rttMs: s.rtt_ms,
        jitterBufferMs: s.jitter_buffer_ms,
        jitterBufferLoad: s.jitter_buffer_load,
        latePackets: s.late_packets,
        discardedPackets: s.discarded_packets,
        jitterBufferTargetMs: s.jitter_buffer_target_ms,
        jitterBufferAdaptive: s.jitter_buffer_adaptive,
        plcFrames: s.plc_frames,
        plcRatio: s.plc_ratio,
        fecRecovered: s.fec_recovered,
        plcConcealed: s.plc_concealed,
        fecActive: s.fec_active,
        fecLevelPct: s.fec_level_pct,
        bandwidthKbpsTx: s.bandwidth_kbps_tx,
        bandwidthKbpsRx: s.bandwidth_kbps_rx,
        avgBandwidthKbpsTx: s.avg_bandwidth_kbps_tx,
        avgBandwidthKbpsRx: s.avg_bandwidth_kbps_rx,
        mosLq: s.mos_lq,
        mosCq: s.mos_cq,
        mosLqRx: s.mos_lq_rx,
        mosCqRx: s.mos_cq_rx,
        mosLqMin: s.mos_lq_min,
        mosLqAvg: s.mos_lq_avg,
        mosMethod: s.mos_method,
        codecName: _str(s.codec_name) ?? '',
        codecClockRate: s.codec_clock_rate,
        codecSampleRate: s.codec_sample_rate,
        codecChannels: s.codec_channels,
        payloadType: s.payload_type,
        audioLevelDbov: s.audio_level_dbov,
        micLevelDbov: s.mic_level_dbov,
        ssrcTx: s.ssrc_tx,
        ssrcRx: s.ssrc_rx,
        remoteAddress: _chars(s.remote_addr, 64),
        statsTick: s.stats_tick,
        callDurationMs: s.call_duration_ms,
        isFinal: s.is_final,
        iceState: s.ice_state,
        iceLocalType: s.ice_local_type,
        iceRemoteType: s.ice_remote_type,
        iceRttMs: s.ice_rtt_ms,
        iceGeneration: s.ice_generation,
        iceLocalAddress: _chars(s.ice_local_addr, 64),
        iceRemoteAddress: _chars(s.ice_remote_addr, 64),
      );
}
