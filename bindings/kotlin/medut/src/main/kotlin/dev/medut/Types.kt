package dev.medut

// Kotlin mirrors of the C enums in medut.h.  `value` is the C value (those are
// ABI and never renumbered); the JNI layer reads it by name.  See medut.h for
// the full semantics of each constant.

internal inline fun <reified E : Enum<E>> enumOf(value: Int, fallback: E, crossinline v: (E) -> Int): E =
    enumValues<E>().firstOrNull { v(it) == value } ?: fallback

enum class Transport(val value: Int) {
    UDP(0), TCP(1), TLS(2), WS(3), WSS(4);
    internal companion object { fun of(v: Int) = enumOf(v, UDP) { it.value } }
}

enum class MediaEncryption(val value: Int) {
    NONE(0), SDES(1), DTLS_SRTP(2);
    internal companion object { fun of(v: Int) = enumOf(v, NONE) { it.value } }
}

enum class MosMethod(val value: Int) {
    E_MODEL(0), SIMPLIFIED(1);
    internal companion object { fun of(v: Int) = enumOf(v, E_MODEL) { it.value } }
}

/** Echo-cancellation backend.  On Android the platform's VOICE_COMMUNICATION
 *  capture cancels echo, and SUPPRESSOR runs only where the device refuses it. */
enum class AecMode(val value: Int) {
    OFF(0), SUPPRESSOR(1), WEBRTC(2);
    internal companion object { fun of(v: Int) = enumOf(v, SUPPRESSOR) { it.value } }
}

enum class MediaDirection(val value: Int) {
    RX(0), TX(1);
    internal companion object { fun of(v: Int) = enumOf(v, RX) { it.value } }
}

enum class DtmfMode(val value: Int) {
    RFC4733(0), SIP_INFO(1), AUTO(2);
    internal companion object { fun of(v: Int) = enumOf(v, RFC4733) { it.value } }
}

enum class JitterBufferType(val value: Int) {
    ADAPTIVE(0), FIXED(1);
    internal companion object { fun of(v: Int) = enumOf(v, ADAPTIVE) { it.value } }
}

/** What to do with an ICE call whose candidates could not be re-gathered on
 *  handover (medut_ice_handover_t). */
enum class IceHandoverPolicy(val value: Int) {
    BEST_EFFORT(0), FAIL_FAST(1);
    internal companion object { fun of(v: Int) = enumOf(v, BEST_EFFORT) { it.value } }
}

/** ICE liveness of a call's media path; mirrors RTCIceConnectionState. */
enum class IceState(val value: Int) {
    NEW(0), GATHERING(1), CHECKING(2), CONNECTED(3), COMPLETED(4),
    DISCONNECTED(5), FAILED(6), CLOSED(7);
    internal companion object { fun of(v: Int) = enumOf(v, NEW) { it.value } }
}

enum class IceCandidateType(val value: Int) {
    HOST(0), SERVER_REFLEXIVE(1), PEER_REFLEXIVE(2), RELAY(3), NONE(4);
    internal companion object { fun of(v: Int) = enumOf(v, NONE) { it.value } }
}

enum class IceTransportPolicy(val value: Int) {
    ALL(0),
    /** TURN candidates only. */
    RELAY(1);
    internal companion object { fun of(v: Int) = enumOf(v, ALL) { it.value } }
}

enum class CallState(val value: Int) {
    CALLING(0), RINGING(1), ESTABLISHED(2), HELD(3), ENDED(4), CANCELLED(5), FAILED(6);

    /** The call is over; its [Call] accepts no further operations. */
    val isTerminal: Boolean get() = this == ENDED || this == CANCELLED || this == FAILED

    internal companion object { fun of(v: Int) = enumOf(v, FAILED) { it.value } }
}

enum class RegistrationState(val value: Int) {
    UNREGISTERED(0),
    REGISTERING(1),
    REGISTERED(2),
    /** Terminal: the SDK stopped retrying.  Needs [Account.retryNow] or new
     *  credentials. */
    FAILED(3),
    UNREGISTERING(4),
    /** Transient: the SDK is recovering the registration by itself. */
    RECONNECTING(5);
    internal companion object { fun of(v: Int) = enumOf(v, FAILED) { it.value } }
}

enum class PresenceStatus(val value: Int) {
    UNKNOWN(0), OPEN(1), CLOSED(2), BUSY(3);
    internal companion object { fun of(v: Int) = enumOf(v, UNKNOWN) { it.value } }
}

/** RFC 3262 100rel / PRACK mode. */
enum class ReliableProvisionalMode(val value: Int) {
    DISABLED(0), ENABLED(1), REQUIRED(2)
}

enum class PushProvider(val value: Int) {
    NONE(0), APNS(1), APNS_SANDBOX(2), FCM(3)
}

enum class QualityIssue(val value: Int) {
    MOS(0), LOSS(1), JITTER(2), RTT(3), MEDIA_STALL(4);
    internal companion object { fun of(v: Int) = enumOf(v, MOS) { it.value } }
}

/** Stage of a network handover (medut_net_event_t). */
enum class NetworkStage(val value: Int) {
    CHANGE_DETECTED(0), DOWN(1), UP(2), TRANSPORT_RESET(3), REREGISTERING(4),
    CALL_MIGRATING(5), CALL_MIGRATE_ACCEPTED(6), CALL_MIGRATED(7),
    CALL_MIGRATION_FAILED(8), CALL_DEFERRED(9), HANDOVER_FAILED(10),
    CALL_ICE_STALE(11), CALL_ICE_FAILED(12);
    internal companion object { fun of(v: Int) = enumOf(v, CHANGE_DETECTED) { it.value } }
}

enum class LogLevel(val value: Int) {
    ERROR(0), WARNING(1), INFO(2), DEBUG(3);
    internal companion object { fun of(v: Int) = enumOf(v, WARNING) { it.value } }
}
