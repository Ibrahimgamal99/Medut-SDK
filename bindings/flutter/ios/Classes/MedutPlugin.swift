import AVFoundation
import Flutter
import Network
import UIKit

/// Platform shim for the Medut FFI plugin (iOS).
///
/// The SIP/media work happens in the native core driven over dart:ffi; this
/// class covers only what FFI cannot reach:
///  - a writable directory for the SDK's tmp state (parity with Android —
///    the core also auto-detects $TMPDIR on iOS),
///  - audio-session activation around calls,
///  - audio-route enumeration/selection (earpiece/speaker/Bluetooth/wired),
///  - NWPathMonitor network-change callbacks, forwarded to Dart so the SDK
///    can re-register / migrate calls on network handover.
public class MedutPlugin: NSObject, FlutterPlugin {
  private var channel: FlutterMethodChannel?
  private var pathMonitor: NWPathMonitor?
  /// Signature of the last path we reported — see `pathSignature(_:)`.
  private var lastPathSignature: String?
  /// Observer for AVAudioSession interruptions (Siri, an inbound cellular call).
  private var interruptionObserver: NSObjectProtocol?
  /// True while THIS plugin holds the activation (host set manageAudioSession).
  /// False when CallKit owns it, in which case we forward its activated/
  /// deactivated callbacks and must never call setActive ourselves.
  private var pluginActivatedSession = false
  private var routeObserver: NSObjectProtocol?

  /// Report-only mode: a CallKit-integrated host app may own routing itself
  /// (CallKit also resets the output override on activation) — enumeration
  /// and change events keep flowing, selection becomes a no-op, so the two
  /// route owners never fight.
  private var externalRouting = false

  /// Reference implementation of the app-owned audio device (VoiceProcessingIO).
  /// Idle until Dart turns it on; the SDK owns the device by default.
  private lazy var appAudio: MedutAudioEngine = {
    let engine = MedutAudioEngine()
    engine.onError = { [weak self] code, message in
      self?.channel?.invokeMethod("onAppOwnedAudioError",
                                  arguments: ["code": code, "message": message])
    }
    return engine
  }()

  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: "medut",
                                       binaryMessenger: registrar.messenger())
    let instance = MedutPlugin()
    instance.channel = channel
    registrar.addMethodCallDelegate(instance, channel: channel)
    instance.startPathMonitor()
    instance.observeRouteChanges()
    instance.observeInterruptions()
  }

  public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
    appAudio.disarm()
    // Hand the audio session back. Android's counterpart (abandonAudioFocus,
    // which also restores MODE_NORMAL) has always run on detach; iOS kept the
    // exclusive PlayAndRecord route, so an engine detached mid-call left the
    // device holding the mic and other apps ducked with nothing playing.
    configureAudioSession(active: false)
    pathMonitor?.cancel()
    pathMonitor = nil
    lastPathSignature = nil
    if let observer = routeObserver {
      NotificationCenter.default.removeObserver(observer)
      routeObserver = nil
    }
    if let observer = interruptionObserver {
      NotificationCenter.default.removeObserver(observer)
      interruptionObserver = nil
    }
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "getCacheDir":
      result(NSTemporaryDirectory())

    case "configureAudioSession":
      let args = call.arguments as? [String: Any]
      let active = args?["active"] as? Bool ?? false
      configureAudioSession(active: active)
      result(nil)

    case "setSpeakerphone":
      let args = call.arguments as? [String: Any]
      let on = args?["on"] as? Bool ?? false
      if !externalRouting {
        let session = AVAudioSession.sharedInstance()
        do {
          try session.overrideOutputAudioPort(on ? .speaker : .none)
        } catch {
          NSLog("Medut: overrideOutputAudioPort failed: %@",
                error.localizedDescription)
        }
      }
      result(nil)

    case "listAudioRoutes":
      result(listAudioRoutes())

    case "setAudioRoute":
      let args = call.arguments as? [String: Any]
      guard let id = args?["id"] as? String else {
        result(FlutterError(code: "bad-args",
                            message: "setAudioRoute needs an id",
                            details: nil))
        return
      }
      if !externalRouting { selectRoute(id: id) }
      result(listAudioRoutes())

    case "setExternalRouting":
      let args = call.arguments as? [String: Any]
      externalRouting = args?["on"] as? Bool ?? false
      result(nil)

    case "startAppOwnedAudio":
      appAudio.arm()
      result(nil)

    case "stopAppOwnedAudio":
      appAudio.disarm()
      result(nil)

    case "appOwnedAudioStatus":
      result(appAudio.status())

    // CallKit hosts own session activation (they pass
    // platformAudioActivate: false and manageAudioSession: false), so the
    // engine cannot learn it any other way. Forward CXProvider's
    // didActivate/didDeactivate here.
    case "notifyCallKitAudioActive":
      let args = call.arguments as? [String: Any]
      if args?["active"] as? Bool ?? false {
        appAudio.sessionActivated()
      } else {
        appAudio.sessionDeactivated()
      }
      result(nil)

    default:
      result(FlutterMethodNotImplemented)
    }
  }

  /// The native core sets the category (PlayAndRecord + VoiceChat) at init;
  /// this only toggles activation around calls. Deactivation is best-effort
  /// and notifies other apps their audio may resume.
  private func configureAudioSession(active: Bool) {
    let session = AVAudioSession.sharedInstance()
    do {
      if active {
        try session.setActive(true)
        pluginActivatedSession = true
        // Tell the engine only once activation actually succeeded — starting
        // the AudioUnit against an inactive session is the ordering bug that
        // owning the device is supposed to remove.
        appAudio.sessionActivated()
      } else {
        appAudio.sessionDeactivated()
        pluginActivatedSession = false
        try session.setActive(false,
                              options: .notifyOthersOnDeactivation)
      }
    } catch {
      NSLog("Medut: audio session setActive(%d) failed: %@",
            active ? 1 : 0, error.localizedDescription)
    }
  }

  // ── Audio routes ──────────────────────────────────────────────────────
  //
  // Under PlayAndRecord + VoiceChat the OUTPUT follows the selected INPUT, so
  // the route list is built from `availableInputs` plus one synthesized
  // loudspeaker entry (the speaker is an output *override*, never an input).
  // `isActive` derives from the actual current output port — the
  // authoritative answer, unlike input-side guessing. A Bluetooth device's
  // OUTPUT port UID differs from its INPUT UID, so matching is always on the
  // ids issued here, never on raw output UIDs.

  private func routeKind(_ port: AVAudioSessionPortDescription) -> String {
    switch port.portType {
    case .builtInMic: return "earpiece"
    case .bluetoothHFP, .bluetoothLE: return "bluetooth"
    default: return "wired-headset" // headsetMic, usbAudio, carAudio, …
    }
  }

  private func activeKind() -> String {
    let out = AVAudioSession.sharedInstance().currentRoute.outputs.first
    switch out?.portType {
    case .some(.builtInSpeaker): return "speaker"
    case .some(.builtInReceiver): return "earpiece"
    case .some(.bluetoothHFP), .some(.bluetoothA2DP), .some(.bluetoothLE):
      return "bluetooth"
    case .none: return "earpiece"
    default: return "wired-headset"
    }
  }

  /// Stable route id for [input]. Semantic for the singletons
  /// ('earpiece' | 'wired-headset' | first 'bluetooth'); port-UID-suffixed
  /// only for additional BT devices.
  private func routeId(_ input: AVAudioSessionPortDescription,
                       sawBluetooth: inout Bool) -> String {
    switch routeKind(input) {
    case "earpiece": return "earpiece"
    case "bluetooth":
      if sawBluetooth { return "bt:\(input.uid)" }
      sawBluetooth = true
      return "bluetooth"
    default: return "wired-headset"
    }
  }

  private func listAudioRoutes() -> [[String: Any]] {
    let session = AVAudioSession.sharedInstance()
    let active = activeKind()
    var routes: [[String: Any]] = []
    var sawBt = false
    for input in session.availableInputs ?? [] {
      let kind = routeKind(input)
      let id = routeId(input, sawBluetooth: &sawBt)
      if routes.contains(where: { ($0["id"] as? String) == id }) { continue }
      routes.append([
        "id": id,
        // Built-ins get '' so the host app can localize ("Earpiece").
        "name": kind == "earpiece" ? "" : input.portName,
        "kind": kind,
        "isActive": kind == active,
      ])
    }
    routes.append([
      "id": "speaker", "name": "", "kind": "speaker",
      "isActive": active == "speaker",
    ])
    return routes
  }

  private func selectRoute(id: String) {
    let session = AVAudioSession.sharedInstance()
    do {
      if id == "speaker" {
        try session.overrideOutputAudioPort(.speaker)
        return
      }
      // Clear the speaker override FIRST — while it is in force,
      // setPreferredInput is ignored.
      try session.overrideOutputAudioPort(.none)
      var sawBt = false
      for input in session.availableInputs ?? [] {
        if routeId(input, sawBluetooth: &sawBt) == id {
          try session.setPreferredInput(input)
          return
        }
      }
      // Unknown id → system default input.
      try session.setPreferredInput(nil)
    } catch {
      NSLog("Medut: selectRoute(%@) failed: %@", id, error.localizedDescription)
    }
  }

  /// Push route changes to Dart so pickers stay honest without polling —
  /// headset plugged/unplugged, BT connected, CallKit override reset, a
  /// system-initiated move.
  private func observeRouteChanges() {
    routeObserver = NotificationCenter.default.addObserver(
      forName: AVAudioSession.routeChangeNotification,
      object: nil, queue: .main
    ) { [weak self] note in
      guard let self = self,
            let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt,
            let reason = AVAudioSession.RouteChangeReason(rawValue: raw) else { return }
      switch reason {
      case .newDeviceAvailable, .oldDeviceUnavailable,
           .routeConfigurationChange, .override, .categoryChange:
        self.channel?.invokeMethod("onAudioRoutesChanged", arguments: nil)
      default:
        break
      }
    }
  }

  /// Forward network-path changes to Dart (drives medut_network_changed()).
  ///
  /// Every notification forwarded from here costs a full handover in the SDK:
  /// `medut_network_changed()` flushes the SIP transports, re-REGISTERs, and
  /// — over WebSocket, where the Contact is tied to the connection — re-INVITEs
  /// every live call to re-bind its dialog. On a call that is already up that
  /// is seconds of audio, so a notification that does not correspond to a real
  /// handover is not a harmless extra event.
  ///
  /// NWPathMonitor cannot be taken at its word for that. It fires on *any*
  /// path property change — `isExpensive`/`isConstrained` flips, DNS changes, a
  /// secondary interface appearing — repeats the same `.satisfied` status
  /// several times for one Wi-Fi transition, and fires again when the app comes
  /// back to the foreground with nothing about the network having changed.
  /// A VoIP app foregrounds exactly when a push arrives for an incoming call,
  /// which is the worst possible moment for a spurious handover: measured
  /// on-device (2026-09-02, inbound over WSS), one such notification 1.3 s into
  /// the ring produced a transport reset 1 s after the answer and 4 s of dead
  /// air at the head of the call.
  ///
  /// So compare against what actually decides a handover — the reachability
  /// status and the set of usable interfaces — and forward only when one of
  /// them moved. Interface *names* rather than types: Wi-Fi to Wi-Fi is still a
  /// different path when it is a different interface, and a same-name repeat is
  /// the case being suppressed.
  ///
  /// The set of available interfaces is NOT sufficient on its own, though.
  /// `availableInterfaces` lists what could carry the path, not what does — so
  /// on a phone with Wi-Fi and cellular both up, a Wi-Fi -> cellular switch of
  /// the PRIMARY path leaves `status == .satisfied` and that set unchanged, and
  /// the filter suppressed a handover that really happened. Android notices,
  /// because a default-network callback keys on the network's identity. And
  /// there is no safety net: Medut.start() sets netMonitorIntervalSeconds: 0
  /// on both mobile platforms, so the built-in getifaddrs() poller is off and a
  /// missed notification means no handover at all — the call stays bound to the
  /// dead path until the keepalive probe times out ~30 s later. Hence the
  /// signature below also carries the primary interface and the interface types
  /// the path actually traverses.
  /// Everything about a path that decides whether a handover is real:
  /// reachability, which interface is primary, every usable interface, and the
  /// interface types the path traverses. Two paths with equal signatures need
  /// no handover; any difference is one worth reporting.
  private func pathSignature(_ path: NWPath) -> String {
    let names = path.availableInterfaces.map { $0.name }
    // availableInterfaces is ordered best-first, so element 0 is the primary.
    let primary = names.first ?? "-"
    let types: [(String, NWInterface.InterfaceType)] = [
      ("wifi", .wifi), ("cell", .cellular), ("wired", .wiredEthernet),
      ("loop", .loopback), ("other", .other),
    ]
    let used = types.filter { path.usesInterfaceType($0.1) }.map { $0.0 }
    return [
      String(describing: path.status),
      primary,
      names.sorted().joined(separator: ","),
      used.joined(separator: ","),
    ].joined(separator: "|")
  }

  private func startPathMonitor() {
    // Idempotent: a second monitor would deliver every path update twice, and
    // each duplicate is another handover. Registering more than once is not
    // hypothetical for a push-woken app whose engine can be re-attached.
    pathMonitor?.cancel()
    lastPathSignature = nil

    let monitor = NWPathMonitor()
    pathMonitor = monitor
    monitor.pathUpdateHandler = { [weak self] path in
      guard let self = self else { return }

      let signature = self.pathSignature(path)

      // First callback after start() always fires and reports the current
      // path; lastPathSignature is nil then, so it is forwarded once. That is
      // the registration-time notification, and the SDK treats a handover
      // with an unchanged address set as a transport refresh.
      if self.lastPathSignature == signature {
        return
      }

      self.lastPathSignature = signature

      DispatchQueue.main.async {
        self.channel?.invokeMethod("onNetworkChanged", arguments: nil)
      }
    }
    monitor.start(queue: DispatchQueue.global(qos: .utility))
  }

  /// Stand the app-owned engine down across an audio-session interruption.
  ///
  /// Android has covered this since the app-owned path existed, through the
  /// audio-focus listener (AUDIOFOCUS_LOSS* -> pause, GAIN -> resume). iOS had
  /// no counterpart: the engine's only session input was the
  /// activated/deactivated pair driven by configureAudioSession and the CallKit
  /// forwarder, so Siri or an inbound cellular call left `running == YES`
  /// against a session iOS had already suspended — a live AudioUnit rendering
  /// into nothing, which is one-way or dead audio with no error surfaced. This
  /// is the same class of failure the mediaServicesWereReset observer exists
  /// for, and it reuses the same two entry points.
  private func observeInterruptions() {
    if let observer = interruptionObserver {
      NotificationCenter.default.removeObserver(observer)
    }
    interruptionObserver = NotificationCenter.default.addObserver(
      forName: AVAudioSession.interruptionNotification,
      object: AVAudioSession.sharedInstance(),
      queue: nil
    ) { [weak self] note in
      guard let self = self,
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
            let type = AVAudioSession.InterruptionType(rawValue: raw)
      else { return }

      switch type {
      case .began:
        NSLog("Medut: audio session interrupted — standing the engine down")
        self.appAudio.sessionDeactivated()
      case .ended:
        // Only reactivate a session this plugin activated. When CallKit owns
        // it (the app-owned-audio configuration on iOS), CXProvider calls
        // didActivateAudioSession itself once the interruption clears and the
        // host forwards that as notifyCallKitAudioActive — calling setActive
        // here as well would be two owners fighting over one session.
        guard self.pluginActivatedSession else {
          NSLog("Medut: interruption ended — CallKit owns the session, "
                + "waiting for its activation")
          return
        }
        // iOS does not reactivate for us. Go through configureAudioSession so
        // activation and the engine's sessionActivated() stay in the one order
        // that works.
        NSLog("Medut: audio session interruption ended — restoring")
        self.configureAudioSession(active: true)
      @unknown default:
        break
      }
    }
  }
}
