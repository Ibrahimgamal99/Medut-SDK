/// End-to-end test against a local Asterisk: registration over UDP **and**
/// WSS, an echo call with media stats — through the exact code path a
/// Flutter app uses (real libmedut, owned events, Dart decode).
///
/// Server setup (docker, host networking, UDP :5060 + WSS :8089 with a
/// self-signed cert): see docs/quickstart/flutter.md.
///
/// Skipped automatically when no server is listening on 127.0.0.1:8089.
///
/// NOTE: the whole flow lives in ONE test body on purpose — the SDK's
/// event callback is bound to the Dart zone that created it, and
/// package:test drops events delivered into a completed setUpAll/test zone.
///
/// Run: flutter test test/e2e_asterisk_test.dart
@Timeout(Duration(minutes: 3))
library;

import 'dart:async';
import 'dart:io';

import 'package:medut/medut.dart';
import 'package:flutter_test/flutter_test.dart';

Future<bool> _portOpen(int port) async {
  try {
    final s = await Socket.connect('127.0.0.1', port,
        timeout: const Duration(milliseconds: 500));
    s.destroy();
    return true;
  } catch (_) {
    return false;
  }
}

void main() {
  final libPath = File('../../dist/linux/x86_64/medut.so').absolute.path;

  test('UDP + WSS registration, echo call, media stats', () async {
    if (!Platform.isLinux || !File(libPath).existsSync()) {
      markTestSkipped('needs dist/linux/x86_64/medut.so');
      return;
    }
    if (!await _portOpen(8089)) {
      markTestSkipped('needs local Asterisk (WSS :8089, UDP :5060)');
      return;
    }

    // baresip excludes loopback from its local-address list, so dialing
    // 127.0.0.1 fails with "no laddr". Use a real host address instead —
    // the test Asterisk binds 0.0.0.0.
    final ifaces = await NetworkInterface.list(
        type: InternetAddressType.IPv4, includeLoopback: false);
    final host = ifaces.expand((i) => i.addresses).first.address;

    final sdk = Medut(
      config: const MedutConfig(
        logLevel: 1,
        statsIntervalMs: 1000,
        verifyServer: false, // self-signed test cert
        netMonitorIntervalSeconds: 0,
      ),
      libPath: libPath,
    );

    // listen+Completer instead of Stream.first: .first futures proved
    // unreliable under the flutter_test zone setup while plain listeners
    // always fire.
    Future<T> firstEvent<T extends MedutEvent>(
        Stream<MedutEvent> stream, bool Function(T) match,
        {Duration timeout = const Duration(seconds: 15)}) {
      final c = Completer<T>();
      late final StreamSubscription sub;
      sub = stream.listen((ev) {
        if (ev is T && match(ev) && !c.isCompleted) {
          c.complete(ev);
          sub.cancel();
        }
      });
      return c.future.timeout(timeout, onTimeout: () {
        sub.cancel();
        throw TimeoutException('no matching ${T.toString()}', timeout);
      });
    }

    Future<RegState> register(Account a) async {
      final done = firstEvent<RegStateEvent>(
          a.events,
          (e) =>
              e.state == RegState.registered || e.state == RegState.failed);
      a.register();
      final ev = await done;
      if (ev.state == RegState.failed) {
        // ignore: avoid_print
        print('registration failed: ${ev.error} ${ev.errorStr ?? ''}');
      }
      return ev.state;
    }

    // ── UDP: register ────────────────────────────────────────────────────
    final alice = sdk.createAccount(
      'alice@$host',
      'secret123',
      config: const AccountConfig(
        transport: Transport.udp,
        audioCodecs: ['opus', 'ulaw'],
      ),
    );
    expect(await register(alice), RegState.registered,
        reason: 'UDP registration failed');

    // ── Echo call with RTP + stats ──────────────────────────────────────
    final established = firstEvent<CallStateEvent>(
        sdk.events,
        (e) =>
            e.state == CallState.established || e.state == CallState.failed);
    final call = alice.call('600@$host');
    final st = await established;
    expect(st.state, CallState.established,
        reason: 'echo call failed: ${st.reason}');

    final stats = await firstEvent<MediaStatsEvent>(
        sdk.events, (e) => e.stats.packetsSent > 0,
        timeout: const Duration(seconds: 10));
    expect(stats.stats.codec.toLowerCase(), contains('opus'));

    // DTMF + hold/resume exercise the dialog. baresip emits no bevent for
    // our own call_hold(), so poll isHeld (tracked natively) rather than
    // waiting for a CallState.held event (that fires only on REMOTE hold).
    call.sendDtmf('5');
    call.hold();
    await Future.delayed(const Duration(milliseconds: 700));
    expect(call.isHeld, isTrue);
    call.resume();
    await Future.delayed(const Duration(milliseconds: 700));
    expect(call.isHeld, isFalse);

    final ended = firstEvent<CallStateEvent>(
        sdk.events, (e) => e.state.isTerminal,
        timeout: const Duration(seconds: 10));
    call.hangup();
    await ended;

    // ── WSS: register over wss://127.0.0.1:8089/ws ──────────────────────
    final bob = sdk.createAccount(
      'bob@$host',
      'secret123',
      config: AccountConfig(
        serverUrl: 'wss://$host:8089/ws',
        verifyTls: false, // self-signed test cert
        audioCodecs: ['opus'],
      ),
    );
    expect(await register(bob), RegState.registered,
        reason: 'WSS registration failed');

    // ── Reject with SIP status code: bob calls alice, alice sends 486 ───
    // (the dialplan Dials PJSIP/alice when bob calls 'alice@host')
    final incoming = firstEvent<IncomingCallEvent>(
        alice.events, (e) => true,
        timeout: const Duration(seconds: 10));
    final bobCallEnded = firstEvent<CallStateEvent>(
        bob.events, (e) => e.state.isTerminal,
        timeout: const Duration(seconds: 15));
    bob.call('alice@$host');
    final inc = await incoming;
    inc.call.reject(statusCode: 486, reason: 'Busy Here');
    final rejected = await bobCallEnded;
    // Asterisk maps the callee's 486 to a failed/ended outgoing call.
    expect(rejected.state.isTerminal, isTrue);

    alice.destroy();
    bob.destroy();

    sdk.shutdown();
  });
}
