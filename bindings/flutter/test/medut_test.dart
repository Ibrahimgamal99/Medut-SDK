// Unit tests for the Dart side of the plugin on the mobile (Pigeon) backend:
// the host channels are answered by a fake native side, which also pushes
// events.  What runs on the native side is tested with the SDKs themselves
// (bindings/kotlin, bindings/swift); the desktop FFI backend is exercised
// against the real library in medut_smoke_test.dart.
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:medut/medut.dart';
import 'package:medut/src/messages.g.dart';

const _codec = MedutHostApi.pigeonChannelCodec;
const _host = 'dev.flutter.pigeon.medut.MedutHostApi';
const _events = 'dev.flutter.pigeon.medut.MedutEventsApi.onEvent';

/// Stands in for the native plugin: answers host calls and records them.
class FakeNative {
  final args = <String, List<Object?>>{};

  TestDefaultBinaryMessenger get _messenger =>
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  void on(String method, Object? Function(List<Object?> args) reply) {
    _messenger.setMockMessageHandler('$_host.$method', (ByteData? message) async {
      final decoded = (_codec.decodeMessage(message) as List<Object?>?) ?? const [];
      args[method] = decoded;
      try {
        return _codec.encodeMessage(<Object?>[reply(decoded)]);
      } on PlatformException catch (e) {
        return _codec.encodeMessage(<Object?>[e.code, e.message, e.details]);
      }
    });
  }

  Future<void> push(EventMessage event) async {
    await _messenger.handlePlatformMessage(
        _events, _codec.encodeMessage(<Object?>[event]), (_) {});
  }

  void reset() {
    for (final method in [
      'start', 'shutdown', 'releaseCall', 'createAccount', 'accountRegister',
      'accountCall', 'callHangup', 'accountDestroy',
      'accountRefreshRegistration', 'accountSetReliableProvisional',
      'pushWake', 'pushNewToken',
    ]) {
      _messenger.setMockMessageHandler('$_host.$method', null);
    }
    args.clear();
  }
}

StartResultMessage _fresh({
  bool reattached = false,
  List<AccountMessage> accounts = const [],
  List<CallMessage> calls = const [],
}) =>
    StartResultMessage(
        reattached: reattached, version: '1.0.0', accounts: accounts, calls: calls);

CallMessage _call(String id, CallState state, {String? accountId = 'a1'}) =>
    CallMessage(
        id: id,
        accountId: accountId,
        state: state.raw,
        isOutgoing: false,
        remoteUri: 'sip:bob@pbx');

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final native = FakeNative();

  // The tests run on the host (Linux/macOS), which would pick the FFI
  // backend; these are about the Pigeon one.
  setUpAll(() => Medut.debugUseFfiBackend = false);
  tearDownAll(() => Medut.debugUseFfiBackend = null);

  setUp(() {
    native.on('releaseCall', (_) => null);
  });

  tearDown(() {
    Medut.instance?.detach();
    native.reset();
  });

  Future<(Medut, Account)> startWithAccount() async {
    native.on('start', (_) => _fresh());
    native.on('createAccount',
        (_) => AccountMessage(id: 'a1', aor: 'sip:alice@pbx', registrationState: 0));
    final sdk = await Medut.start();
    final account = await sdk.createAccount('alice@pbx', 'secret');
    return (sdk, account);
  }

  test('start sends only the fields that were set', () async {
    native.on('start', (_) => _fresh());
    final sdk = await Medut.start(
      config: const MedutConfig(
        statsIntervalMs: 5000,
        opus: OpusConfig(fec: false),
        audioCodecs: ['opus'],
        appOwnedAudio: true,
      ),
      audioSession: AudioSessionMode.callFramework,
    );
    final warnings = <String?>[];
    final savedDebugPrint = debugPrint;
    debugPrint = (String? message, {int? wrapWidth}) => warnings.add(message);
    addTearDown(() => debugPrint = savedDebugPrint);

    final config = native.args['start']![0] as ConfigMessage;
    final options = native.args['start']![1] as StartOptionsMessage;
    expect(config.statsIntervalMs, 5000);
    expect(config.opusFec, isFalse);
    expect(config.audioCodecs, ['opus']);
    // Unset means "the C default": nothing is sent for it.
    expect(config.regExpires, isNull);
    expect(config.opusBitrate, isNull);
    expect(config.wsExtraHeaders, isNull);
    expect(config.iceServers, isNull);
    expect(options.audioMode, 1);
    expect(options.monitorNetwork, isTrue);
    expect(options.appOwnedAudio, isTrue);
    expect(sdk.version, '1.0.0');
    expect(sdk.reattached, isFalse);
    expect(identical(await Medut.start(), sdk), isTrue,
        reason: 'a second start returns the running instance');
    expect(warnings.single, contains('ignored'),
        reason: 'a second start with other options says they are not applied');
    warnings.clear();
    await Medut.start(
      config: const MedutConfig(
        statsIntervalMs: 5000,
        opus: OpusConfig(fec: false),
        audioCodecs: ['opus'],
        appOwnedAudio: true,
      ),
      audioSession: AudioSessionMode.callFramework,
    );
    expect(warnings, isEmpty, reason: 'same config and options: no warning');
  });

  test('audio modes cross as their raw values', () async {
    for (final (mode, telecom, raw) in [
      (AudioSessionMode.automatic, false, 0),
      (AudioSessionMode.callFramework, false, 1),
      (AudioSessionMode.manual, false, 2),
      (AudioSessionMode.manual, true, 1), // telecom implies the framework
    ]) {
      native.on('start', (_) => _fresh());
      native.on('shutdown', (_) => null);
      final sdk = await Medut.start(audioSession: mode, telecom: telecom);
      final options = native.args['start']![1] as StartOptionsMessage;
      expect(options.audioMode, raw, reason: '$mode telecom=$telecom');
      expect(options.telecom, telecom);
      await sdk.shutdown();
    }
  });

  test('log level crosses as given, 0 to 4', () async {
    for (final level in [0, 1, 2, 3, 4]) {
      native.on('start', (_) => _fresh());
      native.on('shutdown', (_) => null);
      final sdk = await Medut.start(config: MedutConfig(logLevel: level));
      expect((native.args['start']![0] as ConfigMessage).logLevel, level);
      await sdk.shutdown();
    }
  });

  test('attaches to a running stack and adopts its accounts and calls', () async {
    native.on(
        'start',
        (_) => _fresh(
              reattached: true,
              accounts: [
                AccountMessage(id: 'a1', aor: 'sip:alice@pbx', registrationState: 2),
              ],
              calls: [_call('c1', CallState.ringing)],
            ));
    final sdk = await Medut.start();

    expect(sdk.reattached, isTrue);
    final account = sdk.accounts.single;
    expect(account.aor, 'sip:alice@pbx');
    expect(account.regState, RegState.registered);
    final call = sdk.calls.single;
    expect(call.state, CallState.ringing);
    expect(call.account, same(account));
  });

  test('events reach the SDK stream and the account; a call keeps its identity',
      () async {
    final (sdk, account) = await startWithAccount();
    final all = <MedutEvent>[];
    final scoped = <MedutEvent>[];
    sdk.events.listen(all.add);
    account.events.listen(scoped.add);

    await native.push(EventMessage(
        type: 2, accountId: 'a1', call: _call('c1', CallState.ringing),
        fromUri: 'sip:bob@pbx'));
    await native.push(EventMessage(
        type: 3, accountId: 'a1', call: _call('c1', CallState.established),
        state: CallState.established.raw));
    expect(sdk.calls.single.state, CallState.established);
    await native.push(EventMessage(
        type: 3, accountId: 'a1', call: _call('c1', CallState.ended),
        state: CallState.ended.raw, reason: 'Remote hangup'));
    await pumpEventQueue();

    final incoming = all.whereType<IncomingCallEvent>().single;
    final ended = all.whereType<CallStateEvent>().last;
    expect(incoming.fromUri, 'sip:bob@pbx');
    expect(ended.call, same(incoming.call), reason: 'one Dart object per call');
    expect(ended.reason, 'Remote hangup');
    expect(ended.endedLocally, isFalse, reason: 'absent means the far end');
    expect(incoming.call.state, CallState.ended);
    expect(sdk.calls, isEmpty, reason: 'ended calls leave the live list');
    expect(scoped, hasLength(3), reason: 'account-scoped events reach the account');
  });

  test('a call declined from the system UI arrives as ended locally', () async {
    final (sdk, _) = await startWithAccount();
    final all = <CallStateEvent>[];
    sdk.events.listen((e) {
      if (e is CallStateEvent) all.add(e);
    });

    await native.push(EventMessage(
        type: 2, accountId: 'a1', call: _call('c1', CallState.ringing),
        fromUri: 'sip:bob@pbx'));
    await native.push(EventMessage(
        type: 3, accountId: 'a1', call: _call('c1', CallState.ended),
        state: CallState.ended.raw, endedLocally: true));
    await pumpEventQueue();

    expect(all.single.endedLocally, isTrue);
  });

  test('call methods address the native call by id', () async {
    final (sdk, account) = await startWithAccount();
    native.on('accountCall', (_) => _call('c9', CallState.calling));
    native.on('callHangup', (_) => null);

    final call = await account.call('sip:bob@pbx');
    expect(native.args['accountCall'], ['a1', 'sip:bob@pbx']);
    expect(sdk.calls.single, same(call));
    await call.hangup();
    expect(native.args['callHangup'], ['c9']);
  });

  test('registration events update the account state', () async {
    final (_, account) = await startWithAccount();
    await native.push(EventMessage(type: 1, accountId: 'a1', state: RegState.registered.raw));
    await pumpEventQueue();
    expect(account.regState, RegState.registered);
  });

  test('native failures surface as MedutException with the SDK code', () async {
    final (_, account) = await startWithAccount();
    native.on('accountRegister', (_) {
      throw PlatformException(code: 'AUTHENTICATION', message: 'bad password', details: -6);
    });

    await expectLater(
      account.register(),
      throwsA(isA<MedutException>()
          .having((e) => e.code, 'code', 'AUTHENTICATION')
          .having((e) => e.rawValue, 'rawValue', -6)
          .having((e) => e.error, 'error', MedutError.auth)
          .having((e) => e.operation, 'operation', 'register')),
    );
  });

  test('media stats decode onto the call', () async {
    final (sdk, _) = await startWithAccount();
    final stats = <MediaStatsEvent>[];
    sdk.events.listen((e) {
      if (e is MediaStatsEvent) stats.add(e);
    });

    await native.push(EventMessage(
      type: 7,
      call: _call('c1', CallState.established),
      stats: MediaStatsMessage(
        packetsSent: 100, packetsReceived: 98, packetsLost: 1, packetsLostRx: 2,
        bytesSent: 16000, bytesReceived: 15680, txErrors: 0, rxErrors: 0,
        lossPct: 1.0, lossPctRx: 2.0, jitterMs: 4.5, txJitterMs: 3.0, rttMs: 42.0,
        jitterBufferMs: 60, jitterBufferLoad: 3, latePackets: 0, discardedPackets: 0,
        jitterBufferTargetMs: 40, jitterBufferAdaptive: true, plcFrames: 1,
        plcRatio: 0.01, fecRecovered: 1, plcConcealed: 0, fecActive: true,
        fecLevelPct: 5, bandwidthKbpsTx: 32, bandwidthKbpsRx: 31,
        avgBandwidthKbpsTx: 32, avgBandwidthKbpsRx: 31, mosLq: 4.3, mosCq: 4.1,
        mosLqRx: 4.2, mosCqRx: 4.0, mosLqMin: 4.0, mosLqAvg: 4.2, mosMethod: 0,
        codecName: 'opus', codecClockRate: 48000, codecSampleRate: 48000,
        codecChannels: 1, payloadType: 111, audioLevelDbov: -30.0,
        micLevelDbov: -28.0, ssrcTx: 1, ssrcRx: 2, remoteAddress: '192.0.2.1:4000',
        statsTick: 3, callDurationMs: 6000, isFinal: false, iceState: 4,
        iceLocalType: 0, iceRemoteType: 3, iceRttMs: 40.0, iceGeneration: 1,
        iceLocalAddress: '10.0.0.2:5000', iceRemoteAddress: '192.0.2.1:4000',
      ),
    ));
    await pumpEventQueue();

    final s = stats.single.stats;
    expect(s.codec, 'opus');
    expect(s.rttMs, 42.0);
    expect(s.bandwidthTx, 32);
    expect(s.iceState, IceState.completed);
    expect(s.iceRemoteType, IceCandType.relay);
    expect(s.call.id, 'c1');
  });

  test('route changes and app-owned audio errors have their own streams', () async {
    final (sdk, _) = await startWithAccount();
    final routes = <List<AudioRoute>>[];
    final errors = <AppOwnedAudioError>[];
    sdk.audioRoutes.listen(routes.add);
    sdk.appOwnedAudioErrors.listen(errors.add);

    await native.push(EventMessage(type: 100, routes: [
      AudioRouteMessage(id: 'speaker', name: '', kind: 'speaker', isActive: true),
      AudioRouteMessage(id: 'bluetooth', name: 'Buds', kind: 'bluetooth', isActive: false),
    ]));
    await native.push(EventMessage(type: 101, errorCode: 'mic-permission', message: 'denied'));
    await pumpEventQueue();

    expect(routes.single.map((r) => r.kind),
        [AudioRouteKind.speaker, AudioRouteKind.bluetooth]);
    expect(routes.single.first.isActive, isTrue);
    expect(errors.single.code, 'mic-permission');
  });

  test('an event without its call or stats arrives as UnknownEvent', () async {
    final (sdk, _) = await startWithAccount();
    final all = <MedutEvent>[];
    sdk.events.listen(all.add);

    await native.push(EventMessage(type: 3, accountId: 'a1', state: 2));
    await native.push(EventMessage(type: 7, call: _call('c1', CallState.established)));
    await native.push(EventMessage(type: 2, accountId: 'a1'));
    await native.push(EventMessage(type: 4242));
    await pumpEventQueue();

    expect(all.map((e) => (e as UnknownEvent).rawType), [3, 7, 2, 4242]);
  });

  test('log and registrar warnings reach every account', () async {
    final (sdk, account) = await startWithAccount();
    final scoped = <MedutEvent>[];
    account.events.listen(scoped.add);

    await native.push(EventMessage(type: 0, message: 'hello'));
    await native.push(EventMessage(type: 8, message: 'expires lowered'));
    await pumpEventQueue();

    expect((scoped[0] as LogEvent).message, 'hello');
    expect((scoped[1] as RegistrarWarningEvent).message, 'expires lowered');
  });

  test('SDP negotiation carries rejected codecs and warnings', () async {
    final (sdk, _) = await startWithAccount();
    final sdp = <SdpNegotiationEvent>[];
    sdk.events.listen((e) {
      if (e is SdpNegotiationEvent) sdp.add(e);
    });

    await native.push(EventMessage(
        type: 5,
        call: _call('c1', CallState.established),
        negotiatedCodec: 'opus',
        rejectedCodecs: ['G722'],
        warnings: ['no common crypto']));
    await native.push(EventMessage(
        type: 5, call: _call('c1', CallState.established), negotiatedCodec: 'opus'));
    await pumpEventQueue();

    expect(sdp[0].rejectedCodecs, ['G722']);
    expect(sdp[0].warnings, ['no common crypto']);
    expect(sdp[1].rejectedCodecs, isEmpty);
    expect(sdp[1].warnings, isEmpty);
  });

  test('refreshRegistration and setReliableProvisional reach the host',
      () async {
    final (_, account) = await startWithAccount();
    native.on('accountRefreshRegistration', (_) => null);
    native.on('accountSetReliableProvisional', (_) => null);

    await account.refreshRegistration();
    await account.setReliableProvisional(ReliableProvisionalMode.required);

    expect(native.args['accountRefreshRegistration'], ['a1']);
    expect(native.args['accountSetReliableProvisional'], ['a1', 2]);
  });

  test('destroying an account twice does not throw', () async {
    final (sdk, account) = await startWithAccount();
    native.on('accountDestroy', (_) => null);

    await account.destroy(forget: true);
    await account.destroy();
    expect(native.args['accountDestroy'], ['a1', false]);
    expect(sdk.accounts, isEmpty);
  });

  test('push wake-up and token are static host calls', () async {
    native.on('pushWake', (_) => true);
    native.on('pushNewToken', (_) => null);

    expect(await Medut.pushWake({'type': 'call'},
        wait: const Duration(seconds: 3)), isTrue);
    expect(native.args['pushWake'], [
      {'type': 'call'},
      3000
    ]);
    await Medut.pushNewToken('tok');
    expect(native.args['pushNewToken'], ['tok']);

    native.on('pushWake', (_) {
      throw PlatformException(code: 'UNSUPPORTED', message: 'iOS', details: 0);
    });
    await expectLater(
        Medut.pushWake(const {}),
        throwsA(isA<MedutException>()
            .having((e) => e.code, 'code', 'UNSUPPORTED')));
  });

  test('restarting after shutdown gives a fresh instance with its config',
      () async {
    native.on('start', (_) => _fresh());
    native.on('shutdown', (_) => null);
    final first = await Medut.start();
    await first.shutdown();
    expect(Medut.instance, isNull);

    final second =
        await Medut.start(config: const MedutConfig(statsIntervalMs: 1000));
    expect(second, isNot(same(first)));
    expect((native.args['start']![0] as ConfigMessage).statsIntervalMs, 1000);

    // Routes still reach the new instance.
    final routes = <List<AudioRoute>>[];
    second.audioRoutes.listen(routes.add);
    await native.push(EventMessage(type: 100, routes: [
      AudioRouteMessage(id: 'earpiece', name: '', kind: 'earpiece', isActive: true),
    ]));
    await pumpEventQueue();
    expect(routes.single.single.kind, AudioRouteKind.earpiece);
  });

  test('concurrent starts share one instance', () async {
    native.on('start', (_) => _fresh());
    final results = await Future.wait([Medut.start(), Medut.start()]);
    expect(results[0], same(results[1]));
  });
}
