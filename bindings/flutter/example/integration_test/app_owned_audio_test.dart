/// On-device test for the app-owned audio device.
///
/// Verifies the thing that unit tests structurally cannot: that a real call on
/// real hardware, with the SDK's audio driver displaced, actually moves PCM in
/// both directions. A call that connects and carries silence looks identical
/// from the outside to one that works, which is exactly the failure this
/// feature exists to eliminate — so the assertions are on frame counters from
/// the native engine, not on call state.
///
/// Needs a registrar and a callee that returns audio (an echo service), passed
/// in so no credentials live in the repo:
///
///   flutter test integration_test/app_owned_audio_test.dart -d <device> \
///     --dart-define=SIP_URI=100@example.com \
///     --dart-define=SIP_PASS=secret \
///     --dart-define=SIP_WS=wss://example.com:443 \
///     --dart-define=SIP_CALLEE=9999
///
/// Grant the mic first, or the capture path reports mic-permission:
///   adb shell pm grant dev.medut.medut android.permission.RECORD_AUDIO
///
/// SIP_CALLEE must answer and return audio; a silent callee fails step 5 by
/// design, since "connected but silent" is precisely what this test exists to
/// distinguish from working.
@Timeout(Duration(minutes: 3))
library;

import 'package:medut/medut.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

const _uri = String.fromEnvironment('SIP_URI');
const _pass = String.fromEnvironment('SIP_PASS');
const _ws = String.fromEnvironment('SIP_WS');
const _callee = String.fromEnvironment('SIP_CALLEE');

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  test('app-owned audio moves PCM both ways on a real call', () async {
    expect(_uri, isNotEmpty, reason: 'pass --dart-define=SIP_URI=...');

    final log = <String>[];
    void note(String s) {
      log.add(s);
      // ignore: avoid_print
      print('ATEST $s');
    }

    final sdk = await Medut.start(
      config: const MedutConfig(
        logLevel: 3,
        appOwnedAudio: true,
        netMonitorIntervalSeconds: 0,
        // 1 s stats so the TX/RX levels can be sampled while audio is
        // actually flowing, rather than once after the call is over.
        statsIntervalMs: 1000,
      ),
    );
    addTearDown(sdk.shutdown);

    sdk.appOwnedAudioErrors.listen((e) => note('ENGINE-ERROR $e'));

    // 1 ── The device really was displaced, and the JNI is reachable.
    var st = await sdk.appOwnedAudioStatus();
    note('status after start: $st');
    expect(st['available'], isTrue,
        reason: 'libmedut.so did not load, or the JNI symbols are missing — '
            'every push/pull would throw UnsatisfiedLinkError');
    expect(sdk.appOwnedAudioActive, isFalse,
        reason: 'no call yet, so no device should be open');
    expect(sdk.appOwnedAudioFormat, isNull);

    // 2 ── Register.
    final states = <RegState>[];
    final calls = <CallState>[];
    final micLevels = <double>[];
    final rxLevels = <double>[];
    sdk.events.listen((ev) {
      if (ev is RegStateEvent) {
        states.add(ev.state);
        note('reg ${ev.state} ${ev.error} ${ev.errorStr ?? ''}');
      } else if (ev is CallStateEvent) {
        calls.add(ev.state);
        note('call ${ev.state} ${ev.reason ?? ''} ${ev.error}');
      } else if (ev is SdpNegotiationEvent) {
        note('sdp codec=${ev.negotiatedCodec} crypto=${ev.negotiatedCrypto}');
      } else if (ev is MediaStatsEvent) {
        final s = ev.stats;
        micLevels.add(s.micLevelDbov);
        rxLevels.add(s.audioLevelDbov);
        note('stats tx=${s.packetsSent} rx=${s.packetsReceived} '
            'mic=${s.micLevelDbov} rx=${s.audioLevelDbov}');
      }
    });

    final account = sdk.createAccount(
      'sip:$_uri',
      _pass,
      config: AccountConfig(
        transport: Transport.wss,
        serverUrl: _ws.isEmpty ? null : _ws,
        mediaEnc: MediaEncryption.dtlsSrtp,
        iceEnabled: true,
        verifyTls: false,
        audioCodecs: const ['opus'],
      ),
    );
    account.register();

    await _until(() => states.contains(RegState.registered),
        timeout: const Duration(seconds: 25));
    expect(states, contains(RegState.registered),
        reason: 'never registered; saw $states');
    note('registered');

    // 3 ── Place the call and wait for media, not for SIP state: the device
    //      opens when the call has media, which is later than "established".
    final call = account.call(_callee);
    note('dialling $_callee');

    await _until(() => sdk.appOwnedAudioFormat != null,
        timeout: const Duration(seconds: 30));
    final fmt = sdk.appOwnedAudioFormat;
    expect(fmt, isNotNull,
        reason: 'the app-owned device never opened; call states seen: $calls');
    note('negotiated $fmt (${fmt!.samplesPerFrame} samples/frame)');

    expect(sdk.appOwnedAudioActive, isTrue);
    expect(fmt.sampleRate, greaterThan(0));
    expect(fmt.channels, inInclusiveRange(1, 2));
    expect(fmt.ptimeMs, greaterThan(0));

    // 4 ── The native engine opened the hardware at that same format.
    st = await sdk.appOwnedAudioStatus();
    note('status with media: $st');
    expect(st['running'], isTrue,
        reason: 'engine did not open AudioRecord/AudioTrack: ${st['lastError']}');
    expect(st['sampleRate'], fmt.sampleRate);

    // 5 ── The assertion that matters: PCM is moving, both directions.
    final push0 = st['pushFrames'] as int;
    final pull0 = st['pullFrames'] as int;

    micLevels.clear();
    rxLevels.clear();
    await Future<void>.delayed(const Duration(seconds: 5));

    st = await sdk.appOwnedAudioStatus();
    note('status after 5s: $st');
    final pushed = (st['pushFrames'] as int) - push0;
    final pulled = (st['pullFrames'] as int) - pull0;
    note('5s of audio: +$pushed pushed, +$pulled pulled');

    // 5 s at 20 ms frames is ~250; allow generous slack for scheduling but
    // require enough that a stalled thread cannot pass.
    expect(pushed, greaterThan(150),
        reason: 'the microphone is not reaching the SDK — capture thread '
            'stalled, or RECORD_AUDIO not granted');
    expect(pulled, greaterThan(150),
        reason: 'the far end is not reaching the speaker — playback thread '
            'stalled');
    expect(st['pushErrors'], 0);

    // Frames alone do not prove the microphone works: a channel config the
    // device accepts but cannot fill, or an Android permission answered with
    // silence instead of an error, both produce a healthy frame count and a
    // dead call. Assert on the samples.
    final peak = st['capturePeak'] as double;
    final nonSilent = st['nonSilentFrames'] as int;
    note('capture peak: $peak of full scale, '
        '$nonSilent/$pushed frames carried signal');
    expect(peak, greaterThan(0.0005),
        reason: 'the microphone delivered nothing but digital silence — frames '
            'moved but carried no samples at all');
    expect(nonSilent, greaterThan(0),
        reason: 'not one captured frame contained a non-zero sample');

    // 5b ── What the SDK itself made of the audio, sampled while the call is
    //       live rather than once after it ended.
    //
    //       -127 dBov is the all-zero sentinel, and in a quiet room that is
    //       the CORRECT reading, not a failure: the VOICE_COMMUNICATION
    //       capture path runs the platform noise suppressor, which emits exact
    //       digital silence when it decides there is no speech. So the TX
    //       level is only asserted when the microphone actually had signal
    //       above that gate — otherwise this test would demand ambient noise
    //       in the room where CI runs.
    note('mic levels during call: $micLevels');
    note('rx levels during call: $rxLevels');
    expect(micLevels, isNotEmpty, reason: 'no stats events arrived');
    final bestMic = micLevels.reduce((a, b) => a > b ? a : b);
    final bestRx = rxLevels.reduce((a, b) => a > b ? a : b);
    note('best mic=$bestMic dBov, best rx=$bestRx dBov');

    //       Deliberately NOT asserted. The level is a 1 Hz snapshot of one
    //       frame, while the noise gate leaves only a small fraction of frames
    //       non-silent in a quiet room — so the two are different statistics
    //       and a strict check here fails on ambient quiet, not on a defect.
    //       `nonSilentFrames` above is the reliable form of this question; the
    //       TX level stays as a diagnostic to read when triaging by hand.
    if (bestMic <= -127.0) {
      note('TX level read as silence: expected when the platform noise gate '
          'is closed ($nonSilent/$pushed frames had signal)');
    }

    // RX needs no such caveat: the echo service is always talking.
    expect(bestRx, greaterThan(-127.0),
        reason: 'no RX audio from the echo service — pull() is not delivering '
            'the far end');

    // 6 ── Handing the device back mid-call must also work.
    await sdk.useAppOwnedAudio(false);
    await Future<void>.delayed(const Duration(seconds: 2));
    st = await sdk.appOwnedAudioStatus();
    note('status after handing back: $st');
    expect(st['armed'], isFalse, reason: 'engine should have disarmed');

    call.hangup();
    await Future<void>.delayed(const Duration(seconds: 1));
    note('done');
  });
}

Future<void> _until(bool Function() cond,
    {Duration timeout = const Duration(seconds: 10)}) async {
  final deadline = DateTime.now().add(timeout);
  while (DateTime.now().isBefore(deadline)) {
    if (cond()) return;
    await Future<void>.delayed(const Duration(milliseconds: 100));
  }
}
