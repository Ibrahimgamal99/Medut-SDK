import Foundation
import Medut

/// Who activates the AVAudioSession around calls.
public enum AudioSessionPolicy: Sendable {
    /// The SDK activates the session when a call is placed or answered, and
    /// deactivates it when the last call ends.  For apps without CallKit.
    case automatic
    /// CallKit owns activation.  `start` sets `platformAudioActivate` to
    /// false, and the SDK never calls `setActive`.  Answer the SIP call from
    /// `provider(_:didActivate:)`, not from `CXAnswerCallAction`, so media
    /// opens against an active session.
    case callKit
    /// The app activates and deactivates the session itself.
    case manual
}

/// How `start` wires the stack into the app.
public struct StartOptions: Sendable {
    /// Drive network handover from NWPathMonitor.  Sets
    /// `netMonitorIntervalSeconds` to 0, since the built-in poller is only a
    /// fallback for platforms without a connectivity callback.
    public var monitorNetwork = true
    public var audioSession: AudioSessionPolicy = .automatic
    #if os(iOS)
    /// Report every call to CallKit through the SDK's own provider
    /// (`MedutCallKit`).  Forces `audioSession` to `.callKit`.
    public var callKit: CallKitOptions?
    #endif
    /// On a fresh start, re-create and register every account in
    /// `Medut.shared.accountStore`, so a process started by a push is
    /// registered before any UI exists.
    public var restoreAccounts = true
    /// Write the stack's log and call events to the unified log (subsystem
    /// `dev.medut`), so a process running with no UI still leaves a trace.
    public var logToSystemLog = true

    public init() {}
}

/// The Medut stack.  One per process, owned by the native layer for the
/// process lifetime: start it at launch or from a push, and leave it running.
///
/// Thread-safe: every method can be called from any thread.  Events arrive on
/// the queues you subscribe with (main by default).
public final class Medut: @unchecked Sendable {
    public static let shared = Medut()

    /// Library version string.
    public static var version: String { String(cString: medut_version()) }

    public let audio = MedutAudio()
    public let network = MedutNetwork()
    /// Accounts kept across process restarts (see `AccountStore`).
    public let accountStore = AccountStore()

    let gate = StackGate()
    /// Serialises start and shutdown against each other.
    private let lifecycle = NSLock()
    /// Guards everything below.
    private let lock = NSLock()
    private var accountsByHandle: [OpaquePointer: Account] = [:]
    private var callsByHandle: [OpaquePointer: Call] = [:]
    private var subscribers: [UUID: Subscriber] = [:]
    private var _reattached = false
    private var _options = StartOptions()

    #if canImport(Network)
    private var networkMonitor: NetworkMonitor?
    #endif

    private init() {}

    // ── Lifecycle ────────────────────────────────────────────────────────

    public var isRunning: Bool { gate.isRunning }

    /// True when `start` found the stack already running in this process
    /// (started by another consumer) and took it over instead of starting it.
    public var reattached: Bool { lock.locked { _reattached } }

    var options: StartOptions { lock.locked { _options } }

    /// Start the stack.  Throws `.already` if this instance is running.
    ///
    /// If something else in the process already started the native stack,
    /// this takes over its event delivery and adopts its live accounts and
    /// calls rather than failing; `reattached` is then true.
    public func start(_ config: MedutConfig = MedutConfig(),
                      options: StartOptions = StartOptions()) throws {
        try lifecycle.locked {
            var config = config
            var options = options
            #if os(iOS)
            if options.callKit != nil { options.audioSession = .callKit }
            #endif
            if options.monitorNetwork { config.netMonitorIntervalSeconds = 0 }
            if options.audioSession == .callKit { config.platformAudioActivate = false }
            // Before init: events start flowing during medut_init() and the
            // decoder reads the audio-session policy.
            lock.locked { _options = options }

            let reattached: Bool = try gate.write { state in
                if state.running {
                    throw MedutError(rawValue: MEDUT_ERR_ALREADY.rawValue, operation: "start")
                }
                state.advanceGeneration()
                let rc = config.withCConfig { c -> Int32 in
                    c.event_cb = medutEventCallback
                    c.event_userdata = nil
                    c.deliver_owned_events = true
                    return medut_init(&c)
                }
                var reattached = false
                if rc == MEDUT_ERR_ALREADY.rawValue {
                    try check(medut_set_event_handler(medutEventCallback, nil, true),
                              "medut_set_event_handler")
                    reattached = true
                } else {
                    try check(rc, "medut_init")
                }
                state.running = true
                return reattached
            }

            lock.locked { _reattached = reattached }
            if reattached { adoptLiveState() }

            #if canImport(Network)
            if options.monitorNetwork {
                let monitor = NetworkMonitor(reportInitialPath: reattached) { [weak self] in
                    try? self?.network.changed()
                }
                monitor.start()
                lock.locked { networkMonitor = monitor }
            }
            #endif

            if options.logToSystemLog { SystemLog.attach() }
            #if os(iOS)
            if let callKit = options.callKit { MedutCallKit.shared.enable(callKit) }
            #endif
            if options.restoreAccounts && !reattached { accountStore.restore() }
        }
    }

    /// Stop the stack: ends every call, unregisters and frees every account,
    /// and joins the SDK's threads.  Accounts and calls from before this call
    /// stop working; their methods throw `.notRunning`.
    public func shutdown() {
        lifecycle.locked {
            #if os(iOS)
            MedutCallKit.shared.disable()
            #endif
            #if canImport(Network)
            let monitor: NetworkMonitor? = lock.locked {
                defer { networkMonitor = nil }
                return networkMonitor
            }
            monitor?.stop()
            #endif

            let wasRunning: Bool = gate.write { state in
                guard state.running else { return false }
                state.running = false
                state.advanceGeneration()
                medut_shutdown()
                return true
            }
            guard wasRunning else { return }

            let policy = lock.locked { () -> AudioSessionPolicy in
                accountsByHandle.removeAll()
                callsByHandle.removeAll()
                _reattached = false
                return _options.audioSession
            }
            #if os(iOS)
            if policy == .automatic { MedutAudioSession.shared.callsEnded() }
            #else
            _ = policy
            #endif
        }
    }

    // ── Accounts and calls ───────────────────────────────────────────────

    /// Live accounts.
    public var accounts: [Account] { lock.locked { Array(accountsByHandle.values) } }

    /// Live calls (not yet ended).
    public var calls: [Call] { lock.locked { Array(callsByHandle.values) } }

    /// Create an account.  It does not register until `register()`.
    public func createAccount(_ config: AccountConfig) throws -> Account {
        let account: Account = try gate.read(nil, "medut_account_create") {
            var out: OpaquePointer?
            try config.withCConfig { c in
                try check(medut_account_create(c, &out), "medut_account_create")
            }
            guard let handle = out else {
                throw MedutError(rawValue: MEDUT_ERR_NOMEM.rawValue, operation: "medut_account_create")
            }
            return track(account: handle)
        }
        do {
            for (name, value) in config.headers.sorted(by: { $0.key < $1.key }) {
                try account.addHeader(name: name, value: value)
            }
            for (name, value) in config.registerHeaders.sorted(by: { $0.key < $1.key }) {
                try account.addRegisterHeader(name: name, value: value)
            }
        } catch {
            account.destroy()
            throw error
        }
        return account
    }

    /// The live account with this AOR ("sip:user@host"), if any.
    public func account(aor: String) -> Account? {
        lock.locked { accountsByHandle.values.first { $0.aor == aor } }
    }

    // ── Packet capture ───────────────────────────────────────────────────

    /// Capture SIP and RTP to a Wireshark-readable pcap file.
    public func startPcap(path: String) throws {
        try gate.read(nil, "medut_pcap_start") {
            try check(medut_pcap_start(path), "medut_pcap_start")
        }
    }

    public func stopPcap() throws {
        try gate.read(nil, "medut_pcap_stop") {
            try check(medut_pcap_stop(), "medut_pcap_stop")
        }
    }

    // ── Events ───────────────────────────────────────────────────────────

    /// Call `handler` with every event, asynchronously on `queue`.  Events
    /// stay in order.  Keep the returned subscription; cancel it to stop.
    @discardableResult
    public func addEventHandler(queue: DispatchQueue = .main,
                                _ handler: @escaping @Sendable (MedutEvent) -> Void) -> MedutSubscription {
        let id = UUID()
        lock.locked { subscribers[id] = .handler(queue, handler) }
        return MedutSubscription { [weak self] in self?.removeSubscriber(id) }
    }

    /// Every event, as an async sequence.  Ends when the consuming task is
    /// cancelled.
    public func events(bufferingPolicy: AsyncStream<MedutEvent>.Continuation.BufferingPolicy = .unbounded)
        -> AsyncStream<MedutEvent> {
        AsyncStream(bufferingPolicy: bufferingPolicy) { continuation in
            let id = UUID()
            self.lock.locked { self.subscribers[id] = .stream(continuation) }
            continuation.onTermination = { [weak self] _ in self?.removeSubscriber(id) }
        }
    }

    private func removeSubscriber(_ id: UUID) {
        lock.locked { _ = subscribers.removeValue(forKey: id) }
    }

    // ── Internal: registry ───────────────────────────────────────────────

    /// The wrapper for a handle, creating it on first sight.
    func track(account handle: OpaquePointer) -> Account {
        lock.locked {
            if let a = accountsByHandle[handle] { return a }
            let a = Account(handle: handle, generation: gate.currentGeneration)
            accountsByHandle[handle] = a
            return a
        }
    }

    func untrack(account: Account) {
        lock.locked {
            if accountsByHandle[account.handle] === account {
                accountsByHandle.removeValue(forKey: account.handle)
            }
        }
    }

    /// Lookup only.  An account handle in an event can belong to an account
    /// destroyed since the event was queued, so it is never adopted from there.
    func account(for handle: OpaquePointer?) -> Account? {
        guard let handle = handle else { return nil }
        return lock.locked { accountsByHandle[handle] }
    }

    func existingCall(_ handle: OpaquePointer) -> Call? {
        lock.locked { callsByHandle[handle] }
    }

    /// The wrapper for a live call handle, creating it on first sight.  The
    /// handle must be valid now: being delivered in an event, or just returned
    /// by C.
    func track(call handle: OpaquePointer, account: Account?, state: CallState,
               isOutgoing: Bool, remoteURI: String, remoteDisplayName: String?) -> Call {
        lock.locked {
            if let c = callsByHandle[handle] { return c }
            let c = Call(handle: handle, generation: gate.currentGeneration, account: account,
                         state: state, isOutgoing: isOutgoing, remoteURI: remoteURI,
                         remoteDisplayName: remoteDisplayName)
            callsByHandle[handle] = c
            return c
        }
    }

    /// Track a call first seen in an event that says nothing else about it.
    func track(call handle: OpaquePointer) -> Call {
        if let c = existingCall(handle) { return c }
        return track(call: handle, account: account(for: medut_call_get_account(handle)),
                     state: CallState(c: medut_call_get_state(handle), fallback: .calling),
                     isOutgoing: false, remoteURI: "", remoteDisplayName: nil)
    }

    /// Drop a call that ended.  Returns true when no live calls remain.
    func untrack(call handle: OpaquePointer) -> Bool {
        lock.locked {
            callsByHandle.removeValue(forKey: handle)
            return callsByHandle.isEmpty
        }
    }

    /// Re-derive wrappers for accounts and calls another consumer created.
    private func adoptLiveState() {
        var accountHandles: [OpaquePointer] = []
        withUnsafeMutablePointer(to: &accountHandles) { p in
            medut_account_foreach({ h, arg in
                guard let h = h else { return }
                arg?.assumingMemoryBound(to: [OpaquePointer].self).pointee.append(h)
            }, p)
        }
        for h in accountHandles { _ = track(account: h) }

        var callHandles: [OpaquePointer] = []
        withUnsafeMutablePointer(to: &callHandles) { p in
            medut_call_foreach({ h, arg in
                guard let h = h else { return }
                arg?.assumingMemoryBound(to: [OpaquePointer].self).pointee.append(h)
            }, p)
        }
        for h in callHandles {
            var info = medut_call_info_t()
            let haveInfo = medut_call_get_info(h, &info) == 0
            _ = track(call: h, account: account(for: medut_call_get_account(h)),
                      state: CallState(c: medut_call_get_state(h), fallback: .calling),
                      isOutgoing: haveInfo && info.is_outgoing,
                      remoteURI: haveInfo ? fixedCString(info.peer_uri) : "",
                      remoteDisplayName: haveInfo ? fixedCString(info.peer_display_name) : nil)
        }
    }

    // ── Internal: event delivery ─────────────────────────────────────────

    private enum Subscriber {
        case handler(DispatchQueue, @Sendable (MedutEvent) -> Void)
        case stream(AsyncStream<MedutEvent>.Continuation)
    }

    /// Runs on the SDK's event thread for every event.  Must not take the
    /// gate or make blocking SDK calls: shutdown joins this thread.
    fileprivate func deliver(_ ev: UnsafePointer<medut_event_t>) {
        var retained = false
        let event = decode(ev, retained: &retained)
        if !retained { medut_event_release(ev) }

        let targets = lock.locked { Array(subscribers.values) }
        for target in targets {
            switch target {
            case .handler(let queue, let handler):
                queue.async { handler(event) }
            case .stream(let continuation):
                continuation.yield(event)
            }
        }
    }
}

/// A live event subscription.  Cancel it to stop receiving events.
public final class MedutSubscription: @unchecked Sendable {
    private let lock = NSLock()
    private var onCancel: (() -> Void)?

    init(_ onCancel: @escaping () -> Void) { self.onCancel = onCancel }

    public func cancel() {
        let f: (() -> Void)? = lock.locked {
            defer { onCancel = nil }
            return onCancel
        }
        f?()
    }
}

/// C event sink.  Events are delivered in owned mode: each must be released
/// with medut_event_release(), except a terminal call-state event, which the
/// ended `Call` keeps to hold its native wrapper alive (see Call).
private func medutEventCallback(_ ev: UnsafePointer<medut_event_t>?,
                                _ userdata: UnsafeMutableRawPointer?) {
    guard let ev = ev else { return }
    Medut.shared.deliver(ev)
}
