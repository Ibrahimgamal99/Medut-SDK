import Foundation
import Medut

/// Media statistics for one call (medut_ev_media_stats_t).  Rates are over
/// the last stats interval; packet counters are cumulative.  RTCP-derived
/// numbers (RTT, MOS) read 0 until the first receiver report arrives.
public struct MediaStats: Sendable {
    // Packets
    public let packetsSent: UInt32
    public let packetsReceived: UInt32
    /// Packets the far end did not receive (from its receiver reports).
    public let packetsLost: UInt32
    /// Packets we did not receive.
    public let packetsLostRx: UInt32
    public let bytesSent: UInt32
    public let bytesReceived: UInt32
    public let txErrors: UInt32
    public let rxErrors: UInt32

    // Loss, delay, jitter
    public let lossPct: Float
    public let lossPctRx: Float
    public let jitterMs: Float
    public let txJitterMs: Float
    public let rttMs: Float

    // Jitter buffer and concealment
    public let jitterBufferMs: UInt32
    public let jitterBufferLoad: UInt32
    public let latePackets: UInt32
    public let discardedPackets: UInt32
    public let jitterBufferTargetMs: UInt32
    public let jitterBufferAdaptive: Bool
    public let plcFrames: UInt32
    public let plcRatio: Float
    /// Lost Opus frames rebuilt from in-band FEC, and concealed without it.
    public let fecRecovered: UInt32
    public let plcConcealed: UInt32
    /// Whether our encoder is sending in-band FEC, and the loss it is sized for.
    public let fecActive: Bool
    public let fecLevelPct: UInt32

    // Bandwidth (kbps)
    public let bandwidthKbpsTx: UInt32
    public let bandwidthKbpsRx: UInt32
    public let avgBandwidthKbpsTx: UInt32
    public let avgBandwidthKbpsRx: UInt32

    // MOS: listening (LQ) and conversational (CQ) quality, 1.0-4.5
    public let mosLQ: Float
    public let mosCQ: Float
    public let mosLQRx: Float
    public let mosCQRx: Float
    public let mosLQMin: Float
    public let mosLQAvg: Float
    public let mosMethod: MOSMethod

    // Codec
    public let codecName: String
    public let codecClockRate: UInt32
    public let codecSampleRate: UInt32
    public let codecChannels: UInt8
    public let payloadType: Int32

    /// dBov, 0 = max, -127 = silent; NaN when unavailable.
    public let audioLevelDBov: Float
    public let micLevelDBov: Float

    // Stream identity and session
    public let ssrcTx: UInt32
    public let ssrcRx: UInt32
    public let remoteAddress: String
    public let statsTick: UInt32
    public let callDurationMs: UInt64
    /// The last stats event before the call is torn down.
    public let isFinal: Bool

    // ICE
    public let iceState: ICEState
    public let iceLocalType: ICECandidateType
    public let iceRemoteType: ICECandidateType
    public let iceRttMs: Float
    public let iceGeneration: UInt32
    public let iceLocalAddress: String
    public let iceRemoteAddress: String

    init(_ s: medut_ev_media_stats_t) {
        packetsSent = s.packets_sent
        packetsReceived = s.packets_received
        packetsLost = s.packets_lost
        packetsLostRx = s.packets_lost_rx
        bytesSent = s.bytes_sent
        bytesReceived = s.bytes_received
        txErrors = s.tx_errors
        rxErrors = s.rx_errors
        lossPct = s.loss_pct
        lossPctRx = s.loss_pct_rx
        jitterMs = s.jitter_ms
        txJitterMs = s.tx_jitter_ms
        rttMs = s.rtt_ms
        jitterBufferMs = s.jitter_buffer_ms
        jitterBufferLoad = s.jitter_buffer_load
        latePackets = s.late_packets
        discardedPackets = s.discarded_packets
        jitterBufferTargetMs = s.jitter_buffer_target_ms
        jitterBufferAdaptive = s.jitter_buffer_adaptive
        plcFrames = s.plc_frames
        plcRatio = s.plc_ratio
        fecRecovered = s.fec_recovered
        plcConcealed = s.plc_concealed
        fecActive = s.fec_active
        fecLevelPct = s.fec_level_pct
        bandwidthKbpsTx = s.bandwidth_kbps_tx
        bandwidthKbpsRx = s.bandwidth_kbps_rx
        avgBandwidthKbpsTx = s.avg_bandwidth_kbps_tx
        avgBandwidthKbpsRx = s.avg_bandwidth_kbps_rx
        mosLQ = s.mos_lq
        mosCQ = s.mos_cq
        mosLQRx = s.mos_lq_rx
        mosCQRx = s.mos_cq_rx
        mosLQMin = s.mos_lq_min
        mosLQAvg = s.mos_lq_avg
        mosMethod = MOSMethod(c: s.mos_method, fallback: .eModel)
        codecName = cString(s.codec_name) ?? ""
        codecClockRate = s.codec_clock_rate
        codecSampleRate = s.codec_sample_rate
        codecChannels = s.codec_channels
        payloadType = s.payload_type
        audioLevelDBov = s.audio_level_dbov
        micLevelDBov = s.mic_level_dbov
        ssrcTx = s.ssrc_tx
        ssrcRx = s.ssrc_rx
        remoteAddress = fixedCString(s.remote_addr)
        statsTick = s.stats_tick
        callDurationMs = s.call_duration_ms
        isFinal = s.is_final
        iceState = ICEState(c: s.ice_state, fallback: .new)
        iceLocalType = ICECandidateType(c: s.ice_local_type, fallback: .none)
        iceRemoteType = ICECandidateType(c: s.ice_remote_type, fallback: .none)
        iceRttMs = s.ice_rtt_ms
        iceGeneration = s.ice_generation
        iceLocalAddress = fixedCString(s.ice_local_addr)
        iceRemoteAddress = fixedCString(s.ice_remote_addr)
    }
}

/// Static and slow-moving facts about a call (medut_call_info_t).  A value
/// you own: nothing in it refers back to the call.
public struct CallInfo: Sendable {
    public let peerURI: String
    public let peerDisplayName: String
    public let localURI: String
    public let contactURI: String
    public let callID: String
    /// Diversion / History-Info URI when the call was forwarded to us.
    public let diverterURI: String
    public let isOutgoing: Bool
    /// The peer put us on hold (local hold is `Call.isHeld`).
    public let isRemoteHold: Bool
    public let sipStatus: UInt16
    public let durationMs: UInt64
    /// INVITE to answer, in whole seconds scaled to ms.
    public let setupDurationMs: UInt32
    public let lineNumber: UInt32
    public let transport: Transport
    public let state: CallState

    init(_ c: medut_call_info_t) {
        peerURI = fixedCString(c.peer_uri)
        peerDisplayName = fixedCString(c.peer_display_name)
        localURI = fixedCString(c.local_uri)
        contactURI = fixedCString(c.contact_uri)
        callID = fixedCString(c.call_id)
        diverterURI = fixedCString(c.diverter_uri)
        isOutgoing = c.is_outgoing
        isRemoteHold = c.is_remote_hold
        sipStatus = c.sip_status
        durationMs = c.duration_ms
        setupDurationMs = c.setup_duration_ms
        lineNumber = c.line_number
        transport = Transport(c: c.transport, fallback: .udp)
        state = CallState(c: c.state, fallback: .ended)
    }
}

/// An audio device from `MedutAudio.inputDevices()` / `outputDevices()`.
public struct AudioDevice: Equatable, Sendable {
    /// Pass this to `setInputDevice(_:)` / `setOutputDevice(_:)`.
    public let name: String
    public let description: String
    public let isDefault: Bool

    init(_ c: medut_audio_device_t) {
        name = fixedCString(c.name)
        description = fixedCString(c.description)
        isDefault = c.is_default
    }
}

/// The format the current call negotiated, for sizing an app-owned device.
public struct ExternalAudioFormat: Equatable, Sendable {
    public let sampleRate: UInt32
    public let channels: UInt8
    public let ptimeMs: UInt32
}
