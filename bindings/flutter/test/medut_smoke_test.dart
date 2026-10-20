/// Desktop smoke test for the FFI backend against the core the plugin ships
/// for this host: `linux/<arch>/medut.so` (scripts/build-linux.sh) or
/// `windows/<arch>/medut.dll` (.github/workflows/desktop-libs.yml,
/// scripts/build-windows.ps1). Skipped on other hosts or when the library is
/// missing.
///
/// Run: flutter test test/medut_smoke_test.dart
@Timeout(Duration(minutes: 2))
library;

import 'dart:ffi';
import 'dart:io';

import 'package:medut/medut.dart';
import 'package:medut/src/ffi_bindings.dart' as c;
import 'package:flutter_test/flutter_test.dart';

void main() {
  final arch = switch (Abi.current()) {
    Abi.linuxArm64 || Abi.windowsArm64 => 'arm64',
    _ => 'x64',
  };
  final libPath = File(Platform.isWindows
          ? 'windows/$arch/medut.dll'
          : 'linux/$arch/medut.so')
      .absolute
      .path;
  final haveLib = (Platform.isLinux || Platform.isWindows) &&
      File(libPath).existsSync();
  // The library may predate medut_account_refresh_registration(); the rest
  // of the test still runs against it.
  final haveRefresh = haveLib &&
      DynamicLibrary.open(libPath)
          .providesSymbol('medut_account_refresh_registration');

  setUpAll(() => Medut.debugUseFfiBackend = true);
  tearDownAll(() => Medut.debugUseFfiBackend = null);

  Matcher throwsCode(String code) =>
      throwsA(isA<MedutException>().having((e) => e.code, 'code', code));

  test('Dart enums carry the C values', () {
    expect(Transport.wss.raw, c.medut_transport_t.MEDUT_TRANSPORT_WSS);
    expect(MediaEncryption.dtlsSrtp.raw,
        c.medut_media_enc_t.MEDUT_MEDIA_ENC_DTLS_SRTP);
    expect(RegState.reconnecting.raw,
        c.medut_reg_state_t.MEDUT_REG_RECONNECTING);
    expect(CallState.failed.raw, c.medut_call_state_t.MEDUT_CALL_FAILED);
    expect(IceState.closed.raw, c.medut_ice_state_t.MEDUT_ICE_CLOSED);
    expect(IceCandType.none.raw, c.medut_ice_cand_type_t.MEDUT_ICE_CAND_NONE);
    expect(NetworkStage.callIceFailed.raw,
        c.medut_net_event_t.MEDUT_NET_CALL_ICE_FAILED);
    expect(QualityIssue.mediaStall.raw,
        c.medut_quality_issue_t.MEDUT_QUALITY_MEDIA_STALL);
    expect(PushProvider.fcm.raw,
        c.medut_push_provider_t.MEDUT_PUSH_PROVIDER_FCM);
    expect(ReliableProvisionalMode.required.raw,
        c.medut_100rel_mode_t.MEDUT_100REL_REQUIRED);
    expect(IceHandover.failFast.raw,
        c.medut_ice_handover_t.MEDUT_ICE_HANDOVER_FAIL_FAST);
    expect(MedutError.forbidden.raw, c.medut_error_t.MEDUT_ERR_FORBIDDEN);
  });

  test('register failure path delivers owned events safely', () async {
    final sdk = await Medut.start(
      config: const MedutConfig(
        logLevel: 3,
        traceSip: true,
        netMonitorIntervalSeconds: 0,
      ),
      libPath: libPath,
    );
    expect(sdk.version, isNotEmpty);
    expect(sdk.reattached, isFalse);
    expect(Medut.instance, same(sdk));

    // The native stack is process-wide: a second start returns this one.
    expect(await Medut.start(libPath: libPath), same(sdk));

    final logs = <LogEvent>[];
    final regs = <RegStateEvent>[];
    sdk.events.listen((ev) {
      if (ev is LogEvent) logs.add(ev);
    });

    // Black-hole registrar (TEST-NET-1) — deterministic failure path.
    final account = await sdk.createAccount(
      'alice@192.0.2.1:5060',
      'secret',
      config: const AccountConfig(
        transport: Transport.udp,
        audioCodecs: ['opus', 'ulaw'],
      ),
    );
    expect(account.aor, 'sip:alice@192.0.2.1');
    expect(account.id, matches(RegExp(r'^[0-9a-f]{8}-[0-9a-f]{4}-4')));
    account.events.listen((ev) {
      if (ev is RegStateEvent) regs.add(ev);
    });
    await account.setRetryPolicy(initialMs: 200, maxMs: 1000, backoff: 1.5);
    await account.setReliableProvisional(ReliableProvisionalMode.enabled);
    await account.register();

    // Wait for the REGISTERING event (immediate) and some logs.
    await Future.delayed(const Duration(seconds: 3));

    expect(regs, isNotEmpty, reason: 'no RegStateEvent received');
    expect(regs.first.state, RegState.registering);
    expect(account.regState, regs.last.state,
        reason: 'the cached state follows the events');
    expect(logs, isNotEmpty, reason: 'no LogEvent received');
    for (final l in logs) {
      expect(l.message, isNotNull);
    }

    if (haveRefresh) {
      await account.refreshRegistration();
    }

    // App-owned audio device: switching works on a live stack, and nothing
    // is open with no call up.
    await sdk.useAppOwnedAudio(true);
    expect(await sdk.appOwnedAudioActive(), isFalse,
        reason: 'no call is up, so no device should be open');
    expect(await sdk.appOwnedAudioFormat(), isNull,
        reason: 'format is only known once a call has media');
    await sdk.useAppOwnedAudio(false);

    // Mobile-only features are predictable no-ops here.
    expect(await sdk.listAudioRoutes(), isEmpty);
    expect(await sdk.setAudioRoute('speaker'), isEmpty);
    await sdk.setSpeakerphone(true);
    expect(await sdk.rememberedAccounts(), isEmpty);
    expect(await sdk.appOwnedAudioStatus(), isEmpty);
    await expectLater(
        Medut.pushWake(const {'type': 'call'}), throwsCode('UNSUPPORTED'));
    await Medut.pushNewToken('token');

    expect(await sdk.strerror(-6), isNotEmpty);
    expect(await sdk.listOutputDevices(), isA<List<AudioDevice>>());

    // Destroy is idempotent; afterwards the account is gone.
    await account.destroy();
    await account.destroy();
    await expectLater(account.register(), throwsCode('ACCOUNT_DESTROYED'));

    await sdk.shutdown();
    expect(Medut.instance, isNull);

    // Refused once the stack is down.
    await expectLater(sdk.useAppOwnedAudio(true), throwsCode('NOT_RUNNING'));
  }, skip: haveLib ? false : '$libPath not built');

  // One body on purpose: the event callback is bound to the zone that created
  // it, so a second test's events would be dropped.
  test('reattaches to a stack an earlier instance left running', () async {
    final first = await Medut.start(
      config: const MedutConfig(
        logLevel: 3,
        netMonitorIntervalSeconds: 0,
        // Global (not per-account) codec list.
        audioCodecs: ['ulaw', 'opus'],
      ),
      libPath: libPath,
    );
    expect(first.reattached, isFalse);

    final account = await first.createAccount('alice@192.0.2.1:5060', 'secret',
        config: const AccountConfig(transport: Transport.udp));
    await account.setRetryPolicy(initialMs: 200, maxMs: 1000, backoff: 1.5);
    await account.register();
    await Future.delayed(const Duration(seconds: 1));

    // detach() leaves the native stack, its registration and its calls up.
    first.detach();
    expect(Medut.instance, isNull);
    await expectLater(account.register(), throwsCode('NOT_RUNNING'));

    final second = await Medut.start(
      config: const MedutConfig(netMonitorIntervalSeconds: 0),
      libPath: libPath,
    );
    expect(second.reattached, isTrue,
        reason: 'a start on a live stack did not reattach');
    expect(Medut.instance, same(second));

    // The account the first instance created is adopted, not lost or
    // duplicated.
    expect(second.accounts.length, 1);
    final adopted = second.accounts.first;
    expect(adopted.aor, 'sip:alice@192.0.2.1');
    expect(
        adopted.regState,
        anyOf(RegState.registering, RegState.reconnecting, RegState.failed,
            RegState.registered));
    expect(second.calls, isEmpty);

    // Events now reach the new instance, and route to the adopted account.
    final sdkEvents = <MedutEvent>[];
    final acctEvents = <MedutEvent>[];
    second.events.listen(sdkEvents.add);
    adopted.events.listen(acctEvents.add);
    await adopted.unregister();
    await Future.delayed(const Duration(milliseconds: 300));
    await adopted.register();
    await Future.delayed(const Duration(seconds: 2));

    expect(sdkEvents, isNotEmpty, reason: 'no events after reattach');
    expect(acctEvents.whereType<RegStateEvent>(), isNotEmpty,
        reason: 'adopted account received no registration events');

    await adopted.destroy();
    await second.shutdown();
    expect(Medut.instance, isNull);
  }, skip: haveLib ? false : '$libPath not built');
}
