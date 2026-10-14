package dev.medut

import dev.medut.internal.RawCallInfo
import dev.medut.internal.RawStats

/**
 * Media statistics for one call (medut_ev_media_stats_t).  Rates are over the
 * last stats interval; packet counters are cumulative.  RTCP-derived numbers
 * (RTT, MOS) read 0 until the first receiver report arrives.
 */
data class MediaStats(
    val packetsSent: Long,
    val packetsReceived: Long,
    /** Packets the far end did not receive (from its receiver reports). */
    val packetsLost: Long,
    /** Packets we did not receive. */
    val packetsLostRx: Long,
    val bytesSent: Long,
    val bytesReceived: Long,
    val txErrors: Long,
    val rxErrors: Long,
    val lossPct: Float,
    val lossPctRx: Float,
    val jitterMs: Float,
    val txJitterMs: Float,
    val rttMs: Float,
    val jitterBufferMs: Long,
    val jitterBufferLoad: Long,
    val latePackets: Long,
    val discardedPackets: Long,
    val jitterBufferTargetMs: Long,
    val jitterBufferAdaptive: Boolean,
    val plcFrames: Long,
    val plcRatio: Float,
    /** Lost Opus frames rebuilt from in-band FEC, and concealed without it. */
    val fecRecovered: Long,
    val plcConcealed: Long,
    /** Whether our encoder is sending in-band FEC, and the loss it is sized for. */
    val fecActive: Boolean,
    val fecLevelPct: Long,
    val bandwidthKbpsTx: Long,
    val bandwidthKbpsRx: Long,
    val avgBandwidthKbpsTx: Long,
    val avgBandwidthKbpsRx: Long,
    /** Listening (LQ) and conversational (CQ) quality, 1.0-4.5. */
    val mosLq: Float,
    val mosCq: Float,
    val mosLqRx: Float,
    val mosCqRx: Float,
    val mosLqMin: Float,
    val mosLqAvg: Float,
    val mosMethod: MosMethod,
    val codecName: String,
    val codecClockRate: Long,
    val codecSampleRate: Long,
    val codecChannels: Int,
    val payloadType: Int,
    /** dBov, 0 = max, -127 = silent; NaN when unavailable. */
    val audioLevelDbov: Float,
    val micLevelDbov: Float,
    val ssrcTx: Long,
    val ssrcRx: Long,
    val remoteAddress: String,
    val statsTick: Long,
    val callDurationMs: Long,
    /** The last stats event before the call is torn down. */
    val isFinal: Boolean,
    val iceState: IceState,
    val iceLocalType: IceCandidateType,
    val iceRemoteType: IceCandidateType,
    val iceRttMs: Float,
    val iceGeneration: Long,
    val iceLocalAddress: String,
    val iceRemoteAddress: String,
) {
    internal companion object {
        fun from(s: RawStats) = MediaStats(
            packetsSent = s.packets_sent,
            packetsReceived = s.packets_received,
            packetsLost = s.packets_lost,
            packetsLostRx = s.packets_lost_rx,
            bytesSent = s.bytes_sent,
            bytesReceived = s.bytes_received,
            txErrors = s.tx_errors,
            rxErrors = s.rx_errors,
            lossPct = s.loss_pct,
            lossPctRx = s.loss_pct_rx,
            jitterMs = s.jitter_ms,
            txJitterMs = s.tx_jitter_ms,
            rttMs = s.rtt_ms,
            jitterBufferMs = s.jitter_buffer_ms,
            jitterBufferLoad = s.jitter_buffer_load,
            latePackets = s.late_packets,
            discardedPackets = s.discarded_packets,
            jitterBufferTargetMs = s.jitter_buffer_target_ms,
            jitterBufferAdaptive = s.jitter_buffer_adaptive,
            plcFrames = s.plc_frames,
            plcRatio = s.plc_ratio,
            fecRecovered = s.fec_recovered,
            plcConcealed = s.plc_concealed,
            fecActive = s.fec_active,
            fecLevelPct = s.fec_level_pct,
            bandwidthKbpsTx = s.bandwidth_kbps_tx,
            bandwidthKbpsRx = s.bandwidth_kbps_rx,
            avgBandwidthKbpsTx = s.avg_bandwidth_kbps_tx,
            avgBandwidthKbpsRx = s.avg_bandwidth_kbps_rx,
            mosLq = s.mos_lq,
            mosCq = s.mos_cq,
            mosLqRx = s.mos_lq_rx,
            mosCqRx = s.mos_cq_rx,
            mosLqMin = s.mos_lq_min,
            mosLqAvg = s.mos_lq_avg,
            mosMethod = MosMethod.of(s.mos_method),
            codecName = s.codec_name.orEmpty(),
            codecClockRate = s.codec_clock_rate,
            codecSampleRate = s.codec_sample_rate,
            codecChannels = s.codec_channels,
            payloadType = s.payload_type,
            audioLevelDbov = s.audio_level_dbov,
            micLevelDbov = s.mic_level_dbov,
            ssrcTx = s.ssrc_tx,
            ssrcRx = s.ssrc_rx,
            remoteAddress = s.remote_addr.orEmpty(),
            statsTick = s.stats_tick,
            callDurationMs = s.call_duration_ms,
            isFinal = s.is_final,
            iceState = IceState.of(s.ice_state),
            iceLocalType = IceCandidateType.of(s.ice_local_type),
            iceRemoteType = IceCandidateType.of(s.ice_remote_type),
            iceRttMs = s.ice_rtt_ms,
            iceGeneration = s.ice_generation,
            iceLocalAddress = s.ice_local_addr.orEmpty(),
            iceRemoteAddress = s.ice_remote_addr.orEmpty(),
        )
    }
}

/** Static and slow-moving facts about a call (medut_call_info_t). */
data class CallInfo(
    val peerUri: String,
    val peerDisplayName: String,
    val localUri: String,
    val contactUri: String,
    val callId: String,
    /** Diversion / History-Info URI when the call was forwarded to us. */
    val diverterUri: String,
    val isOutgoing: Boolean,
    /** The peer put us on hold (local hold is [Call.isHeld]). */
    val isRemoteHold: Boolean,
    val sipStatus: Int,
    val durationMs: Long,
    /** INVITE to answer, in whole seconds scaled to ms. */
    val setupDurationMs: Long,
    val lineNumber: Long,
    val transport: Transport,
    val state: CallState,
) {
    internal companion object {
        fun from(i: RawCallInfo) = CallInfo(
            peerUri = i.peer_uri.orEmpty(),
            peerDisplayName = i.peer_display_name.orEmpty(),
            localUri = i.local_uri.orEmpty(),
            contactUri = i.contact_uri.orEmpty(),
            callId = i.call_id.orEmpty(),
            diverterUri = i.diverter_uri.orEmpty(),
            isOutgoing = i.is_outgoing,
            isRemoteHold = i.is_remote_hold,
            sipStatus = i.sip_status,
            durationMs = i.duration_ms,
            setupDurationMs = i.setup_duration_ms,
            lineNumber = i.line_number,
            transport = Transport.of(i.transport),
            state = CallState.of(i.state),
        )
    }
}

/** An audio device from [MedutAudio.inputDevices] / [MedutAudio.outputDevices]. */
data class AudioDevice(val name: String, val description: String, val isDefault: Boolean)

/** The format the current call negotiated, for sizing an app-owned device. */
data class ExternalAudioFormat(val sampleRate: Int, val channels: Int, val ptimeMs: Int)
