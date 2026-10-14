// Swift mirrors of the C enums in medut.h.  Raw values are the C values —
// those are ABI and never renumbered — so each case converts with a raw-value
// copy (see Internal/CInterop.swift).  See medut.h for the full semantics of
// each value.

public enum Transport: Int, Sendable, CaseIterable {
    case udp = 0, tcp, tls, ws, wss
}

public enum MediaEncryption: Int, Sendable, CaseIterable {
    case none = 0
    case sdes
    case dtlsSRTP
}

public enum MOSMethod: Int, Sendable, CaseIterable {
    case eModel = 0
    case simplified
}

/// Echo-cancellation backend.  `.webRTC` is desktop Linux only; on iOS the
/// VoiceProcessingIO unit cancels echo and `.suppressor` is ignored there.
public enum AECMode: Int, Sendable, CaseIterable {
    case off = 0
    case suppressor = 1
    case webRTC = 2
}

public enum MediaDirection: Int, Sendable, CaseIterable {
    case rx = 0
    case tx
}

public enum DTMFMode: Int, Sendable, CaseIterable {
    case rfc4733 = 0
    case sipInfo
    case auto
}

public enum JitterBufferType: Int, Sendable, CaseIterable {
    case adaptive = 0
    case fixed
}

/// What to do with an ICE call whose candidates could not be re-gathered on
/// handover (medut_ice_handover_t).
public enum ICEHandoverPolicy: Int, Sendable, CaseIterable {
    case bestEffort = 0
    case failFast
}

/// ICE liveness of a call's media path; mirrors RTCIceConnectionState.
public enum ICEState: Int, Sendable, CaseIterable {
    case new = 0, gathering, checking, connected, completed, disconnected, failed, closed
}

public enum ICECandidateType: Int, Sendable, CaseIterable {
    case host = 0, serverReflexive, peerReflexive, relay, none
}

public enum ICETransportPolicy: Int, Sendable, CaseIterable {
    case all = 0
    /// TURN candidates only.
    case relay
}

public enum CallState: Int, Sendable, CaseIterable {
    case calling = 0, ringing, established, held, ended, cancelled, failed

    /// The call is over; its `Call` accepts no further operations.
    public var isTerminal: Bool {
        self == .ended || self == .cancelled || self == .failed
    }
}

public enum RegistrationState: Int, Sendable, CaseIterable {
    case unregistered = 0
    case registering
    case registered
    /// Terminal: the SDK stopped retrying.  Needs `retryNow()` or new
    /// credentials.
    case failed
    case unregistering
    /// Transient: the SDK is recovering the registration by itself.
    case reconnecting
}

public enum PresenceStatus: Int, Sendable, CaseIterable {
    case unknown = 0, open, closed, busy
}

/// RFC 3262 100rel / PRACK mode.
public enum ReliableProvisionalMode: Int, Sendable, CaseIterable {
    case disabled = 0, enabled, required
}

public enum PushProvider: Int, Sendable, CaseIterable {
    case none = 0
    case apns
    case apnsSandbox
    case fcm
}

public enum QualityIssue: Int, Sendable, CaseIterable {
    case mos = 0, loss, jitter, rtt, mediaStall
}

/// Stage of a network handover (medut_net_event_t).
public enum NetworkStage: Int, Sendable, CaseIterable {
    case changeDetected = 0
    case down
    case up
    case transportReset
    case reregistering
    case callMigrating
    case callMigrateAccepted
    case callMigrated
    case callMigrationFailed
    case callDeferred
    case handoverFailed
    case callICEStale
    case callICEFailed
}

public enum LogLevel: Int, Sendable, CaseIterable {
    case error = 0, warning, info, debug
}
