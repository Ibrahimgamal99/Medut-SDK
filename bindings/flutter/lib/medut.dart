/// Medut — Flutter SIP softphone SDK.
///
/// One Dart API over two backends:
///
///  - **Android and iOS:** the stack lives in native code (the Kotlin SDK on
///    Android, MedutKit on iOS), which owns it for the whole process
///    lifetime: calls can be received and answered with no Flutter engine
///    running.  This library talks to it over Pigeon; when the app already
///    started the stack natively, [Medut.start] attaches to it.
///  - **Linux, macOS and Windows:** this library drives the C library
///    directly over `dart:ffi` (`lib/src/ffi_backend.dart`), with the same
///    methods, events and errors.
///
/// Quick start:
///
/// ```dart
/// import 'package:medut/medut.dart';
///
/// final sdk = await Medut.start(
///   config: const MedutConfig(statsIntervalMs: 5000),
/// );
/// final account = await sdk.createAccount(
///   'alice@pbx.example.com', 'secret',
///   config: const AccountConfig(
///     serverUrl: 'wss://pbx.example.com:8089/ws',   // WS/WSS need a URL
///     mediaEnc: MediaEncryption.dtlsSrtp,
///     audioCodecs: ['opus', 'ulaw', 'alaw'],
///   ),
/// );
/// await account.register();
///
/// account.events.listen((ev) {
///   if (ev is RegStateEvent && ev.state == RegState.registered) {
///     account.call('bob@pbx.example.com');
///   } else if (ev is IncomingCallEvent) {
///     ev.call.answer();
///   }
/// });
/// ```
library medut;

import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'src/enums.dart';
import 'src/ffi_backend.dart';
import 'src/messages.g.dart';

export 'src/enums.dart';

part 'src/config.dart';
part 'src/types.dart';

/// A failed SDK call.
///
/// [code] is the SDK's error code name — `NOT_RUNNING`, `CALL_ENDED`,
/// `ACCOUNT_DESTROYED`, `AUTHENTICATION`, `TIMEOUT`, `INVALID_ARGUMENT`,
/// `INVALID_STATE`, `SERVER_ERROR`, `WEBSOCKET_REJECTED`, `ALREADY`,
/// `FORBIDDEN`, `TLS_CERTIFICATE`, `DNS`, `TRANSPORT`, `OUT_OF_MEMORY`,
/// `SYSTEM`, `UNKNOWN`, the
/// same names on every platform — plus `UNSUPPORTED` for a feature the
/// platform does not have (see [Medut.pushWake]).  [rawValue] is the C
/// return value: a negative `MEDUT_ERR_*`, a positive errno, or 0 when the
/// failure did not come from C.
class MedutException implements Exception {
  final String code;
  final int rawValue;
  final String operation;
  final String? message;

  const MedutException(
      this.code, this.rawValue, this.operation, this.message);

  /// The C error as an [MedutError], when it is one.
  MedutError? get error => rawValue < 0
      ? MedutError.values.where((e) => e.raw == rawValue).firstOrNull
      : null;

  @override
  String toString() => 'MedutException($code, $rawValue): $operation: '
      '${message ?? ''}';
}

/// Who activates call audio (the audio session on iOS, audio mode and focus
/// on Android).  Ignored on desktop, where nothing needs activating.
enum AudioSessionMode {
  /// The SDK does, around each call.  For apps without CallKit / Telecom.
  automatic(0),

  /// The platform call framework does: CallKit on iOS, Telecom on Android.
  /// Report its activations with [Medut.notifyCallKitAudioActive].  On
  /// iOS, answer from `provider(_:didActivate:)`, not from
  /// `CXAnswerCallAction`.
  callFramework(1),

  /// The app does.
  manual(2);

  /// The value the native SDKs take (`StartOptionsMessage.audioMode`).
  /// Explicit, so reordering the enum cannot change what is sent.
  final int raw;
  const AudioSessionMode(this.raw);
}

// ── Call ─────────────────────────────────────────────────────────────────────

/// A call, incoming or outgoing.  The native call lives in the SDK; this is
/// a handle to it by [id].
///
/// Safe to keep after the call ends: the native side holds an ended call
/// until this object is garbage-collected (or a cap of 32 recent ended calls
/// evicts it, after which methods throw [MedutException] `CALL_ENDED`).
class Call {
  /// The native call's stable id (UUID).
  final String id;

  /// The account this call belongs to; null when it could not be resolved.
  final Account? account;
  final bool isOutgoing;

  /// The far end: the dialled URI for an outgoing call, the From URI for an
  /// incoming one.
  final String remoteUri;
  final String? remoteDisplayName;
  final Medut _sdk;

  /// The state from the latest call-state event.
  CallState state;

  // Lets the native side drop an ended call once nothing here refers to it.
  // The SDK may be detached or shut down by then, so a failure, synchronous
  // or not, is not news.
  static final Finalizer<void Function()> _release = Finalizer((f) {
    try {
      f();
    } catch (_) {}
  });

  Call._(CallMessage m, this.account, this._sdk)
      : id = m.id,
        isOutgoing = m.isOutgoing,
        remoteUri = m.remoteUri,
        remoteDisplayName = m.remoteDisplayName,
        state = CallState.fromRaw(m.state) {
    final api = _sdk._api;
    final callId = id;
    _release.attach(this, () => api.releaseCall(callId).ignore());
  }

  Future<T> _run<T>(String op, Future<T> Function(MedutHostApi api) f) =>
      _sdk._run(op, f);

  /// Accept an incoming call.  With [AudioSessionMode.callFramework] on iOS,
  /// call this once CallKit has activated the audio session.
  Future<void> answer() => _run('answer', (a) => a.callAnswer(id));

  /// End the call; a no-op on a call that already ended.
  Future<void> hangup() => _run('hangup', (a) => a.callHangup(id));

  /// Terminate with an explicit SIP status code — for an unanswered incoming
  /// call this sends the final response (486 "Busy Here", 603 "Decline", ...).
  Future<void> reject({int statusCode = 486, String? reason = 'Busy Here'}) =>
      _run('reject', (a) => a.callReject(id, statusCode, reason));

  Future<void> hold() => _run('hold', (a) => a.callHold(id));
  Future<void> resume() => _run('resume', (a) => a.callResume(id));

  /// True while the call is on hold, either side.
  Future<bool> get isHeld => _run('isHeld', (a) => a.callIsHeld(id));

  /// Current ICE liveness of this call's media path.
  Future<IceState> get iceState => _run(
      'iceState', (a) async => IceState.fromRaw(await a.callIceState(id)));

  Future<void> mute({bool on = true}) =>
      _run('mute', (a) => a.callSetMuted(id, on));
  Future<bool> get isMuted => _run('isMuted', (a) => a.callIsMuted(id));
  Future<void> muteRx({bool on = true}) =>
      _run('muteRx', (a) => a.callSetReceiveMuted(id, on));

  /// digit: '0'-'9', '*', '#', 'A'-'D'.
  Future<void> sendDtmf(String digit) =>
      _run('sendDtmf', (a) => a.callSendDtmf(id, digit));

  /// Blind transfer (REFER).
  Future<void> transfer(String uri) =>
      _run('transfer', (a) => a.callTransfer(id, uri));

  /// Follow an incoming REFER (see [TransferRequestEvent]); returns the new
  /// call placed to the transfer target, or null if it could not be placed.
  /// This call stays up; end it once the new one connects.
  Future<Call?> transferAccept() async {
    try {
      final m = await _run('transferAccept', (a) => a.callAcceptTransfer(id));
      return _sdk._trackCall(m);
    } on MedutException {
      return null;
    }
  }

  /// Refuse an incoming REFER, leaving this call up.  Returns false if the
  /// refusal could not be sent (no transfer was pending).
  Future<bool> transferReject(
      {int scode = 603, String? reason = 'Declined'}) async {
    try {
      await _run(
          'transferReject', (a) => a.callRejectTransfer(id, scode, reason));
      return true;
    } on MedutException {
      return false;
    }
  }

  /// Attended transfer: bridge this call to [other] (REFER w/ Replaces).
  Future<void> attendedTransfer(Call other) =>
      _run('attendedTransfer', (a) => a.callAttendedTransfer(id, other.id));

  /// Add a custom SIP header to subsequent requests in this dialog.
  Future<void> addHeader(String name, String value) =>
      _run('addHeader', (a) => a.callAddHeader(id, name, value));

  /// Metadata about this call: peer, URIs, direction, duration.  Null if it
  /// could not be read.  After the call ends only state and duration are
  /// still filled in.
  Future<CallInfo?> info() async {
    try {
      return CallInfo._fromMessage(await _run('info', (a) => a.callInfo(id)));
    } on MedutException {
      return null;
    }
  }

  /// Current media stats for this call, or null if they could not be read.
  Future<MediaStats?> getStats() async {
    try {
      return MediaStats._fromMessage(
          this, await _run('getStats', (a) => a.callStats(id)));
    } on MedutException {
      return null;
    }
  }

  /// Record both call directions mixed into one WAV file (PCM S16LE).
  Future<void> recordStart(String path) =>
      _run('recordStart', (a) => a.callStartRecording(id, path));
  Future<void> recordStop() =>
      _run('recordStop', (a) => a.callStopRecording(id));

  /// DSCP/TOS on the RTP socket (46 = EF voice, 0 = best effort).
  Future<void> setDscpRtp(int dscp) =>
      _run('setDscpRtp', (a) => a.callSetDscp(id, dscp));

  /// End this call after [seconds] with no inbound RTP; 0 = never.
  Future<void> setRtpTimeout(int seconds) =>
      _run('setRtpTimeout', (a) => a.callSetRtpTimeout(id, seconds));

  /// Encoder bitrate in bps; 0 restores the negotiated rate.
  Future<void> setBitrate(int bitrateBps) =>
      _run('setBitrate', (a) => a.callSetBitrate(id, bitrateBps));

  @override
  String toString() => 'Call($remoteUri, $state)';
}

// ── Account ──────────────────────────────────────────────────────────────────

/// A SIP account.  The native account lives in the SDK; this is a handle to
/// it by [id].
class Account {
  /// The native account's stable id (UUID).
  final String id;

  /// This account's AOR, e.g. `sip:alice@pbx.example.com`.
  final String aor;
  final Medut _sdk;
  final StreamController<MedutEvent> _ctrl =
      StreamController<MedutEvent>.broadcast();

  /// Registration state from the latest registration event (or from the
  /// native stack when this account was adopted).
  RegState regState;

  Account._(AccountMessage m, this._sdk)
      : id = m.id,
        aor = m.aor,
        regState = RegState.fromRaw(m.registrationState);

  /// Account-scoped events: registration, this account's calls, MWI,
  /// messages, presence — plus every [LogEvent] and [RegistrarWarningEvent],
  /// which belong to no account.
  Stream<MedutEvent> get events => _ctrl.stream;

  void _add(MedutEvent ev) {
    if (!_ctrl.isClosed) _ctrl.add(ev);
  }

  Future<T> _run<T>(String op, Future<T> Function(MedutHostApi api) f) =>
      _sdk._run(op, f);

  Future<void> register() => _run('register', (a) => a.accountRegister(id));
  Future<void> unregister() =>
      _run('unregister', (a) => a.accountUnregister(id));

  /// Refresh the registration now: what to send when a push wakes the app,
  /// or when it returns to the foreground.
  ///
  /// A registered account re-sends its REGISTER in place — same Call-ID,
  /// next CSeq, same Contact — so an RFC 8599 server sees the binding
  /// refreshed and releases the INVITE it holds.  [register] starts a new
  /// client instead, which such a server sees as a binding removed and
  /// another added.  An account that is not registered just registers.
  /// Progress arrives as [RegStateEvent]s; a successful refresh reports
  /// [RegState.registered] again with no [RegState.registering] first.
  Future<void> refreshRegistration() =>
      _run('refreshRegistration', (a) => a.accountRefreshRegistration(id));

  /// Read the registration state from the native stack now.
  Future<RegState> refreshRegState() async {
    regState = RegState.fromRaw(
        await _run('regState', (a) => a.accountRegistrationState(id)));
    return regState;
  }

  /// Start an outgoing call.
  Future<Call> call(String uri) async =>
      _sdk._trackCall(await _run('call', (a) => a.accountCall(id, uri)));

  /// Override the retry/backoff policy; [maxAttempts] 0 = retry forever.
  Future<void> setRetryPolicy({
    required int initialMs,
    required int maxMs,
    required double backoff,
    int maxAttempts = 0,
  }) =>
      _run(
          'setRetryPolicy',
          (a) => a.accountSetRetryPolicy(
              id, initialMs, maxMs, backoff, maxAttempts));

  /// Cancel a pending retry.  A reconnecting account reports
  /// [RegState.failed] and stays there until [register].
  Future<void> cancelRetry() =>
      _run('cancelRetry', (a) => a.accountCancelRetry(id));

  /// Skip the backoff delay and re-register now.
  Future<void> retryNow() => _run('retryNow', (a) => a.accountRetryNow(id));

  /// Send a reachability probe (SIP OPTIONS) now, e.g. before placing a
  /// call.  Not what a push wake wants: RFC 8599 servers release a held
  /// INVITE on a REGISTER ([refreshRegistration]).
  Future<void> keepaliveNow() =>
      _run('keepaliveNow', (a) => a.accountKeepaliveNow(id));

  /// Update the RFC 8599 push token; null clears the push params.  A
  /// remembered account keeps the new token, so a push that starts the app
  /// registers with it.  For a token refresh that should reach every
  /// account, see [Medut.pushNewToken]; on Android, prefer
  /// `MedutPush.onNewToken` in the app's FirebaseMessagingService: it
  /// needs no Dart running.
  Future<void> setPushToken(String? token) =>
      _run('setPushToken', (a) => a.accountSetPushToken(id, token));

  /// Replace this account's STUN/TURN list; an empty list falls back to the
  /// global one.  Reaches the next gather (a new call or an ICE restart).
  Future<void> setIceServers(List<IceServer> servers,
          {IceTransportPolicy policy = IceTransportPolicy.all}) =>
      _run(
          'setIceServers',
          (a) => a.accountSetIceServers(
              id, servers.map((s) => s._toMessage()).toList(), policy.raw));

  /// Header on all outgoing requests for this account.
  Future<void> addHeader(String name, String value) =>
      _run('addHeader', (a) => a.accountAddHeader(id, name, value));

  /// Header on REGISTER only (not leaked to call peers).
  Future<void> addRegisterHeader(String name, String value) => _run(
      'addRegisterHeader', (a) => a.accountAddRegisterHeader(id, name, value));

  /// RFC 3262 reliable provisional responses (100rel / PRACK) for this
  /// account's calls.
  Future<void> setReliableProvisional(ReliableProvisionalMode mode) => _run(
      'setReliableProvisional',
      (a) => a.accountSetReliableProvisional(id, mode.raw));

  Future<void> sendMessage(String to, String body,
          {String contentType = 'text/plain'}) =>
      _run('sendMessage',
          (a) => a.accountSendMessage(id, to, body, contentType));

  Future<void> subscribePresence(String targetUri) => _run(
      'subscribePresence', (a) => a.accountSubscribePresence(id, targetUri));

  Future<void> unsubscribePresence(String targetUri) => _run(
      'unsubscribePresence',
      (a) => a.accountUnsubscribePresence(id, targetUri));

  Future<void> publishPresence(PresenceStatus status) =>
      _run('publishPresence', (a) => a.accountPublishPresence(id, status.raw));

  /// Unregister and free the account, ending its calls.  [forget] also
  /// drops it from the native account store (see [Medut.createAccount]).
  ///
  /// Idempotent on every platform: destroying an account the native side no
  /// longer has (destroyed already, or gone with an [Medut.shutdown]) does
  /// nothing and does not throw.
  Future<void> destroy({bool forget = false}) async {
    try {
      await _run('destroy', (a) => a.accountDestroy(id, forget));
    } finally {
      _sdk._accounts.remove(id);
      await _ctrl.close();
    }
  }

  @override
  String toString() => 'Account($aor)';
}

// ── SDK ──────────────────────────────────────────────────────────────────────

class Medut {
  static Medut? _instance;
  static Future<Medut>? _starting;

  /// The live instance, once [start] completed.
  static Medut? get instance => _instance;

  /// Force the backend instead of choosing it by platform: true = the
  /// desktop `dart:ffi` backend, false = Pigeon to the native SDKs.  For
  /// tests, which run on the host but may want either.
  @visibleForTesting
  static bool? debugUseFfiBackend;

  static bool get _useFfi =>
      debugUseFfiBackend ?? !(Platform.isAndroid || Platform.isIOS);

  final MedutHostApi _api;

  /// Non-null on desktop: [_api] itself, for what is not a host call (event
  /// delivery, detach).
  final FfiBackend? _ffi;
  final ConfigMessage _startConfig;
  final StartOptionsMessage _startOptions;
  final Map<String, Account> _accounts = {};
  final Map<String, Call> _calls = {};
  final StreamController<MedutEvent> _ctrl =
      StreamController<MedutEvent>.broadcast();
  final StreamController<List<AudioRoute>> _routes =
      StreamController<List<AudioRoute>>.broadcast();
  final StreamController<AppOwnedAudioError> _appOwnedAudioErrors =
      StreamController<AppOwnedAudioError>.broadcast();

  bool _reattached = false;
  String _version = '';

  Medut._(this._api, this._ffi, this._startConfig, this._startOptions);

  /// Every event from the stack, including the ones that belong to no
  /// account (network handover, log).  Account-scoped events also arrive on
  /// [Account.events].
  Stream<MedutEvent> get events => _ctrl.stream;

  /// True when the native stack was already running — started by the app at
  /// launch or from a push, or left running by [detach] — and this instance
  /// attached to it.  Its accounts and calls are in [accounts] and [calls];
  /// [start]'s config was not used.
  bool get reattached => _reattached;

  /// Native library version.
  String get version => _version;

  Iterable<Account> get accounts => _accounts.values;

  /// Live calls.
  Iterable<Call> get calls => _calls.values;

  /// Start the native stack, or attach to the one already running.
  ///
  /// [audioSession] says who activates call audio (see [AudioSessionMode]).
  /// [monitorNetwork] lets the SDK drive network handover from the OS
  /// connectivity callback (Android/iOS); on desktop, false turns the C
  /// library's interface polling off unless
  /// [MedutConfig.netMonitorIntervalSeconds] sets it.
  ///
  /// [telecom] (Android, API 26+) registers every call with Telecom through
  /// the SDK's own self-managed ConnectionService: calls show in the system
  /// call UI and on Bluetooth / Auto / Wear, never fight a cellular call for
  /// audio, and an incoming call rings with no Flutter engine up.  It implies
  /// [AudioSessionMode.callFramework]; the SDK forwards Telecom's audio
  /// activation itself, so [notifyCallKitAudioActive] is not needed.  Request
  /// the notification permission on API 33+ for the built-in incoming-call
  /// notification.  On iOS it is the SDK's CallKit integration, which does
  /// the same through the system call UI.  Ignored on desktop.
  ///
  /// [libPath] (desktop only) loads the C library from this path instead of
  /// the platform search path (`medut.so` / `medut.dylib` /
  /// `medut.dll`).
  ///
  /// The stack is process-wide, and so is the instance: while one is
  /// running, [start] returns it and the [config] and options passed here
  /// are **not applied** (a debug-console warning says so when they
  /// differ).  [shutdown] first to start with new ones.
  static Future<Medut> start({
    MedutConfig config = const MedutConfig(),
    AudioSessionMode audioSession = AudioSessionMode.automatic,
    bool monitorNetwork = true,
    bool telecom = false,
    String? libPath,
  }) async {
    final configMessage = config._toMessage();
    final options = StartOptionsMessage(
      monitorNetwork: monitorNetwork,
      audioMode:
          telecom ? AudioSessionMode.callFramework.raw : audioSession.raw,
      appOwnedAudio: config.appOwnedAudio,
      telecom: telecom,
    );

    // One start at a time: a second caller waits for the first and gets
    // its instance.
    final pending = _starting;
    if (pending != null) {
      try {
        await pending;
      } catch (_) {
        // The first start failed; this one tries for itself below.
      }
    }
    final existing = _instance;
    if (existing != null) {
      if (existing._startConfig != configMessage ||
          existing._startOptions != options) {
        debugPrint('Medut.start: the SDK is already running, so the config '
            'and options passed to this start() are ignored; the running '
            'instance keeps the ones it started with. Call shutdown() first '
            'to apply new ones.');
      }
      return existing;
    }

    final starting = _start(configMessage, options, libPath);
    _starting = starting;
    try {
      return await starting;
    } finally {
      if (identical(_starting, starting)) _starting = null;
    }
  }

  static Future<Medut> _start(ConfigMessage configMessage,
      StartOptionsMessage options, String? libPath) async {
    final ffi = _useFfi ? FfiBackend(libPath: libPath) : null;
    final sdk =
        Medut._(ffi ?? MedutHostApi(), ffi, configMessage, options);
    // Listen first, so no event that fires while the stack starts is lost.
    if (ffi != null) {
      ffi.onEvent = sdk._onEvent;
    } else {
      MedutEventsApi.setUp(_EventSink(sdk));
    }
    final StartResultMessage result;
    try {
      result = await sdk._run('start', (a) => a.start(configMessage, options));
    } catch (_) {
      sdk._stopListening();
      rethrow;
    }
    sdk._reattached = result.reattached;
    sdk._version = result.version;
    for (final a in result.accounts) {
      sdk._trackAccount(a);
    }
    for (final c in result.calls) {
      sdk._trackCall(c);
    }
    _instance = sdk;
    return sdk;
  }

  /// Stop listening, leaving the native stack running (registered, calls
  /// up).  The next [start] attaches again.
  void detach() {
    _ffi?.detach();
    _stopListening();
    _close();
  }

  /// Tear down the native stack: every call ends, every account unregisters.
  Future<void> shutdown() async {
    try {
      await _run('shutdown', (a) => a.shutdown());
    } finally {
      _stopListening();
      _close();
    }
  }

  void _stopListening() {
    final ffi = _ffi;
    if (ffi != null) {
      ffi.onEvent = null;
    } else {
      MedutEventsApi.setUp(null);
    }
  }

  void _close() {
    _ctrl.close();
    _routes.close();
    _appOwnedAudioErrors.close();
    for (final a in _accounts.values) {
      a._ctrl.close();
    }
    _accounts.clear();
    _calls.clear();
    if (identical(_instance, this)) _instance = null;
  }

  /// Create a SIP account.  Registration starts with [Account.register].
  ///
  /// With [remember], the native SDK keeps the config (encrypted, on the
  /// device) and re-creates and registers the account whenever the stack
  /// starts without Dart: at app launch from `Application.onCreate` /
  /// `application(_:didFinishLaunchingWithOptions:)`, or from a push.  After
  /// such a start this instance finds it in [accounts] with [reattached]
  /// set.  [Account.destroy] with `forget` or [forgetAccount] removes it
  /// again.  Android and iOS only: on desktop nothing starts the stack but
  /// Dart, so [remember] is ignored there and [rememberedAccounts] is empty.
  Future<Account> createAccount(String uri, String password,
      {AccountConfig config = const AccountConfig(),
      bool remember = false}) async {
    final m = await _run('createAccount',
        (a) => a.createAccount(config._toMessage(uri, password), remember));
    return _trackAccount(m);
  }

  /// URIs the native account store holds (see [createAccount]); always
  /// empty on desktop.
  Future<List<String>> rememberedAccounts() =>
      _run('rememberedAccounts', (a) => a.rememberedAccounts());

  /// Drop an account from the native store without touching a live one; a
  /// no-op on desktop.
  Future<void> forgetAccount(String uri) =>
      _run('forgetAccount', (a) => a.forgetAccount(uri));

  /// Human-readable text for an `MEDUT_ERR_*` code (or errno) from the C
  /// library.
  Future<String> strerror(int code) =>
      _run('strerror', (a) => a.strerror(code));

  // ── Push ────────────────────────────────────────────────────────────────

  /// Hand a push the app received in Dart (e.g. from firebase_messaging's
  /// `onBackgroundMessage`) to the native wake-up path.  Android only.
  ///
  /// Runs `MedutPush.wake(context, data)` on a background thread: every
  /// account refreshes its registration, and with Telecom on the phone rings
  /// at once for a call push.  Completes with true when the announced INVITE
  /// arrived within [wait]; false for a cancel push, a plain wake-up, or no
  /// INVITE in time.  The stack must be running — start it from
  /// `Application.onCreate` — or this throws `NOT_RUNNING`; no [Medut]
  /// instance is needed in the calling isolate.
  ///
  /// Prefer calling `MedutPush.wake()` from your own
  /// FirebaseMessagingService (see the README): it needs no Flutter engine.
  /// Throws `UNSUPPORTED` on iOS (PushKit must reach `MedutPush`
  /// natively, see the example AppDelegate) and on desktop.
  static Future<bool> pushWake(Map<String, String> data,
      {Duration wait = const Duration(seconds: 10)}) {
    if (_useFfi) {
      return Future.error(const MedutException('UNSUPPORTED', 0, 'pushWake',
          'push wake-up is Android-only; desktop has no push'));
    }
    return _runStatic('pushWake',
        () => MedutHostApi().pushWake(data, wait.inMilliseconds));
  }

  /// A new push token (FCM `onNewToken`, PushKit `didUpdate`): every account
  /// with a push provider sends it, and the remembered accounts keep it, so
  /// a process a later push starts registers with it.  No [Medut] instance
  /// is needed in the calling isolate.  On iOS pass the token as hex.  On
  /// desktop it sets the token on the running instance's accounts, if any.
  static Future<void> pushNewToken(String token) {
    if (_useFfi) {
      final ffi = _instance?._ffi;
      return ffi == null
          ? Future.value()
          : _runStatic('pushNewToken', () => ffi.pushNewToken(token));
    }
    return _runStatic(
        'pushNewToken', () => MedutHostApi().pushNewToken(token));
  }

  // ── Audio processing and devices ────────────────────────────────────────

  Future<void> setAec(bool enable) =>
      _run('setAec', (a) => a.setEchoCancellation(enable));
  Future<void> setAecMode(AecMode mode) =>
      _run('setAecMode', (a) => a.setAecMode(mode.raw));
  Future<void> setNs(bool enable) =>
      _run('setNs', (a) => a.setNoiseSuppression(enable));
  Future<void> setAgc(bool enable) =>
      _run('setAgc', (a) => a.setAutoGainControl(enable));
  Future<void> setAecSuppressionLevel(double level) =>
      _run('setAecSuppressionLevel', (a) => a.setAecSuppressionLevel(level));
  Future<void> setMicGainDb(double db) =>
      _run('setMicGainDb', (a) => a.setMicGainDb(db));
  Future<void> setSpeakerGainDb(double db) =>
      _run('setSpeakerGainDb', (a) => a.setSpeakerGainDb(db));
  Future<void> setJitterBuffer(int minMs, int maxMs) =>
      _run('setJitterBuffer', (a) => a.setJitterBuffer(minMs, maxMs));
  Future<void> setJitterBufferType(JitterBufferType type) =>
      _run('setJitterBufferType', (a) => a.setJitterBufferType(type.raw));
  Future<void> setAdaptiveBitrate(bool enabled,
          {int minBps = 0, int maxBps = 0}) =>
      _run('setAdaptiveBitrate',
          (a) => a.setAdaptiveBitrate(enabled, minBps, maxBps));

  Future<List<AudioDevice>> listInputDevices() => _run('listInputDevices',
      (a) async => (await a.inputDevices()).map(_device).toList());
  Future<List<AudioDevice>> listOutputDevices() => _run('listOutputDevices',
      (a) async => (await a.outputDevices()).map(_device).toList());
  static AudioDevice _device(AudioDeviceMessage m) =>
      AudioDevice(m.name, m.label, m.isDefault);

  Future<void> setInputDevice(String? name) =>
      _run('setInputDevice', (a) => a.setInputDevice(name));
  Future<void> setOutputDevice(String? name) =>
      _run('setOutputDevice', (a) => a.setOutputDevice(name));

  // ── Call-audio routes ───────────────────────────────────────────────────
  //
  // Android and iOS.  Desktop has no routes: the lists are empty, selecting
  // does nothing and [audioRoutes] never emits; pick a device with
  // [setOutputDevice] there.

  /// Routes call audio can take (earpiece / speaker / Bluetooth / wired).
  Future<List<AudioRoute>> listAudioRoutes() => _run(
      'listAudioRoutes',
      (a) async =>
          (await a.audioRoutes()).map(AudioRoute._fromMessage).toList());

  /// Route call audio to [id] from [listAudioRoutes]; returns the routes as
  /// they are now (Bluetooth may become active a moment later, reported on
  /// [audioRoutes]).
  Future<List<AudioRoute>> setAudioRoute(String id) => _run(
      'setAudioRoute',
      (a) async =>
          (await a.selectAudioRoute(id)).map(AudioRoute._fromMessage).toList());

  /// Loudspeaker on, or back to the best other route.
  Future<void> setSpeakerphone(bool on) =>
      _run('setSpeakerphone', (a) => a.setSpeakerphone(on));

  /// Report-only routing, for apps whose CallKit / Telecom integration owns
  /// the route: routes are still listed and changes reported, selection
  /// does nothing.
  Future<void> setExternalRouting(bool on) =>
      _run('setExternalRouting', (a) => a.setExternalRouting(on));

  /// The routes, each time they change (device plugged, Bluetooth
  /// connected, a system move).
  Stream<List<AudioRoute>> get audioRoutes => _routes.stream;

  // ── App-owned audio ─────────────────────────────────────────────────────

  /// Hand the microphone and speaker to the SDK's app-owned engine
  /// (VoiceProcessingIO on iOS, AudioRecord/AudioTrack through
  /// VOICE_COMMUNICATION on Android), which opens only once call audio is
  /// active.  On desktop this only switches the C library to the external
  /// device; feeding it is the app's business.
  Future<void> useAppOwnedAudio(bool enable) =>
      _run('useAppOwnedAudio', (a) => a.useAppOwnedAudio(enable));

  /// Diagnostics for the app-owned engine: armed, running, format, frame
  /// counters, last error.  Empty on desktop.
  Future<Map<String, Object?>> appOwnedAudioStatus() =>
      _run('appOwnedAudioStatus', (a) => a.appOwnedAudioStatus());

  /// True while a call is capturing or playing through the app-owned device.
  Future<bool> appOwnedAudioActive() =>
      _run('appOwnedAudioActive', (a) => a.appOwnedAudioActive());

  /// What the current call negotiated, or null before it has media.  Poll
  /// it: a re-INVITE can change the codec with no call-state change.
  Future<ExternalAudioFormat?> appOwnedAudioFormat() =>
      _run('appOwnedAudioFormat', (a) async {
        final f = await a.appOwnedAudioFormat();
        return f == null
            ? null
            : ExternalAudioFormat(f.sampleRate, f.channels, f.ptimeMs);
      });

  /// Failures of the app-owned engine, which the SDK cannot see.  Never
  /// emits on desktop.
  Stream<AppOwnedAudioError> get appOwnedAudioErrors =>
      _appOwnedAudioErrors.stream;

  /// Forward the platform call framework's audio activation: CallKit's
  /// `provider(_:didActivate:)` / `didDeactivate` on iOS, Telecom making the
  /// call active / inactive on Android.  Needed with
  /// [AudioSessionMode.callFramework], where the SDK does not activate audio
  /// itself.  A no-op on desktop.
  Future<void> notifyCallKitAudioActive(bool active) => _run(
      'notifyCallKitAudioActive', (a) => a.platformCallAudioActive(active));

  // ── Network and capture ─────────────────────────────────────────────────

  Future<void> networkChanged() =>
      _run('networkChanged', (a) => a.networkChanged());
  Future<void> networkSetMonitorInterval(int seconds) => _run(
      'networkSetMonitorInterval', (a) => a.networkSetMonitorInterval(seconds));
  Future<void> networkSetHandoverPolicy(
          bool reinviteCalls, bool hangupOnFailure) =>
      _run('networkSetHandoverPolicy',
          (a) => a.networkSetHandoverPolicy(reinviteCalls, hangupOnFailure));

  /// The local IP the stack is using; empty when there is none.
  Future<String> networkLocalAddr() =>
      _run('networkLocalAddr', (a) => a.networkLocalAddress());
  Future<bool> networkIsUp() => _run('networkIsUp', (a) => a.networkIsUp());

  /// Capture SIP to a Wireshark-readable pcap file.  RTP, STUN and DTLS are
  /// not captured; for those, capture on the network.
  Future<void> pcapStart(String path) =>
      _run('pcapStart', (a) => a.startPcap(path));
  Future<void> pcapStop() => _run('pcapStop', (a) => a.stopPcap());

  // ── Internals ───────────────────────────────────────────────────────────

  Future<T> _run<T>(String op, Future<T> Function(MedutHostApi api) f) =>
      _runStatic(op, () => f(_api));

  static Future<T> _runStatic<T>(String op, Future<T> Function() f) async {
    try {
      return await f();
    } on PlatformException catch (e) {
      throw MedutException(
          e.code, (e.details is int) ? e.details as int : 0, op, e.message);
    }
  }

  Account _trackAccount(AccountMessage m) {
    final existing = _accounts[m.id];
    if (existing != null) return existing;
    return _accounts[m.id] = Account._(m, this);
  }

  /// The Call for a message, creating it on first sight and taking its state.
  /// Ended calls are dropped from [calls]; the object handed out stays usable.
  Call _trackCall(CallMessage m) {
    final call = _calls[m.id] ??
        Call._(m, m.accountId == null ? null : _accounts[m.accountId], this);
    call.state = CallState.fromRaw(m.state);
    if (call.state.isTerminal) {
      _calls.remove(m.id);
    } else {
      _calls[m.id] = call;
    }
    return call;
  }

  /// Decode and route one event.  An event without what its type needs (a
  /// call, the stats) arrives as [UnknownEvent] rather than throwing.
  void _onEvent(EventMessage m) {
    final account = m.accountId == null ? null : _accounts[m.accountId];
    final mc = m.call;
    final call = mc == null ? null : _trackCall(mc);
    final error = MedutError.fromRaw(m.error ?? 0);

    MedutEvent? event;
    var everyAccount = false;
    switch (m.type) {
      case _EventType.log:
        event = LogEvent(m.message ?? '');
        everyAccount = true;
      case _EventType.regState:
        final state = RegState.fromRaw(m.state ?? 0);
        account?.regState = state;
        event = RegStateEvent(state, error, m.errorText,
            retryAttempt: m.retryAttempt ?? 0,
            retryDelayMs: m.retryDelayMs ?? 0);
      case _EventType.incomingCall:
        if (call != null) {
          event = IncomingCallEvent(call, m.fromUri ?? '', m.displayName);
        }
      case _EventType.callState:
        if (call != null) {
          event = CallStateEvent(
              call, CallState.fromRaw(m.state ?? 0), error, m.reason,
              endedLocally: m.endedLocally ?? false);
        }
      case _EventType.dtmf:
        if (call != null) event = CallDtmfEvent(call, m.digit ?? '');
      case _EventType.sdpNegotiation:
        if (call != null) {
          event = SdpNegotiationEvent(call, m.localSdp, m.remoteSdp,
              m.negotiatedCodec, m.negotiatedCrypto,
              rejectedCodecs: m.rejectedCodecs ?? const [],
              warnings: m.warnings ?? const []);
        }
      case _EventType.sipTrace:
        event = SipTraceEvent(
            MediaDirection.fromRaw(m.direction ?? 0),
            m.transport ?? '',
            m.remoteAddress ?? '',
            m.rawMessage ?? '',
            m.timestampUs ?? 0);
      case _EventType.mediaStats:
        final stats = m.stats;
        if (call != null && stats != null) {
          event = MediaStatsEvent(MediaStats._fromMessage(call, stats));
        }
      case _EventType.registrarWarning:
        event = RegistrarWarningEvent(m.message ?? '');
        everyAccount = true;
      case _EventType.transferRequest:
        if (call != null) {
          event = TransferRequestEvent(
              call, m.referToUri ?? '', m.isAttended ?? false,
              autoFollowed: m.autoFollowed ?? false);
        }
      case _EventType.mwi:
        event = MwiEvent(m.messagesWaiting ?? false, m.newVoice ?? 0,
            m.oldVoice ?? 0, m.newUrgent ?? 0, m.oldUrgent ?? 0, m.rawBody);
      case _EventType.message:
        event = MessageEvent(
            m.fromUri ?? '', m.body ?? '', m.contentType ?? 'text/plain');
      case _EventType.presence:
        event = PresenceStateEvent(
            m.targetUri ?? '', PresenceStatus.fromRaw(m.status ?? 0));
      case _EventType.qualityAlert:
        if (call != null) {
          event = QualityAlertEvent(call, QualityIssue.fromRaw(m.issue ?? 0),
              m.value ?? 0, m.threshold ?? 0, m.recovering ?? false);
        }
      case _EventType.network:
        event = NetworkEvent(
          stage: NetworkStage.fromRaw(m.stage ?? 0),
          call: call,
          localAddr: m.localAddress ?? '',
          attempt: m.attempt ?? 0,
          maxAttempts: m.maxAttempts ?? 0,
          elapsedMs: m.elapsedMs ?? 0,
          ice: m.ice ?? false,
          error: error,
        );
      case _EventType.transferFailed:
        if (call != null) event = TransferFailedEvent(call, m.reason ?? '');
      case _EventType.iceState:
        event = IceStateEvent(
          call: call,
          state: IceState.fromRaw(m.state ?? 0),
          prev: IceState.fromRaw(m.previousState ?? 0),
          generation: m.generation ?? 0,
          localType: IceCandType.fromRaw(m.localType ?? 4),
          remoteType: IceCandType.fromRaw(m.remoteType ?? 4),
          localAddr: m.localAddress ?? '',
          remoteAddr: m.remoteAddress ?? '',
          rttMs: m.rttMs ?? 0,
          error: error,
        );
      case _EventType.audioRoutesChanged:
        if (!_routes.isClosed) {
          _routes.add(
              (m.routes ?? const []).map(AudioRoute._fromMessage).toList());
        }
        return;
      case _EventType.appOwnedAudioError:
        if (!_appOwnedAudioErrors.isClosed) {
          _appOwnedAudioErrors
              .add(AppOwnedAudioError(m.errorCode ?? '', m.message ?? ''));
        }
        return;
    }
    // An unknown type, or a known one without what it needs.
    event ??= UnknownEvent(m.type);

    if (!_ctrl.isClosed) _ctrl.add(event);
    if (everyAccount) {
      for (final a in _accounts.values.toList()) {
        a._add(event);
      }
    } else {
      (account ?? call?.account)?._add(event);
    }
  }
}

/// medut_event_type_t, plus the plugin's own events.
abstract final class _EventType {
  static const log = 0;
  static const regState = 1;
  static const incomingCall = 2;
  static const callState = 3;
  static const dtmf = 4;
  static const sdpNegotiation = 5;
  static const sipTrace = 6;
  static const mediaStats = 7;
  static const registrarWarning = 8;
  static const transferRequest = 9;
  static const mwi = 10;
  static const message = 11;
  static const presence = 12;
  static const qualityAlert = 13;
  static const network = 14;
  static const transferFailed = 15;
  static const iceState = 16;
  static const audioRoutesChanged = 100;
  static const appOwnedAudioError = 101;
}

class _EventSink implements MedutEventsApi {
  final Medut _sdk;
  _EventSink(this._sdk);

  @override
  void onEvent(EventMessage event) => _sdk._onEvent(event);
}
