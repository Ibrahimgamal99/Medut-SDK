/// Medut example softphone.
///
/// Three tabs:
///  1. Account  — registration over UDP/TCP/TLS/WS/WSS, codec preference,
///                retry controls.
///  2. Call     — dial/answer/hold/mute/transfer, DTMF pad, speakerphone.
///  3. Diagnostics — live media stats, quality alerts, handover timeline,
///                in-app log console (the SDK never writes to the terminal).
library;

import 'dart:async';

import 'package:medut/medut.dart';
import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import 'account_store.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const MedutExampleApp());
}

class MedutExampleApp extends StatelessWidget {
  const MedutExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Medut example',
      theme: ThemeData(colorSchemeSeed: Colors.teal, useMaterial3: true),
      home: const PhonePage(),
    );
  }
}

class PhonePage extends StatefulWidget {
  const PhonePage({super.key});

  @override
  State<PhonePage> createState() => _PhonePageState();
}

class _PhonePageState extends State<PhonePage>
    with SingleTickerProviderStateMixin {
  late final TabController _tabs = TabController(length: 3, vsync: this);

  Medut? _sdk;
  Account? _account;
  Call? _call;
  StreamSubscription<MedutEvent>? _sub;

  // ── form state ─────────────────────────────────────────────────────────
  final _user = TextEditingController(text: 'alice@pbx.example.com');
  final _pass = TextEditingController();
  final _serverUrl = TextEditingController();
  /// Pre-filled because ICE without one is a trap: host-only candidates
  /// cannot traverse NAT, and a strict peer (Asterisk) silently drops media
  /// arriving from the peer-reflexive address it was never told about.
  final _stunServer =
      TextEditingController(text: 'stun:stun.l.google.com:19302');

  /// TURN, not just STUN, is what a carrier-grade NAT needs. A NAT that maps
  /// the same local port to a *different* public IP per destination makes STUN
  /// actively misleading: the reflexive address the STUN server reports is not
  /// the one the PBX sees, ICE nominates a peer-reflexive candidate that was
  /// never signalled, and a strict peer (Asterisk) drops the media. Re-offering
  /// does not rescue it — Asterisk only re-reads candidates on an ICE restart,
  /// which needs a new ufrag/pwd that baresip fixes at session creation. A TURN
  /// relay candidate is signalled up front and stays put, so it sidesteps the
  /// whole problem.
  final _turnServer = TextEditingController();
  final _turnUser = TextEditingController();
  final _turnPass = TextEditingController();
  final _callee = TextEditingController();
  final _transferTo = TextEditingController();
  Transport _transport = Transport.udp;
  /// DTLS-SRTP, not none: a WSS/WebRTC-facing PBX offers
  /// UDP/TLS/RTP/SAVPF, and an account answering plain RTP/AVP fails every
  /// incoming call — reported by baresip as "no common audio codecs", which
  /// sends you looking at the codec list instead of the media profile.
  MediaEncryption _mediaEnc = MediaEncryption.dtlsSrtp;
  bool _ice = false;
  bool _verifyTls = true;
  final List<String> _codecPrefs = ['opus', 'ulaw', 'alaw'];
  final Set<String> _enabledCodecs = {'opus', 'ulaw'};

  // ── persistence ────────────────────────────────────────────────────────
  final _store = AccountStore();

  /// Whether a successful register writes the form back to disk. Off means
  /// the next launch starts blank — and switching it off also wipes what is
  /// already stored, since leaving a password behind after the user opted out
  /// is the one thing the switch is there to prevent.
  bool _rememberAccount = true;

  /// Blocks the Account tab until the stored profile has been read, so the
  /// user cannot start typing into fields that are about to be overwritten.
  bool _profileLoaded = false;

  // ── live state ─────────────────────────────────────────────────────────
  RegState _regState = RegState.unregistered;
  String _regDetail = '';
  CallState? _callState;
  String _callPeer = '';
  bool _muted = false;
  bool _held = false;
  bool _speaker = false;
  bool _appAudio = false;
  MediaStats? _stats;

  /// When the call was answered, and a 1 Hz tick to redraw the elapsed time.
  ///
  /// Wall clock rather than MediaStatsEvent.callDurationMs: that only advances
  /// once per `statsIntervalMs` and is not emitted at all when stats are off,
  /// so a call timer built on it either stutters or never moves. The SDK's
  /// figure is the right one for reporting; this one is the right one for a
  /// clock the user is watching.
  DateTime? _answeredAt;
  Timer? _callTicker;
  final List<String> _alerts = [];
  final List<String> _handover = [];
  final List<String> _log = [];

  @override
  void initState() {
    super.initState();
    _restoreProfile();
  }

  // ── persistence ────────────────────────────────────────────────────────

  /// Repopulate the Account tab from the last saved profile.
  ///
  /// Nothing saved (first launch, or the user cleared it) leaves the form at
  /// its built-in defaults — so this only ever writes fields it actually has.
  Future<void> _restoreProfile() async {
    final remember = await _store.loadRemember();
    final p = remember ? await _store.load() : null;
    if (!mounted) return;
    setState(() {
      _profileLoaded = true;
      _rememberAccount = remember;
      if (p == null) return;
      _user.text = p.uri;
      _pass.text = p.password;
      _serverUrl.text = p.serverUrl;
      _stunServer.text = p.stunServer;
      _turnServer.text = p.turnServer;
      _turnUser.text = p.turnUser;
      _turnPass.text = p.turnPass;
      _transport = p.transport;
      _mediaEnc = p.mediaEnc;
      _ice = p.ice;
      _verifyTls = p.verifyTls;
      if (p.codecPrefs.isNotEmpty) {
        _codecPrefs
          ..clear()
          ..addAll(p.codecPrefs);
      }
      _enabledCodecs
        ..clear()
        ..addAll(p.enabledCodecs);
    });
    _logLine('account profile restored for ${p == null ? '(none)' : p.uri}');
  }

  /// Snapshot the form as it stands.
  AccountProfile _currentProfile() => AccountProfile(
        uri: _user.text,
        password: _pass.text,
        transport: _transport,
        serverUrl: _serverUrl.text,
        mediaEnc: _mediaEnc,
        ice: _ice,
        stunServer: _stunServer.text,
        turnServer: _turnServer.text,
        turnUser: _turnUser.text,
        turnPass: _turnPass.text,
        verifyTls: _verifyTls,
        codecPrefs: List.of(_codecPrefs),
        enabledCodecs: _codecPrefs.where(_enabledCodecs.contains).toList(),
      );

  /// Persist the form. Awaited nowhere on the call path — a slow keystore
  /// write must not delay the REGISTER — but failures are logged, because a
  /// profile that silently fails to save is worse than one that never tried.
  Future<void> _saveProfile() async {
    if (!_rememberAccount) return;
    try {
      await _store.save(_currentProfile());
    } catch (e) {
      _logLine('saving account profile failed: $e');
    }
  }

  /// Drop the stored profile, leaving the form as it is on screen.
  Future<void> _forgetProfile() async {
    await _store.clear();
    if (mounted) _snack('Saved account cleared');
  }

  /// Flip the remember switch, and act on it immediately in both directions.
  Future<void> _setRememberAccount(bool remember) async {
    setState(() => _rememberAccount = remember);
    await _store.saveRemember(remember);
    // Turning it off has to erase what is already on disk, not just stop
    // future writes — otherwise the password the user just opted out of
    // storing stays stored.
    if (remember) {
      await _saveProfile();
    } else {
      await _forgetProfile();
    }
  }

  @override
  void dispose() {
    _callTicker?.cancel();
    _sub?.cancel();
    _sdk?.shutdown();
    _tabs.dispose();
    super.dispose();
  }

  // ── Call timer ─────────────────────────────────────────────────────────

  /// Runs only while a call is up: a periodic timer that outlives the call it
  /// belongs to keeps the widget rebuilding forever, and on a phone that is a
  /// battery drain nobody attributes to the dialer.
  void _startCallTimer() {
    if (_callTicker != null) return;
    _answeredAt = DateTime.now();
    _callTicker = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  void _stopCallTimer() {
    _callTicker?.cancel();
    _callTicker = null;
    _answeredAt = null;
  }

  /// Elapsed call time as `M:SS`, or `H:MM:SS` once it runs past an hour.
  static String _fmtDuration(Duration d) {
    final h = d.inHours;
    final m = d.inMinutes % 60;
    final sec = d.inSeconds % 60;
    final ss = sec.toString().padLeft(2, '0');
    return h > 0 ? '$h:${m.toString().padLeft(2, '0')}:$ss' : '$m:$ss';
  }

  /// What to put under the peer name: the call's phase, and once it is up, how
  /// long it has been running.
  String get _callStatusLine {
    final st = _callState;
    if (st == null) return '';
    if (st == CallState.established || st == CallState.held) {
      final since = _answeredAt;
      final elapsed =
          since == null ? '0:00' : _fmtDuration(DateTime.now().difference(since));
      // Hold is still "in call" — the timer keeps running, as it does on every
      // phone, because the call is still connected and still being billed.
      return st == CallState.held ? 'On hold · $elapsed' : 'In call · $elapsed';
    }
    return switch (st) {
      CallState.calling => 'Dialing…',
      CallState.ringing => 'Ringing…',
      _ => st.name,
    };
  }

  void _snack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text(msg)));
  }

  void _logLine(String s) {
    // ignore: avoid_print
    print('BSDKLOG $s');
    setState(() {
      _log.add(s.trimRight());
      if (_log.length > 500) _log.removeRange(0, _log.length - 500);
    });
  }

  // ── registration ───────────────────────────────────────────────────────

  Future<void> _register() async {
    // Check the result. Without RECORD_AUDIO the OpenSL recorder still starts
    // and reports success — it just never delivers a buffer, so the call comes
    // up, the far end hears silence, and nothing anywhere says why. A denied
    // mic has to be said out loud.
    final mic = await Permission.microphone.request();
    if (!mic.isGranted) {
      _snack('Microphone denied — calls will have no outgoing audio');
      _logLine('microphone permission: $mic (calls will be one-way)');
    }

    try {
      _sdk ??= await Medut.start(
        config: const MedutConfig(
          logLevel: 3,
          // Experimental: gather on cellular next to Wi-Fi so a handover can
          // reuse candidates that already exist. Off here — on the Android
          // device it was tried on, the network-pinned socket never received
          // replies (see docs/guides/network_handover.md).
          iceMultipath: false,
          traceSip: true,
          statsIntervalMs: 2000,
          traceSdpDiff: true,
          mosAlertThreshold: 3.5,
          lossAlertThreshold: 5.0,
          jitterAlertThreshold: 40.0,
        ),
      );
    } on MedutException catch (e) {
      _snack(e.message ?? e.code);
      return;
    }

    final needsUrl =
        _transport == Transport.ws || _transport == Transport.wss;
    if (needsUrl && _serverUrl.text.isEmpty) {
      _snack('WS/WSS needs a server URL, e.g. wss://pbx:8089/ws');
      return;
    }

    // Registering again replaces the account — the old one must go, not just
    // be forgotten. A left-behind account keeps retrying its own registration
    // forever (a mistyped domain never resolves, so it never gives up) and
    // counts as a second SIP server for as long as it lives, which switches
    // off the SDK's WebSocket connection pinning for the account that works.
    await _account?.destroy();
    _account = null;

    final account = await _sdk!.createAccount(
      _user.text,
      _pass.text,
      config: AccountConfig(
        transport: _transport,
        serverUrl: _serverUrl.text.isEmpty ? null : _serverUrl.text,
        mediaEnc: _mediaEnc,
        iceEnabled: _ice,
        stunServer: _ice && _stunServer.text.isNotEmpty ? _stunServer.text : null,
        turnServer: _ice && _turnServer.text.isNotEmpty ? _turnServer.text : null,
        turnUser: _turnUser.text.isEmpty ? null : _turnUser.text,
        turnPass: _turnPass.text.isEmpty ? null : _turnPass.text,
        verifyTls: _verifyTls,
        audioCodecs:
            _codecPrefs.where(_enabledCodecs.contains).toList(),
      ),
    );
    _account = account;

    _sub?.cancel();
    _sub = _sdk!.events.listen(_onEvent);

    try {
      await account.register();
    } on MedutException catch (e) {
      _snack('register: ${e.message ?? e.code}');
      return;
    }
    setState(() => _regState = RegState.registering);

    // Save on register rather than on every keystroke: it is the point where
    // the user has declared this set of fields to be the one they meant.
    unawaited(_saveProfile());
  }

  void _unregister() {
    _account?.destroy();
    _account = null;
    setState(() {
      _regState = RegState.unregistered;
      _regDetail = '';
      _call = null;
      _callState = null;
    });
  }

  // ── event handling ─────────────────────────────────────────────────────

  void _onEvent(MedutEvent ev) {
    if (!mounted) return;
    switch (ev) {
      case RegStateEvent e:
        setState(() {
          _regState = e.state;
          _regDetail = switch (e.state) {
            // A retry is armed: count it down.  Reconnecting without one is a
            // handover or a dead keepalive path, where there is no delay to
            // show — just why.
            RegState.reconnecting when e.retryAttempt > 0 =>
              'attempt #${e.retryAttempt} in '
                  '${(e.retryDelayMs / 1000).toStringAsFixed(0)}s',
            RegState.reconnecting => e.errorStr ?? '',
            RegState.failed => e.errorStr ?? e.error.name,
            _ => '',
          };
        });

      case IncomingCallEvent e:
        setState(() {
          _call = e.call;
          _callState = CallState.ringing;
          _callPeer = e.displayName ?? e.fromUri;
        });
        _tabs.animateTo(1);
        _snack('Incoming call from $_callPeer');

      case CallStateEvent e:
        // Start on `established`, not on dial: the duration a user expects to
        // see is talk time, which begins when the far end answers.
        if (e.state == CallState.established) {
          _startCallTimer();
        } else if (e.state.isTerminal) {
          _stopCallTimer();
        }
        setState(() {
          _callState = e.state;
          if (e.state.isTerminal) {
            _call = null;
            _stats = null;
            _muted = false;
            _held = false;
            if (e.reason != null) _logLine('call ended: ${e.reason}');
          }
        });

      case CallDtmfEvent e:
        _logLine('DTMF received: ${e.digit}');

      case MediaStatsEvent e:
        setState(() => _stats = e.stats);
        // Also to the log: the two audio levels are what distinguish "the mic
        // is dead" from "nothing is coming back" from "it is all working and
        // you are holding it wrong", and reading them off a live call is
        // easier from a log than from the Diagnostics tab.
        // remoteAddr is where the stack is actually sending RTP. If that is a
        // private address the phone cannot route to, every packet is counted
        // as sent and then dropped by the local router — which looks identical
        // to a working transmitter from in here.
        // The mute state belongs on this line. baresip applies TX mute in
        // ausrc_read_handler(), before the aubuf and so before the filter the
        // level is measured in — so a muted call reports mic -127.0 dBov,
        // which is byte-for-byte what a starved or dead capture path reports.
        // Without the flag here the two are indistinguishable in a log, and
        // that ambiguity has already cost one debugging session.
        _logLine('level mic ${e.stats.micLevelDbov.toStringAsFixed(1)} dBov'
            '${_muted ? " (MUTED)" : ""} / '
            'spk ${e.stats.audioLevelDbov.toStringAsFixed(1)} dBov  '
            'tx ${e.stats.packetsSent} rx ${e.stats.packetsReceived}  '
            'peer ${e.stats.remoteAddr}');

      case QualityAlertEvent e:
        final msg = e.recovering
            ? '${e.issue.name} recovered '
                '(${e.value.toStringAsFixed(1)})'
            : '${e.issue.name} ${e.value.toStringAsFixed(1)} crossed '
                '${e.threshold.toStringAsFixed(1)}';
        // Also to the log. A snackbar and a Diagnostics list are both gone
        // by the time anyone reads a device log, and mediaStall on a call
        // with no audio is exactly what a capture of a broken call needs.
        _logLine('quality $msg');
        setState(() {
          _alerts.insert(0, '${_ts()} $msg');
          if (_alerts.length > 50) _alerts.removeLast();
        });
        if (!e.recovering) _snack('Quality alert: $msg');

      case NetworkEvent e:
        setState(() {
          _handover.insert(
              0,
              '${_ts()} ${e.stage.name}'
              '${e.attempt > 0 ? ' ${e.attempt}/${e.maxAttempts}' : ''}'
              '${e.localAddr.isNotEmpty ? ' @${e.localAddr}' : ''}'
              '${e.stage == NetworkStage.callMigrated ? ' (audio back after ${(e.elapsedMs / 1000).toStringAsFixed(1)}s)' : ''}');
          if (_handover.length > 50) _handover.removeLast();
        });

      case SdpNegotiationEvent e:
        _logLine('SDP: codec=${e.negotiatedCodec ?? '?'} '
            'crypto=${e.negotiatedCrypto ?? '?'}');

      case TransferRequestEvent e:
        _snack('Peer asks to transfer us to ${e.referToUri}');
        // Answer it. The far end is waiting for the SIP NOTIFY that says what
        // happened, and only accept/reject sends one. Accepting keeps the new
        // call linked to this one so the SDK reports the outcome for us —
        // hanging up and dialling the URI would not.
        e.call.transferAccept().then((accepted) {
          if (accepted == null) e.call.transferReject();
        });

      case RegistrarWarningEvent e:
        _logLine('registrar warning: ${e.message}');

      case MwiEvent e:
        _snack('Voicemail: ${e.newVoice} new / ${e.oldVoice} old');

      case MessageEvent e:
        _snack('${e.fromUri}: ${e.body}');

      case LogEvent e:
        _logLine(e.message);

      case SipTraceEvent e:
        // Full message, one logcat line per SIP line. Three lines was enough to
        // identify a message but not to debug one: Contact, Record-Route and
        // the SDP (a=setup, a=fingerprint, c=, m=) are exactly what you need
        // when media or in-dialog routing misbehaves, and all of them are below
        // the cut.
        _logLine('SIP ${e.direction.name} ${e.transport} ${e.remoteAddr}\n'
            '${e.rawMessage.split('\r\n').where((l) => l.isNotEmpty).join('\n  ')}');

      default:
        break;
    }
  }

  static String _ts() {
    final now = DateTime.now();
    return '${now.hour.toString().padLeft(2, '0')}:'
        '${now.minute.toString().padLeft(2, '0')}:'
        '${now.second.toString().padLeft(2, '0')}';
  }

  // ── call actions ───────────────────────────────────────────────────────

  Future<void> _dial() async {
    final acct = _account;
    if (acct == null || _callee.text.isEmpty) return;
    final peer = _callee.text;
    try {
      final call = await acct.call(peer);
      setState(() {
        _call = call;
        _callState = CallState.calling;
        _callPeer = peer;
      });
    } on MedutException catch (e) {
      _snack('call: ${e.message ?? e.code}');
    }
  }

  // ── UI ─────────────────────────────────────────────────────────────────

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Medut'),
        bottom: TabBar(controller: _tabs, tabs: const [
          Tab(icon: Icon(Icons.account_circle), text: 'Account'),
          Tab(icon: Icon(Icons.call), text: 'Call'),
          Tab(icon: Icon(Icons.monitor_heart), text: 'Diagnostics'),
        ]),
      ),
      body: TabBarView(controller: _tabs, children: [
        _buildAccountTab(),
        _buildCallTab(),
        _buildDiagnosticsTab(),
      ]),
    );
  }

  Widget _statusBanner() {
    final (color, text) = switch (_regState) {
      RegState.registered => (Colors.green, 'Registered'),
      RegState.registering => (Colors.orange, 'Registering…'),
      // Amber, not red: the SDK is fixing this one itself.
      RegState.reconnecting => (
          Colors.amber.shade700,
          _regDetail.isEmpty ? 'Reconnecting…' : 'Reconnecting — $_regDetail'
        ),
      RegState.failed => (Colors.red, 'Failed: $_regDetail'),
      RegState.unregistering => (Colors.orange, 'Unregistering…'),
      RegState.unregistered => (Colors.grey, 'Not registered'),
    };
    return Material(
      color: color.withOpacity(.15),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(children: [
          Icon(Icons.circle, color: color, size: 12),
          const SizedBox(width: 8),
          Expanded(child: Text(text)),
          if (_regState == RegState.failed ||
              _regState == RegState.reconnecting) ...[
            TextButton(
              onPressed: () => _account?.retryNow(),
              child: const Text('Retry now'),
            ),
            TextButton(
              onPressed: () => _account?.cancelRetry(),
              child: const Text('Stop'),
            ),
          ],
        ]),
      ),
    );
  }

  Widget _buildAccountTab() {
    // Reconnecting counts as registered here: the account object is alive and
    // the SDK is still working on it, so the credential fields stay locked.
    final registered = _regState == RegState.registered ||
        _regState == RegState.registering ||
        _regState == RegState.reconnecting;
    // The stored profile arrives one frame or two after the first build.
    // Showing the empty form in the meantime invites the user to start typing
    // into fields that _restoreProfile is about to overwrite.
    if (!_profileLoaded) {
      return const Center(child: CircularProgressIndicator());
    }
    return ListView(padding: const EdgeInsets.all(16), children: [
      _statusBanner(),
      const SizedBox(height: 16),
      TextField(
        controller: _user,
        decoration: const InputDecoration(
            labelText: 'SIP URI', hintText: 'user@pbx.example.com'),
        enabled: !registered,
      ),
      TextField(
        controller: _pass,
        decoration: const InputDecoration(labelText: 'Password'),
        obscureText: true,
        enabled: !registered,
      ),
      DropdownButtonFormField<Transport>(
        value: _transport,
        decoration: const InputDecoration(labelText: 'Transport'),
        items: Transport.values
            .map((t) => DropdownMenuItem(
                value: t, child: Text(t.name.toUpperCase())))
            .toList(),
        onChanged: registered
            ? null
            : (t) => setState(() => _transport = t ?? Transport.udp),
      ),
      TextField(
        controller: _serverUrl,
        decoration: const InputDecoration(
          labelText: 'Server URL (required for WS/WSS)',
          hintText: 'wss://pbx.example.com:8089/ws',
        ),
        enabled: !registered,
      ),
      DropdownButtonFormField<MediaEncryption>(
        value: _mediaEnc,
        decoration: const InputDecoration(labelText: 'Media encryption'),
        items: const [
          DropdownMenuItem(
              value: MediaEncryption.none, child: Text('None (RTP)')),
          DropdownMenuItem(
              value: MediaEncryption.sdes, child: Text('SDES-SRTP')),
          DropdownMenuItem(
              value: MediaEncryption.dtlsSrtp, child: Text('DTLS-SRTP')),
        ],
        onChanged: registered
            ? null
            : (v) => setState(() => _mediaEnc = v ?? MediaEncryption.none),
      ),
      SwitchListTile(
        title: const Text('ICE'),
        value: _ice,
        onChanged: registered ? null : (v) => setState(() => _ice = v),
      ),
      if (_ice)
        TextField(
          controller: _stunServer,
          enabled: !registered,
          decoration: const InputDecoration(
            labelText: 'STUN server',
            hintText: 'stun:stun.example.com:3478',
            helperText: 'Without one, ICE offers only this device\'s LAN '
                'address and media is dropped behind NAT',
            helperMaxLines: 3,
          ),
        ),
      if (_ice) ...[
        TextField(
          controller: _turnServer,
          enabled: !registered,
          decoration: const InputDecoration(
            labelText: 'TURN server (optional)',
            hintText: 'turn:turn.example.com:3478',
            helperText: 'Needed on a carrier NAT that gives out a different '
                'public IP per destination — STUN alone cannot fix that',
            helperMaxLines: 3,
          ),
        ),
        TextField(
          controller: _turnUser,
          enabled: !registered,
          decoration: const InputDecoration(labelText: 'TURN username'),
        ),
        TextField(
          controller: _turnPass,
          enabled: !registered,
          obscureText: true,
          decoration: const InputDecoration(labelText: 'TURN password'),
        ),
      ],
      SwitchListTile(
        title: const Text('Verify TLS certificate'),
        value: _verifyTls,
        onChanged:
            registered ? null : (v) => setState(() => _verifyTls = v),
      ),
      const SizedBox(height: 8),
      Text('Codecs (drag to reorder, tap to toggle)',
          style: Theme.of(context).textTheme.labelLarge),
      SizedBox(
        height: 72,
        child: ReorderableListView(
          scrollDirection: Axis.horizontal,
          onReorder: registered
              ? (a, b) {}
              : (oldIdx, newIdx) => setState(() {
                    if (newIdx > oldIdx) newIdx--;
                    _codecPrefs.insert(
                        newIdx, _codecPrefs.removeAt(oldIdx));
                  }),
          children: [
            for (final codec in _codecPrefs)
              Padding(
                key: ValueKey(codec),
                padding: const EdgeInsets.all(8),
                child: FilterChip(
                  label: Text(codec),
                  selected: _enabledCodecs.contains(codec),
                  onSelected: registered
                      ? null
                      : (on) => setState(() {
                            on
                                ? _enabledCodecs.add(codec)
                                : _enabledCodecs.remove(codec);
                          }),
                ),
              ),
          ],
        ),
      ),
      const SizedBox(height: 8),
      SwitchListTile(
        title: const Text('Remember this account'),
        subtitle: const Text('Settings are restored on the next launch; the '
            'SIP and TURN passwords go to the device keystore'),
        isThreeLine: true,
        value: _rememberAccount,
        onChanged: _setRememberAccount,
      ),
      const SizedBox(height: 8),
      FilledButton.icon(
        icon: Icon(registered ? Icons.logout : Icons.login),
        label: Text(registered ? 'Unregister' : 'Register'),
        onPressed: registered ? _unregister : _register,
      ),
    ]);
  }

  Widget _buildCallTab() {
    final call = _call;
    final inCall = call != null;
    return ListView(padding: const EdgeInsets.all(16), children: [
      _statusBanner(),
      const SizedBox(height: 12),
      if (!inCall) ...[
        TextField(
          controller: _callee,
          decoration: const InputDecoration(
              labelText: 'Call to', hintText: 'bob@pbx.example.com'),
        ),
        const SizedBox(height: 12),
        FilledButton.icon(
          icon: const Icon(Icons.call),
          label: const Text('Dial'),
          onPressed: _regState == RegState.registered ? _dial : null,
        ),
      ] else ...[
        Card(
          child: ListTile(
            leading: const Icon(Icons.person),
            title: Text(_callPeer),
            subtitle: Text(_callStatusLine),
          ),
        ),
        const SizedBox(height: 8),
        Wrap(spacing: 8, runSpacing: 8, children: [
          if (_callState == CallState.ringing)
            FilledButton.icon(
              icon: const Icon(Icons.call),
              label: const Text('Answer'),
              onPressed: () => call.answer(),
            ),
          FilledButton.tonalIcon(
            icon: const Icon(Icons.call_end),
            label: const Text('Hang up'),
            onPressed: () => call.hangup(),
          ),
          FilledButton.tonalIcon(
            icon: Icon(_held ? Icons.play_arrow : Icons.pause),
            label: Text(_held ? 'Resume' : 'Hold'),
            onPressed: () {
              _held ? call.resume() : call.hold();
              setState(() => _held = !_held);
            },
          ),
          FilledButton.tonalIcon(
            icon: Icon(_muted ? Icons.mic : Icons.mic_off),
            label: Text(_muted ? 'Unmute' : 'Mute'),
            onPressed: () {
              call.mute(on: !_muted);
              setState(() => _muted = !_muted);
            },
          ),
          FilledButton.tonalIcon(
            icon:
                Icon(_speaker ? Icons.phone_in_talk : Icons.volume_up),
            label: Text(_speaker ? 'Earpiece' : 'Speaker'),
            onPressed: () {
              _sdk?.setSpeakerphone(!_speaker);
              setState(() => _speaker = !_speaker);
            },
          ),
          // Switchable mid-call on purpose: that is the interesting case, and
          // the one worth having in front of you when a device misbehaves.
          FilledButton.tonalIcon(
            icon: Icon(_appAudio ? Icons.headset_mic : Icons.settings_voice),
            label: Text(_appAudio ? 'SDK audio' : 'App audio'),
            onPressed: () async {
              final sdk = _sdk;
              if (sdk == null) return;
              try {
                await sdk.useAppOwnedAudio(!_appAudio);
                setState(() => _appAudio = !_appAudio);
                _logLine(_appAudio
                    ? 'App owns the mic and speaker'
                    : 'SDK owns the mic and speaker');
              } catch (e) {
                _logLine('app-owned audio: $e');
              }
            },
          ),
        ]),
        const Divider(height: 32),
        Text('DTMF', style: Theme.of(context).textTheme.labelLarge),
        _dtmfPad(call),
        const Divider(height: 32),
        TextField(
          controller: _transferTo,
          decoration: const InputDecoration(
              labelText: 'Transfer to',
              hintText: 'carol@pbx.example.com'),
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          icon: const Icon(Icons.phone_forwarded),
          label: const Text('Blind transfer'),
          onPressed: () {
            if (_transferTo.text.isNotEmpty) call.transfer(_transferTo.text);
          },
        ),
      ],
    ]);
  }

  Widget _dtmfPad(Call call) {
    const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#'];
    return GridView.count(
      crossAxisCount: 3,
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      childAspectRatio: 1.9,
      children: [
        for (final k in keys)
          Padding(
            padding: const EdgeInsets.all(4),
            child: OutlinedButton(
              onPressed: () {
                call.sendDtmf(k);
                _logLine('DTMF sent: $k');
              },
              child: Text(k, style: const TextStyle(fontSize: 20)),
            ),
          ),
      ],
    );
  }

  Widget _buildDiagnosticsTab() {
    final s = _stats;
    return ListView(padding: const EdgeInsets.all(16), children: [
      Text('Media stats', style: Theme.of(context).textTheme.titleMedium),
      Card(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: s == null
              ? const Text('No active call / stats not started yet.')
              : Text(
                  'codec        ${s.codec} @${s.codecClockRate} Hz  pt=${s.payloadType}\n'
                  'MOS-LQ       ${s.mosLq.toStringAsFixed(2)}  (rx ${s.mosLqRx.toStringAsFixed(2)}, '
                  'min ${s.mosLqMin.toStringAsFixed(2)}, avg ${s.mosLqAvg.toStringAsFixed(2)})\n'
                  'RTT          ${s.rttMs.toStringAsFixed(1)} ms\n'
                  'jitter       rx ${s.jitterMs.toStringAsFixed(1)} ms / tx ${s.txJitterMs.toStringAsFixed(1)} ms\n'
                  'loss         tx ${s.lossPct.toStringAsFixed(1)}% / rx ${s.lossPctRx.toStringAsFixed(1)}%\n'
                  'bandwidth    up ${s.bandwidthTx} / down ${s.bandwidthRx} kbps '
                  '(avg ${s.avgBandwidthTx}/${s.avgBandwidthRx})\n'
                  'jitter buf   ${s.jitterBufferMs} ms, ${s.jitterBufferLoad} pkts, '
                  'late ${s.latePackets}, drop ${s.discardedPackets}\n'
                  'PLC          ${s.plcFrames} frames (${(s.plcRatio * 100).toStringAsFixed(1)}%)\n'
                  'level        spk ${s.audioLevelDbov.toStringAsFixed(0)} dBov / '
                  'mic ${s.micLevelDbov.toStringAsFixed(0)} dBov\n'
                  'packets      tx ${s.packetsSent} / rx ${s.packetsReceived}\n'
                  'remote       ${s.remoteAddr}  duration ${(s.callDurationMs / 1000).toStringAsFixed(0)}s',
                  style: const TextStyle(
                      fontFamily: 'monospace', fontSize: 12),
                ),
        ),
      ),
      const SizedBox(height: 12),
      Text('Quality alerts',
          style: Theme.of(context).textTheme.titleMedium),
      Card(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: _alerts.isEmpty
              ? const Text('None.')
              : Text(_alerts.join('\n'),
                  style: const TextStyle(
                      fontFamily: 'monospace', fontSize: 12)),
        ),
      ),
      const SizedBox(height: 12),
      Text('Network handover',
          style: Theme.of(context).textTheme.titleMedium),
      Card(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: _handover.isEmpty
              ? const Text('No network changes observed.')
              : Text(_handover.join('\n'),
                  style: const TextStyle(
                      fontFamily: 'monospace', fontSize: 12)),
        ),
      ),
      const SizedBox(height: 12),
      Row(children: [
        Text('SDK log', style: Theme.of(context).textTheme.titleMedium),
        const Spacer(),
        TextButton(
          onPressed: () => setState(_log.clear),
          child: const Text('Clear'),
        ),
      ]),
      Card(
        color: Colors.black,
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Text(
            _log.isEmpty ? '(empty)' : _log.reversed.take(100).join('\n'),
            style: const TextStyle(
                color: Colors.greenAccent,
                fontFamily: 'monospace',
                fontSize: 11),
          ),
        ),
      ),
    ]);
  }
}
