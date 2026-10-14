#if os(iOS) && canImport(CallKit)
import AVFoundation
import CallKit
import Foundation
import UIKit

/// Settings for the SDK's CallKit integration (`StartOptions.callKit`).
public struct CallKitOptions: Sendable {
    /// The name the system call UI shows; nil uses the app name.
    public var localizedName: String?
    /// Asset name of a template image for the system call UI.
    public var iconTemplateImageName: String?
    /// Bundle ringtone file name; nil uses the system ringtone.
    public var ringtoneSound: String?
    public var includesCallsInRecents = true
    public var maximumCallGroups = 1
    public var maximumCallsPerCallGroup = 1

    public init() {}
}

/// CallKit integration: every SIP call is also a CallKit call, so the system
/// knows about it.  Calls show in the native call UI and on the lock screen,
/// a cellular call and a SIP call never fight for audio, CallKit owns the
/// audio session, and a push can ring before the INVITE arrives.
///
/// Turn it on with `StartOptions.callKit` in `Medut.shared.start`.  Nothing
/// else changes: `account.call()` places a call and `call.answer()` answers
/// one, and the SDK routes them through CallKit's actions so the system UI
/// stays in step.  Answering, holding, muting and ending from the system side
/// drive the SIP call the same way.
///
/// The Kotlin SDK's Telecom integration is the same design.
public final class MedutCallKit: NSObject, @unchecked Sendable, CXProviderDelegate {
    public static let shared = MedutCallKit()

    public var isEnabled: Bool { lock.locked { provider != nil } }

    /// CallKit refused to start an outgoing call (another call has the
    /// line).  The SIP call has been hung up by the time this runs.  Main
    /// queue.
    public var onOutgoingCallRefused: ((Call, Error) -> Void)?

    /// A push announced a call but no INVITE arrived in time; the placeholder
    /// CallKit call was ended as unanswered.  Main queue.
    public var onPushCallTimedOut: (() -> Void)?

    /// Decide on an incoming call before CallKit hears of it: return a SIP
    /// status to reject it with (486 for "two calls already up", 603 for a
    /// blocked number or Do Not Disturb), or nil to ring.  A refused call
    /// never reaches the system UI; a push placeholder it would have taken
    /// over ends quietly (declined elsewhere).  Called on the SDK's event
    /// thread, before the incoming-call event is delivered; keep it quick.
    public var incomingCallFilter: ((Call) -> UInt16?)?

    private let lock = NSLock()
    private var provider: CXProvider?
    private let controller = CXCallController()
    private let observer = CXCallObserver()
    private var subscription: MedutSubscription?
    private var options = CallKitOptions()

    // Guarded by `lock`.
    private var calls: [UUID: Call] = [:]
    private var pendingAnswers: [UUID] = []
    private var pendingRejects: [UUID: UInt16] = [:]
    private var endedByAction: Set<UUID> = []
    private var placeholders: [Placeholder] = []
    private var filtered: Set<UUID> = []
    private var sessionActive = false

    /// A CallKit call reported from a push before its INVITE arrived.
    private struct Placeholder {
        let uuid: UUID
        var answerRequested = false
        var endRequested = false
        var expiry: DispatchWorkItem?
    }

    private override init() {
        super.init()
    }

    // ── Lifecycle (from Medut.start / shutdown) ──────────────────────────

    func enable(_ options: CallKitOptions) {
        let configuration = CXProviderConfiguration(
            localizedName: options.localizedName
                ?? (Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String)
                ?? (Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String)
                ?? "Medut")
        configuration.supportsVideo = false
        configuration.maximumCallGroups = options.maximumCallGroups
        configuration.maximumCallsPerCallGroup = options.maximumCallsPerCallGroup
        configuration.supportedHandleTypes = [.generic, .phoneNumber]
        configuration.includesCallsInRecents = options.includesCallsInRecents
        if let name = options.iconTemplateImageName {
            configuration.iconTemplateImageData = UIImage(named: name)?.pngData()
        }
        configuration.ringtoneSound = options.ringtoneSound

        let provider = CXProvider(configuration: configuration)
        provider.setDelegate(self, queue: nil)
        lock.locked {
            self.options = options
            self.provider = provider
        }
        subscription = Medut.shared.addEventHandler(queue: .main) { [weak self] event in
            self?.handle(event)
        }
        // A stack that was already running (reattached) may be ringing.
        for call in Medut.shared.calls where !call.isOutgoing && call.state == .ringing {
            if let code = incomingCallFilter?(call) {
                try? call.rejectNow(statusCode: code, reason: nil)
                continue
            }
            reportIncoming(call)
        }
    }

    func disable() {
        subscription?.cancel()
        subscription = nil
        let (provider, managed, pending): (CXProvider?, [UUID], [Placeholder]) = lock.locked {
            defer {
                self.provider = nil
                calls.removeAll()
                pendingAnswers.removeAll()
                pendingRejects.removeAll()
                endedByAction.removeAll()
                placeholders.removeAll()
                filtered.removeAll()
            }
            return (self.provider, Array(calls.keys), placeholders)
        }
        guard let provider = provider else { return }
        for uuid in managed { provider.reportCall(with: uuid, endedAt: Date(), reason: .failed) }
        for ph in pending {
            ph.expiry?.cancel()
            provider.reportCall(with: ph.uuid, endedAt: Date(), reason: .failed)
        }
        provider.invalidate()
    }

    /// True when this call is a CallKit call, so app-initiated actions on it
    /// must go through CallKit.
    func manages(_ call: Call) -> Bool {
        lock.locked { calls[call.id] === call }
    }

    // ── SDK -> CallKit ───────────────────────────────────────────────────

    private func handle(_ event: MedutEvent) {
        switch event {
        case .incomingCall(let e):
            reportIncoming(e.call)
        case .callState(let e):
            sync(e)
        default:
            break
        }
    }

    /// Called on the SDK's event thread, before the incoming-call event is
    /// delivered: a push placeholder waiting for this INVITE lends the call
    /// its CallKit identity, so every subscriber sees the final id.  Takes
    /// only this object's lock.
    func bindIncoming(_ call: Call) {
        guard isEnabled else { return }
        if let code = incomingCallFilter?(call) {
            refuse(call, code: code)
            return
        }
        let bound: Placeholder? = lock.locked {
            guard provider != nil, !placeholders.isEmpty else { return nil }
            let ph = placeholders.removeFirst()
            call.adopt(id: ph.uuid)
            calls[ph.uuid] = call
            return ph
        }
        guard let ph = bound else { return }
        ph.expiry?.cancel()
        DispatchQueue.main.async { [self] in
            guard let provider = lock.locked({ self.provider }) else { return }
            if ph.endRequested {
                // Declined from the placeholder: the system call is already
                // over, only the INVITE is left to answer.
                lock.locked { _ = endedByAction.insert(ph.uuid) }
                try? call.rejectNow(statusCode: 603, reason: "Decline")
                return
            }
            provider.reportCall(with: ph.uuid, updated: update(for: call))
            if ph.answerRequested { answerWhenActive(call) }
        }
    }

    /// The filter turned `call` away: reject it, and end quietly the push
    /// placeholder that would have become it, so nothing rings.
    private func refuse(_ call: Call, code: UInt16) {
        let ph: Placeholder? = lock.locked {
            _ = filtered.insert(call.id)
            // The one bindIncoming would have given this INVITE.
            guard !placeholders.isEmpty else { return nil }
            return placeholders.removeFirst()
        }
        ph?.expiry?.cancel()
        DispatchQueue.main.async { [self] in
            try? call.rejectNow(statusCode: code, reason: nil)
            // A declined placeholder is already over in CallKit.
            if let ph = ph, !ph.endRequested, let provider = lock.locked({ self.provider }) {
                provider.reportCall(with: ph.uuid, endedAt: Date(), reason: .declinedElsewhere)
            }
        }
    }

    private func reportIncoming(_ call: Call) {
        let (provider, alreadyManaged): (CXProvider?, Bool) = lock.locked {
            if filtered.remove(call.id) != nil { return (nil, true) }
            if calls[call.id] === call { return (self.provider, true) }
            calls[call.id] = call
            return (self.provider, false)
        }
        guard let provider = provider, !alreadyManaged else { return }
        provider.reportNewIncomingCall(with: call.id, update: update(for: call)) { [weak self] error in
            guard let error = error else { return }
            // Do Not Disturb, or a call already in the group: the system
            // will not show it, so the far end gets busy.
            NSLog("Medut: CallKit refused the incoming call: %@", error.localizedDescription)
            self?.lock.locked { _ = self?.calls.removeValue(forKey: call.id) }
            try? call.rejectNow(statusCode: 486, reason: "Busy Here")
        }
    }

    /// A push announced a call.  Reports a CallKit call now, as iOS requires
    /// on every VoIP push, and waits up to `timeout` for the INVITE to bind
    /// to it.  `completion` runs once CallKit has the call (or refused it).
    func reportPushIncoming(callerName: String?, callerURI: String?, timeout: TimeInterval,
                            completion: @escaping (Error?) -> Void) {
        guard let provider = lock.locked({ self.provider }) else {
            completion(MedutError(.invalidState, operation: "CallKit is not enabled"))
            return
        }
        let uuid = UUID()
        let update = CXCallUpdate()
        update.remoteHandle = handle(for: callerURI ?? "")
        update.localizedCallerName = callerName ?? callerURI.map(MedutCallKit.userPart) ?? "Incoming call"
        update.supportsHolding = true
        update.supportsGrouping = false
        update.supportsUngrouping = false
        update.supportsDTMF = true
        update.hasVideo = false

        let expiry = DispatchWorkItem { [weak self] in self?.expirePlaceholder(uuid) }
        lock.locked {
            placeholders.append(Placeholder(uuid: uuid, expiry: expiry))
        }
        provider.reportNewIncomingCall(with: uuid, update: update) { [weak self] error in
            if let error = error {
                self?.lock.locked { self?.placeholders.removeAll { $0.uuid == uuid } }
                expiry.cancel()
            } else {
                DispatchQueue.main.asyncAfter(deadline: .now() + timeout, execute: expiry)
            }
            completion(error)
        }
    }

    private func expirePlaceholder(_ uuid: UUID) {
        let (provider, removed): (CXProvider?, Bool) = lock.locked {
            let before = placeholders.count
            placeholders.removeAll { $0.uuid == uuid }
            return (self.provider, placeholders.count != before)
        }
        guard removed, let provider = provider else { return }
        provider.reportCall(with: uuid, endedAt: Date(), reason: .unanswered)
        onPushCallTimedOut?()
    }

    /// Before an outgoing INVITE: refuse when another app's call has the
    /// line, which is what CallKit would do a moment later.
    func checkOutgoingPermitted() throws {
        let foreign = lock.locked {
            observer.calls.contains { !$0.hasEnded && calls[$0.uuid] == nil }
        }
        if foreign {
            throw MedutError(.invalidState,
                             operation: "CallKit refuses a new outgoing call (another call is up)")
        }
    }

    /// The INVITE is out; ask CallKit for the call.  Any thread.
    func reportOutgoing(_ call: Call) {
        lock.locked { calls[call.id] = call }
        let action = CXStartCallAction(call: call.id, handle: handle(for: call.remoteURI))
        action.contactIdentifier = nil
        controller.request(CXTransaction(action: action)) { [weak self] error in
            guard let error = error, let self = self else { return }
            self.lock.locked { _ = self.calls.removeValue(forKey: call.id) }
            try? call.hangupNow()
            DispatchQueue.main.async { self.onOutgoingCallRefused?(call, error) }
        }
    }

    private func sync(_ e: CallStateEvent) {
        let call = e.call
        let (provider, managed): (CXProvider?, Bool) = lock.locked {
            (self.provider, calls[call.id] === call)
        }
        guard let provider = provider, managed else { return }
        switch e.state {
        case .established:
            if call.isOutgoing { provider.reportOutgoingCall(with: call.id, connectedAt: Date()) }
        case .ended, .cancelled, .failed:
            let byAction: Bool = lock.locked {
                _ = calls.removeValue(forKey: call.id)
                _ = pendingRejects.removeValue(forKey: call.id)
                pendingAnswers.removeAll { $0 == call.id }
                return endedByAction.remove(call.id) != nil
            }
            // A call ended through a CXEndCallAction is already gone from
            // CallKit's point of view.
            guard !byAction else { return }
            let reason: CXCallEndedReason
            if call.endRequestedLocally {
                reason = .remoteEnded // local end outside CallKit: nothing better fits
            } else if e.state == .cancelled && !call.isOutgoing {
                reason = .unanswered
            } else if e.state == .failed {
                reason = .failed
            } else {
                reason = .remoteEnded
            }
            provider.reportCall(with: call.id, endedAt: Date(), reason: reason)
        default:
            break
        }
    }

    // ── App-initiated actions (from Call) ────────────────────────────────

    func requestAnswer(_ call: Call) {
        request(CXAnswerCallAction(call: call.id)) { try call.answerNow() }
    }

    func requestEnd(_ call: Call, rejectCode: UInt16?) {
        lock.locked { pendingRejects[call.id] = rejectCode }
        request(CXEndCallAction(call: call.id)) {
            self.lock.locked { _ = self.pendingRejects.removeValue(forKey: call.id) }
            try self.end(call, rejectCode: rejectCode)
        }
    }

    func requestHeld(_ call: Call, onHold: Bool) {
        request(CXSetHeldCallAction(call: call.id, onHold: onHold)) {
            try onHold ? call.holdNow() : call.resumeNow()
        }
    }

    func requestMuted(_ call: Call, muted: Bool) {
        request(CXSetMutedCallAction(call: call.id, muted: muted)) { try call.setMutedNow(muted) }
    }

    /// Send an action; if CallKit refuses it, do the SIP side directly so
    /// the call never gets stuck on a system that will not cooperate.
    private func request(_ action: CXAction, fallback: @escaping () throws -> Void) {
        controller.request(CXTransaction(action: action)) { error in
            guard let error = error else { return }
            NSLog("Medut: CallKit refused %@: %@", String(describing: type(of: action)),
                  error.localizedDescription)
            try? fallback()
        }
    }

    private func end(_ call: Call, rejectCode: UInt16?) throws {
        lock.locked { _ = endedByAction.insert(call.id) }
        if call.isEnded { return }
        if let code = rejectCode {
            try call.rejectNow(statusCode: code, reason: nil)
        } else if !call.isOutgoing && call.state == .ringing {
            try call.rejectNow(statusCode: 603, reason: "Decline")
        } else {
            try call.hangupNow()
        }
    }

    /// Answer once the audio session is active: CallKit activates it after
    /// the answer action, and media opened against an inactive session is
    /// the race this integration exists to remove.
    private func answerWhenActive(_ call: Call) {
        let now: Bool = lock.locked {
            if sessionActive { return true }
            pendingAnswers.append(call.id)
            return false
        }
        if now { try? call.answerNow() }
    }

    // ── CXProviderDelegate (main queue) ──────────────────────────────────

    public func providerDidReset(_ provider: CXProvider) {
        let managed: [Call] = lock.locked {
            defer {
                calls.removeAll()
                pendingAnswers.removeAll()
                pendingRejects.removeAll()
                endedByAction.removeAll()
                placeholders.removeAll()
                filtered.removeAll()
            }
            return Array(calls.values)
        }
        for call in managed { try? call.hangupNow() }
    }

    public func provider(_ provider: CXProvider, perform action: CXStartCallAction) {
        guard lock.locked({ calls[action.callUUID] }) != nil else {
            action.fail()
            return
        }
        provider.reportOutgoingCall(with: action.callUUID, startedConnectingAt: Date())
        action.fulfill()
    }

    public func provider(_ provider: CXProvider, perform action: CXAnswerCallAction) {
        if let call = lock.locked({ calls[action.callUUID] }) {
            answerWhenActive(call)
            action.fulfill()
            return
        }
        // Answered from the push placeholder before the INVITE arrived.
        let found: Bool = lock.locked {
            guard let i = placeholders.firstIndex(where: { $0.uuid == action.callUUID }) else {
                return false
            }
            placeholders[i].answerRequested = true
            return true
        }
        found ? action.fulfill() : action.fail()
    }

    public func provider(_ provider: CXProvider, perform action: CXEndCallAction) {
        if let call = lock.locked({ calls[action.callUUID] }) {
            let code = lock.locked { pendingRejects.removeValue(forKey: action.callUUID) }
            try? end(call, rejectCode: code)
            action.fulfill()
            return
        }
        // Declined before the INVITE arrived: the system call is over, but
        // the INVITE still has to be answered with a decline when it lands,
        // so the placeholder stays a while longer and then goes quietly.
        let uuid = action.callUUID
        let found: Bool = lock.locked {
            guard let i = placeholders.firstIndex(where: { $0.uuid == uuid }) else { return false }
            placeholders[i].expiry?.cancel()
            placeholders[i].endRequested = true
            let removal = DispatchWorkItem { [weak self] in
                self?.lock.locked { self?.placeholders.removeAll { $0.uuid == uuid } }
            }
            placeholders[i].expiry = removal
            DispatchQueue.main.asyncAfter(deadline: .now() + 30, execute: removal)
            return true
        }
        found ? action.fulfill() : action.fail()
    }

    public func provider(_ provider: CXProvider, perform action: CXSetHeldCallAction) {
        guard let call = lock.locked({ calls[action.callUUID] }) else {
            action.fail()
            return
        }
        do {
            try action.isOnHold ? call.holdNow() : call.resumeNow()
            action.fulfill()
        } catch {
            action.fail()
        }
    }

    public func provider(_ provider: CXProvider, perform action: CXSetMutedCallAction) {
        guard let call = lock.locked({ calls[action.callUUID] }) else {
            action.fail()
            return
        }
        try? call.setMutedNow(action.isMuted)
        action.fulfill()
    }

    public func provider(_ provider: CXProvider, perform action: CXPlayDTMFCallAction) {
        guard let call = lock.locked({ calls[action.callUUID] }) else {
            action.fail()
            return
        }
        for digit in action.digits { try? call.sendDTMF(digit) }
        action.fulfill()
    }

    public func provider(_ provider: CXProvider, timedOutPerforming action: CXAction) {
        action.fulfill()
    }

    public func provider(_ provider: CXProvider, didActivate audioSession: AVAudioSession) {
        let answers: [Call] = lock.locked {
            sessionActive = true
            defer { pendingAnswers.removeAll() }
            return pendingAnswers.compactMap { calls[$0] }
        }
        MedutAudioSession.shared.callKitDidActivate()
        for call in answers { try? call.answerNow() }
    }

    public func provider(_ provider: CXProvider, didDeactivate audioSession: AVAudioSession) {
        lock.locked { sessionActive = false }
        MedutAudioSession.shared.callKitDidDeactivate()
    }

    // ── Helpers ──────────────────────────────────────────────────────────

    private func update(for call: Call) -> CXCallUpdate {
        let update = CXCallUpdate()
        update.remoteHandle = handle(for: call.remoteURI)
        update.localizedCallerName = call.remoteDisplayName.flatMap { $0.isEmpty ? nil : $0 }
            ?? MedutCallKit.userPart(call.remoteURI)
        update.supportsHolding = true
        update.supportsGrouping = false
        update.supportsUngrouping = false
        update.supportsDTMF = true
        update.hasVideo = false
        return update
    }

    private func handle(for uri: String) -> CXHandle {
        let user = MedutCallKit.userPart(uri)
        let digits = user.filter { !"+*#".contains($0) }
        if !digits.isEmpty && digits.allSatisfy(\.isNumber) {
            return CXHandle(type: .phoneNumber, value: user)
        }
        var bare = uri
        for prefix in ["sips:", "sip:"] where bare.hasPrefix(prefix) {
            bare = String(bare.dropFirst(prefix.count))
        }
        return CXHandle(type: .generic, value: bare.isEmpty ? "unknown" : bare)
    }

    /// "sip:1003@echo.example.com;transport=wss" -> "1003".
    static func userPart(_ uri: String) -> String {
        var s = uri
        for prefix in ["sips:", "sip:"] where s.hasPrefix(prefix) {
            s = String(s.dropFirst(prefix.count))
        }
        if let at = s.firstIndex(of: "@") { s = String(s[..<at]) }
        return s.isEmpty ? uri : s
    }
}
#endif
