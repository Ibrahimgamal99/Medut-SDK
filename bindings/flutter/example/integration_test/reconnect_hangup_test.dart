/// Does call termination still work after the stack reconnects?
///
/// Runs three phases against a callee that stays up (an echo test):
///   1. establish
///   2. force a reconnect (networkChanged, and optionally a real Wi-Fi bounce)
///   3. hang up LOCALLY and see whether the terminal event arrives
///
/// A local hangup is the discriminator: if that sticks too, outbound in-dialog
/// requests are broken after the reconnect (which would point at the WebSocket
/// connection-reuse change). If only a remote BYE sticks, the problem is on the
/// inbound side.
@Timeout(Duration(minutes: 6))
library;

import 'package:medut/medut.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

const _uri = String.fromEnvironment('SIP_URI');
const _pass = String.fromEnvironment('SIP_PASS');
const _ws = String.fromEnvironment('SIP_WS');
const _callee = String.fromEnvironment('SIP_CALLEE', defaultValue: '*43');
const _preSec = int.fromEnvironment('SIP_PRE', defaultValue: 8);
const _postSec = int.fromEnvironment('SIP_POST', defaultValue: 12);
/// When set, wait for the FAR end to hang up instead of hanging up locally.
const _remote = bool.fromEnvironment('SIP_REMOTE', defaultValue: false);

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  test('terminate after reconnect', () async {
    final sdk = await Medut.start(
      config: const MedutConfig(
        logLevel: 4,
        traceSip: true,
        netMonitorIntervalSeconds: 0,
        statsIntervalMs: 3000,
      ),
    );
    addTearDown(sdk.shutdown);

    final sw = Stopwatch()..start();
    CallState? last;
    void p(String s) {
      // ignore: avoid_print
      print('RC ${sw.elapsedMilliseconds}ms $s');
    }

    sdk.events.listen((ev) {
      switch (ev) {
        case CallStateEvent e:
          last = e.state;
          p('CALL ${e.state.name} ${e.reason ?? ''}');
        case RegStateEvent e:
          p('REG ${e.state.name} ${e.errorStr ?? ''}');
        case NetworkEvent e:
          p('NET ${e.stage.name} ${e.localAddr} err=${e.error}');
        case SipTraceEvent e:
          final first = e.rawMessage
              .split('\n')
              .firstWhere((l) => l.trim().isNotEmpty, orElse: () => '');
          final contact = e.rawMessage
              .split('\n')
              .where((l) => l.toLowerCase().startsWith('contact:'))
              .join(' ')
              .trim();
          p('SIP ${e.direction.name} $first'
              '${contact.isNotEmpty ? "  [$contact]" : ""}');
        case LogEvent e:
          final m = e.message.trim();
          if (m.contains('websock') ||
              m.contains('reusing') ||
              m.contains('ws connect') ||
              m.contains('session closed') ||
              m.contains('terminate') ||
              m.contains('failed')) {
            p('LOG $m');
          }
        default:
          break;
      }
    });

    final account = await sdk.createAccount('sip:$_uri', _pass,
        config: AccountConfig(
          transport: Transport.wss,
          serverUrl: _ws.isEmpty ? null : _ws,
          mediaEnc: MediaEncryption.dtlsSrtp,
          iceEnabled: true,
          verifyTls: false,
          audioCodecs: const ['opus'],
        ));
    await account.register();
    await Future<void>.delayed(const Duration(seconds: 6));

    p('=== phase 1: dial $_callee ===');
    final call = await account.call(_callee);
    for (var i = 0; i < _preSec * 10; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
      if (last == CallState.established) break;
    }
    if (last != CallState.established) {
      p('never established (last=$last) — cannot test');
      return;
    }
    await Future<void>.delayed(Duration(seconds: _preSec));

    p('=== phase 2: force reconnect ===');
    sdk.networkChanged();
    await Future<void>.delayed(Duration(seconds: _postSec));
    p('after reconnect: last=$last sdkCalls=${sdk.calls.length}');

    if (_remote) {
      p('=== phase 3: waiting for the FAR end to hang up (no local hangup) ===');
      final before = last;
      var ms = -1;
      for (var i = 0; i < 1500; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
        if (last != null && last!.isTerminal) {
          ms = i * 100;
          break;
        }
      }
      p('RESULT-REMOTE stateBefore=$before terminal=$last afterMs=$ms '
          'sdkCalls=${sdk.calls.length}');
      expect(ms, greaterThanOrEqualTo(0),
          reason: 'the far end hung up but no terminal call-state event ever '
              'arrived after the reconnect — the call is stuck');
      return;
    }

    p('=== phase 3: LOCAL hangup ===');
    final before = last;
    await call.hangup();
    var arrivedMs = -1;
    for (var i = 0; i < 150; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
      if (last != null && last!.isTerminal) {
        arrivedMs = i * 100;
        break;
      }
    }
    p('RESULT stateBefore=$before terminal=$last afterMs=$arrivedMs '
        'sdkCalls=${sdk.calls.length}');
    expect(arrivedMs, greaterThanOrEqualTo(0),
        reason: 'no terminal call-state event after a local hangup following a '
            'reconnect — the call is stuck');
  });
}
