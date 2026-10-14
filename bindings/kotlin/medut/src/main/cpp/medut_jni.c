/**
 * @file medut_jni.c  JNI layer for the Kotlin binding (dev.medut)
 *
 * One native method per medut.h function, on dev.medut.internal.Native, plus
 * the event sink.  Kept free of Android APIs so the same file builds against
 * a desktop JDK and the Linux library for tests.
 *
 * Structs cross by field NAME, never by position.  C reads MedutConfig /
 * AccountConfig fields by their Kotlin names, and fills the Raw* holder
 * classes (dev/medut/internal/Raw.kt), whose fields carry the C names
 * verbatim: the SET_* macros below use one token both as the C member and as
 * the Java field name, so the two cannot drift apart.  A name or type that
 * does not match raises NoSuchFieldError instead of shifting every later
 * field.
 *
 * Strings cross as real UTF-8 (String.getBytes / new String(bytes, "UTF-8")),
 * not JNI's modified UTF-8: raw SIP messages can carry any byte, and
 * NewStringUTF aborts on input that is not valid modified UTF-8.
 *
 * Events are delivered in owned mode.  The sink returns whether Kotlin kept
 * the event (an ended Call holds its terminal event); anything else is
 * released here.
 */

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "medut.h"

#define FN(name) JNICALL Java_dev_medut_internal_Native_##name
#define H(x)     ((void *)(uintptr_t)(x))
#define J(p)     ((jlong)(uintptr_t)(p))

static JavaVM        *s_vm;
static pthread_key_t  s_detach_key;

static jclass    s_string_cls;
static jmethodID s_string_ctor;      /* String(byte[], String) */
static jmethodID s_string_getbytes;  /* byte[] getBytes(String) */
static jstring   s_utf8;             /* "UTF-8" */
static jmethodID s_list_size;
static jmethodID s_list_get;
static jmethodID s_boolean_value;    /* boolean Boolean.booleanValue() */

static jclass    s_raw_event_cls;
static jmethodID s_raw_event_ctor;
static jclass    s_raw_stats_cls;
static jmethodID s_raw_stats_ctor;
static jclass    s_sink_cls;
static jmethodID s_sink_on_event;    /* static boolean onEvent(RawEvent, long) */

/* ── Strings ──────────────────────────────────────────────────────────────── */

/* malloc'd UTF-8 copy of a Java string, or NULL for null (or on failure, with
 * an exception pending). */
static char *jstr_dup(JNIEnv *env, jstring s)
{
	if (!s || (*env)->ExceptionCheck(env))
		return NULL;

	jbyteArray b = (*env)->CallObjectMethod(env, s, s_string_getbytes, s_utf8);
	if ((*env)->ExceptionCheck(env) || !b)
		return NULL;

	jsize n = (*env)->GetArrayLength(env, b);
	char *out = malloc((size_t)n + 1);
	if (out) {
		(*env)->GetByteArrayRegion(env, b, 0, n, (jbyte *)out);
		out[n] = '\0';
	}
	(*env)->DeleteLocalRef(env, b);
	return out;
}

static jstring new_jstr(JNIEnv *env, const char *s)
{
	if (!s || (*env)->ExceptionCheck(env))
		return NULL;

	jsize n = (jsize)strlen(s);
	jbyteArray b = (*env)->NewByteArray(env, n);
	if (!b)
		return NULL;
	(*env)->SetByteArrayRegion(env, b, 0, n, (const jbyte *)s);
	jstring r = (*env)->NewObject(env, s_string_cls, s_string_ctor, b, s_utf8);
	if ((*env)->ExceptionCheck(env))
		r = NULL;
	(*env)->DeleteLocalRef(env, b);
	return r;
}

/* String array from a NULL-terminated C array; NULL for a NULL array. */
static jobjectArray new_jstr_array(JNIEnv *env, const char *const *list)
{
	if (!list || (*env)->ExceptionCheck(env))
		return NULL;

	jsize n = 0;
	while (list[n])
		n++;

	jobjectArray arr = (*env)->NewObjectArray(env, n, s_string_cls, NULL);
	for (jsize i = 0; arr && i < n; i++) {
		jstring s = new_jstr(env, list[i]);
		(*env)->SetObjectArrayElement(env, arr, i, s);
		if ((*env)->ExceptionCheck(env))
			break;
		if (s)
			(*env)->DeleteLocalRef(env, s);
	}
	return arr;
}

/* Strings that must outlive one C call, freed together. */
struct pool {
	void   **p;
	size_t   n, cap;
};

static void *pool_add(struct pool *pl, void *ptr)
{
	if (!ptr)
		return NULL;
	if (pl->n == pl->cap) {
		size_t cap = pl->cap ? pl->cap * 2 : 32;
		void **np = realloc(pl->p, cap * sizeof(*np));
		if (!np) {
			free(ptr);
			return NULL;
		}
		pl->p = np;
		pl->cap = cap;
	}
	pl->p[pl->n++] = ptr;
	return ptr;
}

static void pool_free(struct pool *pl)
{
	for (size_t i = 0; i < pl->n; i++)
		free(pl->p[i]);
	free(pl->p);
	memset(pl, 0, sizeof(*pl));
}

/* ── Reading Kotlin objects by field name ─────────────────────────────────── */

static jfieldID fid(JNIEnv *env, jobject o, const char *name, const char *sig)
{
	if (!o || (*env)->ExceptionCheck(env))
		return NULL;
	jclass c = (*env)->GetObjectClass(env, o);
	jfieldID f = (*env)->GetFieldID(env, c, name, sig);
	(*env)->DeleteLocalRef(env, c);
	return f;
}

static jint get_i(JNIEnv *env, jobject o, const char *name)
{
	jfieldID f = fid(env, o, name, "I");
	return f ? (*env)->GetIntField(env, o, f) : 0;
}

static jboolean get_z(JNIEnv *env, jobject o, const char *name)
{
	jfieldID f = fid(env, o, name, "Z");
	return f ? (*env)->GetBooleanField(env, o, f) : JNI_FALSE;
}

static jfloat get_f(JNIEnv *env, jobject o, const char *name)
{
	jfieldID f = fid(env, o, name, "F");
	return f ? (*env)->GetFloatField(env, o, f) : 0.0f;
}

static jobject get_o(JNIEnv *env, jobject o, const char *name, const char *sig)
{
	jfieldID f = fid(env, o, name, sig);
	return f ? (*env)->GetObjectField(env, o, f) : NULL;
}

/* An enum field: the Kotlin enums carry their C value in `value`. */
static jint get_enum(JNIEnv *env, jobject o, const char *name, const char *sig)
{
	jobject e = get_o(env, o, name, sig);
	if (!e)
		return 0;
	jint v = get_i(env, e, "value");
	(*env)->DeleteLocalRef(env, e);
	return v;
}

static const char *get_s(JNIEnv *env, jobject o, const char *name,
			 struct pool *pl)
{
	jstring s = get_o(env, o, name, "Ljava/lang/String;");
	char *c = jstr_dup(env, s);
	if (s)
		(*env)->DeleteLocalRef(env, s);
	return pool_add(pl, c);
}

/* A List<String> field as a NULL-terminated array (NULL when empty). */
static const char **get_s_list(JNIEnv *env, jobject o, const char *name,
			       struct pool *pl, int *count)
{
	*count = 0;
	jobject list = get_o(env, o, name, "Ljava/util/List;");
	if (!list)
		return NULL;

	jint n = (*env)->CallIntMethod(env, list, s_list_size);
	const char **arr = NULL;
	if (!(*env)->ExceptionCheck(env) && n > 0)
		arr = pool_add(pl, calloc((size_t)n + 1, sizeof(*arr)));
	for (jint i = 0; arr && i < n; i++) {
		jstring s = (*env)->CallObjectMethod(env, list, s_list_get, i);
		if ((*env)->ExceptionCheck(env))
			break;
		arr[i] = pool_add(pl, jstr_dup(env, s));
		if (s)
			(*env)->DeleteLocalRef(env, s);
	}
	(*env)->DeleteLocalRef(env, list);
	if (arr)
		*count = n;
	return arr;
}

/* A List<IceServer> field (url / username / password). */
static medut_ice_server_t *get_ice_servers(JNIEnv *env, jobject o,
					   const char *name, struct pool *pl,
					   uint32_t *count)
{
	*count = 0;
	jobject list = get_o(env, o, name, "Ljava/util/List;");
	if (!list)
		return NULL;

	jint n = (*env)->CallIntMethod(env, list, s_list_size);
	medut_ice_server_t *arr = NULL;
	if (!(*env)->ExceptionCheck(env) && n > 0)
		arr = pool_add(pl, calloc((size_t)n, sizeof(*arr)));
	for (jint i = 0; arr && i < n; i++) {
		jobject s = (*env)->CallObjectMethod(env, list, s_list_get, i);
		if ((*env)->ExceptionCheck(env))
			break;
		arr[i].url  = get_s(env, s, "url", pl);
		arr[i].user = get_s(env, s, "username", pl);
		arr[i].pass = get_s(env, s, "password", pl);
		if (s)
			(*env)->DeleteLocalRef(env, s);
	}
	(*env)->DeleteLocalRef(env, list);
	if (arr)
		*count = (uint32_t)n;
	return arr;
}

static void copy_codec_names(JNIEnv *env, jobject o, struct pool *pl,
			     char names[8][32], int *count)
{
	int n = 0;
	const char **list = get_s_list(env, o, "audioCodecs", pl, &n);
	*count = 0;
	for (int i = 0; list && i < n && i < 8; i++) {
		if (!list[i])
			continue;
		strncpy(names[*count], list[i], 31);
		names[*count][31] = '\0';
		(*count)++;
	}
}

#define SIG_E(cls) "Ldev/medut/" cls ";"

static void fill_config(JNIEnv *env, jobject o, medut_config_t *c,
			struct pool *pl)
{
	medut_config_init(c);

	c->transport      = (medut_transport_t)get_enum(env, o, "transport", SIG_E("Transport"));
	c->local_ip       = get_s(env, o, "localIp", pl);
	c->local_port     = (uint16_t)get_i(env, o, "localPort");
	c->bind_interface = get_s(env, o, "bindInterface", pl);
	c->prefer_ipv6    = get_z(env, o, "preferIpv6");
	c->sip_domain     = get_s(env, o, "sipDomain", pl);
	c->server_url     = get_s(env, o, "serverUrl", pl);
	c->server_host    = get_s(env, o, "serverHost", pl);
	c->server_port    = (uint16_t)get_i(env, o, "serverPort");
	c->outbound_proxy = get_s(env, o, "outboundProxy", pl);

	c->ca_cert_path  = get_s(env, o, "caCertPath", pl);
	c->client_cert   = get_s(env, o, "clientCert", pl);
	c->client_key    = get_s(env, o, "clientKey", pl);
	c->verify_server = get_z(env, o, "verifyServer");
	c->sni_hostname  = get_s(env, o, "sniHostname", pl);
	c->user_agent    = get_s(env, o, "userAgent", pl);

	int n = 0;
	c->ws_origin        = get_s(env, o, "wsOrigin", pl);
	c->ws_extra_headers = get_s_list(env, o, "wsExtraHeaders", pl, &n);
	c->ws_keepalive_ms  = (uint32_t)get_i(env, o, "wsKeepaliveMs");

	c->stun_server  = get_s(env, o, "stunServer", pl);
	c->turn_server  = get_s(env, o, "turnServer", pl);
	c->turn_user    = get_s(env, o, "turnUser", pl);
	c->turn_pass    = get_s(env, o, "turnPass", pl);
	c->ice_enabled  = get_z(env, o, "iceEnabled");
	c->rtcp_mux     = get_z(env, o, "rtcpMux");
	c->ice_servers  = get_ice_servers(env, o, "iceServers", pl, &c->ice_server_count);
	c->ice_transport_policy = (medut_ice_transport_policy_t)
		get_enum(env, o, "iceTransportPolicy", SIG_E("IceTransportPolicy"));
	c->ice_gathering_timeout_ms = (uint32_t)get_i(env, o, "iceGatheringTimeoutMs");
	c->ice_multipath = get_z(env, o, "iceMultipath");

	c->media_enc = (medut_media_enc_t)get_enum(env, o, "mediaEncryption", SIG_E("MediaEncryption"));
	copy_codec_names(env, o, pl, c->audio_codec_names, &c->audio_codec_name_count);
	c->dscp_sip = (uint8_t)get_i(env, o, "dscpSip");
	c->dscp_rtp = (uint8_t)get_i(env, o, "dscpRtp");

	c->aec_mode = (medut_aec_mode_t)get_enum(env, o, "aecMode", SIG_E("AecMode"));
	c->ns  = get_z(env, o, "noiseSuppression");
	c->agc = get_z(env, o, "autoGainControl");
	c->aec_suppression_level = get_f(env, o, "aecSuppressionLevel");
	c->mic_gain_db     = get_f(env, o, "micGainDb");
	c->speaker_gain_db = get_f(env, o, "speakerGainDb");
	jobject op = get_o(env, o, "opus", SIG_E("OpusConfig"));
	if (op) {
		c->opus.bitrate    = get_i(env, op, "bitrate");
		c->opus.complexity = get_i(env, op, "complexity");
		c->opus.cbr        = get_z(env, op, "constantBitrate");
		c->opus.dtx        = get_z(env, op, "discontinuousTransmission");
		c->opus.fec        = get_z(env, op, "forwardErrorCorrection");
		c->opus.stereo     = get_z(env, op, "stereo");
		(*env)->DeleteLocalRef(env, op);
	}
	c->platform_audio_activate = get_z(env, o, "platformAudioActivate");

	c->jbuf_type = (medut_jbuf_type_t)get_enum(env, o, "jitterBufferType", SIG_E("JitterBufferType"));
	c->jitter_buffer_min_ms = (uint32_t)get_i(env, o, "jitterBufferMinMs");
	c->jitter_buffer_max_ms = (uint32_t)get_i(env, o, "jitterBufferMaxMs");

	c->reg_expires            = (uint32_t)get_i(env, o, "regExpires");
	c->reg_refresh_pct        = (uint32_t)get_i(env, o, "regRefreshPct");
	c->keepalive_interval     = (uint32_t)get_i(env, o, "keepaliveIntervalMs");
	c->keepalive_reregister   = get_z(env, o, "keepaliveReregister");
	c->reg_retry_initial_ms   = (uint32_t)get_i(env, o, "regRetryInitialMs");
	c->reg_retry_max_ms       = (uint32_t)get_i(env, o, "regRetryMaxMs");
	c->reg_retry_backoff      = get_f(env, o, "regRetryBackoff");
	c->reg_retry_max_attempts = (uint32_t)get_i(env, o, "regRetryMaxAttempts");
	c->reg_retry_jitter       = get_f(env, o, "regRetryJitter");
	c->dns_srv_failover       = get_z(env, o, "dnsSrvFailover");

	c->sip_timer_b_ms        = (uint32_t)get_i(env, o, "sipTimerBMs");
	c->sip_timer_f_ms        = (uint32_t)get_i(env, o, "sipTimerFMs");
	c->session_timer_enabled = get_z(env, o, "sessionTimerEnabled");
	c->session_expires_s     = (uint32_t)get_i(env, o, "sessionExpiresSeconds");
	c->session_min_se_s      = (uint32_t)get_i(env, o, "sessionMinSeSeconds");

	c->stats_interval_ms      = (uint32_t)get_i(env, o, "statsIntervalMs");
	c->mos_method             = (medut_mos_method_t)get_enum(env, o, "mosMethod", SIG_E("MosMethod"));
	c->mos_alert_threshold    = get_f(env, o, "mosAlertThreshold");
	c->loss_alert_threshold   = get_f(env, o, "lossAlertThreshold");
	c->jitter_alert_threshold = get_f(env, o, "jitterAlertThreshold");

	c->rtp_timeout_s          = (uint32_t)get_i(env, o, "rtpTimeoutSeconds");
	c->media_stall_ms         = (uint32_t)get_i(env, o, "mediaStallMs");
	c->adaptive_bitrate       = get_z(env, o, "adaptiveBitrate");
	c->adapt_min_bitrate      = (uint32_t)get_i(env, o, "adaptMinBitrate");
	c->adapt_max_bitrate      = (uint32_t)get_i(env, o, "adaptMaxBitrate");
	c->adapt_loss_down_pct    = get_f(env, o, "adaptLossDownPct");
	c->adapt_loss_up_pct      = get_f(env, o, "adaptLossUpPct");
	c->adapt_recover_ticks    = (uint32_t)get_i(env, o, "adaptRecoverTicks");
	c->opus_expected_loss_pct = (uint32_t)get_i(env, o, "opusExpectedLossPct");

	c->net_monitor_interval_s = (uint32_t)get_i(env, o, "netMonitorIntervalSeconds");
	c->net_settle_ms          = (uint32_t)get_i(env, o, "netSettleMs");
	c->net_reinvite_calls     = get_z(env, o, "netReinviteCalls");
	c->net_hangup_on_migration_failure = get_z(env, o, "netHangupOnMigrationFailure");
	c->net_verify_ms          = (uint32_t)get_i(env, o, "netVerifyMs");
	c->net_max_attempts       = (uint32_t)get_i(env, o, "netMaxAttempts");
	c->net_ice_handover       = (medut_ice_handover_t)
		get_enum(env, o, "netIceHandover", SIG_E("IceHandoverPolicy"));

	c->tmp_dir        = get_s(env, o, "tmpDir", pl);
	c->trace_sip      = get_z(env, o, "traceSip");
	c->trace_sdp_diff = get_z(env, o, "traceSdpDiff");
	c->pcap_path      = get_s(env, o, "pcapPath", pl);
	c->log_level      = get_enum(env, o, "logLevel", SIG_E("LogLevel"));
}

static void fill_account_config(JNIEnv *env, jobject o,
				medut_account_config_t *c, struct pool *pl)
{
	memset(c, 0, sizeof(*c));

	c->uri          = get_s(env, o, "uri", pl);
	c->password     = get_s(env, o, "password", pl);
	c->transport    = (medut_transport_t)get_enum(env, o, "transport", SIG_E("Transport"));
	c->server_url   = get_s(env, o, "serverUrl", pl);
	c->server_host  = get_s(env, o, "serverHost", pl);
	c->server_port  = (uint16_t)get_i(env, o, "serverPort");
	c->outbound_proxy = get_s(env, o, "outboundProxy", pl);
	c->auth_user    = get_s(env, o, "authUser", pl);
	c->display_name = get_s(env, o, "displayName", pl);
	c->media_enc    = (medut_media_enc_t)get_enum(env, o, "mediaEncryption", SIG_E("MediaEncryption"));
	c->ice_enabled  = get_z(env, o, "iceEnabled");

	/* rtcpMux is a nullable Boolean: null inherits the stack-wide value. */
	jobject mux = get_o(env, o, "rtcpMux", "Ljava/lang/Boolean;");
	c->rtcp_mux_set = mux != NULL;
	if (mux) {
		/* booleanValue(), not the private `value` field: Android
		 * restricts JNI access to non-SDK members of the platform. */
		c->rtcp_mux = (*env)->CallBooleanMethod(env, mux, s_boolean_value);
		if (!(*env)->ExceptionCheck(env))
			(*env)->DeleteLocalRef(env, mux);
	}

	c->stun_server = get_s(env, o, "stunServer", pl);
	c->turn_server = get_s(env, o, "turnServer", pl);
	c->turn_user   = get_s(env, o, "turnUser", pl);
	c->turn_pass   = get_s(env, o, "turnPass", pl);
	c->ice_servers = get_ice_servers(env, o, "iceServers", pl, &c->ice_server_count);
	c->ice_transport_policy = (medut_ice_transport_policy_t)
		get_enum(env, o, "iceTransportPolicy", SIG_E("IceTransportPolicy"));
	c->verify_tls  = get_z(env, o, "verifyTls");

	c->push_provider = (medut_push_provider_t)get_enum(env, o, "pushProvider", SIG_E("PushProvider"));
	c->push_token    = get_s(env, o, "pushToken", pl);
	c->push_param    = get_s(env, o, "pushParam", pl);

	copy_codec_names(env, o, pl, c->audio_codec_names, &c->audio_codec_name_count);
	c->dtmf_mode = (medut_dtmf_mode_t)get_enum(env, o, "dtmfMode", SIG_E("DtmfMode"));
}

/* ── Filling Raw* holders: one token is both the C member and the field ──── */

#define SET_(o, name, sig, Setter, val) do {                                  \
	jfieldID f_ = fid(env, (o), (name), (sig));                           \
	if (f_)                                                               \
		(*env)->Setter(env, (o), f_, (val));                          \
} while (0)

#define SET_I(o, s, f)   SET_((o), #f, "I", SetIntField,     (jint)(s)->f)
#define SET_J(o, s, f)   SET_((o), #f, "J", SetLongField,    (jlong)(s)->f)
#define SET_Z(o, s, f)   SET_((o), #f, "Z", SetBooleanField, (jboolean)(s)->f)
#define SET_F(o, s, f)   SET_((o), #f, "F", SetFloatField,   (jfloat)(s)->f)
#define SET_H(o, s, f)   SET_((o), #f, "J", SetLongField,    J((s)->f))
#define SET_STR_(o, name, cstr) do {                                          \
	jstring v_ = new_jstr(env, (cstr));                                   \
	SET_((o), (name), "Ljava/lang/String;", SetObjectField, v_);          \
	if (v_)                                                               \
		(*env)->DeleteLocalRef(env, v_);                              \
} while (0)
#define SET_S(o, s, f)   SET_STR_((o), #f, (s)->f)
/* A fixed `char f[N]` member, which is NUL-terminated by the SDK. */
#define SET_A(o, s, f)   SET_STR_((o), #f, (s)->f)
#define SET_SL(o, s, f) do {                                                  \
	jobjectArray v_ = new_jstr_array(env, (s)->f);                        \
	SET_((o), #f, "[Ljava/lang/String;", SetObjectField, v_);             \
	if (v_)                                                               \
		(*env)->DeleteLocalRef(env, v_);                              \
} while (0)

static void fill_raw_config(JNIEnv *env, jobject o, const medut_config_t *c)
{
	SET_I(o, c, transport);
	SET_I(o, c, local_port);
	SET_Z(o, c, prefer_ipv6);
	SET_I(o, c, server_port);
	SET_Z(o, c, verify_server);
	SET_I(o, c, ws_keepalive_ms);
	SET_Z(o, c, ice_enabled);
	SET_Z(o, c, rtcp_mux);
	SET_I(o, c, ice_transport_policy);
	SET_I(o, c, ice_gathering_timeout_ms);
	SET_Z(o, c, ice_multipath);
	SET_I(o, c, media_enc);
	SET_I(o, c, dscp_sip);
	SET_I(o, c, dscp_rtp);
	SET_I(o, c, aec_mode);
	SET_Z(o, c, ns);
	SET_Z(o, c, agc);
	SET_F(o, c, aec_suppression_level);
	SET_F(o, c, mic_gain_db);
	SET_F(o, c, speaker_gain_db);
	SET_((o), "opus_bitrate", "I", SetIntField, (jint)c->opus.bitrate);
	SET_((o), "opus_complexity", "I", SetIntField, (jint)c->opus.complexity);
	SET_((o), "opus_cbr", "Z", SetBooleanField, (jboolean)c->opus.cbr);
	SET_((o), "opus_dtx", "Z", SetBooleanField, (jboolean)c->opus.dtx);
	SET_((o), "opus_fec", "Z", SetBooleanField, (jboolean)c->opus.fec);
	SET_((o), "opus_stereo", "Z", SetBooleanField, (jboolean)c->opus.stereo);
	SET_Z(o, c, platform_audio_activate);
	SET_I(o, c, jbuf_type);
	SET_I(o, c, jitter_buffer_min_ms);
	SET_I(o, c, jitter_buffer_max_ms);
	SET_I(o, c, reg_expires);
	SET_I(o, c, reg_refresh_pct);
	SET_I(o, c, keepalive_interval);
	SET_Z(o, c, keepalive_reregister);
	SET_I(o, c, reg_retry_initial_ms);
	SET_I(o, c, reg_retry_max_ms);
	SET_F(o, c, reg_retry_backoff);
	SET_I(o, c, reg_retry_max_attempts);
	SET_F(o, c, reg_retry_jitter);
	SET_Z(o, c, dns_srv_failover);
	SET_I(o, c, sip_timer_b_ms);
	SET_I(o, c, sip_timer_f_ms);
	SET_Z(o, c, session_timer_enabled);
	SET_I(o, c, session_expires_s);
	SET_I(o, c, session_min_se_s);
	SET_I(o, c, stats_interval_ms);
	SET_I(o, c, mos_method);
	SET_F(o, c, mos_alert_threshold);
	SET_F(o, c, loss_alert_threshold);
	SET_F(o, c, jitter_alert_threshold);
	SET_I(o, c, rtp_timeout_s);
	SET_I(o, c, media_stall_ms);
	SET_Z(o, c, adaptive_bitrate);
	SET_I(o, c, adapt_min_bitrate);
	SET_I(o, c, adapt_max_bitrate);
	SET_F(o, c, adapt_loss_down_pct);
	SET_F(o, c, adapt_loss_up_pct);
	SET_I(o, c, adapt_recover_ticks);
	SET_I(o, c, opus_expected_loss_pct);
	SET_I(o, c, net_monitor_interval_s);
	SET_I(o, c, net_settle_ms);
	SET_Z(o, c, net_reinvite_calls);
	SET_Z(o, c, net_hangup_on_migration_failure);
	SET_I(o, c, net_verify_ms);
	SET_I(o, c, net_max_attempts);
	SET_I(o, c, net_ice_handover);
	SET_Z(o, c, trace_sip);
	SET_Z(o, c, trace_sdp_diff);
	SET_I(o, c, log_level);
}

static void fill_raw_stats(JNIEnv *env, jobject o, const medut_ev_media_stats_t *s)
{
	SET_J(o, s, packets_sent);
	SET_J(o, s, packets_received);
	SET_J(o, s, packets_lost);
	SET_J(o, s, packets_lost_rx);
	SET_J(o, s, bytes_sent);
	SET_J(o, s, bytes_received);
	SET_J(o, s, tx_errors);
	SET_J(o, s, rx_errors);
	SET_F(o, s, loss_pct);
	SET_F(o, s, loss_pct_rx);
	SET_F(o, s, jitter_ms);
	SET_F(o, s, tx_jitter_ms);
	SET_F(o, s, rtt_ms);
	SET_J(o, s, jitter_buffer_ms);
	SET_J(o, s, jitter_buffer_load);
	SET_J(o, s, late_packets);
	SET_J(o, s, discarded_packets);
	SET_J(o, s, jitter_buffer_target_ms);
	SET_Z(o, s, jitter_buffer_adaptive);
	SET_J(o, s, plc_frames);
	SET_F(o, s, plc_ratio);
	SET_J(o, s, bandwidth_kbps_tx);
	SET_J(o, s, bandwidth_kbps_rx);
	SET_J(o, s, avg_bandwidth_kbps_tx);
	SET_J(o, s, avg_bandwidth_kbps_rx);
	SET_F(o, s, mos_lq);
	SET_F(o, s, mos_cq);
	SET_F(o, s, mos_lq_rx);
	SET_F(o, s, mos_cq_rx);
	SET_I(o, s, mos_method);
	SET_S(o, s, codec_name);
	SET_J(o, s, codec_clock_rate);
	SET_J(o, s, codec_sample_rate);
	SET_I(o, s, codec_channels);
	SET_I(o, s, payload_type);
	SET_F(o, s, audio_level_dbov);
	SET_F(o, s, mic_level_dbov);
	SET_J(o, s, ssrc_tx);
	SET_J(o, s, ssrc_rx);
	SET_A(o, s, remote_addr);
	SET_F(o, s, mos_lq_min);
	SET_F(o, s, mos_lq_avg);
	SET_J(o, s, stats_tick);
	SET_J(o, s, call_duration_ms);
	SET_Z(o, s, is_final);
	SET_I(o, s, ice_state);
	SET_I(o, s, ice_local_type);
	SET_I(o, s, ice_remote_type);
	SET_F(o, s, ice_rtt_ms);
	SET_J(o, s, ice_generation);
	SET_A(o, s, ice_local_addr);
	SET_A(o, s, ice_remote_addr);
	SET_Z(o, s, fec_active);
	SET_J(o, s, fec_level_pct);
	SET_J(o, s, fec_recovered);
	SET_J(o, s, plc_concealed);
}

static void fill_raw_call_info(JNIEnv *env, jobject o, const medut_call_info_t *i)
{
	SET_A(o, i, peer_uri);
	SET_A(o, i, peer_display_name);
	SET_A(o, i, local_uri);
	SET_A(o, i, contact_uri);
	SET_A(o, i, call_id);
	SET_A(o, i, diverter_uri);
	SET_Z(o, i, is_outgoing);
	SET_Z(o, i, is_remote_hold);
	SET_I(o, i, sip_status);
	SET_J(o, i, duration_ms);
	SET_J(o, i, setup_duration_ms);
	SET_J(o, i, line_number);
	SET_I(o, i, transport);
	SET_I(o, i, state);
}

/* RawEvent is the union of every payload's fields, named as in medut.h. */
static void fill_raw_event(JNIEnv *env, jobject o, const medut_event_t *ev)
{
	SET_I(o, ev, type);

	switch (ev->type) {
	case MEDUT_EV_LOG:
		SET_S(o, &ev->u.log, message);
		break;
	case MEDUT_EV_REGISTRAR_WARNING:
		SET_S(o, &ev->u.reg_warn, message);
		break;
	case MEDUT_EV_REG_STATE: {
		const medut_ev_reg_state_t *p = &ev->u.reg;
		SET_H(o, p, account);
		SET_I(o, p, state);
		SET_I(o, p, error);
		SET_J(o, p, retry_attempt);
		SET_J(o, p, retry_delay_ms);
		SET_S(o, p, error_str);
		break;
	}
	case MEDUT_EV_INCOMING_CALL: {
		const medut_ev_incoming_call_t *p = &ev->u.incoming;
		SET_H(o, p, account);
		SET_H(o, p, call);
		SET_S(o, p, from_uri);
		SET_S(o, p, display_name);
		break;
	}
	case MEDUT_EV_CALL_STATE: {
		const medut_ev_call_state_t *p = &ev->u.call_state;
		SET_H(o, p, account);
		SET_H(o, p, call);
		SET_I(o, p, state);
		SET_I(o, p, error);
		SET_S(o, p, reason);
		break;
	}
	case MEDUT_EV_CALL_DTMF: {
		const medut_ev_call_dtmf_t *p = &ev->u.dtmf;
		SET_H(o, p, call);
		SET_((o), "digit", "I", SetIntField, (jint)(unsigned char)p->digit);
		break;
	}
	case MEDUT_EV_SDP_NEGOTIATION: {
		const medut_ev_sdp_negotiation_t *p = &ev->u.sdp;
		SET_H(o, p, call);
		SET_S(o, p, local_sdp);
		SET_S(o, p, remote_sdp);
		SET_S(o, p, negotiated_codec);
		SET_S(o, p, negotiated_crypto);
		SET_SL(o, p, rejected_codecs);
		SET_SL(o, p, warnings);
		break;
	}
	case MEDUT_EV_SIP_TRACE: {
		const medut_ev_sip_trace_t *p = &ev->u.sip_trace;
		SET_I(o, p, dir);
		SET_S(o, p, transport);
		SET_S(o, p, remote_addr);
		SET_S(o, p, raw_message);
		SET_J(o, p, timestamp_us);
		break;
	}
	case MEDUT_EV_MEDIA_STATS: {
		const medut_ev_media_stats_t *p = &ev->u.stats;
		SET_H(o, p, call);
		jobject st = (*env)->NewObject(env, s_raw_stats_cls, s_raw_stats_ctor);
		if (st) {
			fill_raw_stats(env, st, p);
			SET_((o), "stats", "Ldev/medut/internal/RawStats;", SetObjectField, st);
			(*env)->DeleteLocalRef(env, st);
		}
		break;
	}
	case MEDUT_EV_TRANSFER_REQUEST: {
		const medut_ev_transfer_req_t *p = &ev->u.transfer_req;
		SET_H(o, p, account);
		SET_H(o, p, call);
		SET_S(o, p, refer_to_uri);
		SET_Z(o, p, has_replaces);
		SET_Z(o, p, auto_followed);
		break;
	}
	case MEDUT_EV_TRANSFER_FAILED: {
		const medut_ev_transfer_failed_t *p = &ev->u.transfer_failed;
		SET_H(o, p, account);
		SET_H(o, p, call);
		SET_S(o, p, reason);
		break;
	}
	case MEDUT_EV_MWI: {
		const medut_ev_mwi_t *p = &ev->u.mwi;
		SET_H(o, p, account);
		SET_Z(o, p, messages_waiting);
		SET_J(o, p, new_voice);
		SET_J(o, p, old_voice);
		SET_J(o, p, new_urgent);
		SET_J(o, p, old_urgent);
		SET_S(o, p, raw_body);
		break;
	}
	case MEDUT_EV_MESSAGE: {
		const medut_ev_message_t *p = &ev->u.msg;
		SET_H(o, p, account);
		SET_S(o, p, from_uri);
		SET_S(o, p, body);
		SET_S(o, p, content_type);
		break;
	}
	case MEDUT_EV_PRESENCE_STATE: {
		const medut_ev_presence_state_t *p = &ev->u.presence;
		SET_H(o, p, account);
		SET_S(o, p, target_uri);
		SET_I(o, p, status);
		break;
	}
	case MEDUT_EV_QUALITY_ALERT: {
		const medut_ev_quality_alert_t *p = &ev->u.quality_alert;
		SET_H(o, p, call);
		SET_I(o, p, issue);
		SET_F(o, p, value);
		SET_F(o, p, threshold);
		SET_Z(o, p, recovering);
		break;
	}
	case MEDUT_EV_NETWORK: {
		const medut_ev_network_t *p = &ev->u.network;
		SET_I(o, p, event);
		SET_H(o, p, call);
		SET_H(o, p, account);
		SET_S(o, p, local_addr);
		SET_J(o, p, attempt);
		SET_J(o, p, max_attempts);
		SET_J(o, p, elapsed_ms);
		SET_Z(o, p, ice);
		SET_I(o, p, error);
		break;
	}
	case MEDUT_EV_ICE_STATE: {
		const medut_ev_ice_state_t *p = &ev->u.ice;
		SET_H(o, p, call);
		SET_I(o, p, state);
		SET_I(o, p, prev);
		SET_J(o, p, generation);
		SET_I(o, p, local_type);
		SET_I(o, p, remote_type);
		SET_S(o, p, local_addr);
		SET_S(o, p, remote_addr);
		SET_F(o, p, rtt_ms);
		SET_I(o, p, error);
		break;
	}
	default:
		break;
	}
}

/* ── Event sink ───────────────────────────────────────────────────────────── */

static void detach_thread(void *unused)
{
	(void)unused;
	if (s_vm)
		(*s_vm)->DetachCurrentThread(s_vm);
}

/* JNIEnv for the SDK's event thread, attaching it on first use.  ART aborts
 * when an attached native thread exits without detaching, so a thread-local
 * destructor detaches it as the SDK joins the thread on shutdown. */
static JNIEnv *thread_env(void)
{
	JNIEnv *env = NULL;
	if ((*s_vm)->GetEnv(s_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK)
		return env;

	JavaVMAttachArgs args = { JNI_VERSION_1_6, "medut-events", NULL };
	/* (void *) because Android declares JNIEnv** and the JDK void**. */
	if ((*s_vm)->AttachCurrentThreadAsDaemon(s_vm, (void *)&env, &args) != JNI_OK)
		return NULL;
	pthread_setspecific(s_detach_key, env);
	return env;
}

static void on_event(const medut_event_t *ev, void *userdata)
{
	(void)userdata;

	JNIEnv *env = thread_env();
	if (!env) {
		medut_event_release(ev);
		return;
	}

	jboolean retained = JNI_FALSE;
	if ((*env)->PushLocalFrame(env, 32) == 0) {
		jobject raw = (*env)->NewObject(env, s_raw_event_cls, s_raw_event_ctor);
		if (raw) {
			fill_raw_event(env, raw, ev);
			if (!(*env)->ExceptionCheck(env))
				retained = (*env)->CallStaticBooleanMethod(
					env, s_sink_cls, s_sink_on_event, raw, J(ev));
		}
		if ((*env)->ExceptionCheck(env)) {
			/* A binding bug (a Raw* field that does not match) or OOM.
			 * The sink never throws by itself, so nothing kept the event. */
			(*env)->ExceptionDescribe(env);
			(*env)->ExceptionClear(env);
			retained = JNI_FALSE;
		}
		(*env)->PopLocalFrame(env, NULL);
	}
	if (!retained)
		medut_event_release(ev);
}

/* ── Load ─────────────────────────────────────────────────────────────────── */

static jclass global_class(JNIEnv *env, const char *name)
{
	jclass local = (*env)->FindClass(env, name);
	if (!local)
		return NULL;
	jclass g = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	return g;
}

/* Classes are resolved here, on a thread with the app's class loader: the
 * SDK's event thread attaches with the system loader, where FindClass cannot
 * see them. */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)reserved;
	JNIEnv *env = NULL;
	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
		return JNI_ERR;
	s_vm = vm;
	if (pthread_key_create(&s_detach_key, detach_thread) != 0)
		return JNI_ERR;

	s_string_cls = global_class(env, "java/lang/String");
	if (!s_string_cls)
		return JNI_ERR;
	s_string_ctor = (*env)->GetMethodID(env, s_string_cls, "<init>",
					    "([BLjava/lang/String;)V");
	s_string_getbytes = (*env)->GetMethodID(env, s_string_cls, "getBytes",
						"(Ljava/lang/String;)[B");
	jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
	s_utf8 = (*env)->NewGlobalRef(env, utf8);
	(*env)->DeleteLocalRef(env, utf8);

	jclass list = (*env)->FindClass(env, "java/util/List");
	if (!list)
		return JNI_ERR;
	s_list_size = (*env)->GetMethodID(env, list, "size", "()I");
	s_list_get  = (*env)->GetMethodID(env, list, "get", "(I)Ljava/lang/Object;");
	(*env)->DeleteLocalRef(env, list);

	jclass boolean = (*env)->FindClass(env, "java/lang/Boolean");
	if (!boolean)
		return JNI_ERR;
	s_boolean_value = (*env)->GetMethodID(env, boolean, "booleanValue", "()Z");
	(*env)->DeleteLocalRef(env, boolean);

	s_raw_event_cls = global_class(env, "dev/medut/internal/RawEvent");
	s_raw_stats_cls = global_class(env, "dev/medut/internal/RawStats");
	s_sink_cls      = global_class(env, "dev/medut/internal/EventSink");
	if (!s_raw_event_cls || !s_raw_stats_cls || !s_sink_cls)
		return JNI_ERR;
	s_raw_event_ctor = (*env)->GetMethodID(env, s_raw_event_cls, "<init>", "()V");
	s_raw_stats_ctor = (*env)->GetMethodID(env, s_raw_stats_cls, "<init>", "()V");
	s_sink_on_event  = (*env)->GetStaticMethodID(env, s_sink_cls, "onEvent",
				"(Ldev/medut/internal/RawEvent;J)Z");
	if (!s_string_ctor || !s_string_getbytes || !s_list_size || !s_list_get ||
	    !s_boolean_value || !s_raw_event_ctor || !s_raw_stats_ctor ||
	    !s_sink_on_event)
		return JNI_ERR;

	return JNI_VERSION_1_6;
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

JNIEXPORT jstring FN(version)(JNIEnv *env, jclass cls)
{
	(void)cls;
	return new_jstr(env, medut_version());
}

JNIEXPORT jstring FN(strerror)(JNIEnv *env, jclass cls, jint err)
{
	(void)cls;
	return new_jstr(env, medut_strerror(err));
}

JNIEXPORT void FN(configDefaults)(JNIEnv *env, jclass cls, jobject out)
{
	(void)cls;
	medut_config_t c;
	medut_config_init(&c);
	fill_raw_config(env, out, &c);
}

JNIEXPORT jint FN(stackInit)(JNIEnv *env, jclass cls, jobject config)
{
	(void)cls;
	struct pool pl = {0};
	medut_config_t c;
	fill_config(env, config, &c, &pl);
	if ((*env)->ExceptionCheck(env)) {
		pool_free(&pl);
		return MEDUT_ERR_INVAL;
	}
	c.event_cb             = on_event;
	c.event_userdata       = NULL;
	c.deliver_owned_events = true;
	int err = medut_init(&c);
	pool_free(&pl);
	return err;
}

JNIEXPORT jint FN(setEventHandler)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return medut_set_event_handler(on_event, NULL, true);
}

JNIEXPORT jboolean FN(isInitialized)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return medut_is_initialized();
}

JNIEXPORT void FN(shutdown)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	medut_shutdown();
}

JNIEXPORT void FN(eventRelease)(JNIEnv *env, jclass cls, jlong ev)
{
	(void)env; (void)cls;
	medut_event_release(H(ev));
}

/* ── Accounts ─────────────────────────────────────────────────────────────── */

JNIEXPORT jint FN(accountCreate)(JNIEnv *env, jclass cls, jobject config,
				 jlongArray out)
{
	(void)cls;
	struct pool pl = {0};
	medut_account_config_t c;
	fill_account_config(env, config, &c, &pl);
	if ((*env)->ExceptionCheck(env)) {
		pool_free(&pl);
		return MEDUT_ERR_INVAL;
	}
	medut_account_handle_t h = NULL;
	int err = medut_account_create(&c, &h);
	pool_free(&pl);
	jlong v = J(h);
	(*env)->SetLongArrayRegion(env, out, 0, 1, &v);
	return err;
}

JNIEXPORT void FN(accountDestroy)(JNIEnv *env, jclass cls, jlong a)
{
	(void)env; (void)cls;
	medut_account_destroy(H(a));
}

#define ACCOUNT_OP(jname, cfn)                                                \
JNIEXPORT jint FN(jname)(JNIEnv *env, jclass cls, jlong a)                    \
{                                                                             \
	(void)env; (void)cls;                                                 \
	return cfn(H(a));                                                     \
}

ACCOUNT_OP(accountRegister,     medut_account_register)
ACCOUNT_OP(accountUnregister,   medut_account_unregister)
ACCOUNT_OP(accountCancelRetry,  medut_account_cancel_retry)
ACCOUNT_OP(accountRetryNow,     medut_account_retry_now)
ACCOUNT_OP(accountKeepaliveNow, medut_account_keepalive_now)

JNIEXPORT jint FN(accountSetRetryPolicy)(JNIEnv *env, jclass cls, jlong a,
					 jint initial_ms, jint max_ms,
					 jfloat backoff, jint max_attempts)
{
	(void)env; (void)cls;
	return medut_account_set_retry_policy(H(a), (uint32_t)initial_ms,
					      (uint32_t)max_ms, backoff,
					      (uint32_t)max_attempts);
}

JNIEXPORT jint FN(accountSetIceServers)(JNIEnv *env, jclass cls, jlong a,
					jobject servers, jint policy)
{
	(void)cls;
	/* `servers` is a holder with one `iceServers` List<IceServer> field. */
	struct pool pl = {0};
	uint32_t n = 0;
	medut_ice_server_t *list = get_ice_servers(env, servers, "iceServers", &pl, &n);
	int err = (*env)->ExceptionCheck(env)
		? MEDUT_ERR_INVAL
		: medut_account_set_ice_servers(H(a), list, n,
						(medut_ice_transport_policy_t)policy);
	pool_free(&pl);
	return err;
}

JNIEXPORT jint FN(accountSetPushToken)(JNIEnv *env, jclass cls, jlong a,
				       jstring token)
{
	(void)cls;
	char *t = jstr_dup(env, token);
	int err = medut_account_set_push_token(H(a), t);
	free(t);
	return err;
}

#define ACCOUNT_STR2_OP(jname, cfn)                                           \
JNIEXPORT jint FN(jname)(JNIEnv *env, jclass cls, jlong a, jstring s1,        \
			 jstring s2)                                          \
{                                                                             \
	(void)cls;                                                            \
	char *c1 = jstr_dup(env, s1);                                         \
	char *c2 = jstr_dup(env, s2);                                         \
	int err = (c1 && c2) ? cfn(H(a), c1, c2) : MEDUT_ERR_INVAL;           \
	free(c1);                                                             \
	free(c2);                                                             \
	return err;                                                           \
}

ACCOUNT_STR2_OP(accountAddHeader,         medut_account_add_header)
ACCOUNT_STR2_OP(accountAddRegisterHeader, medut_account_add_register_header)

#define ACCOUNT_STR_OP(jname, cfn)                                            \
JNIEXPORT jint FN(jname)(JNIEnv *env, jclass cls, jlong a, jstring s)         \
{                                                                             \
	(void)cls;                                                            \
	char *c = jstr_dup(env, s);                                           \
	int err = c ? cfn(H(a), c) : MEDUT_ERR_INVAL;                         \
	free(c);                                                              \
	return err;                                                           \
}

ACCOUNT_STR_OP(accountSubscribePresence,   medut_account_subscribe_presence)
ACCOUNT_STR_OP(accountUnsubscribePresence, medut_account_unsubscribe_presence)

JNIEXPORT jint FN(accountPublishPresence)(JNIEnv *env, jclass cls, jlong a,
					  jint status)
{
	(void)env; (void)cls;
	return medut_account_publish_presence(H(a), (medut_presence_status_t)status);
}

JNIEXPORT jint FN(accountSet100rel)(JNIEnv *env, jclass cls, jlong a, jint mode)
{
	(void)env; (void)cls;
	return medut_account_set_100rel(H(a), (medut_100rel_mode_t)mode);
}

JNIEXPORT jint FN(messageSend)(JNIEnv *env, jclass cls, jlong a, jstring to,
			       jstring body, jstring content_type)
{
	(void)cls;
	char *t  = jstr_dup(env, to);
	char *b  = jstr_dup(env, body);
	char *ct = jstr_dup(env, content_type);
	int err = (t && b) ? medut_message_send(H(a), t, b, ct) : MEDUT_ERR_INVAL;
	free(t);
	free(b);
	free(ct);
	return err;
}

/* Handles collected by a *_foreach, as a long[]. */
struct handle_list {
	jlong  *v;
	size_t  n, cap;
};

static void collect_handle(void *h, void *arg)
{
	struct handle_list *l = arg;
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 8;
		jlong *nv = realloc(l->v, cap * sizeof(*nv));
		if (!nv)
			return;
		l->v = nv;
		l->cap = cap;
	}
	l->v[l->n++] = J(h);
}

static void collect_account(medut_account_handle_t h, void *arg) { collect_handle(h, arg); }
static void collect_call(medut_call_handle_t h, void *arg)       { collect_handle(h, arg); }

static jlongArray handle_array(JNIEnv *env, struct handle_list *l)
{
	jlongArray arr = (*env)->NewLongArray(env, (jsize)l->n);
	if (arr && l->n)
		(*env)->SetLongArrayRegion(env, arr, 0, (jsize)l->n, l->v);
	free(l->v);
	return arr;
}

JNIEXPORT jlongArray FN(accountForeach)(JNIEnv *env, jclass cls)
{
	(void)cls;
	struct handle_list l = {0};
	medut_account_foreach(collect_account, &l);
	return handle_array(env, &l);
}

JNIEXPORT jstring FN(accountGetAor)(JNIEnv *env, jclass cls, jlong a)
{
	(void)cls;
	char buf[512];
	if (medut_account_get_aor(H(a), buf, sizeof(buf)) != 0)
		return NULL;
	return new_jstr(env, buf);
}

JNIEXPORT jint FN(accountGetRegState)(JNIEnv *env, jclass cls, jlong a)
{
	(void)env; (void)cls;
	return (jint)medut_account_get_reg_state(H(a));
}

/* ── Calls ────────────────────────────────────────────────────────────────── */

JNIEXPORT jint FN(callInvite)(JNIEnv *env, jclass cls, jlong a, jstring uri,
			      jlongArray out)
{
	(void)cls;
	char *u = jstr_dup(env, uri);
	medut_call_handle_t h = NULL;
	int err = u ? medut_call_invite(H(a), u, &h) : MEDUT_ERR_INVAL;
	free(u);
	jlong v = J(h);
	(*env)->SetLongArrayRegion(env, out, 0, 1, &v);
	return err;
}

#define CALL_OP(jname, cfn)                                                   \
JNIEXPORT jint FN(jname)(JNIEnv *env, jclass cls, jlong c)                    \
{                                                                             \
	(void)env; (void)cls;                                                 \
	return cfn(H(c));                                                     \
}

CALL_OP(callAnswer,     medut_call_answer)
CALL_OP(callHangup,     medut_call_hangup)
CALL_OP(callHold,       medut_call_hold)
CALL_OP(callResume,     medut_call_resume)
CALL_OP(callRecordStop, medut_call_record_stop)

JNIEXPORT jint FN(callReject)(JNIEnv *env, jclass cls, jlong c, jint scode,
			      jstring reason)
{
	(void)cls;
	char *r = jstr_dup(env, reason);
	int err = medut_call_reject(H(c), (uint16_t)scode, r);
	free(r);
	return err;
}

JNIEXPORT jboolean FN(callIsHeld)(JNIEnv *env, jclass cls, jlong c)
{
	(void)env; (void)cls;
	return medut_call_is_held(H(c));
}

JNIEXPORT jint FN(callSendDtmf)(JNIEnv *env, jclass cls, jlong c, jchar digit)
{
	(void)env; (void)cls;
	return medut_call_send_dtmf(H(c), (char)digit);
}

#define CALL_STR_OP(jname, cfn)                                               \
JNIEXPORT jint FN(jname)(JNIEnv *env, jclass cls, jlong c, jstring s)         \
{                                                                             \
	(void)cls;                                                            \
	char *cs = jstr_dup(env, s);                                          \
	int err = cs ? cfn(H(c), cs) : MEDUT_ERR_INVAL;                       \
	free(cs);                                                             \
	return err;                                                           \
}

CALL_STR_OP(callTransfer,    medut_call_transfer)
CALL_STR_OP(callRecordStart, medut_call_record_start)

JNIEXPORT jint FN(callAddHeader)(JNIEnv *env, jclass cls, jlong c,
				 jstring name, jstring value)
{
	(void)cls;
	char *n = jstr_dup(env, name);
	char *v = jstr_dup(env, value);
	int err = (n && v) ? medut_call_add_header(H(c), n, v) : MEDUT_ERR_INVAL;
	free(n);
	free(v);
	return err;
}

JNIEXPORT jint FN(callTransferAccept)(JNIEnv *env, jclass cls, jlong c,
				      jlongArray out)
{
	(void)cls;
	medut_call_handle_t h = NULL;
	int err = medut_call_transfer_accept(H(c), &h);
	jlong v = J(h);
	(*env)->SetLongArrayRegion(env, out, 0, 1, &v);
	return err;
}

JNIEXPORT jint FN(callTransferReject)(JNIEnv *env, jclass cls, jlong c,
				      jint scode, jstring reason)
{
	(void)cls;
	char *r = jstr_dup(env, reason);
	int err = medut_call_transfer_reject(H(c), (uint16_t)scode, r);
	free(r);
	return err;
}

JNIEXPORT jint FN(callAttendedTransfer)(JNIEnv *env, jclass cls, jlong a,
					jlong b)
{
	(void)env; (void)cls;
	return medut_call_attended_transfer(H(a), H(b));
}

JNIEXPORT jlongArray FN(callForeach)(JNIEnv *env, jclass cls)
{
	(void)cls;
	struct handle_list l = {0};
	medut_call_foreach(collect_call, &l);
	return handle_array(env, &l);
}

JNIEXPORT jlong FN(callGetAccount)(JNIEnv *env, jclass cls, jlong c)
{
	(void)env; (void)cls;
	return J(medut_call_get_account(H(c)));
}

JNIEXPORT jint FN(callGetState)(JNIEnv *env, jclass cls, jlong c)
{
	(void)env; (void)cls;
	return (jint)medut_call_get_state(H(c));
}

JNIEXPORT jint FN(callIceState)(JNIEnv *env, jclass cls, jlong c)
{
	(void)env; (void)cls;
	return (jint)medut_call_ice_state(H(c));
}

JNIEXPORT jint FN(callGetInfo)(JNIEnv *env, jclass cls, jlong c, jobject out)
{
	(void)cls;
	medut_call_info_t info;
	int err = medut_call_get_info(H(c), &info);
	if (err == 0)
		fill_raw_call_info(env, out, &info);
	return err;
}

JNIEXPORT jint FN(callGetStats)(JNIEnv *env, jclass cls, jlong c, jobject out)
{
	(void)cls;
	medut_ev_media_stats_t s;
	int err = medut_call_get_stats(H(c), &s);
	if (err == 0)
		fill_raw_stats(env, out, &s);
	return err;
}

JNIEXPORT jint FN(callSetDscpRtp)(JNIEnv *env, jclass cls, jlong c, jint dscp)
{
	(void)env; (void)cls;
	return medut_call_set_dscp_rtp(H(c), (uint8_t)dscp);
}

JNIEXPORT jint FN(callSetRtpTimeout)(JNIEnv *env, jclass cls, jlong c,
				     jint seconds)
{
	(void)env; (void)cls;
	return medut_call_set_rtp_timeout(H(c), (uint32_t)seconds);
}

JNIEXPORT jint FN(callSetBitrate)(JNIEnv *env, jclass cls, jlong c, jint bps)
{
	(void)env; (void)cls;
	return medut_call_set_bitrate(H(c), (uint32_t)bps);
}

JNIEXPORT jint FN(audioMute)(JNIEnv *env, jclass cls, jlong c, jboolean on)
{
	(void)env; (void)cls;
	return medut_audio_mute(H(c), on);
}

JNIEXPORT jboolean FN(audioIsMuted)(JNIEnv *env, jclass cls, jlong c)
{
	(void)env; (void)cls;
	return medut_audio_is_muted(H(c));
}

JNIEXPORT jint FN(audioMuteRx)(JNIEnv *env, jclass cls, jlong c, jboolean on)
{
	(void)env; (void)cls;
	return medut_audio_mute_rx(H(c), on);
}

/* ── Audio (stack-wide) ───────────────────────────────────────────────────── */

JNIEXPORT void FN(setAec)(JNIEnv *env, jclass cls, jboolean on)
{
	(void)env; (void)cls;
	medut_set_aec(on);
}

JNIEXPORT jint FN(setAecMode)(JNIEnv *env, jclass cls, jint mode)
{
	(void)env; (void)cls;
	return medut_set_aec_mode((medut_aec_mode_t)mode);
}

JNIEXPORT void FN(setAecSuppressionLevel)(JNIEnv *env, jclass cls, jfloat level)
{
	(void)env; (void)cls;
	medut_set_aec_suppression_level(level);
}

JNIEXPORT void FN(setNs)(JNIEnv *env, jclass cls, jboolean on)
{
	(void)env; (void)cls;
	medut_set_ns(on);
}

JNIEXPORT void FN(setAgc)(JNIEnv *env, jclass cls, jboolean on)
{
	(void)env; (void)cls;
	medut_set_agc(on);
}

JNIEXPORT void FN(setMicGainDb)(JNIEnv *env, jclass cls, jfloat db)
{
	(void)env; (void)cls;
	medut_set_mic_gain_db(db);
}

JNIEXPORT void FN(setSpeakerGainDb)(JNIEnv *env, jclass cls, jfloat db)
{
	(void)env; (void)cls;
	medut_set_speaker_gain_db(db);
}

JNIEXPORT void FN(setJitterBuffer)(JNIEnv *env, jclass cls, jint min_ms,
				   jint max_ms)
{
	(void)env; (void)cls;
	medut_set_jitter_buffer((uint32_t)min_ms, (uint32_t)max_ms);
}

JNIEXPORT void FN(setJitterBufferType)(JNIEnv *env, jclass cls, jint type)
{
	(void)env; (void)cls;
	medut_set_jitter_buffer_type((medut_jbuf_type_t)type);
}

JNIEXPORT void FN(setAdaptiveBitrate)(JNIEnv *env, jclass cls, jboolean on,
				      jint min_bps, jint max_bps)
{
	(void)env; (void)cls;
	medut_set_adaptive_bitrate(on, (uint32_t)min_bps, (uint32_t)max_bps);
}

/* Devices as a flat String[]: name, description, "1"/"0" (default) per
 * device. */
static jobjectArray device_array(JNIEnv *env,
				 int (*list)(medut_audio_device_t *, int))
{
	medut_audio_device_t devs[32];
	int n = list(devs, 32);
	if (n < 0)
		n = 0;
	jobjectArray arr = (*env)->NewObjectArray(env, n * 3, s_string_cls, NULL);
	for (int i = 0; arr && i < n; i++) {
		const char *fields[3] = { devs[i].name, devs[i].description,
					  devs[i].is_default ? "1" : "0" };
		for (int k = 0; k < 3; k++) {
			jstring s = new_jstr(env, fields[k]);
			(*env)->SetObjectArrayElement(env, arr, i * 3 + k, s);
			if ((*env)->ExceptionCheck(env))
				return arr;
			if (s)
				(*env)->DeleteLocalRef(env, s);
		}
	}
	return arr;
}

JNIEXPORT jobjectArray FN(audioListInputDevices)(JNIEnv *env, jclass cls)
{
	(void)cls;
	return device_array(env, medut_audio_list_input_devices);
}

JNIEXPORT jobjectArray FN(audioListOutputDevices)(JNIEnv *env, jclass cls)
{
	(void)cls;
	return device_array(env, medut_audio_list_output_devices);
}

JNIEXPORT jint FN(audioSetInputDevice)(JNIEnv *env, jclass cls, jstring name)
{
	(void)cls;
	char *n = jstr_dup(env, name);
	int err = medut_audio_set_input_device(n);
	free(n);
	return err;
}

JNIEXPORT jint FN(audioSetOutputDevice)(JNIEnv *env, jclass cls, jstring name)
{
	(void)cls;
	char *n = jstr_dup(env, name);
	int err = medut_audio_set_output_device(n);
	free(n);
	return err;
}

JNIEXPORT jint FN(audioUseExternal)(JNIEnv *env, jclass cls, jboolean on)
{
	(void)env; (void)cls;
	return medut_audio_use_external(on);
}

/* Samples in a direct ByteBuffer, or -1: a heap buffer has no stable address
 * to hand C.  Push and pull run on real-time audio threads, so this path
 * allocates nothing. */
static jlong direct_samples(JNIEnv *env, jobject buf, int16_t **out)
{
	*out = buf ? (*env)->GetDirectBufferAddress(env, buf) : NULL;
	if (!*out)
		return -1;
	return (*env)->GetDirectBufferCapacity(env, buf) / (jlong)sizeof(int16_t);
}

JNIEXPORT jint FN(audioExternalPush)(JNIEnv *env, jclass cls, jobject buf,
				     jint nsamp)
{
	(void)cls;
	int16_t *pcm;
	jlong cap = direct_samples(env, buf, &pcm);
	if (cap < 0 || nsamp < 0 || nsamp > cap)
		return 22; /* EINVAL */
	return medut_audio_external_push(pcm, (size_t)nsamp);
}

JNIEXPORT jint FN(audioExternalPull)(JNIEnv *env, jclass cls, jobject buf,
				     jint nsamp)
{
	(void)cls;
	int16_t *pcm;
	jlong cap = direct_samples(env, buf, &pcm);
	if (cap < 0 || nsamp < 0 || nsamp > cap)
		return 22; /* EINVAL */
	return medut_audio_external_pull(pcm, (size_t)nsamp);
}

JNIEXPORT jint FN(audioExternalFormat)(JNIEnv *env, jclass cls, jintArray out)
{
	(void)cls;
	uint32_t srate = 0, ptime = 0;
	uint8_t ch = 0;
	int err = medut_audio_external_format(&srate, &ch, &ptime);
	jint v[3] = { (jint)srate, (jint)ch, (jint)ptime };
	(*env)->SetIntArrayRegion(env, out, 0, 3, v);
	return err;
}

JNIEXPORT jboolean FN(audioExternalIsActive)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return medut_audio_external_is_active();
}

JNIEXPORT void FN(audioExternalSetVoicePath)(JNIEnv *env, jclass cls,
					     jboolean active)
{
	(void)env; (void)cls;
	medut_audio_external_set_voice_path(active);
}

/* ── Network, pcap ────────────────────────────────────────────────────────── */

JNIEXPORT jint FN(networkChanged)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return medut_network_changed();
}

JNIEXPORT jint FN(networkSetInterfaceHandle)(JNIEnv *env, jclass cls,
					     jstring ifname, jlong handle)
{
	(void)cls;
	char *n = jstr_dup(env, ifname);
	int err = n ? medut_network_set_interface_handle(n, (uint64_t)handle)
		    : MEDUT_ERR_INVAL;
	free(n);
	return err;
}

JNIEXPORT jint FN(networkClearInterfaceHandle)(JNIEnv *env, jclass cls,
					       jstring ifname)
{
	(void)cls;
	char *n = jstr_dup(env, ifname);
	int err = n ? medut_network_clear_interface_handle(n) : MEDUT_ERR_INVAL;
	free(n);
	return err;
}

JNIEXPORT jint FN(networkSetMonitorInterval)(JNIEnv *env, jclass cls,
					     jint seconds)
{
	(void)env; (void)cls;
	return medut_network_set_monitor_interval((uint32_t)seconds);
}

JNIEXPORT jint FN(networkSetHandoverPolicy)(JNIEnv *env, jclass cls,
					    jboolean reinvite, jboolean hangup)
{
	(void)env; (void)cls;
	return medut_network_set_handover_policy(reinvite, hangup);
}

JNIEXPORT jstring FN(networkLocalAddr)(JNIEnv *env, jclass cls)
{
	(void)cls;
	char buf[64];
	if (medut_network_local_addr(buf, sizeof(buf)) != 0)
		return NULL;
	return new_jstr(env, buf);
}

JNIEXPORT jboolean FN(networkIsUp)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return medut_network_is_up();
}

JNIEXPORT jint FN(pcapStart)(JNIEnv *env, jclass cls, jstring path)
{
	(void)cls;
	char *p = jstr_dup(env, path);
	int err = p ? medut_pcap_start(p) : MEDUT_ERR_INVAL;
	free(p);
	return err;
}

JNIEXPORT jint FN(pcapStop)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return medut_pcap_stop();
}
