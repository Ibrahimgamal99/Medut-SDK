/// Desktop smoke test for the Dart binding against the real Linux build.
///
/// Requires `scripts/build-linux.sh` to have produced
/// `dist/linux/x86_64/medut.so`. Skipped automatically when missing.
///
/// Run: flutter test test/medut_smoke_test.dart
@Timeout(Duration(minutes: 2))
library;

import 'dart:io';

import 'package:medut/medut.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  final libPath = File('../../dist/linux/x86_64/medut.so').absolute.path;
  final haveLib = File(libPath).existsSync() && Platform.isLinux;

  test('register failure path delivers owned events safely', () async {
    final sdk = Medut(
      config: const MedutConfig(
        logLevel: 3,
        traceSip: true,
        netMonitorIntervalSeconds: 0,
      ),
      libPath: libPath,
    );
    expect(sdk.version, isNotEmpty);
    expect(Medut.instance, same(sdk));

    // Second construction must be refused (native stack is process-global).
    expect(() => Medut(), throwsStateError);

    final logs = <LogEvent>[];
    final regs = <RegStateEvent>[];
    sdk.events.listen((ev) {
      if (ev is LogEvent) logs.add(ev);
    });

    // Black-hole registrar (TEST-NET-1) — deterministic failure path.
    final account = sdk.createAccount(
      'alice@192.0.2.1:5060',
      'secret',
      config: const AccountConfig(
        transport: Transport.udp,
        audioCodecs: ['opus', 'ulaw'],
      ),
    );
    account.events.listen((ev) {
      if (ev is RegStateEvent) regs.add(ev);
    });
    account.setRetryPolicy(initialMs: 200, maxMs: 1000, backoff: 1.5);
    account.register();

    // Wait for the REGISTERING event (immediate) and some logs.
    await Future.delayed(const Duration(seconds: 3));

    expect(regs, isNotEmpty, reason: 'no RegStateEvent received');
    expect(regs.first.state, RegState.registering);
    expect(logs, isNotEmpty, reason: 'no LogEvent received');

    // Strings decoded from owned events must be intact well after delivery.
    for (final l in logs) {
      expect(l.message, isNotNull);
    }

    // App-owned audio device. Exercised here rather than in its own test so
    // it runs against a live stack, and because this also proves the
    // regenerated ffi_bindings.dart resolves against the real .so — a bad
    // regen fails here instead of on a device.
    await sdk.useAppOwnedAudio(true);
    expect(sdk.appOwnedAudioActive, isFalse,
        reason: 'no call is up, so no device should be open');
    expect(sdk.appOwnedAudioFormat, isNull,
        reason: 'format is only known once a call has media');
    await sdk.useAppOwnedAudio(false);

    account.destroy();
    sdk.shutdown();
    expect(Medut.instance, isNull);

    // Refused once the stack is down — the switch is a dispatch onto the SIP
    // thread, and it is not running. expectLater, not expect: the method is
    // async, so it completes with the error rather than throwing inline.
    await expectLater(sdk.useAppOwnedAudio(true), throwsStateError);
  }, skip: haveLib ? false : 'dist/linux/x86_64/medut.so not built');

  // One body on purpose: the event callback is bound to the zone that created
  // it, so a second test's events would be dropped.
  test('reattaches to a stack this isolate did not start', () async {
    // Stand-in for the Android headless engine's first push wakeup.
    final first = Medut(
      config: const MedutConfig(
        logLevel: 3,
        netMonitorIntervalSeconds: 0,
        // Global (not per-account) codec list — the marshalling this exercises
        // used to stop at the Dart object.
        audioCodecs: ['ulaw', 'opus'],
      ),
      libPath: libPath,
    );
    expect(first.reattached, isFalse);

    final account = first.createAccount('alice@192.0.2.1:5060', 'secret',
        config: const AccountConfig(transport: Transport.udp));
    account.setRetryPolicy(initialMs: 200, maxMs: 1000, backoff: 1.5);
    account.register();
    await Future.delayed(const Duration(seconds: 1));

    // The isolate goes away; the native stack, its registration and its calls
    // do not. detach() is the graceful form of what a destroyed headless
    // isolate does to this process abruptly.
    first.detach();
    expect(Medut.instance, isNull);

    // Second push wakeup: same start-up code, new isolate, stack still up.
    final second = Medut(
      config: const MedutConfig(netMonitorIntervalSeconds: 0),
      libPath: libPath,
    );
    expect(second.reattached, isTrue,
        reason: 'construction on a live stack did not reattach');
    expect(Medut.instance, same(second));

    // The account the dead isolate created is adopted, not lost or duplicated.
    expect(second.accounts.length, 1);
    final adopted = second.accounts.first;
    expect(adopted.aor, 'sip:alice@192.0.2.1');
    expect(
        adopted.regState,
        anyOf(RegState.registering, RegState.reconnecting, RegState.failed,
            RegState.registered));
    expect(second.calls, isEmpty);

    // Events now reach the new isolate, and route to the adopted account.
    final sdkEvents = <MedutEvent>[];
    final acctEvents = <MedutEvent>[];
    second.events.listen(sdkEvents.add);
    adopted.events.listen(acctEvents.add);
    adopted.unregister();
    await Future.delayed(const Duration(milliseconds: 300));
    adopted.register();
    await Future.delayed(const Duration(seconds: 2));

    expect(sdkEvents, isNotEmpty, reason: 'no events after reattach');
    expect(acctEvents.whereType<RegStateEvent>(), isNotEmpty,
        reason: 'adopted account received no registration events');

    // Opting out restores the hard failure: stack up, no Dart instance
    // holding it, and init refused rather than reattached.
    second.detach();
    expect(() => Medut(libPath: libPath, reattachIfRunning: false),
        throwsStateError);

    // Reattach once more — this time to tear the stack down for real.
    final third = Medut(libPath: libPath);
    expect(third.reattached, isTrue);
    expect(third.accounts.length, 1);
    third.accounts.first.destroy();
    third.shutdown();
    expect(Medut.instance, isNull);
  }, skip: haveLib ? false : 'dist/linux/x86_64/medut.so not built');
}
