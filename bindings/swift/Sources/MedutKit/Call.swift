import Foundation
import Medut

/// One audio frame from a media tap.  `samples` is S16 interleaved and valid
/// only for the duration of the tap callback; copy it to keep it.
public struct MediaFrame {
    public let direction: MediaDirection
    public let samples: UnsafeBufferPointer<Int16>
    public let sampleRate: UInt32
    public let channels: UInt8
    public let timestampMicros: UInt64
}

/// A call, incoming or outgoing.
///
/// A `Call` stays safe to use for as long as you hold it, including after it
/// has ended: an ended call keeps its native wrapper alive until the `Call` is
/// released (it owns the terminal state event, which holds the last native
/// reference).  Don't keep ended calls around in bulk; keep a `CallInfo`.
///
/// Operations on an ended call throw `.callEnded`, except `hangup()`, which
/// is a no-op then.
public final class Call: @unchecked Sendable, Hashable, Identifiable, CustomStringConvertible {
    /// Stable identity for this call, e.g. the CallKit call UUID.
    public let id = UUID()
    public let account: Account?
    public let isOutgoing: Bool
    /// The far end: the dialled URI for an outgoing call, the From URI for an
    /// incoming one.
    public let remoteURI: String
    public let remoteDisplayName: String?

    let handle: OpaquePointer
    let generation: UInt64

    private let lock = NSLock()
    private var _state: CallState
    private var _endError: MedutError?
    private var _endReason: String?
    /// The terminal CALL_STATE event, kept unreleased: it owns the native
    /// wrapper's last reference, so `handle` stays valid until deinit.
    private var terminalEvent: UnsafePointer<medut_event_t>?
    /// Every tap ever installed.  The C side reads the tap pointer under a lock
    /// but invokes it outside, so a replaced tap can still be running; boxes
    /// are freed only with the call.
    private var taps: [TapBox] = []

    init(handle: OpaquePointer, generation: UInt64, account: Account?, state: CallState,
         isOutgoing: Bool, remoteURI: String, remoteDisplayName: String?) {
        self.handle = handle
        self.generation = generation
        self.account = account
        self._state = state
        self.isOutgoing = isOutgoing
        self.remoteURI = remoteURI
        self.remoteDisplayName = remoteDisplayName
    }

    deinit {
        if let ev = terminalEvent { medut_event_release(ev) }
    }

    public var description: String { "Call(\(remoteURI), \(state))" }

    public static func == (a: Call, b: Call) -> Bool { a === b }
    public func hash(into h: inout Hasher) { h.combine(ObjectIdentifier(self)) }

    // ── State ────────────────────────────────────────────────────────────

    /// The state from the latest call-state event.
    public var state: CallState { lock.locked { _state } }

    public var isEnded: Bool { state.isTerminal }

    /// Why the call ended, once it has: the error the terminal event carried
    /// (nil for a normal hangup) and the reason text.
    public var endError: MedutError? { lock.locked { _endError } }
    public var endReason: String? { lock.locked { _endReason } }

    func update(state: CallState) {
        lock.locked { _state = state }
    }

    /// Take over the terminal event.  Returns false if one is already held
    /// (the caller then releases it).
    func takeTerminalEvent(_ ev: UnsafePointer<medut_event_t>, state: CallState,
                           error: MedutError?, reason: String?) -> Bool {
        lock.locked {
            _state = state
            _endError = error
            _endReason = reason
            guard terminalEvent == nil else { return false }
            terminalEvent = ev
            return true
        }
    }

    // ── Control ──────────────────────────────────────────────────────────

    /// Answer an incoming call.
    ///
    /// With CallKit, call this from `provider(_:didActivate:)`, so media
    /// opens against an active audio session.
    public func answer() throws {
        #if os(iOS)
        if Medut.shared.options.audioSession == .automatic {
            MedutAudioSession.shared.callStarting()
        }
        #endif
        try perform("medut_call_answer") { medut_call_answer($0) }
    }

    /// End the call (BYE, or CANCEL while it is still being set up).
    /// A no-op on a call that already ended.
    public func hangup() throws {
        if isEnded { return }
        try perform("medut_call_hangup") { medut_call_hangup($0) }
    }

    /// End with a SIP status: for an unanswered incoming call this is the
    /// final response, e.g. 486 Busy Here or 603 Decline.
    public func reject(statusCode: UInt16 = 486, reason: String? = nil) throws {
        try perform("medut_call_reject") { medut_call_reject($0, statusCode, reason) }
    }

    public func hold() throws {
        try perform("medut_call_hold") { medut_call_hold($0) }
    }

    public func resume() throws {
        try perform("medut_call_resume") { medut_call_resume($0) }
    }

    /// True while the call is on hold, either side.
    public var isHeld: Bool {
        (try? read("medut_call_is_held") { medut_call_is_held($0) }) ?? false
    }

    /// Send one DTMF digit: 0-9, *, #, A-D.
    public func sendDTMF(_ digit: Character) throws {
        guard let ascii = digit.asciiValue, "0123456789*#ABCDabcd".contains(digit) else {
            throw MedutError(rawValue: MEDUT_ERR_INVAL.rawValue, operation: "medut_call_send_dtmf")
        }
        try perform("medut_call_send_dtmf") {
            medut_call_send_dtmf($0, CChar(truncatingIfNeeded: ascii))
        }
    }

    /// Add a header to later requests in this dialog (re-INVITE, BYE, REFER).
    public func addHeader(name: String, value: String) throws {
        try perform("medut_call_add_header") { medut_call_add_header($0, name, value) }
    }

    // ── Transfer ─────────────────────────────────────────────────────────

    /// Blind transfer (REFER).  If the far end refuses, a `.transferFailed`
    /// event arrives and this call stays up.
    public func transfer(to uri: String) throws {
        try perform("medut_call_transfer") { medut_call_transfer($0, uri) }
    }

    /// Attended transfer: REFER with Replaces, bridging this call to
    /// `consultation`, an established call to the transfer target.
    public func attendedTransfer(to consultation: Call) throws {
        guard consultation.generation == generation else {
            throw MedutError(.notRunning, operation: "medut_call_attended_transfer")
        }
        try perform("medut_call_attended_transfer") {
            medut_call_attended_transfer($0, consultation.handle)
        }
    }

    /// Follow an incoming REFER (`.transferRequest`): place the call to the
    /// target, linked so the transferor learns the outcome.  This call stays
    /// up; end it when the new one connects.
    public func acceptTransfer() throws -> Call {
        try read("medut_call_transfer_accept") { h in
            if isEnded { throw MedutError(.callEnded, operation: "medut_call_transfer_accept") }
            var out: OpaquePointer?
            try check(medut_call_transfer_accept(h, &out), "medut_call_transfer_accept")
            guard let ch = out else {
                throw MedutError(rawValue: MEDUT_ERR_NOMEM.rawValue,
                                 operation: "medut_call_transfer_accept")
            }
            return Medut.shared.track(call: ch, account: account, state: .calling,
                                      isOutgoing: true, remoteURI: "", remoteDisplayName: nil)
        }
    }

    /// Refuse an incoming REFER; this call stays up.  603 is "declined",
    /// 486 "busy".
    public func rejectTransfer(statusCode: UInt16 = 603, reason: String? = nil) throws {
        try perform("medut_call_transfer_reject") {
            medut_call_transfer_reject($0, statusCode, reason)
        }
    }

    // ── Audio ────────────────────────────────────────────────────────────

    /// Mute the microphone for this call.
    public func setMuted(_ muted: Bool) throws {
        try perform("medut_audio_mute") { medut_audio_mute($0, muted) }
    }

    public var isMuted: Bool {
        (try? read("medut_audio_is_muted") { medut_audio_is_muted($0) }) ?? false
    }

    /// Silence the far end's audio for this call.
    public func setReceiveMuted(_ muted: Bool) throws {
        try perform("medut_audio_mute_rx") { medut_audio_mute_rx($0, muted) }
    }

    /// Record both directions, mixed, to a WAV file.
    public func startRecording(to path: String) throws {
        try perform("medut_call_record_start") { medut_call_record_start($0, path) }
    }

    public func stopRecording() throws {
        try perform("medut_call_record_stop", allowEnded: true) { medut_call_record_stop($0) }
    }

    /// Receive every decoded (rx) and pre-encode (tx) audio frame.  `tap`
    /// runs on the audio thread: it must not block or allocate heavily.
    /// nil removes the tap.
    public func setMediaTap(_ tap: ((MediaFrame) -> Void)?) throws {
        guard let tap = tap else {
            try perform("medut_call_set_media_tap") { medut_call_set_media_tap($0, nil, nil) }
            return
        }
        let box = TapBox(tap)
        lock.locked { taps.append(box) }
        try perform("medut_call_set_media_tap") {
            medut_call_set_media_tap($0, mediaTapCallback, Unmanaged.passUnretained(box).toOpaque())
        }
    }

    // ── Media tuning ─────────────────────────────────────────────────────

    /// DSCP for this call's RTP, e.g. 46 (EF).
    public func setDSCP(_ dscp: UInt8) throws {
        try perform("medut_call_set_dscp_rtp") { medut_call_set_dscp_rtp($0, dscp) }
    }

    /// End the call after this long without inbound RTP; 0 disables.
    public func setRTPTimeout(seconds: UInt32) throws {
        try perform("medut_call_set_rtp_timeout") { medut_call_set_rtp_timeout($0, seconds) }
    }

    /// Encoder bit rate in bps; 0 returns to the negotiated rate.
    public func setBitrate(_ bps: UInt32) throws {
        try perform("medut_call_set_bitrate") { medut_call_set_bitrate($0, bps) }
    }

    // ── Observability ────────────────────────────────────────────────────

    public var iceState: ICEState {
        (try? read("medut_call_ice_state") {
            ICEState(c: medut_call_ice_state($0), fallback: .new)
        }) ?? .closed
    }

    /// Facts about the call: peer, Call-ID, direction, duration.  After the
    /// call ends only `state` and `duration` are still filled in.
    public func info() throws -> CallInfo {
        try read("medut_call_get_info") { h in
            var c = medut_call_info_t()
            try check(medut_call_get_info(h, &c), "medut_call_get_info")
            return CallInfo(c)
        }
    }

    /// Current media statistics.
    public func stats() throws -> MediaStats {
        try read("medut_call_get_stats") { h in
            var c = medut_ev_media_stats_t()
            try check(medut_call_get_stats(h, &c), "medut_call_get_stats")
            return MediaStats(c)
        }
    }

    // ── Internal ─────────────────────────────────────────────────────────

    /// Run `body` with the handle, stack up and on this call's generation.
    private func read<T>(_ operation: String, _ body: (OpaquePointer) throws -> T) throws -> T {
        try Medut.shared.gate.read(generation, operation) { try body(handle) }
    }

    private func perform(_ operation: String, allowEnded: Bool = false,
                         _ fn: (OpaquePointer) -> Int32) throws {
        try read(operation) { h in
            if !allowEnded && isEnded { throw MedutError(.callEnded, operation: operation) }
            try check(fn(h), operation)
        }
    }
}

private final class TapBox {
    let tap: (MediaFrame) -> Void
    init(_ tap: @escaping (MediaFrame) -> Void) { self.tap = tap }
}

private func mediaTapCallback(_ call: OpaquePointer?, _ dir: medut_media_dir_t,
                              _ pcm: UnsafePointer<Int16>?, _ samples: Int,
                              _ sampleRate: UInt32, _ channels: UInt8,
                              _ timestamp: UInt64, _ userdata: UnsafeMutableRawPointer?) {
    guard let userdata = userdata else { return }
    let box = Unmanaged<TapBox>.fromOpaque(userdata).takeUnretainedValue()
    box.tap(MediaFrame(direction: MediaDirection(c: dir, fallback: .rx),
                       samples: UnsafeBufferPointer(start: pcm, count: pcm == nil ? 0 : samples),
                       sampleRate: sampleRate, channels: channels, timestampMicros: timestamp))
}
