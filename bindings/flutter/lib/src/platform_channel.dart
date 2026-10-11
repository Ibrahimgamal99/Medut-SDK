/// Mobile platform shim.
///
/// Talks to the plugin's native side (`EchoSDKPlugin` — Kotlin on Android,
/// Swift on iOS) over the `echo_sdk` MethodChannel for the few things FFI
/// cannot do:
///  - a writable temp dir (EchoSDK's required tmp_dir on Android),
///  - audio focus / session activation around calls,
///  - audio-route enumeration + selection (earpiece / speaker / Bluetooth /
///    wired) with change pushes,
///  - network-change callbacks (ConnectivityManager / NWPathMonitor) that
///    drive handover via networkChanged().
///
/// Everything here works WITHOUT [EchoSDK.start] — the plugin registers with
/// the Flutter engine independently of the SIP stack, so a host app can adopt
/// the route API before (or without) running SIP through EchoSDK.
///
/// Every call is a no-op on desktop so the package still works as a pure
/// FFI binding there.
library;

import 'dart:io' show Platform;

import 'package:flutter/services.dart';

/// Normalised category of an audio-output route, one enum across Android's
/// AudioDeviceInfo types and iOS's AVAudioSession ports.
enum AudioRouteKind { earpiece, speaker, bluetooth, wired, unknown }

/// A selectable audio-output route for a call.
class AudioRoute {
  const AudioRoute({
    required this.id,
    required this.name,
    required this.kind,
    required this.isActive,
  });

  /// Stable opaque id, handed back to [EchoSDKPlatform.setAudioRoute]
  /// ('earpiece' | 'speaker' | 'wired-headset' | 'bluetooth' | 'bt:<dev>').
  final String id;

  /// Device name for headsets; '' for built-ins so the app can localize.
  final String name;

  final AudioRouteKind kind;

  /// Whether call audio currently flows through this route. On pre-31
  /// Android a just-selected Bluetooth route stays inactive until the SCO
  /// link actually connects (0.5–2s) — an `onAudioRoutesChanged` push
  /// follows when it does.
  final bool isActive;

  static AudioRoute fromMap(Map<Object?, Object?> map) => AudioRoute(
        id: (map['id'] ?? '') as String,
        name: (map['name'] ?? '') as String,
        kind: switch (map['kind']) {
          'earpiece' => AudioRouteKind.earpiece,
          'speaker' => AudioRouteKind.speaker,
          'bluetooth' => AudioRouteKind.bluetooth,
          'wired-headset' => AudioRouteKind.wired,
          _ => AudioRouteKind.unknown,
        },
        isActive: (map['isActive'] ?? false) as bool,
      );

  @override
  String toString() =>
      'AudioRoute($id, $kind${isActive ? ', active' : ''}'
      '${name.isNotEmpty ? ', "$name"' : ''})';
}

class EchoSDKPlatform {
  static const MethodChannel _channel = MethodChannel('echo_sdk');
  static bool _handlerInstalled = false;
  static void Function()? onNetworkChanged;

  /// Android: a secondary (cellular) network came up or went away while the
  /// plugin had one requested; `handle` is `Network.getNetworkHandle()` or
  /// null when it was lost.  Feeds `echosdk_network_set_interface_handle`.
  static void Function(String ifname, int? handle)? onInterfaceHandle;

  /// Fired when the system's audio-route set (or the active route) changed —
  /// headset plugged/unplugged, Bluetooth connected, a system-initiated move.
  /// Re-list with [listAudioRoutes] to get the new truth.
  static void Function()? onAudioRoutesChanged;

  /// Fired when the app-owned audio engine could not do its job — the mic
  /// permission is missing, the negotiated rate will not open, the device
  /// died. Codes: `mic-permission`, `unsupported-rate`, `device-open`,
  /// `capture-dead`, `playback-dead`, `unavailable`.
  static void Function(String code, String message)? onAppOwnedAudioError;

  static bool get _isMobile {
    try {
      return Platform.isAndroid || Platform.isIOS;
    } catch (_) {
      return false;
    }
  }

  /// Install the inbound handler (network/route pings from the native side).
  static void ensureHandler() {
    if (_handlerInstalled || !_isMobile) return;
    _handlerInstalled = true;
    _channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'onNetworkChanged':
          onNetworkChanged?.call();
        case 'onInterfaceHandle':
          final args = call.arguments;
          if (args is Map) {
            final name = args['ifname'];
            final handle = args['handle'];
            if (name is String) {
              onInterfaceHandle?.call(name, handle is int ? handle : null);
            }
          }
        case 'onAudioRoutesChanged':
          onAudioRoutesChanged?.call();
        case 'onAppOwnedAudioError':
          final args = call.arguments;
          if (args is Map) {
            onAppOwnedAudioError?.call(
              (args['code'] ?? 'unknown') as String,
              (args['message'] ?? '') as String,
            );
          }
      }
      return null;
    });
  }

  /// Android: keep a cellular network requested (radio up next to Wi-Fi) so
  /// the SDK can gather ICE candidates on it; released when `on` is false.
  /// No-op elsewhere.
  static Future<void> setCellularRequested(bool on) async {
    if (!_isMobile) return;
    try {
      await _channel.invokeMethod('setCellularRequested', {'on': on});
    } catch (_) {
      // Older plugin binary or no cellular: multipath simply stays off.
    }
  }

  /// Writable temp directory — used as EchoSDK `tmp_dir`. Null on desktop.
  static Future<String?> getCacheDir() async {
    if (!_isMobile) return null;
    return _channel.invokeMethod<String>('getCacheDir');
  }

  /// Request/abandon the voice-call audio session (Android: audio focus +
  /// MODE_IN_COMMUNICATION; iOS: AVAudioSession activation).
  static Future<void> configureAudioSession(bool active) async {
    if (!_isMobile) return;
    await _channel.invokeMethod('configureAudioSession', {'active': active});
  }

  /// Route audio to the loudspeaker (true) or back to the best non-speaker
  /// route (false — Bluetooth, then wired, then earpiece; never blindly the
  /// earpiece while a headset is connected). Android + iOS.
  static Future<void> setSpeakerphone(bool on) async {
    if (!_isMobile) return;
    await _channel.invokeMethod('setSpeakerphone', {'on': on});
  }

  /// The audio-output routes the system currently offers for a call.
  /// [ensureHandler] + [onAudioRoutesChanged] keep the list fresh without
  /// polling. Empty on desktop (use the device APIs on [EchoSDK] there).
  static Future<List<AudioRoute>> listAudioRoutes() async {
    if (!_isMobile) return const [];
    final raw = await _channel.invokeListMethod<Object?>('listAudioRoutes');
    return _decodeRoutes(raw);
  }

  /// Point call audio at the route [id] (from [listAudioRoutes]). Returns the
  /// refreshed list — the route ACTUALLY in force, which can differ from the
  /// request (selection can fail, or complete asynchronously: pre-31
  /// Bluetooth SCO). No-op while external routing is on.
  static Future<List<AudioRoute>> setAudioRoute(String id) async {
    if (!_isMobile) return const [];
    final raw =
        await _channel.invokeListMethod<Object?>('setAudioRoute', {'id': id});
    return _decodeRoutes(raw);
  }

  /// Report-only mode for hosts whose call surface owns routing (Android
  /// self-managed Telecom holds CallAudioState while its call is active;
  /// CallKit resets the iOS output override on activation). Enumeration and
  /// [onAudioRoutesChanged] keep working; [setAudioRoute]/[setSpeakerphone]
  /// stop applying until turned back off.
  static Future<void> setExternalRouting(bool on) async {
    if (!_isMobile) return;
    await _channel.invokeMethod('setExternalRouting', {'on': on});
  }

  /// Start the native realtime audio loops for the app-owned device.
  ///
  /// Arms a watcher only — the actual capture/playback devices open when the
  /// call has media, which is later than "answered". Pair with
  /// `echosdk_audio_use_external(true)`; on its own this does nothing, because
  /// the SDK would still hold the platform device.
  static Future<void> startAppOwnedAudio() async {
    if (!_isMobile) return;
    await _channel.invokeMethod('startAppOwnedAudio');
  }

  /// Stop the native audio loops and release the devices.
  static Future<void> stopAppOwnedAudio() async {
    if (!_isMobile) return;
    await _channel.invokeMethod('stopAppOwnedAudio');
  }

  /// iOS + CallKit only: tell the app-owned audio engine that `CXProvider`
  /// activated or deactivated the session.
  ///
  /// A CallKit host passes `platformAudioActivate: false` and
  /// `manageAudioSession: false`, so nothing else can tell the engine when the
  /// session is usable — and starting the AudioUnit before `didActivate` is
  /// exactly the ordering bug owning the device is meant to remove. Forward
  /// `provider(_:didActivate:)` and `didDeactivate` here. No-op on Android.
  static Future<void> notifyCallKitAudioActive(bool active) async {
    if (!_isMobile) return;
    await _channel
        .invokeMethod('notifyCallKitAudioActive', {'active': active});
  }

  /// Diagnostics from the native engine: `armed`, `paused`, `running`,
  /// `available`, `sampleRate`, `channels`, `ptimeMs`, `lastError`.
  static Future<Map<String, Object?>> appOwnedAudioStatus() async {
    if (!_isMobile) return const {};
    final raw =
        await _channel.invokeMapMethod<String, Object?>('appOwnedAudioStatus');
    return raw ?? const {};
  }

  static List<AudioRoute> _decodeRoutes(List<Object?>? raw) => [
        for (final entry in raw ?? const <Object?>[])
          if (entry is Map<Object?, Object?>) AudioRoute.fromMap(entry),
      ];
}
