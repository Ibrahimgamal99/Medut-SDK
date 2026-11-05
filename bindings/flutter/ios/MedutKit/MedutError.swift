import Foundation
import Medut

/// An error returned by the SDK.
///
/// Most failures come from the C core: a negative `MEDUT_ERR_*` value, or a
/// positive errno from the layers under it.  A few are raised by this binding
/// before reaching C: the stack is not running, the call has ended, or the
/// account was destroyed.
public struct MedutError: Error, Equatable, Sendable, CustomStringConvertible {
    public enum Code: Equatable, Sendable {
        case invalidArgument
        case outOfMemory
        case invalidState
        case dns
        case transport
        case authentication
        case serverError
        case webSocketRejected
        case timeout
        case already
        case forbidden
        /// The server's TLS certificate was rejected (wrong host name,
        /// expired, untrusted).  Registration stops retrying.
        case tlsCertificate
        /// The stack is not running: `start()` was not called, or
        /// `shutdown()` already ran.
        case notRunning
        /// The call has ended; nothing more can be done with it.
        case callEnded
        /// The account was destroyed.
        case accountDestroyed
        /// A positive errno from below the SDK (ENOENT, EALREADY, ...).
        case system(Int32)
        /// A negative code this binding does not know.
        case unknown(Int32)
    }

    public let code: Code
    /// The integer the C function returned (MEDUT_ERR_STATE for the
    /// binding-raised codes).
    public let rawValue: Int32
    /// The operation that failed, e.g. "medut_call_answer".
    public let operation: String

    init(rawValue: Int32, operation: String) {
        self.rawValue = rawValue
        self.operation = operation
        self.code = MedutError.code(for: rawValue)
    }

    init(_ code: Code, operation: String) {
        self.code = code
        self.rawValue = MEDUT_ERR_STATE.rawValue
        self.operation = operation
    }

    public var description: String {
        let what: String
        switch code {
        case .notRunning: what = "the Medut stack is not running"
        case .callEnded: what = "the call has ended"
        case .accountDestroyed: what = "the account was destroyed"
        case .system(let errno): what = String(cString: strerror(errno))
        default: what = String(cString: medut_strerror(rawValue))
        }
        return "\(operation): \(what) (\(rawValue))"
    }

    private static func code(for rc: Int32) -> Code {
        if rc > 0 { return .system(rc) }
        switch medut_error_t(rawValue: rc) {
        case MEDUT_ERR_INVAL: return .invalidArgument
        case MEDUT_ERR_NOMEM: return .outOfMemory
        case MEDUT_ERR_STATE: return .invalidState
        case MEDUT_ERR_DNS: return .dns
        case MEDUT_ERR_TRANSPORT: return .transport
        case MEDUT_ERR_AUTH: return .authentication
        case MEDUT_ERR_SERVER_5XX: return .serverError
        case MEDUT_ERR_WS_PROTOCOL_REJECTED: return .webSocketRejected
        case MEDUT_ERR_TIMEOUT: return .timeout
        case MEDUT_ERR_ALREADY: return .already
        case MEDUT_ERR_FORBIDDEN: return .forbidden
        // MEDUT_ERR_TLS_CERT, by value: the committed xcframework's header
        // predates it until CI rebuilds the framework.
        case medut_error_t(rawValue: -12): return .tlsCertificate
        default: return .unknown(rc)
        }
    }
}

/// Throw for a non-zero C return code.
@inline(__always)
func check(_ rc: Int32, _ operation: String) throws {
    if rc != 0 { throw MedutError(rawValue: rc, operation: operation) }
}

/// Swift view of the `medut_error_t` carried in event payloads.  `nil` for
/// MEDUT_OK.
func eventError(_ e: medut_error_t, _ operation: String) -> MedutError? {
    e.rawValue == 0 ? nil : MedutError(rawValue: e.rawValue, operation: operation)
}
