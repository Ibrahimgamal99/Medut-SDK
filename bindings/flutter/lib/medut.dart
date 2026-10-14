/// Medut — Flutter plugin over the native Medut SDKs.
///
/// The stack lives in native code (the Kotlin SDK on Android, MedutKit on
/// iOS), which owns it for the whole process lifetime: calls can be received
/// and answered with no Flutter engine running.  This library talks to it
/// over Pigeon; when the app already started the stack natively,
/// [Medut.start] attaches to it.
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

import 'package:flutter/services.dart';

import 'src/enums.dart';
import 'src/messages.g.dart';

export 'src/enums.dart';

part 'src/config.dart';
part 'src/types.dart';

/// A failed SDK call.
///
/// [code] is the SDK's error code name — `NOT_RUNNING`, `CALL_ENDED`,
/// `ACCOUNT_DESTROYED`, `AUTHENTICATION`, `TIMEOUT`, `INVALID_ARGUMENT`, ...
/// — and [rawValue] the C return value (a negative `MEDUT_ERR_*`, or a
/// positive errno).
class MedutException implements Exception {
  final String code;
  final int rawValue;
  final String operation;
  final String? message;

  const MedutException(this.code, this.rawValue, this.operation, this.message);

  /// The C error as a [MedutError], when it is one.
  MedutError? get error => rawValue < 0
      ? MedutError.values.where((e) => e.raw == rawValue).firstOrNull
      : null;

  @override
  String toString() => 'MedutException($code, $rawValue): $operation: '
      '${message ?? ''}';
}

/// Who activates call audio (the audio session on iOS, audio mode and focus
/// on Android).
enum AudioSessionMode {
  /// The SDK does, around each call.  For apps without CallKit / Telecom.
  automatic,

  /// The platform call framework does: CallKit on iOS, Telecom on Android.
  /// Report its activations with [Medut.notifyCallKitAudioActive].  On iOS,
  /// answer from `provider(_:didActivate:)`, not from `CXAnswerCallAction`.
  callFramework,

  /// The app does.
  manual,
}

// ── Call ─────────────────────────────────────────────────────────────────────

/// A call, incoming or outgoing.  The native call lives in the SDK; this is
/// a handle to it by [id].
///
/// Safe to keep after the call ends: the native side holds an ended call
/// until this object is garbage-collected (or a cap of recent ended calls
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

  static final Finalizer<void Function()> _release = Finalizer((f) => f());

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
  Future<bool> transferReject({int scode = 603, String? reason = 'Declined'}) async {
    try {
      await _run('transferReject', (a) => a.callRejectTransfer(id, scode, reason));
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
  Future<void> recordStop() => _run('recordStop', (a) => a.callStopRecording(id));

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

  /// Account-scoped events (registration, this account's calls, ...).
  Stream<MedutEvent> get events => _ctrl.stream;

  void _add(MedutEvent ev) {
    if (!_ctrl.isClosed) _ctrl.add(ev);
  }

  Future<T> _run<T>(String op, Future<T> Function(MedutHostApi api) f) =>
      _sdk._run(op, f);

  Future<void> register() => _run('register', (a) => a.accountRegister(id));
  Future<void> unregister() => _run('unregister', (a) => a.accountUnregister(id));

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
      _run('setRetryPolicy',
          (a) => a.accountSetRetryPolicy(id, initialMs, maxMs, backoff, maxAttempts));

  /// Cancel a pending retry.  A reconnecting account reports
  /// [RegState.failed] and stays there until [register].
  Future<void> cancelRetry() => _run('cancelRetry', (a) => a.accountCancelRetry(id));

  /// Skip the backoff delay and re-register now.
  Future<void> retryNow() => _run('retryNow', (a) => a.accountRetryNow(id));

  /// Send a reachability probe (SIP OPTIONS) now, e.g. on a push wake before
  /// placing a call.
  Future<void> keepaliveNow() =>
      _run('keepaliveNow', (a) => a.accountKeepaliveNow(id));

  /// Update the RFC 8599 push token; null clears the push params.
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
  Future<void> addRegisterHeader(String name, String value) =>
      _run('addRegisterHeader', (a) => a.accountAddRegisterHeader(id, name, value));

  Future<void> sendMessage(String to, String body,
          {String contentType = 'text/plain'}) =>
      _run('sendMessage', (a) => a.accountSendMessage(id, to, body, contentType));

  Future<void> subscribePresence(String targetUri) =>
      _run('subscribePresence', (a) => a.accountSubscribePresence(id, targetUri));

  Future<void> unsubscribePresence(String targetUri) => _run(
      'unsubscribePresence', (a) => a.accountUnsubscribePresence(id, targetUri));

  Future<void> publishPresence(PresenceStatus status) =>
      _run('publishPresence', (a) => a.accountPublishPresence(id, status.raw));

  /// Unregister and free the account, ending its calls.
  Future<void> destroy() async {
    await _run('destroy', (a) => a.accountDestroy(id));
    _sdk._accounts.remove(id);
    await _ctrl.close();
  }

  @override
  String toString() => 'Account($aor)';
}

// ── SDK ──────────────────────────────────────────────────────────────────────

class Medut {
  static Medut? _instance;

  /// The live instance, once [start] completed.
  static Medut? get instance => _instance;

  final MedutHostApi _api;
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

  Medut._(this._api);

  /// Every event from the stack, including the ones that belong to no
  /// account (network handover, log).  Account-scoped events also arrive on
  /// [Account.events].
  Stream<MedutEvent> get events => _ctrl.stream;

  /// True when the native stack was already running — started by the app at
  /// launch or from a push — and this instance attached to it.  Its accounts
  /// and calls are in [accounts] and [calls]; [start]'s config was not used.
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
  /// connectivity callback.
  ///
  /// [telecom] (Android, API 26+) registers every call with Telecom through
  /// the SDK's own self-managed ConnectionService: calls show in the system
  /// call UI and on Bluetooth / Auto / Wear, never fight a cellular call for
  /// audio, and an incoming call rings with no Flutter engine up.  It implies
  /// [AudioSessionMode.callFramework]; the SDK forwards Telecom's audio
  /// activation itself, so [notifyCallKitAudioActive] is not needed.  Request
  /// the notification permission on API 33+ for the built-in incoming-call
  /// notification.  Ignored on iOS.
  static Future<Medut> start({
    MedutConfig config = const MedutConfig(),
    AudioSessionMode audioSession = AudioSessionMode.automatic,
    bool monitorNetwork = true,
    bool telecom = false,
  }) async {
    final existing = _instance;
    if (existing != null) return existing;

    final sdk = Medut._(MedutHostApi());
    // Listen first, so no event that fires while the stack starts is lost.
    MedutEventsApi.setUp(_EventSink(sdk));
    final StartResultMessage result;
    try {
      result = await sdk._run(
          'start',
          (a) => a.start(
              config._toMessage(),
              StartOptionsMessage(
                monitorNetwork: monitorNetwork,
                audioMode: telecom
                    ? AudioSessionMode.callFramework.index
                    : audioSession.index,
                appOwnedAudio: config.appOwnedAudio,
                telecom: telecom,
              )));
    } catch (_) {
      MedutEventsApi.setUp(null);
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
    MedutEventsApi.setUp(null);
    _close();
  }

  /// Tear down the native stack: every call ends, every account unregisters.
  Future<void> shutdown() async {
    try {
      await _run('shutdown', (a) => a.shutdown());
    } finally {
      MedutEventsApi.setUp(null);
      _close();
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
  Future<Account> createAccount(String uri, String password,
      {AccountConfig config = const AccountConfig()}) async {
    final m = await _run(
        'createAccount', (a) => a.createAccount(config._toMessage(uri, password)));
    return _trackAccount(m);
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
  Future<void> setAdaptiveBitrate(bool enabled, {int minBps = 0, int maxBps = 0}) =>
      _run('setAdaptiveBitrate', (a) => a.setAdaptiveBitrate(enabled, minBps, maxBps));

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

  /// Routes call audio can take (earpiece / speaker / Bluetooth / wired).
  Future<List<AudioRoute>> listAudioRoutes() => _run('listAudioRoutes',
      (a) async => (await a.audioRoutes()).map(AudioRoute._fromMessage).toList());

  /// Route call audio to [id] from [listAudioRoutes]; returns the routes as
  /// they are now (Bluetooth may become active a moment later, reported on
  /// [audioRoutes]).
  Future<List<AudioRoute>> setAudioRoute(String id) => _run('setAudioRoute',
      (a) async => (await a.selectAudioRoute(id)).map(AudioRoute._fromMessage).toList());

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
  /// active.
  Future<void> useAppOwnedAudio(bool enable) =>
      _run('useAppOwnedAudio', (a) => a.useAppOwnedAudio(enable));

  /// Diagnostics for the app-owned engine: armed, running, format, frame
  /// counters, last error.
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

  /// Failures of the app-owned engine, which the SDK cannot see.
  Stream<AppOwnedAudioError> get appOwnedAudioErrors =>
      _appOwnedAudioErrors.stream;

  /// Forward the platform call framework's audio activation: CallKit's
  /// `provider(_:didActivate:)` / `didDeactivate` on iOS, Telecom making the
  /// call active / inactive on Android.  Needed with
  /// [AudioSessionMode.callFramework], where the SDK does not activate audio
  /// itself.
  Future<void> notifyCallKitAudioActive(bool active) =>
      _run('notifyCallKitAudioActive', (a) => a.platformCallAudioActive(active));

  // ── Network and capture ─────────────────────────────────────────────────

  Future<void> networkChanged() => _run('networkChanged', (a) => a.networkChanged());
  Future<void> networkSetMonitorInterval(int seconds) =>
      _run('networkSetMonitorInterval', (a) => a.networkSetMonitorInterval(seconds));
  Future<void> networkSetHandoverPolicy(bool reinviteCalls, bool hangupOnFailure) =>
      _run('networkSetHandoverPolicy',
          (a) => a.networkSetHandoverPolicy(reinviteCalls, hangupOnFailure));

  /// The local IP the stack is using; empty when there is none.
  Future<String> networkLocalAddr() =>
      _run('networkLocalAddr', (a) => a.networkLocalAddress());
  Future<bool> networkIsUp() => _run('networkIsUp', (a) => a.networkIsUp());

  /// Capture SIP + RTP to a Wireshark-readable pcap file.
  Future<void> pcapStart(String path) => _run('pcapStart', (a) => a.startPcap(path));
  Future<void> pcapStop() => _run('pcapStop', (a) => a.stopPcap());

  // ── Internals ───────────────────────────────────────────────────────────

  Future<T> _run<T>(String op, Future<T> Function(MedutHostApi api) f) async {
    try {
      return await f(_api);
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

  /// Decode and route one event.
  void _onEvent(EventMessage m) {
    final account = m.accountId == null ? null : _accounts[m.accountId];
    final call = m.call == null ? null : _trackCall(m.call!);
    final error = MedutError.fromRaw(m.error ?? 0);

    final MedutEvent event;
    switch (m.type) {
      case _EventType.log:
        event = LogEvent(m.message ?? '');
      case _EventType.regState:
        final state = RegState.fromRaw(m.state ?? 0);
        account?.regState = state;
        event = RegStateEvent(state, error, m.errorText,
            retryAttempt: m.retryAttempt ?? 0, retryDelayMs: m.retryDelayMs ?? 0);
      case _EventType.incomingCall:
        event = IncomingCallEvent(call!, m.fromUri ?? '', m.displayName);
      case _EventType.callState:
        event = CallStateEvent(call!, CallState.fromRaw(m.state ?? 0), error, m.reason);
      case _EventType.dtmf:
        event = CallDtmfEvent(call!, m.digit ?? '');
      case _EventType.sdpNegotiation:
        event = SdpNegotiationEvent(call!, m.localSdp, m.remoteSdp,
            m.negotiatedCodec, m.negotiatedCrypto);
      case _EventType.sipTrace:
        event = SipTraceEvent(MediaDirection.fromRaw(m.direction ?? 0),
            m.transport ?? '', m.remoteAddress ?? '', m.rawMessage ?? '',
            m.timestampUs ?? 0);
      case _EventType.mediaStats:
        event = MediaStatsEvent(MediaStats._fromMessage(call!, m.stats!));
      case _EventType.registrarWarning:
        event = RegistrarWarningEvent(m.message ?? '');
      case _EventType.transferRequest:
        event = TransferRequestEvent(call!, m.referToUri ?? '', m.isAttended ?? false,
            autoFollowed: m.autoFollowed ?? false);
      case _EventType.mwi:
        event = MwiEvent(m.messagesWaiting ?? false, m.newVoice ?? 0, m.oldVoice ?? 0,
            m.newUrgent ?? 0, m.oldUrgent ?? 0, m.rawBody);
      case _EventType.message:
        event = MessageEvent(m.fromUri ?? '', m.body ?? '', m.contentType ?? 'text/plain');
      case _EventType.presence:
        event = PresenceStateEvent(m.targetUri ?? '', PresenceStatus.fromRaw(m.status ?? 0));
      case _EventType.qualityAlert:
        event = QualityAlertEvent(call!, QualityIssue.fromRaw(m.issue ?? 0),
            m.value ?? 0, m.threshold ?? 0, m.recovering ?? false);
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
        event = TransferFailedEvent(call!, m.reason ?? '');
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
          _routes.add((m.routes ?? const []).map(AudioRoute._fromMessage).toList());
        }
        return;
      case _EventType.appOwnedAudioError:
        if (!_appOwnedAudioErrors.isClosed) {
          _appOwnedAudioErrors.add(AppOwnedAudioError(m.errorCode ?? '', m.message ?? ''));
        }
        return;
      default:
        event = UnknownEvent(m.type);
    }

    if (!_ctrl.isClosed) _ctrl.add(event);
    (account ?? call?.account)?._add(event);
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
