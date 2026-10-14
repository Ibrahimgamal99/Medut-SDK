#if os(iOS)
import AVFoundation
import Foundation
#if canImport(MedutAudioEngine)
import MedutAudioEngine  // SwiftPM; under CocoaPods the engine is in this module
#endif

/// A place the call audio can go.
public struct AudioRoute: Equatable, Sendable {
    public enum Kind: String, Sendable {
        case earpiece, speaker, bluetooth, wiredHeadset
    }

    /// Stable id: "earpiece", "speaker", "wired-headset", "bluetooth" for the
    /// first Bluetooth device and "bt:<uid>" for any others.
    public let id: String
    /// The device name; empty for the built-ins, so the app can localise them.
    public let name: String
    public let kind: Kind
    public let isActive: Bool
}

/// The app's AVAudioSession as the call stack sees it: route listing and
/// selection, and activation around calls under `AudioSessionPolicy.automatic`.
///
/// The stack sets the category (PlayAndRecord + VoiceChat) at start; this
/// class never changes it.  Ported from the Flutter plugin.
public final class MedutAudioSession: @unchecked Sendable {
    public static let shared = MedutAudioSession()

    /// Posted on the main queue when the available routes or the active route
    /// change: headset plugged, Bluetooth connected, a system move.
    public static let routesDidChangeNotification =
        Notification.Name("MedutAudioSessionRoutesDidChange")

    /// Report-only mode, for an app that owns routing itself (CallKit resets
    /// the output override on activation): routes are still listed and
    /// change notifications still posted, but `selectRoute` does nothing.
    public var routingIsExternal: Bool {
        get { lock.locked { _routingIsExternal } }
        set { lock.locked { _routingIsExternal = newValue } }
    }

    /// Failures of the app-owned device, which the SDK cannot see.  Main
    /// queue.
    public var onAppOwnedAudioError: ((_ code: String, _ message: String) -> Void)? {
        get { lock.locked { _onAppOwnedAudioError } }
        set { lock.locked { _onAppOwnedAudioError = newValue } }
    }

    private let lock = NSLock()
    private var _routingIsExternal = false
    private var _onAppOwnedAudioError: ((String, String) -> Void)?
    /// Serialises activation and the engine.  Everything below is touched
    /// only on it.
    private let queue = DispatchQueue(label: "dev.medut.audio-session")
    private var activatedBySDK = false
    private var sessionActive = false
    private var appOwnedAudio = false
    private lazy var engine: MedutAudioEngine = {
        let engine = MedutAudioEngine()
        engine.onError = { [weak self] code, message in
            self?.onAppOwnedAudioError?(code, message)
        }
        return engine
    }()
    private var observers: [NSObjectProtocol] = []

    private init() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(
            forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main
        ) { note in
            guard let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt,
                  let reason = AVAudioSession.RouteChangeReason(rawValue: raw) else { return }
            switch reason {
            case .newDeviceAvailable, .oldDeviceUnavailable, .routeConfigurationChange,
                 .override, .categoryChange:
                NotificationCenter.default.post(name: MedutAudioSession.routesDidChangeNotification,
                                                object: nil)
            default:
                break
            }
        })
        observers.append(center.addObserver(
            forName: AVAudioSession.interruptionNotification,
            object: AVAudioSession.sharedInstance(), queue: nil
        ) { [weak self] note in
            guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
            self?.queue.async {
                guard let self = self else { return }
                switch type {
                case .began:
                    // Siri or a cellular call suspended the session: an
                    // app-owned unit left running renders into nothing.
                    self.sessionActive = false
                    if self.appOwnedAudio { self.engine.sessionDeactivated() }
                case .ended:
                    // iOS does not reactivate for us.  Only a session the SDK
                    // activated: under CallKit, CXProvider reactivates it and
                    // reports it through callKitDidActivate().
                    if self.activatedBySDK { self.setActive(true) }
                @unknown default:
                    break
                }
            }
        })
    }

    // ── Routes ───────────────────────────────────────────────────────────
    //
    // Under PlayAndRecord + VoiceChat the output follows the selected input,
    // so the list is built from availableInputs plus a synthesised speaker
    // entry (the speaker is an output override, never an input).  isActive
    // comes from the current output port.

    public func availableRoutes() -> [AudioRoute] {
        let session = AVAudioSession.sharedInstance()
        let active = activeKind()
        var routes: [AudioRoute] = []
        var sawBluetooth = false
        for input in session.availableInputs ?? [] {
            let kind = MedutAudioSession.kind(of: input)
            let id = MedutAudioSession.routeID(input, sawBluetooth: &sawBluetooth)
            if routes.contains(where: { $0.id == id }) { continue }
            routes.append(AudioRoute(id: id, name: kind == .earpiece ? "" : input.portName,
                                     kind: kind, isActive: kind == active))
        }
        routes.append(AudioRoute(id: "speaker", name: "", kind: .speaker,
                                 isActive: active == .speaker))
        return routes
    }

    /// Route call audio to `id` (from `availableRoutes()`).  An unknown id
    /// selects the system default.  Ignored while `routingIsExternal`.
    public func selectRoute(_ id: String) throws {
        guard !routingIsExternal else { return }
        let session = AVAudioSession.sharedInstance()
        if id == "speaker" {
            try session.overrideOutputAudioPort(.speaker)
            return
        }
        // Clear the speaker override first: while it is in force,
        // setPreferredInput is ignored.
        try session.overrideOutputAudioPort(.none)
        var sawBluetooth = false
        for input in session.availableInputs ?? []
            where MedutAudioSession.routeID(input, sawBluetooth: &sawBluetooth) == id {
            try session.setPreferredInput(input)
            return
        }
        try session.setPreferredInput(nil)
    }

    public func setSpeakerphone(_ on: Bool) throws {
        guard !routingIsExternal else { return }
        try AVAudioSession.sharedInstance().overrideOutputAudioPort(on ? .speaker : .none)
    }

    private func activeKind() -> AudioRoute.Kind {
        switch AVAudioSession.sharedInstance().currentRoute.outputs.first?.portType {
        case .some(.builtInSpeaker): return .speaker
        case .some(.builtInReceiver), .none: return .earpiece
        case .some(.bluetoothHFP), .some(.bluetoothA2DP), .some(.bluetoothLE): return .bluetooth
        default: return .wiredHeadset
        }
    }

    private static func kind(of port: AVAudioSessionPortDescription) -> AudioRoute.Kind {
        switch port.portType {
        case .builtInMic: return .earpiece
        case .bluetoothHFP, .bluetoothLE: return .bluetooth
        default: return .wiredHeadset // headsetMic, usbAudio, carAudio, ...
        }
    }

    /// A Bluetooth device's output UID differs from its input UID, so
    /// matching is always on these ids, never on raw port UIDs.
    private static func routeID(_ input: AVAudioSessionPortDescription,
                                sawBluetooth: inout Bool) -> String {
        switch kind(of: input) {
        case .earpiece: return "earpiece"
        case .bluetooth:
            if sawBluetooth { return "bt:\(input.uid)" }
            sawBluetooth = true
            return "bluetooth"
        default: return "wired-headset"
        }
    }

    // ── App-owned audio device ───────────────────────────────────────────

    /// Take the microphone and speaker over from the SDK: a VoiceProcessingIO
    /// unit (MedutAudioEngine) feeds the stack instead of its own AudioUnit.
    /// The unit starts only once the session is active, so under CallKit it
    /// never opens before `provider(_:didActivate:)`, the race the SDK's own
    /// device cannot avoid.
    public func useAppOwnedAudio(_ enable: Bool) throws {
        try Medut.shared.audio.useExternalDevice(enable)
        queue.sync {
            appOwnedAudio = enable
            if enable {
                engine.arm()
                if sessionActive { engine.sessionActivated() }
            } else {
                engine.disarm()
            }
        }
    }

    /// Diagnostics for the app-owned device: armed, sessionActive, running,
    /// sampleRate, channels, ptimeMs, lastError.
    public var appOwnedAudioStatus: [String: Any] {
        queue.sync { appOwnedAudio ? engine.status() : [:] }
    }

    // ── CallKit (AudioSessionPolicy.callKit) ─────────────────────────────

    /// Forward `provider(_:didActivate:)`.  CXProvider owns activation, so
    /// this is the only way the SDK learns the session is usable.
    public func callKitDidActivate() {
        queue.async {
            self.sessionActive = true
            if self.appOwnedAudio { self.engine.sessionActivated() }
        }
    }

    /// Forward `provider(_:didDeactivate:)`.
    public func callKitDidDeactivate() {
        queue.async {
            self.sessionActive = false
            if self.appOwnedAudio { self.engine.sessionDeactivated() }
        }
    }

    // ── Activation (AudioSessionPolicy.automatic) ────────────────────────

    /// A call is being placed, answered or is live: make sure the session is
    /// active before media opens.  Blocks until it is.
    func callStarting() {
        queue.sync {
            guard !activatedBySDK else { return }
            activatedBySDK = setActive(true)
        }
    }

    /// A call is live (a call-state event): same as `callStarting()` without
    /// blocking, for the SDK's event thread.
    func callLive() {
        queue.async {
            guard !self.activatedBySDK else { return }
            self.activatedBySDK = self.setActive(true)
        }
    }

    /// The last call ended: hand the audio route back to other apps, unless a
    /// new call started in the meantime.
    func callsEnded() {
        queue.async {
            guard self.activatedBySDK, Medut.shared.calls.isEmpty else { return }
            self.activatedBySDK = false
            self.setActive(false)
        }
    }

    /// On `queue`.  The engine is told only after activation succeeded, and
    /// before deactivation: starting a unit against an inactive session is
    /// the ordering bug owning the device exists to remove.
    @discardableResult
    private func setActive(_ active: Bool) -> Bool {
        if !active {
            sessionActive = false
            if appOwnedAudio { engine.sessionDeactivated() }
        }
        do {
            try AVAudioSession.sharedInstance().setActive(
                active, options: active ? [] : .notifyOthersOnDeactivation)
            if active {
                sessionActive = true
                if appOwnedAudio { engine.sessionActivated() }
            }
            return true
        } catch {
            NSLog("Medut: AVAudioSession setActive(%@) failed: %@", active ? "true" : "false",
                  error.localizedDescription)
            return false
        }
    }
}
#endif
