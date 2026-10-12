 
              const char *medut_version(void);
 const char *medut_strerror(int err);

typedef struct medut_account *medut_account_handle_t;
typedef struct medut_call *medut_call_handle_t;

typedef enum {
 MEDUT_TRANSPORT_UDP = 0,
 MEDUT_TRANSPORT_TCP,
 MEDUT_TRANSPORT_TLS,
 MEDUT_TRANSPORT_WS,
 MEDUT_TRANSPORT_WSS,
} medut_transport_t;

typedef enum {
 MEDUT_MEDIA_ENC_NONE = 0,
 MEDUT_MEDIA_ENC_SDES,
 MEDUT_MEDIA_ENC_DTLS_SRTP,
} medut_media_enc_t;
typedef enum {
 MEDUT_CODEC_OPUS = 0,
 MEDUT_CODEC_PCMU,
 MEDUT_CODEC_PCMA,
 MEDUT_CODEC_G722,
 MEDUT_CODEC_G726_32,
} medut_codec_t;

typedef enum {
 MEDUT_MOS_EMODEL = 0,
 MEDUT_MOS_SIMPLIFIED,
} medut_mos_method_t;
typedef uint8_t medut_aec_mode_t;
typedef enum {
 MEDUT_MEDIA_DIR_RX = 0,
 MEDUT_MEDIA_DIR_TX,
} medut_media_dir_t;

typedef enum {
 MEDUT_DTMF_RFC4733 = 0,
 MEDUT_DTMF_SIP_INFO = 1,
 MEDUT_DTMF_AUTO = 2,
} medut_dtmf_mode_t;

typedef enum {
 MEDUT_JBUF_ADAPTIVE = 0,
 MEDUT_JBUF_FIXED = 1,
} medut_jbuf_type_t;
typedef enum {

 MEDUT_ICE_HANDOVER_BEST_EFFORT = 0,
 MEDUT_ICE_HANDOVER_FAIL_FAST,
} medut_ice_handover_t;
typedef enum {
 MEDUT_ICE_NEW = 0,
 MEDUT_ICE_GATHERING,
 MEDUT_ICE_CHECKING,
 MEDUT_ICE_CONNECTED,
 MEDUT_ICE_COMPLETED,
 MEDUT_ICE_DISCONNECTED,
 MEDUT_ICE_FAILED,
 MEDUT_ICE_CLOSED,
} medut_ice_state_t;
typedef enum {
 MEDUT_ICE_CAND_HOST = 0,
 MEDUT_ICE_CAND_SRFLX,
 MEDUT_ICE_CAND_PRFLX,
 MEDUT_ICE_CAND_RELAY,
 MEDUT_ICE_CAND_NONE,
} medut_ice_cand_type_t;
typedef struct {
 const char *url;
 const char *user;
 const char *pass;
} medut_ice_server_t;
typedef enum {
 MEDUT_ICE_TRANSPORT_ALL = 0,
 MEDUT_ICE_TRANSPORT_RELAY,
} medut_ice_transport_policy_t;

typedef struct {
 int bitrate;
 int complexity;
 bool cbr;
 bool dtx;

 bool fec;
 bool stereo;
} medut_opus_config_t;

typedef enum {
 MEDUT_OK = 0,
 MEDUT_ERR_INVAL = -1,
 MEDUT_ERR_NOMEM = -2,
 MEDUT_ERR_STATE = -3,
 MEDUT_ERR_DNS = -4,
 MEDUT_ERR_TRANSPORT = -5,
 MEDUT_ERR_AUTH = -6,
 MEDUT_ERR_SERVER_5XX = -7,
 MEDUT_ERR_WS_PROTOCOL_REJECTED = -8,
 MEDUT_ERR_TIMEOUT = -9,
 MEDUT_ERR_ALREADY = -10,

 MEDUT_ERR_FORBIDDEN = -11,
} medut_error_t;

typedef enum {
 MEDUT_CALL_CALLING = 0,
 MEDUT_CALL_RINGING,
 MEDUT_CALL_ESTABLISHED,
 MEDUT_CALL_HELD,
 MEDUT_CALL_ENDED,
 MEDUT_CALL_CANCELLED,
 MEDUT_CALL_FAILED,
} medut_call_state_t;

typedef enum {
 MEDUT_REG_UNREGISTERED = 0,
 MEDUT_REG_REGISTERING,
 MEDUT_REG_REGISTERED,

 MEDUT_REG_FAILED,
 MEDUT_REG_UNREGISTERING,
 MEDUT_REG_RECONNECTING,
} medut_reg_state_t;

typedef enum {
 MEDUT_PRESENCE_UNKNOWN = 0,
 MEDUT_PRESENCE_OPEN,
 MEDUT_PRESENCE_CLOSED,
 MEDUT_PRESENCE_BUSY,
} medut_presence_status_t;

typedef enum {
 MEDUT_100REL_DISABLED = 0,
 MEDUT_100REL_ENABLED = 1,
 MEDUT_100REL_REQUIRED = 2,
} medut_100rel_mode_t;
typedef enum {
 MEDUT_PUSH_PROVIDER_NONE = 0,
 MEDUT_PUSH_PROVIDER_APNS = 1,
 MEDUT_PUSH_PROVIDER_APNS_SANDBOX = 2,
 MEDUT_PUSH_PROVIDER_FCM = 3,
} medut_push_provider_t;

typedef enum {
 MEDUT_EV_LOG = 0,
 MEDUT_EV_REG_STATE,
 MEDUT_EV_INCOMING_CALL,
 MEDUT_EV_CALL_STATE,
 MEDUT_EV_CALL_DTMF,
 MEDUT_EV_SDP_NEGOTIATION,
 MEDUT_EV_SIP_TRACE,
 MEDUT_EV_MEDIA_STATS,
 MEDUT_EV_REGISTRAR_WARNING,
 MEDUT_EV_TRANSFER_REQUEST,
 MEDUT_EV_MWI,
 MEDUT_EV_MESSAGE,
 MEDUT_EV_PRESENCE_STATE,
 MEDUT_EV_QUALITY_ALERT,
 MEDUT_EV_NETWORK,

 MEDUT_EV_TRANSFER_FAILED,
 MEDUT_EV_ICE_STATE,
} medut_event_type_t;
typedef enum {
 MEDUT_NET_CHANGE_DETECTED = 0,
 MEDUT_NET_DOWN,
 MEDUT_NET_UP,
 MEDUT_NET_TRANSPORT_RESET,
 MEDUT_NET_REREGISTERING,
 MEDUT_NET_CALL_MIGRATING,
 MEDUT_NET_CALL_MIGRATE_ACCEPTED,
 MEDUT_NET_CALL_MIGRATED,
 MEDUT_NET_CALL_MIGRATION_FAILED,
 MEDUT_NET_CALL_DEFERRED,
 MEDUT_NET_HANDOVER_FAILED,
 MEDUT_NET_CALL_ICE_STALE,
 MEDUT_NET_CALL_ICE_FAILED,
} medut_net_event_t;
typedef struct {
 medut_net_event_t event;
 medut_call_handle_t call;
 medut_account_handle_t account;
 const char *local_addr;
 uint32_t attempt;
 uint32_t max_attempts;
 uint32_t elapsed_ms;
 bool ice;
 medut_error_t error;
} medut_ev_network_t;
typedef struct {
 medut_call_handle_t call;
 medut_ice_state_t state;
 medut_ice_state_t prev;
 uint32_t generation;
 medut_ice_cand_type_t local_type;
 medut_ice_cand_type_t remote_type;
 const char *local_addr;
 const char *remote_addr;
 float rtt_ms;
 medut_error_t error;
} medut_ev_ice_state_t;
typedef struct {
 medut_account_handle_t account;
 medut_reg_state_t state;
 medut_error_t error;
 uint32_t retry_attempt;
 uint32_t retry_delay_ms;
 const char *error_str;
} medut_ev_reg_state_t;

typedef struct {
 medut_account_handle_t account;
 medut_call_handle_t call;
 const char *from_uri;
 const char *display_name;
} medut_ev_incoming_call_t;

typedef struct {
 medut_account_handle_t account;
 medut_call_handle_t call;
 medut_call_state_t state;
 medut_error_t error;
 const char *reason;
} medut_ev_call_state_t;

typedef struct {
 medut_call_handle_t call;
 char digit;
} medut_ev_call_dtmf_t;

typedef struct {
 medut_call_handle_t call;
 const char *local_sdp;
 const char *remote_sdp;
 const char *negotiated_codec;
 const char *negotiated_crypto;
 const char * const *rejected_codecs;
 const char * const *warnings;
} medut_ev_sdp_negotiation_t;

typedef struct {
 medut_media_dir_t dir;
 const char *transport;
 const char *remote_addr;
 const char *raw_message;
 uint64_t timestamp_us;
} medut_ev_sip_trace_t;

typedef struct {
 medut_call_handle_t call;
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
 medut_mos_method_t mos_method;
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
 medut_ice_state_t ice_state;
 medut_ice_cand_type_t ice_local_type;
 medut_ice_cand_type_t ice_remote_type;
 float ice_rtt_ms;
 uint32_t ice_generation;
 char ice_local_addr[64];
 char ice_remote_addr[64];
 bool fec_active;
 uint32_t fec_level_pct;

 uint32_t fec_recovered;
 uint32_t plc_concealed;
} medut_ev_media_stats_t;

typedef struct {
 const char *message;
} medut_ev_log_t;

typedef struct {
 const char *message;
} medut_ev_registrar_warning_t;
typedef struct {
 medut_account_handle_t account;
 medut_call_handle_t call;
 const char *refer_to_uri;
 bool has_replaces;
 bool auto_followed;
} medut_ev_transfer_req_t;
typedef struct {
 medut_account_handle_t account;
 medut_call_handle_t call;
 const char *reason;
} medut_ev_transfer_failed_t;
typedef struct {
 medut_account_handle_t account;
 bool messages_waiting;
 uint32_t new_voice;
 uint32_t old_voice;
 uint32_t new_urgent;
 uint32_t old_urgent;
 const char *raw_body;
} medut_ev_mwi_t;
typedef struct {
 medut_account_handle_t account;
 const char *from_uri;
 const char *body;
 const char *content_type;
} medut_ev_message_t;
typedef struct {
 medut_account_handle_t account;
 const char *target_uri;
 medut_presence_status_t status;
} medut_ev_presence_state_t;

typedef enum {
 MEDUT_QUALITY_MOS = 0,
 MEDUT_QUALITY_LOSS,
 MEDUT_QUALITY_JITTER,
 MEDUT_QUALITY_RTT,
 MEDUT_QUALITY_MEDIA_STALL,
} medut_quality_issue_t;

typedef struct {
 medut_call_handle_t call;
 medut_quality_issue_t issue;
 float value;
 float threshold;
 bool recovering;
} medut_ev_quality_alert_t;

typedef struct {
 medut_event_type_t type;
 union {
  medut_ev_log_t log;
  medut_ev_reg_state_t reg;
  medut_ev_incoming_call_t incoming;
  medut_ev_call_state_t call_state;
  medut_ev_call_dtmf_t dtmf;
  medut_ev_sdp_negotiation_t sdp;
  medut_ev_sip_trace_t sip_trace;
  medut_ev_media_stats_t stats;
  medut_ev_registrar_warning_t reg_warn;
  medut_ev_transfer_req_t transfer_req;
  medut_ev_transfer_failed_t transfer_failed;
  medut_ev_mwi_t mwi;
  medut_ev_message_t msg;
  medut_ev_presence_state_t presence;
  medut_ev_quality_alert_t quality_alert;
  medut_ev_network_t network;
  medut_ev_ice_state_t ice;
 } u;
} medut_event_t;
typedef void (*medut_event_cb_t)(const medut_event_t *ev, void *userdata);

 void medut_event_release(const medut_event_t *ev);

typedef void (*medut_media_tap_cb_t)(
 medut_call_handle_t call,
 medut_media_dir_t direction,
 const int16_t *pcm,
 size_t samples,
 uint32_t sample_rate,
 uint8_t channels,
 uint64_t timestamp_us,
 void *userdata);

typedef struct {

 uint32_t version;
 size_t struct_size;
 medut_transport_t transport;
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
 medut_media_enc_t media_enc;
 medut_codec_t audio_codecs[8];
 int audio_codec_count;
 uint8_t dscp_sip;
 uint8_t dscp_rtp;
 bool enable_video;
 medut_aec_mode_t aec_mode;
 bool ns;
 bool agc;
 float aec_suppression_level;
 float mic_gain_db;
 float speaker_gain_db;
 medut_opus_config_t opus;
 medut_jbuf_type_t jbuf_type;
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
 medut_mos_method_t mos_method;

 float mos_alert_threshold;
 float loss_alert_threshold;
 float jitter_alert_threshold;
 const char *tmp_dir;
 bool trace_sip;
 bool trace_sdp_diff;
 const char *pcap_path;
 int log_level;
 medut_event_cb_t event_cb;
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

 medut_ice_handover_t net_ice_handover;
 uint32_t ice_gathering_timeout_ms;
 const medut_ice_server_t *ice_servers;
 uint32_t ice_server_count;
 medut_ice_transport_policy_t ice_transport_policy;
 bool ice_multipath;

} medut_config_t;

typedef struct {
 const char *uri;

 const char *password;

 medut_transport_t transport;

 const char *server_host;
 uint16_t server_port;

 const char *server_url;

 const char *auth_user;
 const char *display_name;

 medut_media_enc_t media_enc;
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
 medut_push_provider_t push_provider;

 const char *push_token;

 const char *push_param;
 medut_codec_t audio_codecs[8];
 int audio_codec_count;
 char audio_codec_names[8][32];
 int audio_codec_name_count;
 medut_dtmf_mode_t dtmf_mode;
 const medut_ice_server_t *ice_servers;
 uint32_t ice_server_count;
 medut_ice_transport_policy_t ice_transport_policy;

} medut_account_config_t;
 void medut_config_init(medut_config_t *cfg);
 int medut_init(const medut_config_t *cfg);
 bool medut_is_initialized(void);
 int medut_set_event_handler(medut_event_cb_t cb,
                                             void *userdata,
                                             bool deliver_owned_events);

 void medut_shutdown(void);

 int medut_account_create(const medut_account_config_t *cfg,
                             medut_account_handle_t *out);
 void medut_account_destroy(medut_account_handle_t acct);

 int medut_account_register(medut_account_handle_t acct);
 int medut_account_unregister(medut_account_handle_t acct);
 int medut_account_set_retry_policy(medut_account_handle_t acct,
                                                     uint32_t initial_ms,
                                                     uint32_t max_ms,
                                                     float backoff,
                                                     uint32_t max_attempts);
 int medut_account_cancel_retry(medut_account_handle_t acct);

 int medut_account_retry_now(medut_account_handle_t acct);
 int medut_account_set_ice_servers(
 medut_account_handle_t acct,
 const medut_ice_server_t *servers, uint32_t count,
 medut_ice_transport_policy_t policy);
 int medut_account_set_push_token(medut_account_handle_t acct,
                                                   const char *push_token);
 int medut_account_add_register_header(
        medut_account_handle_t acct,
        const char *name, const char *value);
 int medut_account_add_header(medut_account_handle_t acct,
                                const char *name, const char *value);

 int medut_account_subscribe_presence(medut_account_handle_t acct,
                                        const char *target_uri);
 int medut_account_unsubscribe_presence(medut_account_handle_t acct,
                                          const char *target_uri);
typedef void (*medut_account_iter_fn)(medut_account_handle_t acct, void *arg);
 void medut_account_foreach(medut_account_iter_fn fn, void *arg);
 int medut_account_get_aor(medut_account_handle_t acct,
                                            char *buf, size_t sz);
 medut_reg_state_t medut_account_get_reg_state(
        medut_account_handle_t acct);

 int medut_call_invite(medut_account_handle_t acct,
                         const char *uri,
                         medut_call_handle_t *out);
 int medut_call_answer(medut_call_handle_t call);
 int medut_call_hangup(medut_call_handle_t call);
 int medut_call_reject(medut_call_handle_t call,
                                        uint16_t scode, const char *reason);
 int medut_call_hold(medut_call_handle_t call);
 int medut_call_resume(medut_call_handle_t call);
 bool medut_call_is_held(medut_call_handle_t call);
 int medut_call_send_dtmf(medut_call_handle_t call, char digit);
 int medut_call_transfer(medut_call_handle_t call, const char *uri);
 int medut_call_add_header(medut_call_handle_t call,
                             const char *name, const char *value);
 int medut_call_transfer_accept(medut_call_handle_t call,
                                                 medut_call_handle_t *out);
 int medut_call_transfer_reject(medut_call_handle_t call,
                                                 uint16_t scode,
                                                 const char *reason);

 int medut_call_attended_transfer(medut_call_handle_t call_a,
                                    medut_call_handle_t call_b);
typedef void (*medut_call_iter_fn)(medut_call_handle_t call, void *arg);
 void medut_call_foreach(medut_call_iter_fn fn, void *arg);
 medut_account_handle_t medut_call_get_account(
        medut_call_handle_t call);
 medut_call_state_t medut_call_get_state(
        medut_call_handle_t call);

 medut_ice_state_t medut_call_ice_state(
        medut_call_handle_t call);
 int medut_message_send(medut_account_handle_t account,
                          const char *to_uri,
                          const char *body,
                          const char *content_type);

 int medut_account_publish_presence(medut_account_handle_t account,
                                      medut_presence_status_t status);

 int medut_account_set_100rel(medut_account_handle_t account,
                                medut_100rel_mode_t mode);
 int medut_audio_use_external(bool enable);
 int medut_audio_external_push(const int16_t *pcm, size_t nsamp);
 int medut_audio_external_pull(int16_t *pcm, size_t nsamp);
 int medut_audio_external_format(uint32_t *srate, uint8_t *ch,
                                                  uint32_t *ptime);
 bool medut_audio_external_is_active(void);
 void medut_audio_external_set_voice_path(bool active);

typedef struct {
 char name[128];
 char description[256];
 bool is_default;
} medut_audio_device_t;

 int medut_audio_list_input_devices(medut_audio_device_t *devices,
                                                     int max_count);
 int medut_audio_list_output_devices(medut_audio_device_t *devices,
                                                      int max_count);
 void medut_set_aec(bool enable);
 int medut_set_aec_mode(medut_aec_mode_t mode);
 void medut_set_aec_suppression_level(float level);
 void medut_set_ns(bool enable);

 void medut_set_agc(bool enable);

 void medut_set_mic_gain_db(float db);

 void medut_set_speaker_gain_db(float db);
 int medut_call_set_dscp_rtp(medut_call_handle_t call,
                                              uint8_t dscp);

 void medut_set_jitter_buffer(uint32_t min_ms, uint32_t max_ms);
 void medut_set_jitter_buffer_type(medut_jbuf_type_t type);
 int medut_call_set_rtp_timeout(medut_call_handle_t call,
                                                 uint32_t seconds);
 int medut_call_set_bitrate(medut_call_handle_t call,
                                             uint32_t bitrate_bps);
 void medut_set_adaptive_bitrate(bool enabled,
                                                  uint32_t min_bps,
                                                  uint32_t max_bps);
 int medut_account_keepalive_now(
                                        medut_account_handle_t account);
 int medut_audio_mute(medut_call_handle_t call, bool mute);
 bool medut_audio_is_muted(medut_call_handle_t call);
 int medut_audio_mute_rx(medut_call_handle_t call, bool mute);
 int medut_audio_set_input_device(const char *name);
 int medut_audio_set_output_device(const char *name);
 int medut_call_set_media_tap(medut_call_handle_t call,
                                medut_media_tap_cb_t cb,
                                void *userdata);
 int medut_call_record_start(medut_call_handle_t call,
                                              const char *path);
 int medut_call_record_stop(medut_call_handle_t call);
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
 medut_transport_t transport;
 medut_call_state_t state;
} medut_call_info_t;
 int medut_call_get_info(medut_call_handle_t call,
                                          medut_call_info_t *out);
 int medut_call_get_stats(medut_call_handle_t call,
                            medut_ev_media_stats_t *out);
 int medut_network_changed(void);
 int medut_network_set_interface_handle(const char *ifname,
                                                         uint64_t handle);
 int medut_network_clear_interface_handle(const char *ifname);

 int medut_network_set_monitor_interval(uint32_t seconds);
 int medut_network_set_handover_policy(bool reinvite_calls,
                                                        bool hangup_on_failure);
 int medut_network_local_addr(char *buf, size_t sz);
 bool medut_network_is_up(void);

 int medut_pcap_start(const char *path);
 int medut_pcap_stop(void);
