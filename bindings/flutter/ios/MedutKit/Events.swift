import Foundation
import Medut

/// Everything the stack reports.  See medut.h for when each one fires.
public enum MedutEvent: Sendable {
    case log(String)
    case registration(RegistrationEvent)
    case incomingCall(IncomingCallEvent)
    case callState(CallStateEvent)
    case dtmf(DTMFEvent)
    case sdpNegotiation(SDPNegotiationEvent)
    case sipTrace(SIPTraceEvent)
    case mediaStats(MediaStatsEvent)
    case registrarWarning(String)
    case transferRequest(TransferRequestEvent)
    case transferFailed(TransferFailedEvent)
    case messageWaiting(MessageWaitingEvent)
    case message(MessageEvent)
    case presence(PresenceEvent)
    case qualityAlert(QualityAlertEvent)
    case network(NetworkEvent)
    case iceState(ICEStateEvent)
    /// An event type newer than this binding.
    case unknown(type: Int)

    /// The call the event is about, if any.
    public var call: Call? {
        switch self {
        case .incomingCall(let e): return e.call
        case .callState(let e): return e.call
        case .dtmf(let e): return e.call
        case .sdpNegotiation(let e): return e.call
        case .mediaStats(let e): return e.call
        case .transferRequest(let e): return e.call
        case .transferFailed(let e): return e.call
        case .qualityAlert(let e): return e.call
        case .network(let e): return e.call
        case .iceState(let e): return e.call
        default: return nil
        }
    }

    /// The account the event is about, if any.
    public var account: Account? {
        switch self {
        case .registration(let e): return e.account
        case .incomingCall(let e): return e.account
        case .callState(let e): return e.account
        case .transferRequest(let e): return e.account
        case .transferFailed(let e): return e.account
        case .messageWaiting(let e): return e.account
        case .message(let e): return e.account
        case .presence(let e): return e.account
        case .network(let e): return e.account ?? e.call?.account
        default: return call?.account
        }
    }
}

public struct RegistrationEvent: Sendable {
    public let account: Account?
    public let state: RegistrationState
    public let error: MedutError?
    public let errorText: String?
    /// On a `.reconnecting` event with a retry armed: which attempt is next
    /// and how long until it goes out.  0 otherwise.
    public let retryAttempt: UInt32
    public let retryDelayMs: UInt32
}

public struct IncomingCallEvent: Sendable {
    public let account: Account?
    public let call: Call
    public let fromURI: String
    public let displayName: String?
}

public struct CallStateEvent: Sendable {
    public let account: Account?
    public let call: Call
    public let state: CallState
    public let error: MedutError?
    public let reason: String?
    /// On a terminal state: this side ended the call (`hangup` / `reject`),
    /// from the app or from the system: a Decline on the CallKit screen, a
    /// Bluetooth headset or a watch.  An incoming call that ends unanswered
    /// with this set was declined, not missed.  False on non-terminal states.
    public let endedLocally: Bool
}

public struct DTMFEvent: Sendable {
    public let call: Call?
    public let digit: Character
}

public struct SDPNegotiationEvent: Sendable {
    public let call: Call?
    public let localSDP: String?
    public let remoteSDP: String?
    public let negotiatedCodec: String?
    /// "NONE", "SDES" or "DTLS-SRTP".
    public let negotiatedCrypto: String?
    public let rejectedCodecs: [String]
    public let warnings: [String]
}

public struct SIPTraceEvent: Sendable {
    /// `.rx` for received, `.tx` for sent.
    public let direction: MediaDirection
    public let transport: String
    public let remoteAddress: String
    public let message: String
    public let timestampMicros: UInt64
}

public struct MediaStatsEvent: Sendable {
    public let call: Call?
    public let stats: MediaStats
}

public struct TransferRequestEvent: Sendable {
    public let account: Account?
    /// The call that received the REFER.  Answer with
    /// `acceptTransfer()` or `rejectTransfer()` on it, exactly once.
    public let call: Call?
    public let referToURI: String
    /// Attended transfer (Refer-To carries Replaces).
    public let isAttended: Bool
    public let autoFollowed: Bool
}

public struct TransferFailedEvent: Sendable {
    public let account: Account?
    /// The call we tried to transfer.  It is still up.
    public let call: Call?
    public let reason: String?
}

public struct MessageWaitingEvent: Sendable {
    public let account: Account?
    public let messagesWaiting: Bool
    public let newVoice: UInt32
    public let oldVoice: UInt32
    public let newUrgent: UInt32
    public let oldUrgent: UInt32
    public let rawBody: String?
}

public struct MessageEvent: Sendable {
    public let account: Account?
    public let fromURI: String
    public let body: String
    public let contentType: String?
}

public struct PresenceEvent: Sendable {
    public let account: Account?
    public let targetURI: String
    public let status: PresenceStatus
}

public struct QualityAlertEvent: Sendable {
    public let call: Call?
    public let issue: QualityIssue
    public let value: Float
    public let threshold: Float
    /// The metric came back on the good side of the threshold.
    public let recovering: Bool
}

public struct NetworkEvent: Sendable {
    public let stage: NetworkStage
    public let call: Call?
    public let account: Account?
    public let localAddress: String
    public let attempt: UInt32
    public let maxAttempts: UInt32
    public let elapsedMs: UInt32
    /// The call has live ICE and migrates with an ICE restart.
    public let ice: Bool
    public let error: MedutError?
}

public struct ICEStateEvent: Sendable {
    public let call: Call?
    public let state: ICEState
    public let previous: ICEState
    /// ICE restarts so far; 1 is the initial session.
    public let generation: UInt32
    public let localType: ICECandidateType
    public let remoteType: ICECandidateType
    public let localAddress: String
    public let remoteAddress: String
    public let rttMs: Float
    public let error: MedutError?
}

// ── Decoding ──────────────────────────────────────────────────────────────

extension Medut {
    /// Turn a C event into a Swift one, updating the call registry on the way.
    /// Sets `retained` when an ended `Call` took ownership of `ev`; the caller
    /// releases it otherwise.  Runs on the event thread: no gate, no blocking
    /// SDK calls.
    func decode(_ ev: UnsafePointer<medut_event_t>, retained: inout Bool) -> MedutEvent {
        let e = ev.pointee
        switch e.type {
        case MEDUT_EV_LOG:
            return .log(cString(e.u.log.message) ?? "")

        case MEDUT_EV_REG_STATE:
            let r = e.u.reg
            return .registration(RegistrationEvent(
                account: account(for: r.account),
                state: RegistrationState(c: r.state, fallback: .failed),
                error: eventError(r.error, "registration"),
                errorText: cString(r.error_str),
                retryAttempt: r.retry_attempt,
                retryDelayMs: r.retry_delay_ms))

        case MEDUT_EV_INCOMING_CALL:
            let i = e.u.incoming
            guard let handle = i.call else { return .unknown(type: Int(e.type.rawValue)) }
            let acct = account(for: i.account)
            let from = cString(i.from_uri) ?? ""
            let name = cString(i.display_name)
            let call = track(call: handle, account: acct, state: .ringing, isOutgoing: false,
                             remoteURI: from, remoteDisplayName: name)
            #if os(iOS)
            // A push may have reported this call to CallKit already; the
            // call takes that identity before anyone hears about it.
            MedutCallKit.shared.bindIncoming(call)
            #endif
            return .incomingCall(IncomingCallEvent(account: acct, call: call, fromURI: from,
                                                   displayName: name))

        case MEDUT_EV_CALL_STATE:
            let s = e.u.call_state
            guard let handle = s.call else { return .unknown(type: Int(e.type.rawValue)) }
            let state = CallState(c: s.state, fallback: .failed)
            let error = eventError(s.error, "call")
            let reason = cString(s.reason)
            let call = track(call: handle)
            var noCallsLeft = false
            if state.isTerminal {
                retained = call.takeTerminalEvent(ev, state: state, error: error, reason: reason)
                noCallsLeft = untrack(call: handle)
            } else {
                call.update(state: state)
            }
            #if os(iOS)
            if options.audioSession == .automatic {
                if !state.isTerminal {
                    MedutAudioSession.shared.callLive()
                } else if noCallsLeft {
                    MedutAudioSession.shared.callsEnded()
                }
            }
            #else
            _ = noCallsLeft
            #endif
            return .callState(CallStateEvent(account: call.account ?? account(for: s.account),
                                             call: call, state: state, error: error,
                                             reason: reason,
                                             endedLocally: state.isTerminal && call.endRequestedLocally))

        case MEDUT_EV_CALL_DTMF:
            let d = e.u.dtmf
            return .dtmf(DTMFEvent(call: d.call.map { track(call: $0) },
                                   digit: Character(Unicode.Scalar(UInt8(truncatingIfNeeded: d.digit)))))

        case MEDUT_EV_SDP_NEGOTIATION:
            let s = e.u.sdp
            return .sdpNegotiation(SDPNegotiationEvent(
                call: s.call.map { track(call: $0) },
                localSDP: cString(s.local_sdp),
                remoteSDP: cString(s.remote_sdp),
                negotiatedCodec: cString(s.negotiated_codec),
                negotiatedCrypto: cString(s.negotiated_crypto),
                rejectedCodecs: cStringList(s.rejected_codecs),
                warnings: cStringList(s.warnings)))

        case MEDUT_EV_SIP_TRACE:
            let t = e.u.sip_trace
            return .sipTrace(SIPTraceEvent(
                direction: MediaDirection(c: t.dir, fallback: .rx),
                transport: cString(t.transport) ?? "",
                remoteAddress: cString(t.remote_addr) ?? "",
                message: cString(t.raw_message) ?? "",
                timestampMicros: t.timestamp_us))

        case MEDUT_EV_MEDIA_STATS:
            let s = e.u.stats
            return .mediaStats(MediaStatsEvent(call: s.call.map { track(call: $0) },
                                               stats: MediaStats(s)))

        case MEDUT_EV_REGISTRAR_WARNING:
            return .registrarWarning(cString(e.u.reg_warn.message) ?? "")

        case MEDUT_EV_TRANSFER_REQUEST:
            let t = e.u.transfer_req
            return .transferRequest(TransferRequestEvent(
                account: account(for: t.account),
                call: t.call.map { track(call: $0) },
                referToURI: cString(t.refer_to_uri) ?? "",
                isAttended: t.has_replaces,
                autoFollowed: t.auto_followed))

        case MEDUT_EV_TRANSFER_FAILED:
            let t = e.u.transfer_failed
            return .transferFailed(TransferFailedEvent(
                account: account(for: t.account),
                call: t.call.map { track(call: $0) },
                reason: cString(t.reason)))

        case MEDUT_EV_MWI:
            let m = e.u.mwi
            return .messageWaiting(MessageWaitingEvent(
                account: account(for: m.account),
                messagesWaiting: m.messages_waiting,
                newVoice: m.new_voice, oldVoice: m.old_voice,
                newUrgent: m.new_urgent, oldUrgent: m.old_urgent,
                rawBody: cString(m.raw_body)))

        case MEDUT_EV_MESSAGE:
            let m = e.u.msg
            return .message(MessageEvent(account: account(for: m.account),
                                         fromURI: cString(m.from_uri) ?? "",
                                         body: cString(m.body) ?? "",
                                         contentType: cString(m.content_type)))

        case MEDUT_EV_PRESENCE_STATE:
            let p = e.u.presence
            return .presence(PresenceEvent(account: account(for: p.account),
                                           targetURI: cString(p.target_uri) ?? "",
                                           status: PresenceStatus(c: p.status, fallback: .unknown)))

        case MEDUT_EV_QUALITY_ALERT:
            let q = e.u.quality_alert
            return .qualityAlert(QualityAlertEvent(
                call: q.call.map { track(call: $0) },
                issue: QualityIssue(c: q.issue, fallback: .mos),
                value: q.value, threshold: q.threshold, recovering: q.recovering))

        case MEDUT_EV_NETWORK:
            let n = e.u.network
            return .network(NetworkEvent(
                stage: NetworkStage(c: n.event, fallback: .changeDetected),
                call: n.call.map { track(call: $0) },
                account: account(for: n.account),
                localAddress: cString(n.local_addr) ?? "",
                attempt: n.attempt, maxAttempts: n.max_attempts, elapsedMs: n.elapsed_ms,
                ice: n.ice, error: eventError(n.error, "network handover")))

        case MEDUT_EV_ICE_STATE:
            let i = e.u.ice
            return .iceState(ICEStateEvent(
                call: i.call.map { track(call: $0) },
                state: ICEState(c: i.state, fallback: .new),
                previous: ICEState(c: i.prev, fallback: .new),
                generation: i.generation,
                localType: ICECandidateType(c: i.local_type, fallback: .none),
                remoteType: ICECandidateType(c: i.remote_type, fallback: .none),
                localAddress: cString(i.local_addr) ?? "",
                remoteAddress: cString(i.remote_addr) ?? "",
                rttMs: i.rtt_ms,
                error: eventError(i.error, "ICE")))

        default:
            return .unknown(type: Int(e.type.rawValue))
        }
    }
}
