@file:Suppress("PropertyName")

package dev.medut.internal

import dev.medut.IceServer

// Holders medut_jni.c fills by field name.  Every field is named exactly as
// the C struct member it carries (the JNI macros use one token for both), so
// these names are snake_case on purpose.  Unsigned 32-bit values arrive as
// Long, enums as their C Int value.

/** medut_config_init() defaults; the numeric and boolean fields only. */
internal class RawConfig {
    @JvmField var transport = 0
    @JvmField var local_port = 0
    @JvmField var prefer_ipv6 = false
    @JvmField var server_port = 0
    @JvmField var verify_server = false
    @JvmField var ws_keepalive_ms = 0
    @JvmField var ice_enabled = false
    @JvmField var rtcp_mux = false
    @JvmField var ice_transport_policy = 0
    @JvmField var ice_gathering_timeout_ms = 0
    @JvmField var ice_multipath = false
    @JvmField var media_enc = 0
    @JvmField var dscp_sip = 0
    @JvmField var dscp_rtp = 0
    @JvmField var aec_mode = 0
    @JvmField var ns = false
    @JvmField var agc = false
    @JvmField var aec_suppression_level = 0f
    @JvmField var mic_gain_db = 0f
    @JvmField var speaker_gain_db = 0f
    @JvmField var opus_bitrate = 0
    @JvmField var opus_complexity = 0
    @JvmField var opus_cbr = false
    @JvmField var opus_dtx = false
    @JvmField var opus_fec = false
    @JvmField var opus_stereo = false
    @JvmField var platform_audio_activate = false
    @JvmField var jbuf_type = 0
    @JvmField var jitter_buffer_min_ms = 0
    @JvmField var jitter_buffer_max_ms = 0
    @JvmField var reg_expires = 0
    @JvmField var reg_refresh_pct = 0
    @JvmField var keepalive_interval = 0
    @JvmField var keepalive_reregister = false
    @JvmField var reg_retry_initial_ms = 0
    @JvmField var reg_retry_max_ms = 0
    @JvmField var reg_retry_backoff = 0f
    @JvmField var reg_retry_max_attempts = 0
    @JvmField var reg_retry_jitter = 0f
    @JvmField var dns_srv_failover = false
    @JvmField var sip_timer_b_ms = 0
    @JvmField var sip_timer_f_ms = 0
    @JvmField var session_timer_enabled = false
    @JvmField var session_expires_s = 0
    @JvmField var session_min_se_s = 0
    @JvmField var stats_interval_ms = 0
    @JvmField var mos_method = 0
    @JvmField var mos_alert_threshold = 0f
    @JvmField var loss_alert_threshold = 0f
    @JvmField var jitter_alert_threshold = 0f
    @JvmField var rtp_timeout_s = 0
    @JvmField var media_stall_ms = 0
    @JvmField var adaptive_bitrate = false
    @JvmField var adapt_min_bitrate = 0
    @JvmField var adapt_max_bitrate = 0
    @JvmField var adapt_loss_down_pct = 0f
    @JvmField var adapt_loss_up_pct = 0f
    @JvmField var adapt_recover_ticks = 0
    @JvmField var opus_expected_loss_pct = 0
    @JvmField var net_monitor_interval_s = 0
    @JvmField var net_settle_ms = 0
    @JvmField var net_reinvite_calls = false
    @JvmField var net_hangup_on_migration_failure = false
    @JvmField var net_verify_ms = 0
    @JvmField var net_max_attempts = 0
    @JvmField var net_ice_handover = 0
    @JvmField var trace_sip = false
    @JvmField var trace_sdp_diff = false
    @JvmField var log_level = 0
}

/** medut_ev_media_stats_t. */
internal class RawStats {
    @JvmField var packets_sent = 0L
    @JvmField var packets_received = 0L
    @JvmField var packets_lost = 0L
    @JvmField var packets_lost_rx = 0L
    @JvmField var bytes_sent = 0L
    @JvmField var bytes_received = 0L
    @JvmField var tx_errors = 0L
    @JvmField var rx_errors = 0L
    @JvmField var loss_pct = 0f
    @JvmField var loss_pct_rx = 0f
    @JvmField var jitter_ms = 0f
    @JvmField var tx_jitter_ms = 0f
    @JvmField var rtt_ms = 0f
    @JvmField var jitter_buffer_ms = 0L
    @JvmField var jitter_buffer_load = 0L
    @JvmField var late_packets = 0L
    @JvmField var discarded_packets = 0L
    @JvmField var jitter_buffer_target_ms = 0L
    @JvmField var jitter_buffer_adaptive = false
    @JvmField var plc_frames = 0L
    @JvmField var plc_ratio = 0f
    @JvmField var bandwidth_kbps_tx = 0L
    @JvmField var bandwidth_kbps_rx = 0L
    @JvmField var avg_bandwidth_kbps_tx = 0L
    @JvmField var avg_bandwidth_kbps_rx = 0L
    @JvmField var mos_lq = 0f
    @JvmField var mos_cq = 0f
    @JvmField var mos_lq_rx = 0f
    @JvmField var mos_cq_rx = 0f
    @JvmField var mos_method = 0
    @JvmField var codec_name: String? = null
    @JvmField var codec_clock_rate = 0L
    @JvmField var codec_sample_rate = 0L
    @JvmField var codec_channels = 0
    @JvmField var payload_type = 0
    @JvmField var audio_level_dbov = 0f
    @JvmField var mic_level_dbov = 0f
    @JvmField var ssrc_tx = 0L
    @JvmField var ssrc_rx = 0L
    @JvmField var remote_addr: String? = null
    @JvmField var mos_lq_min = 0f
    @JvmField var mos_lq_avg = 0f
    @JvmField var stats_tick = 0L
    @JvmField var call_duration_ms = 0L
    @JvmField var is_final = false
    @JvmField var ice_state = 0
    @JvmField var ice_local_type = 0
    @JvmField var ice_remote_type = 0
    @JvmField var ice_rtt_ms = 0f
    @JvmField var ice_generation = 0L
    @JvmField var ice_local_addr: String? = null
    @JvmField var ice_remote_addr: String? = null
    @JvmField var fec_active = false
    @JvmField var fec_level_pct = 0L
    @JvmField var fec_recovered = 0L
    @JvmField var plc_concealed = 0L
}

/** medut_call_info_t. */
internal class RawCallInfo {
    @JvmField var peer_uri: String? = null
    @JvmField var peer_display_name: String? = null
    @JvmField var local_uri: String? = null
    @JvmField var contact_uri: String? = null
    @JvmField var call_id: String? = null
    @JvmField var diverter_uri: String? = null
    @JvmField var is_outgoing = false
    @JvmField var is_remote_hold = false
    @JvmField var sip_status = 0
    @JvmField var duration_ms = 0L
    @JvmField var setup_duration_ms = 0L
    @JvmField var line_number = 0L
    @JvmField var transport = 0
    @JvmField var state = 0
}

/**
 * medut_event_t: the union of every payload's fields, by their medut.h
 * names.  C fills only the ones the event type carries.
 */
internal class RawEvent {
    @JvmField var type = 0
    @JvmField var message: String? = null
    @JvmField var account = 0L
    @JvmField var call = 0L
    @JvmField var state = 0
    @JvmField var prev = 0
    @JvmField var error = 0
    @JvmField var retry_attempt = 0L
    @JvmField var retry_delay_ms = 0L
    @JvmField var error_str: String? = null
    @JvmField var from_uri: String? = null
    @JvmField var display_name: String? = null
    @JvmField var reason: String? = null
    @JvmField var digit = 0
    @JvmField var local_sdp: String? = null
    @JvmField var remote_sdp: String? = null
    @JvmField var negotiated_codec: String? = null
    @JvmField var negotiated_crypto: String? = null
    @JvmField var rejected_codecs: Array<String?>? = null
    @JvmField var warnings: Array<String?>? = null
    @JvmField var dir = 0
    @JvmField var transport: String? = null
    @JvmField var remote_addr: String? = null
    @JvmField var raw_message: String? = null
    @JvmField var timestamp_us = 0L
    @JvmField var stats: RawStats? = null
    @JvmField var refer_to_uri: String? = null
    @JvmField var has_replaces = false
    @JvmField var auto_followed = false
    @JvmField var messages_waiting = false
    @JvmField var new_voice = 0L
    @JvmField var old_voice = 0L
    @JvmField var new_urgent = 0L
    @JvmField var old_urgent = 0L
    @JvmField var raw_body: String? = null
    @JvmField var body: String? = null
    @JvmField var content_type: String? = null
    @JvmField var target_uri: String? = null
    @JvmField var status = 0
    @JvmField var issue = 0
    @JvmField var value = 0f
    @JvmField var threshold = 0f
    @JvmField var recovering = false
    @JvmField var event = 0
    @JvmField var local_addr: String? = null
    @JvmField var attempt = 0L
    @JvmField var max_attempts = 0L
    @JvmField var elapsed_ms = 0L
    @JvmField var ice = false
    @JvmField var generation = 0L
    @JvmField var local_type = 0
    @JvmField var remote_type = 0
    @JvmField var rtt_ms = 0f
}

/** Argument holder for medut_account_set_ice_servers(). */
internal class RawIceServers(@JvmField val iceServers: List<IceServer>)

/** C event type values (medut_event_type_t). */
internal object EventType {
    const val LOG = 0
    const val REG_STATE = 1
    const val INCOMING_CALL = 2
    const val CALL_STATE = 3
    const val CALL_DTMF = 4
    const val SDP_NEGOTIATION = 5
    const val SIP_TRACE = 6
    const val MEDIA_STATS = 7
    const val REGISTRAR_WARNING = 8
    const val TRANSFER_REQUEST = 9
    const val MWI = 10
    const val MESSAGE = 11
    const val PRESENCE_STATE = 12
    const val QUALITY_ALERT = 13
    const val NETWORK = 14
    const val TRANSFER_FAILED = 15
    const val ICE_STATE = 16
}
