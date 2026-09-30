 
              const char *echosdk_version(void);
 const char *echosdk_strerror(int err);

typedef struct echosdk_account *echosdk_account_handle_t;
typedef struct echosdk_call *echosdk_call_handle_t;

typedef enum {
 ECHOSDK_TRANSPORT_UDP = 0,
 ECHOSDK_TRANSPORT_TCP,
 ECHOSDK_TRANSPORT_TLS,
 ECHOSDK_TRANSPORT_WS,
 ECHOSDK_TRANSPORT_WSS,
} echosdk_transport_t;

typedef enum {
 ECHOSDK_MEDIA_ENC_NONE = 0,
 ECHOSDK_MEDIA_ENC_SDES,
 ECHOSDK_MEDIA_ENC_DTLS_SRTP,
} echosdk_media_enc_t;
typedef enum {
 ECHOSDK_CODEC_OPUS = 0,
 ECHOSDK_CODEC_PCMU,
 ECHOSDK_CODEC_PCMA,
 ECHOSDK_CODEC_G722,
 ECHOSDK_CODEC_G726_32,
} echosdk_codec_t;

typedef enum {
 ECHOSDK_MOS_EMODEL = 0,
 ECHOSDK_MOS_SIMPLIFIED,
} echosdk_mos_method_t;
typedef uint8_t echosdk_aec_mode_t;
typedef enum {
 ECHOSDK_MEDIA_DIR_RX = 0,
 ECHOSDK_MEDIA_DIR_TX,
} echosdk_media_dir_t;

typedef enum {
 ECHOSDK_DTMF_RFC4733 = 0,
 ECHOSDK_DTMF_SIP_INFO = 1,
 ECHOSDK_DTMF_AUTO = 2,
} echosdk_dtmf_mode_t;

typedef enum {
 ECHOSDK_JBUF_ADAPTIVE = 0,
 ECHOSDK_JBUF_FIXED = 1,
} echosdk_jbuf_type_t;
typedef enum {

 ECHOSDK_ICE_HANDOVER_BEST_EFFORT = 0,
 ECHOSDK_ICE_HANDOVER_FAIL_FAST,
} echosdk_ice_handover_t;
typedef enum {
 ECHOSDK_ICE_NEW = 0,
 ECHOSDK_ICE_GATHERING,
 ECHOSDK_ICE_CHECKING,
 ECHOSDK_ICE_CONNECTED,
 ECHOSDK_ICE_COMPLETED,
 ECHOSDK_ICE_DISCONNECTED,
 ECHOSDK_ICE_FAILED,
 ECHOSDK_ICE_CLOSED,
} echosdk_ice_state_t;
typedef enum {
 ECHOSDK_ICE_CAND_HOST = 0,
 ECHOSDK_ICE_CAND_SRFLX,
 ECHOSDK_ICE_CAND_PRFLX,
 ECHOSDK_ICE_CAND_RELAY,
 ECHOSDK_ICE_CAND_NONE,
} echosdk_ice_cand_type_t;

typedef struct {
 int bitrate;
 int complexity;
 bool cbr;
 bool dtx;
 bool fec;
 bool stereo;
} echosdk_opus_config_t;

typedef enum {
 ECHOSDK_OK = 0,
 ECHOSDK_ERR_INVAL = -1,
 ECHOSDK_ERR_NOMEM = -2,
 ECHOSDK_ERR_STATE = -3,
 ECHOSDK_ERR_DNS = -4,
 ECHOSDK_ERR_TRANSPORT = -5,
 ECHOSDK_ERR_AUTH = -6,
 ECHOSDK_ERR_SERVER_5XX = -7,
 ECHOSDK_ERR_WS_PROTOCOL_REJECTED = -8,
 ECHOSDK_ERR_TIMEOUT = -9,
 ECHOSDK_ERR_ALREADY = -10,

 ECHOSDK_ERR_FORBIDDEN = -11,
} echosdk_error_t;

typedef enum {
 ECHOSDK_CALL_CALLING = 0,
 ECHOSDK_CALL_RINGING,
 ECHOSDK_CALL_ESTABLISHED,
 ECHOSDK_CALL_HELD,
 ECHOSDK_CALL_ENDED,
 ECHOSDK_CALL_CANCELLED,
 ECHOSDK_CALL_FAILED,
} echosdk_call_state_t;

typedef enum {
 ECHOSDK_REG_UNREGISTERED = 0,
 ECHOSDK_REG_REGISTERING,
 ECHOSDK_REG_REGISTERED,

 ECHOSDK_REG_FAILED,
 ECHOSDK_REG_UNREGISTERING,
 ECHOSDK_REG_RECONNECTING,
} echosdk_reg_state_t;

typedef enum {
 ECHOSDK_PRESENCE_UNKNOWN = 0,
 ECHOSDK_PRESENCE_OPEN,
 ECHOSDK_PRESENCE_CLOSED,
 ECHOSDK_PRESENCE_BUSY,
} echosdk_presence_status_t;

typedef enum {
 ECHOSDK_100REL_DISABLED = 0,
 ECHOSDK_100REL_ENABLED = 1,
 ECHOSDK_100REL_REQUIRED = 2,
} echosdk_100rel_mode_t;
typedef enum {
 ECHOSDK_PUSH_PROVIDER_NONE = 0,
 ECHOSDK_PUSH_PROVIDER_APNS = 1,
 ECHOSDK_PUSH_PROVIDER_APNS_SANDBOX = 2,
 ECHOSDK_PUSH_PROVIDER_FCM = 3,
} echosdk_push_provider_t;

typedef enum {
 ECHOSDK_EV_LOG = 0,
 ECHOSDK_EV_REG_STATE,
 ECHOSDK_EV_INCOMING_CALL,
 ECHOSDK_EV_CALL_STATE,
 ECHOSDK_EV_CALL_DTMF,
 ECHOSDK_EV_SDP_NEGOTIATION,
 ECHOSDK_EV_SIP_TRACE,
 ECHOSDK_EV_MEDIA_STATS,
 ECHOSDK_EV_REGISTRAR_WARNING,
 ECHOSDK_EV_TRANSFER_REQUEST,
 ECHOSDK_EV_MWI,
 ECHOSDK_EV_MESSAGE,
 ECHOSDK_EV_PRESENCE_STATE,
 ECHOSDK_EV_QUALITY_ALERT,
 ECHOSDK_EV_NETWORK,

 ECHOSDK_EV_TRANSFER_FAILED,
 ECHOSDK_EV_ICE_STATE,
} echosdk_event_type_t;
typedef enum {
 ECHOSDK_NET_CHANGE_DETECTED = 0,
 ECHOSDK_NET_DOWN,
 ECHOSDK_NET_UP,
 ECHOSDK_NET_TRANSPORT_RESET,
 ECHOSDK_NET_REREGISTERING,
 ECHOSDK_NET_CALL_MIGRATING,
 ECHOSDK_NET_CALL_MIGRATE_ACCEPTED,
 ECHOSDK_NET_CALL_MIGRATED,
 ECHOSDK_NET_CALL_MIGRATION_FAILED,
 ECHOSDK_NET_CALL_DEFERRED,
 ECHOSDK_NET_HANDOVER_FAILED,
 ECHOSDK_NET_CALL_ICE_STALE,
 ECHOSDK_NET_CALL_ICE_FAILED,
} echosdk_net_event_t;
typedef struct {
 echosdk_net_event_t event;
 echosdk_call_handle_t call;
 echosdk_account_handle_t account;
 const char *local_addr;
 uint32_t attempt;
 uint32_t max_attempts;
 uint32_t elapsed_ms;
 bool ice;
 echosdk_error_t error;
} echosdk_ev_network_t;
typedef struct {
 echosdk_call_handle_t call;
 echosdk_ice_state_t state;
 echosdk_ice_state_t prev;
 uint32_t generation;
 echosdk_ice_cand_type_t local_type;
 echosdk_ice_cand_type_t remote_type;
 const char *local_addr;
 const char *remote_addr;
 float rtt_ms;
 echosdk_error_t error;
} echosdk_ev_ice_state_t;
typedef struct {
 echosdk_account_handle_t account;
 echosdk_reg_state_t state;
 echosdk_error_t error;
 uint32_t retry_attempt;
 uint32_t retry_delay_ms;
 const char *error_str;
} echosdk_ev_reg_state_t;

typedef struct {
 echosdk_account_handle_t account;
 echosdk_call_handle_t call;
 const char *from_uri;
 const char *display_name;
} echosdk_ev_incoming_call_t;

typedef struct {
 echosdk_account_handle_t account;
 echosdk_call_handle_t call;
 echosdk_call_state_t state;
 echosdk_error_t error;
 const char *reason;
} echosdk_ev_call_state_t;

typedef struct {
 echosdk_call_handle_t call;
 char digit;
} echosdk_ev_call_dtmf_t;

typedef struct {
 echosdk_call_handle_t call;
 const char *local_sdp;
 const char *remote_sdp;
 const char *negotiated_codec;
 const char *negotiated_crypto;
 const char * const *rejected_codecs;
 const char * const *warnings;
} echosdk_ev_sdp_negotiation_t;

typedef struct {
 echosdk_media_dir_t dir;
 const char *transport;
 const char *remote_addr;
 const char *raw_message;
 uint64_t timestamp_us;
} echosdk_ev_sip_trace_t;

typedef struct {
 echosdk_call_handle_t call;
 uint32_t packets_sent;
 uint32_t packets_received;
 uint32_t packets_lost;

 uint32_t packets_lost_rx;

 uint32_t bytes_sent;
 uint32_t bytes_received;
 uint32_t tx_errors;
 uint32_t rx_errors;
 float loss_pct;
 float loss_pct_rx;
 float jitter_ms;

 float tx_jitter_ms;

 float rtt_ms;
 uint32_t jitter_buffer_ms;
 uint32_t jitter_buffer_load;
 uint32_t late_packets;
 uint32_t discarded_packets;
 uint32_t jitter_buffer_target_ms;
 bool jitter_buffer_adaptive;
 uint32_t plc_frames;
 float plc_ratio;
 uint32_t bandwidth_kbps_tx;
 uint32_t bandwidth_kbps_rx;
 uint32_t avg_bandwidth_kbps_tx;
 uint32_t avg_bandwidth_kbps_rx;
 float mos_lq;
 float mos_cq;
 float mos_lq_rx;
 float mos_cq_rx;
 echosdk_mos_method_t mos_method;
 const char *codec_name;
 uint32_t codec_clock_rate;
 uint32_t codec_sample_rate;
 uint8_t codec_channels;
 int payload_type;
 float audio_level_dbov;

 float mic_level_dbov;

 uint32_t ssrc_tx;
 uint32_t ssrc_rx;
 char remote_addr[64];
 float mos_lq_min;
 float mos_lq_avg;
 uint32_t stats_tick;
 uint64_t call_duration_ms;
 bool is_final;
 echosdk_ice_state_t ice_state;
 echosdk_ice_cand_type_t ice_local_type;
 echosdk_ice_cand_type_t ice_remote_type;
 float ice_rtt_ms;
 uint32_t ice_generation;
 char ice_local_addr[64];
 char ice_remote_addr[64];
 bool fec_active;
 uint32_t fec_level_pct;
} echosdk_ev_media_stats_t;

typedef struct {
 const char *message;
} echosdk_ev_log_t;

typedef struct {
 const char *message;
} echosdk_ev_registrar_warning_t;
typedef struct {
 echosdk_account_handle_t account;
 echosdk_call_handle_t call;
 const char *refer_to_uri;
 bool has_replaces;
 bool auto_followed;
} echosdk_ev_transfer_req_t;
typedef struct {
 echosdk_account_handle_t account;
 echosdk_call_handle_t call;
 const char *reason;
} echosdk_ev_transfer_failed_t;
typedef struct {
 echosdk_account_handle_t account;
 bool messages_waiting;
 uint32_t new_voice;
 uint32_t old_voice;
 uint32_t new_urgent;
 uint32_t old_urgent;
 const char *raw_body;
} echosdk_ev_mwi_t;
typedef struct {
 echosdk_account_handle_t account;
 const char *from_uri;
 const char *body;
 const char *content_type;
} echosdk_ev_message_t;
typedef struct {
 echosdk_account_handle_t account;
 const char *target_uri;
 echosdk_presence_status_t status;
} echosdk_ev_presence_state_t;

typedef enum {
 ECHOSDK_QUALITY_MOS = 0,
 ECHOSDK_QUALITY_LOSS,
 ECHOSDK_QUALITY_JITTER,
 ECHOSDK_QUALITY_RTT,
 ECHOSDK_QUALITY_MEDIA_STALL,
} echosdk_quality_issue_t;

typedef struct {
 echosdk_call_handle_t call;
 echosdk_quality_issue_t issue;
 float value;
 float threshold;
 bool recovering;
} echosdk_ev_quality_alert_t;

typedef struct {
 echosdk_event_type_t type;
 union {
  echosdk_ev_log_t log;
  echosdk_ev_reg_state_t reg;
  echosdk_ev_incoming_call_t incoming;
  echosdk_ev_call_state_t call_state;
  echosdk_ev_call_dtmf_t dtmf;
  echosdk_ev_sdp_negotiation_t sdp;
  echosdk_ev_sip_trace_t sip_trace;
  echosdk_ev_media_stats_t stats;
  echosdk_ev_registrar_warning_t reg_warn;
  echosdk_ev_transfer_req_t transfer_req;
  echosdk_ev_transfer_failed_t transfer_failed;
  echosdk_ev_mwi_t mwi;
  echosdk_ev_message_t msg;
  echosdk_ev_presence_state_t presence;
  echosdk_ev_quality_alert_t quality_alert;
  echosdk_ev_network_t network;
  echosdk_ev_ice_state_t ice;
 } u;
} echosdk_event_t;
typedef void (*echosdk_event_cb_t)(const echosdk_event_t *ev, void *userdata);

 void echosdk_event_release(const echosdk_event_t *ev);

typedef void (*echosdk_media_tap_cb_t)(
 echosdk_call_handle_t call,
 echosdk_media_dir_t direction,
 const int16_t *pcm,
 size_t samples,
 uint32_t sample_rate,
 uint8_t channels,
 uint64_t timestamp_us,
 void *userdata);

typedef struct {

 uint32_t version;
 size_t struct_size;
 echosdk_transport_t transport;
 const char *local_ip;
 uint16_t local_port;
 const char *bind_interface;
 bool prefer_ipv6;

 const char *sip_domain;
 const char *server_url;
 const char *server_host;
 uint16_t server_port;

 const char *outbound_proxy;
 const char *ca_cert_path;
 const char *client_cert;
 const char *client_key;
 bool verify_server;
 const char *sni_hostname;
 const char *user_agent;
 const char *ws_origin;
 const char **ws_extra_headers;
 uint32_t ws_keepalive_ms;
 const char *stun_server;
 const char *turn_server;
 const char *turn_user;
 const char *turn_pass;
 bool ice_enabled;
 bool rtcp_mux;
 echosdk_media_enc_t media_enc;
 echosdk_codec_t audio_codecs[8];
 int audio_codec_count;
 uint8_t dscp_sip;
 uint8_t dscp_rtp;
 bool enable_video;
 echosdk_aec_mode_t aec_mode;
 bool ns;
 bool agc;
 float aec_suppression_level;
 float mic_gain_db;
 float speaker_gain_db;
 echosdk_opus_config_t opus;
 echosdk_jbuf_type_t jbuf_type;
 uint32_t jitter_buffer_min_ms;
 uint32_t jitter_buffer_max_ms;
 uint32_t reg_expires;
 uint32_t reg_refresh_pct;
 uint32_t keepalive_interval;
 uint32_t reg_retry_initial_ms;
 uint32_t reg_retry_max_ms;
 float reg_retry_backoff;
 uint32_t reg_retry_max_attempts;
 float reg_retry_jitter;
 uint32_t sip_t1_ms;
 uint32_t sip_t2_ms;
 uint32_t sip_timer_b_ms;

 uint32_t sip_timer_f_ms;
 bool session_timer_enabled;
 uint32_t session_expires_s;
 uint32_t session_min_se_s;
 uint32_t stats_interval_ms;
 echosdk_mos_method_t mos_method;

 float mos_alert_threshold;
 float loss_alert_threshold;
 float jitter_alert_threshold;
 const char *tmp_dir;
 bool trace_sip;
 bool trace_sdp_diff;
 const char *pcap_path;
 int log_level;
 echosdk_event_cb_t event_cb;
 void *event_userdata;
 uint32_t net_monitor_interval_s;
 uint32_t net_settle_ms;
 bool net_reinvite_calls;

 bool net_hangup_on_migration_failure;
 uint32_t net_verify_ms;

 uint32_t net_max_attempts;
 bool deliver_owned_events;
 char audio_codec_names[8][32];
 int audio_codec_name_count;
 bool platform_audio_activate;
 uint32_t rtp_timeout_s;
 uint32_t media_stall_ms;
 bool adaptive_bitrate;
 uint32_t adapt_min_bitrate;
 uint32_t adapt_max_bitrate;
 float adapt_loss_down_pct;
 float adapt_loss_up_pct;
 uint32_t adapt_recover_ticks;
 uint32_t opus_expected_loss_pct;
 bool keepalive_reregister;
 bool dns_srv_failover;

 echosdk_ice_handover_t net_ice_handover;
 uint32_t ice_gathering_timeout_ms;

} echosdk_config_t;

typedef struct {
 const char *uri;

 const char *password;

 echosdk_transport_t transport;

 const char *server_host;
 uint16_t server_port;

 const char *server_url;

 const char *auth_user;
 const char *display_name;

 echosdk_media_enc_t media_enc;
 bool ice_enabled;
 bool rtcp_mux;
 bool rtcp_mux_set;
 const char *stun_server;
 const char *turn_server;
 const char *turn_user;
 const char *turn_pass;

 const char *outbound;
 const char *outbound_proxy;
 bool verify_tls;
 echosdk_push_provider_t push_provider;

 const char *push_token;

 const char *push_param;
 echosdk_codec_t audio_codecs[8];
 int audio_codec_count;
 char audio_codec_names[8][32];
 int audio_codec_name_count;
 echosdk_dtmf_mode_t dtmf_mode;

} echosdk_account_config_t;
 void echosdk_config_init(echosdk_config_t *cfg);
 int echosdk_init(const echosdk_config_t *cfg);
 bool echosdk_is_initialized(void);
 int echosdk_set_event_handler(echosdk_event_cb_t cb,
                                             void *userdata,
                                             bool deliver_owned_events);

 void echosdk_shutdown(void);

 int echosdk_account_create(const echosdk_account_config_t *cfg,
                             echosdk_account_handle_t *out);
 void echosdk_account_destroy(echosdk_account_handle_t acct);

 int echosdk_account_register(echosdk_account_handle_t acct);
 int echosdk_account_unregister(echosdk_account_handle_t acct);
 int echosdk_account_set_retry_policy(echosdk_account_handle_t acct,
                                                     uint32_t initial_ms,
                                                     uint32_t max_ms,
                                                     float backoff,
                                                     uint32_t max_attempts);
 int echosdk_account_cancel_retry(echosdk_account_handle_t acct);

 int echosdk_account_retry_now(echosdk_account_handle_t acct);
 int echosdk_account_set_push_token(echosdk_account_handle_t acct,
                                                   const char *push_token);
 int echosdk_account_add_register_header(
        echosdk_account_handle_t acct,
        const char *name, const char *value);
 int echosdk_account_add_header(echosdk_account_handle_t acct,
                                const char *name, const char *value);

 int echosdk_account_subscribe_presence(echosdk_account_handle_t acct,
                                        const char *target_uri);
 int echosdk_account_unsubscribe_presence(echosdk_account_handle_t acct,
                                          const char *target_uri);
typedef void (*echosdk_account_iter_fn)(echosdk_account_handle_t acct, void *arg);
 void echosdk_account_foreach(echosdk_account_iter_fn fn, void *arg);
 int echosdk_account_get_aor(echosdk_account_handle_t acct,
                                            char *buf, size_t sz);
 echosdk_reg_state_t echosdk_account_get_reg_state(
        echosdk_account_handle_t acct);

 int echosdk_call_invite(echosdk_account_handle_t acct,
                         const char *uri,
                         echosdk_call_handle_t *out);
 int echosdk_call_answer(echosdk_call_handle_t call);
 int echosdk_call_hangup(echosdk_call_handle_t call);
 int echosdk_call_reject(echosdk_call_handle_t call,
                                        uint16_t scode, const char *reason);
 int echosdk_call_hold(echosdk_call_handle_t call);
 int echosdk_call_resume(echosdk_call_handle_t call);
 bool echosdk_call_is_held(echosdk_call_handle_t call);
 int echosdk_call_send_dtmf(echosdk_call_handle_t call, char digit);
 int echosdk_call_transfer(echosdk_call_handle_t call, const char *uri);
 int echosdk_call_add_header(echosdk_call_handle_t call,
                             const char *name, const char *value);
 int echosdk_call_transfer_accept(echosdk_call_handle_t call,
                                                 echosdk_call_handle_t *out);
 int echosdk_call_transfer_reject(echosdk_call_handle_t call,
                                                 uint16_t scode,
                                                 const char *reason);

 int echosdk_call_attended_transfer(echosdk_call_handle_t call_a,
                                    echosdk_call_handle_t call_b);
typedef void (*echosdk_call_iter_fn)(echosdk_call_handle_t call, void *arg);
 void echosdk_call_foreach(echosdk_call_iter_fn fn, void *arg);
 echosdk_account_handle_t echosdk_call_get_account(
        echosdk_call_handle_t call);
 echosdk_call_state_t echosdk_call_get_state(
        echosdk_call_handle_t call);

 echosdk_ice_state_t echosdk_call_ice_state(
        echosdk_call_handle_t call);
 int echosdk_message_send(echosdk_account_handle_t account,
                          const char *to_uri,
                          const char *body,
                          const char *content_type);

 int echosdk_account_publish_presence(echosdk_account_handle_t account,
                                      echosdk_presence_status_t status);

 int echosdk_account_set_100rel(echosdk_account_handle_t account,
                                echosdk_100rel_mode_t mode);
 int echosdk_audio_use_external(bool enable);
 int echosdk_audio_external_push(const int16_t *pcm, size_t nsamp);
 int echosdk_audio_external_pull(int16_t *pcm, size_t nsamp);
 int echosdk_audio_external_format(uint32_t *srate, uint8_t *ch,
                                                  uint32_t *ptime);
 bool echosdk_audio_external_is_active(void);

typedef struct {
 char name[128];
 char description[256];
 bool is_default;
} echosdk_audio_device_t;

 int echosdk_audio_list_input_devices(echosdk_audio_device_t *devices,
                                                     int max_count);
 int echosdk_audio_list_output_devices(echosdk_audio_device_t *devices,
                                                      int max_count);
 void echosdk_set_aec(bool enable);
 int echosdk_set_aec_mode(echosdk_aec_mode_t mode);

 void echosdk_set_aec_suppression_level(float level);
 void echosdk_set_ns(bool enable);
 void echosdk_set_agc(bool enable);

 void echosdk_set_mic_gain_db(float db);

 void echosdk_set_speaker_gain_db(float db);
 int echosdk_call_set_dscp_rtp(echosdk_call_handle_t call,
                                              uint8_t dscp);
 void echosdk_set_jitter_buffer(uint32_t min_ms, uint32_t max_ms);
 void echosdk_set_jitter_buffer_type(echosdk_jbuf_type_t type);
 int echosdk_call_set_rtp_timeout(echosdk_call_handle_t call,
                                                 uint32_t seconds);
 int echosdk_call_set_bitrate(echosdk_call_handle_t call,
                                             uint32_t bitrate_bps);
 void echosdk_set_adaptive_bitrate(bool enabled,
                                                  uint32_t min_bps,
                                                  uint32_t max_bps);
 int echosdk_account_keepalive_now(
                                        echosdk_account_handle_t account);
 int echosdk_audio_mute(echosdk_call_handle_t call, bool mute);
 bool echosdk_audio_is_muted(echosdk_call_handle_t call);
 int echosdk_audio_mute_rx(echosdk_call_handle_t call, bool mute);
 int echosdk_audio_set_input_device(const char *name);
 int echosdk_audio_set_output_device(const char *name);
 int echosdk_call_set_media_tap(echosdk_call_handle_t call,
                                echosdk_media_tap_cb_t cb,
                                void *userdata);
 int echosdk_call_record_start(echosdk_call_handle_t call,
                                              const char *path);
 int echosdk_call_record_stop(echosdk_call_handle_t call);
typedef struct {
 char peer_uri[256];
 char peer_display_name[128];
 char local_uri[256];
 char contact_uri[256];
 char call_id[128];

 char diverter_uri[256];
 bool is_outgoing;

 bool is_remote_hold;
 uint16_t sip_status;
 uint64_t duration_ms;
 uint32_t setup_duration_ms;
 uint32_t line_number;
 echosdk_transport_t transport;
 echosdk_call_state_t state;
} echosdk_call_info_t;
 int echosdk_call_get_info(echosdk_call_handle_t call,
                                          echosdk_call_info_t *out);
 int echosdk_call_get_stats(echosdk_call_handle_t call,
                            echosdk_ev_media_stats_t *out);
 int echosdk_network_changed(void);

 int echosdk_network_set_monitor_interval(uint32_t seconds);
 int echosdk_network_set_handover_policy(bool reinvite_calls,
                                                        bool hangup_on_failure);
 int echosdk_network_local_addr(char *buf, size_t sz);
 bool echosdk_network_is_up(void);

 int echosdk_pcap_start(const char *path);
 int echosdk_pcap_stop(void);
