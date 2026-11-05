/**
 * quickstart.cpp — register an account from a JSON config file and
 *                  make or receive one call.
 *
 * Build (Linux):
 *   g++ -std=c++17 quickstart.cpp medut.so -I. -o quickstart
 *
 * Usage:
 *   ./quickstart account.json                          # receive mode
 *   ./quickstart account.json bob@pbx.example.com      # dial
 *   ./quickstart alice@pbx.example.com secret          # legacy CLI mode (receive)
 *   ./quickstart alice@pbx.example.com secret bob@...  # legacy CLI mode (dial)
 *
 * Debug:
 *   MEDUT_DEBUG_INIT=1 ./quickstart account.json     # verbose init/shutdown trace
 *   $env:MEDUT_DEBUG_INIT=1; .\quickstart.exe ...    # PowerShell equivalent
 *
 * Minimal JSON account config (account.json):
 * {
 *   "enabled":      true,
 *   "uri":          "120@pbx.example.com",   // port optional: "120@pbx.example.com:5090"
 *   "password":     "secret",
 *   "display_name": "Extension 120",
 *   "transport":    "wss",                   // "udp" | "tcp" | "tls" | "ws" | "wss"
 *   "media_enc":    "dtls_srtp",             // "none" | "sdes" | "dtls_srtp"
 *   "ice_enabled":  true,
 *   "rtcp_mux":     true,
 *   "stun_server":  "stun:stun.l.google.com:19302",
 *   "verify_tls":   true,                    // false skips the certificate check
 *   "audio_codec":  "opus"                   // "opus" | "ulaw" | "alaw" | "g722" | "g726"
 * }
 *
 * server_url, outbound_proxy, server_host, server_port, auth_user are all
 * auto-derived from uri + transport. Port defaults: udp/tcp=5060, tls=5061,
 * ws=8088, wss=8089. Include a port in the uri to override: "120@host:443".
 *
 * Optional overrides (add to JSON when needed):
 *   "server_url":     "wss://pbx.example.com:443/ws"  // custom URL + path
 *   "outbound_proxy": "sip:proxy.example.com:5060;transport=udp"
 *   "auth_user":      "alice"
 *   "extra_headers":  { "X-Tenant-Id": "42" }
 *   "audio_codecs":   ["opus", "pcmu"]                // multiple codecs (array form)
 *   "rel100":         "enabled"                       // "disabled" | "enabled" | "required"
 */

#include "../medut.hpp"
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

/* ═══════════════════════════════════════════════════════════════════════════
 * Live in-call timer
 *
 * Talk time is kept on a steady_clock here rather than read from
 * medut_ev_media_stats_t::call_duration_ms: that figure only advances once
 * per stats_interval_ms and is not produced at all when stats are disabled, so
 * a clock built on it moves in jumps.  The SDK's figure is the one
 * to report; this one is the one to watch.
 *
 * The ticker owns a single terminal line and rewrites it in place with '\r'.
 * Event callbacks fire on the SDK's own thread and print freely, so everything
 * that writes while a call is up goes through say() — which clears the status
 * line first, or the two interleave into an unreadable mess.
 * ═══════════════════════════════════════════════════════════════════════════ */
namespace status {

std::mutex        out_mtx;
bool              line_shown = false;
std::atomic<bool> running{false};
/* Bumped by every start_timer().  A ticker thread exits as soon as its own
 * generation is stale, so a second call in the same session cannot end up with
 * two threads drawing the status line: without it, a thread asleep in its
 * one-second wait when the previous call ended sees `running` true again by the
 * time it wakes, and carries on alongside the new one. */
std::atomic<unsigned> generation{0};
std::chrono::steady_clock::time_point started;

inline std::string fmt_elapsed(long long secs)
{
    char buf[32];
    if (secs >= 3600)
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld",
                      secs / 3600, secs / 60 % 60, secs % 60);
    else
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", secs / 60, secs % 60);
    return buf;
}

/* std::cout that does not collide with the in-call status line. */
template <typename F>
void say(F&& emit)
{
    std::lock_guard<std::mutex> lk(out_mtx);
    if (line_shown) {
        std::cout << "\r\033[K";
        line_shown = false;
    }
    emit(std::cout);
    std::cout.flush();
}

inline void say_line(const std::string& text)
{
    say([&](std::ostream& os) { os << text << "\n"; });
}

/* Idempotent, so hold/resume does not restart the clock. */
inline void start_timer()
{
    if (running.exchange(true))
        return;
    started = std::chrono::steady_clock::now();
    const unsigned gen = ++generation;

    std::thread([gen]{
        while (running.load() && generation.load() == gen) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - started).count();
            std::lock_guard<std::mutex> lk(out_mtx);
            /* Re-check under the lock: stop_timer() clears the flag and then
             * wipes the line, and without this a tick already past the loop
             * condition could redraw over the cleared line afterwards. */
            if (!running.load() || generation.load() != gen)
                return;
            std::cout << "\rIn call \u00b7 " << fmt_elapsed(secs) << std::flush;
            line_shown = true;
        }
    }).detach();
}

/* Stops the ticker and returns total talk time in seconds, or -1 if never started. */
inline long long stop_timer()
{
    if (!running.exchange(false))
        return -1;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - started).count();
    say([](std::ostream&) {});   /* clear the status line */
    return secs;
}

} /* namespace status */

/* ═══════════════════════════════════════════════════════════════════════════
 * Minimal JSON parser — no external dependencies.
 * Supports: null, bool, number (integer), string, object, array.
 * Good enough for a flat-ish config file; not a general-purpose parser.
 * ═══════════════════════════════════════════════════════════════════════════ */
struct JsonValue {
    enum Type { Null, Bool, Int, Str, Obj, Arr } type = Null;
    bool                             b{};
    long long                        i{};
    std::string                      s;
    std::map<std::string, JsonValue> obj;
    std::vector<JsonValue>           arr;

    bool        is_null()   const { return type == Null; }
    bool        as_bool()   const { return (type == Bool) ? b : (type == Int && i != 0); }
    long long   as_int()    const { return (type == Int)  ? i : 0; }
    std::string as_str()    const { return (type == Str)  ? s : ""; }

    /** Return pointer to child value, or nullptr if missing / not an object. */
    const JsonValue* get(const std::string& key) const {
        if (type != Obj) return nullptr;
        auto it = obj.find(key);
        return (it != obj.end()) ? &it->second : nullptr;
    }
};

namespace json_detail {

    static void skip_ws(const char*& p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    }

    static std::string parse_string(const char*& p) {
        if (*p != '"') throw std::runtime_error("expected '\"'");
        ++p;
        std::string out;
        while (*p && *p != '"') {
            if (*p == '\\') {
                ++p;
                switch (*p) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    default:   out += *p;   break;
                }
            } else {
                out += *p;
            }
            ++p;
        }
        if (*p != '"') throw std::runtime_error("unterminated string");
        ++p;
        return out;
    }

    static JsonValue parse_value(const char*& p);

    static JsonValue parse_object(const char*& p) {
        if (*p != '{') throw std::runtime_error("expected '{'");
        ++p; skip_ws(p);
        JsonValue v; v.type = JsonValue::Obj;
        while (*p && *p != '}') {
            skip_ws(p);
            std::string key = parse_string(p);
            skip_ws(p);
            if (*p != ':') throw std::runtime_error("expected ':'");
            ++p; skip_ws(p);
            v.obj[key] = parse_value(p);
            skip_ws(p);
            if (*p == ',') { ++p; skip_ws(p); }
        }
        if (*p == '}') ++p;
        return v;
    }

    static JsonValue parse_array(const char*& p) {
        if (*p != '[') throw std::runtime_error("expected '['");
        ++p; skip_ws(p);
        JsonValue v; v.type = JsonValue::Arr;
        while (*p && *p != ']') {
            v.arr.push_back(parse_value(p));
            skip_ws(p);
            if (*p == ',') { ++p; skip_ws(p); }
        }
        if (*p == ']') ++p;
        return v;
    }

    static JsonValue parse_value(const char*& p) {
        skip_ws(p);
        if (*p == '{') return parse_object(p);
        if (*p == '[') return parse_array(p);
        if (*p == '"') {
            JsonValue v; v.type = JsonValue::Str; v.s = parse_string(p); return v;
        }
        if (strncmp(p, "null",  4) == 0) { p += 4; return JsonValue{}; }
        if (strncmp(p, "true",  4) == 0) { p += 4; JsonValue v; v.type = JsonValue::Bool; v.b = true;  return v; }
        if (strncmp(p, "false", 5) == 0) { p += 5; JsonValue v; v.type = JsonValue::Bool; v.b = false; return v; }
        /* integer */
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char* end;
            long long n = strtoll(p, &end, 10);
            JsonValue v; v.type = JsonValue::Int; v.i = n; p = end; return v;
        }
        throw std::runtime_error(std::string("unexpected character: ") + *p);
    }

} // namespace json_detail

static JsonValue parse_json(const std::string& src) {
    const char* p = src.c_str();
    json_detail::skip_ws(p);
    return json_detail::parse_value(p);
}

static JsonValue load_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_json(ss.str());
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Helper: safely extract a C string from a std::string that must outlive the
 * medut_account_config_t that references it.
 * ═══════════════════════════════════════════════════════════════════════════ */
static const char* cstr_or_null(const std::string& s) {
    return s.empty() ? nullptr : s.c_str();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * AccountConfig — owns all std::string storage referenced by the C struct.
 * ═══════════════════════════════════════════════════════════════════════════ */
struct AccountConfig {
    /* ── string storage (must outlive acfg) ── */
    std::string uri, password, display_name, auth_user;
    std::string server_url, server_host;
    std::string outbound_proxy;
    std::string stun_server, turn_server, turn_user, turn_pass;
    std::map<std::string, std::string> extra_headers;
    std::string rel100_str;
    std::vector<std::string> audio_codecs;

    /* ── parsed scalars ── */
    bool        enabled     = true;
    int         server_port = 0;

    medut_transport_t    transport   = MEDUT_TRANSPORT_WSS;
    medut_media_enc_t    media_enc   = MEDUT_MEDIA_ENC_DTLS_SRTP;
    bool                   ice_enabled = true;
    bool                   rtcp_mux    = false;
    bool                   rtcp_mux_set = false;
    bool                   verify_tls  = true;
    medut_100rel_mode_t  rel100      = MEDUT_100REL_DISABLED;
    medut_dtmf_mode_t    dtmf_mode   = MEDUT_DTMF_RFC4733;

    /* ── the C struct; populated by build() ── */
    medut_account_config_t acfg{};

    /* ── parse a transport string ── */
    static medut_transport_t parse_transport(const std::string& s) {
        if (s == "udp") return MEDUT_TRANSPORT_UDP;
        if (s == "tcp") return MEDUT_TRANSPORT_TCP;
        if (s == "tls") return MEDUT_TRANSPORT_TLS;
        if (s == "ws")  return MEDUT_TRANSPORT_WS;
        if (s == "wss") return MEDUT_TRANSPORT_WSS;
        throw std::runtime_error("unknown transport: " + s);
    }

    /* ── parse a media_enc string ── */
    static medut_media_enc_t parse_media_enc(const std::string& s) {
        if (s == "none")      return MEDUT_MEDIA_ENC_NONE;
        if (s == "sdes")      return MEDUT_MEDIA_ENC_SDES;
        if (s == "dtls_srtp") return MEDUT_MEDIA_ENC_DTLS_SRTP;
        throw std::runtime_error("unknown media_enc: " + s);
    }

    /* ── parse a 100rel string ── */
    static medut_100rel_mode_t parse_100rel(const std::string& s) {
        if (s == "disabled") return MEDUT_100REL_DISABLED;
        if (s == "enabled")  return MEDUT_100REL_ENABLED;
        if (s == "required") return MEDUT_100REL_REQUIRED;
        throw std::runtime_error("unknown rel100 value: " + s);
    }

    /* ── parse a dtmf_mode string ── */
    static medut_dtmf_mode_t parse_dtmf_mode(const std::string& s) {
        if (s == "rfc4733")  return MEDUT_DTMF_RFC4733;
        if (s == "sip_info") return MEDUT_DTMF_SIP_INFO;
        if (s == "auto")     return MEDUT_DTMF_AUTO;
        throw std::runtime_error("unknown dtmf_mode value: " + s);
    }

    /* ── construct from CLI args (legacy mode) ── */
    void from_cli(const char* sip_uri, const char* pw) {
        uri      = sip_uri;
        password = pw;

        /* derive server_url from URI host */
        std::string u = uri;
        if (u.rfind("sip:", 0) == 0) u = u.substr(4);
        auto at  = u.find('@');
        std::string host = (at != std::string::npos) ? u.substr(at + 1) : u;
        auto colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);

        server_url   = "wss://" + host + ":443/";
        display_name = (at != std::string::npos) ? u.substr(0, at) : u;
        transport    = MEDUT_TRANSPORT_WSS;
        media_enc    = MEDUT_MEDIA_ENC_DTLS_SRTP;
        ice_enabled  = true;
        stun_server  = "stun:stun.l.google.com:19302";
        verify_tls   = true;
    }

    /* ── construct from JSON ── */
    void from_json(const JsonValue& j) {
        auto str = [&](const char* key) -> std::string {
            const auto* v = j.get(key);
            return (v && !v->is_null()) ? v->as_str() : "";
        };
        auto flag = [&](const char* key, bool def) -> bool {
            const auto* v = j.get(key);
            return v ? v->as_bool() : def;
        };
        auto num = [&](const char* key, int def) -> int {
            const auto* v = j.get(key);
            return (v && v->type == JsonValue::Int) ? (int)v->as_int() : def;
        };

        enabled      = flag("enabled", true);
        uri          = str("uri");
        password     = str("password");
        display_name = str("display_name");
        auth_user    = str("auth_user");

        {
            std::string t = str("transport");
            transport = t.empty() ? MEDUT_TRANSPORT_WSS : parse_transport(t);
        }
        server_url   = str("server_url");
        server_host  = str("server_host");
        server_port  = num("server_port", 0);
        outbound_proxy = str("outbound_proxy");

        {
            std::string m = str("media_enc");
            media_enc = m.empty() ? MEDUT_MEDIA_ENC_DTLS_SRTP : parse_media_enc(m);
        }
        ice_enabled  = flag("ice_enabled", true);
        {
            const auto* v = j.get("rtcp_mux");
            if (v && !v->is_null()) { rtcp_mux = v->as_bool(); rtcp_mux_set = true; }
        }
        stun_server  = str("stun_server");
        turn_server  = str("turn_server");
        turn_user    = str("turn_user");
        turn_pass    = str("turn_pass");
        verify_tls   = flag("verify_tls", true);

        {
            std::string r = str("rel100");
            rel100 = r.empty() ? MEDUT_100REL_DISABLED : parse_100rel(r);
        }

        {
            std::string d = str("dtmf_mode");
            dtmf_mode = d.empty() ? MEDUT_DTMF_RFC4733 : parse_dtmf_mode(d);
        }

        /* extra_headers object */
        const auto* hdr_node = j.get("extra_headers");
        if (hdr_node && hdr_node->type == JsonValue::Obj) {
            for (const auto& kv : hdr_node->obj)
                extra_headers[kv.first] = kv.second.as_str();
        }

        /* audio_codecs — array or single string, e.g. ["opus", "pcmu"] or "opus" */
        const auto* codecs_node = j.get("audio_codecs");
        if (codecs_node) {
            if (codecs_node->type == JsonValue::Arr) {
                for (const auto& codec : codecs_node->arr) {
                    std::string name = codec.as_str();
                    if (!name.empty())
                        audio_codecs.push_back(name);
                }
            } else if (codecs_node->type == JsonValue::Str) {
                std::string name = codecs_node->as_str();
                if (!name.empty())
                    audio_codecs.push_back(name);
            }
        }

        /* audio_codec — singular alias; used when audio_codecs is absent */
        if (audio_codecs.empty()) {
            const auto* single = j.get("audio_codec");
            if (single && !single->is_null()) {
                std::string name = single->as_str();
                if (!name.empty()) audio_codecs.push_back(name);
            }
        }
    }

    /* ── populate acfg from owned strings ── */
    void build() {
        acfg = {};
        acfg.uri          = cstr_or_null(uri);
        acfg.password     = cstr_or_null(password);
        acfg.display_name = cstr_or_null(display_name);
        acfg.auth_user    = cstr_or_null(auth_user);
        acfg.transport    = transport;
        acfg.server_url   = cstr_or_null(server_url);
        acfg.server_host  = cstr_or_null(server_host);
        acfg.outbound_proxy = cstr_or_null(outbound_proxy);
        if (server_port > 0) acfg.server_port = (uint16_t)server_port;
        acfg.media_enc    = media_enc;
        acfg.ice_enabled  = ice_enabled;
        acfg.rtcp_mux     = rtcp_mux;
        acfg.rtcp_mux_set = rtcp_mux_set;
        acfg.stun_server  = cstr_or_null(stun_server);
        acfg.turn_server  = cstr_or_null(turn_server);
        acfg.turn_user    = cstr_or_null(turn_user);
        acfg.turn_pass    = cstr_or_null(turn_pass);
        acfg.verify_tls   = verify_tls;
        acfg.verify_tls_set = true;
        acfg.dtmf_mode    = dtmf_mode;

        /* Per-account codec override via audio_codec_names[] */
        if (!audio_codecs.empty()) {
            int n = std::min((int)audio_codecs.size(), 8);
            for (int i = 0; i < n; i++) {
                strncpy(acfg.audio_codec_names[i], audio_codecs[i].c_str(), 31);
                acfg.audio_codec_names[i][31] = '\0';
            }
            acfg.audio_codec_name_count = n;
        }
    }

    void dump() const {
        auto tf = [](bool v){ return v ? "true" : "false"; };
        auto transport_name = [](medut_transport_t t) -> const char* {
            switch(t) {
                case MEDUT_TRANSPORT_UDP: return "udp";
                case MEDUT_TRANSPORT_TCP: return "tcp";
                case MEDUT_TRANSPORT_TLS: return "tls";
                case MEDUT_TRANSPORT_WS:  return "ws";
                case MEDUT_TRANSPORT_WSS: return "wss";
                default: return "?";
            }
        };
        auto enc_name = [](medut_media_enc_t e) -> const char* {
            switch(e) {
                case MEDUT_MEDIA_ENC_NONE:      return "none";
                case MEDUT_MEDIA_ENC_SDES:      return "sdes";
                case MEDUT_MEDIA_ENC_DTLS_SRTP: return "dtls_srtp";
                default: return "?";
            }
        };
        std::cout
        << "Account config:\n"
        << "  uri          : " << uri          << "\n"
        << "  display_name : " << display_name << "\n"
        << "  auth_user    : " << (auth_user.empty() ? "(from uri)" : auth_user) << "\n"
        << "  transport    : " << transport_name(transport)  << "\n"
         << "  server_url   : " << (server_url.empty()  ? "(none)" : server_url)  << "\n"
         << "  server_host  : " << (server_host.empty() ? "(none)" : server_host) << "\n"
         << "  server_port  : " << server_port  << "\n"
         << "  outbound_proxy: " << (outbound_proxy.empty() ? "(auto)" : outbound_proxy) << "\n"
         << "  media_enc    : " << enc_name(media_enc)        << "\n"
        << "  ice_enabled  : " << tf(ice_enabled)            << "\n"
        << "  rtcp_mux     : " << (rtcp_mux_set ? tf(rtcp_mux) : "(global)") << "\n"
        << "  stun_server  : " << (stun_server.empty() ? "(none)" : stun_server) << "\n"
        << "  turn_server  : " << (turn_server.empty() ? "(none)" : turn_server) << "\n"
        << "  verify_tls   : " << tf(verify_tls)             << "\n";
        if (!audio_codecs.empty()) {
            std::cout << "  audio_codecs : ";
            for (size_t i = 0; i < audio_codecs.size(); i++) {
                if (i > 0) std::cout << ", ";
                std::cout << audio_codecs[i];
            }
            std::cout << "\n";
        } else {
            std::cout << "  audio_codecs : (global SDK list)\n";
        }
        if (!extra_headers.empty()) {
            std::cout << "  extra_headers:\n";
            for (const auto& kv : extra_headers)
                std::cout << "    " << kv.first << ": " << kv.second << "\n";
        }
    }
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Media stats printer
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_stats(const medut_ev_media_stats_t& s)
{
    /* Nothing to say before the codec is negotiated: the panel would read
     * "? 0 kHz ch=0" with a flush count from a buffer that has carried no
     * media.  The first real tick follows within stats_interval_ms. */
    if (!s.codec_name) return;

    const char* method = (s.mos_method == MEDUT_MOS_EMODEL) ? "E-model" : "simplified";
    std::cout
    << "┌─ Media Stats ─────────────────────────────────\n"
    << "│  Codec     : " << (s.codec_name ? s.codec_name : "?")
    <<   "  " << s.codec_clock_rate / 1000 << " kHz"
    <<   "  ch=" << (int)s.codec_channels
    <<   "  PT=" << s.payload_type << "\n"
    << "│  Remote    : " << s.remote_addr
    <<   "  SSRC rx=" << s.ssrc_rx << "  tx=" << s.ssrc_tx << "\n"
    << "│  Packets   : tx=" << s.packets_sent
    <<   "  rx=" << s.packets_received
    <<   "  lost_tx=" << s.packets_lost << " (" << s.loss_pct << "% last RR)"
    <<   "  lost_rx=" << s.packets_lost_rx << " (" << s.loss_pct_rx << "% window)\n"
    << "│  Bandwidth : tx=" << s.bandwidth_kbps_tx << " kbps"
    <<   "  rx=" << s.bandwidth_kbps_rx << " kbps"
    <<   "  (avg tx=" << s.avg_bandwidth_kbps_tx
    <<   "  rx=" << s.avg_bandwidth_kbps_rx << ")\n"
    << "│  Delay     : RTT=" << s.rtt_ms << " ms"
    <<   "  jitter=" << s.jitter_ms << " ms"
    <<   "  tx_jitter=" << s.tx_jitter_ms << " ms\n"
    << "│  Jitter buf: depth=" << s.jitter_buffer_ms << " ms"
    <<   "  load=" << s.jitter_buffer_load
    <<   "  late=" << s.late_packets
    <<   "  discarded=" << s.discarded_packets << "\n"
    << "│  FEC/PLC   : fec_tx=" << (s.fec_active ? "on " : "off")
    <<   " " << s.fec_level_pct << "%"
    <<   "  plc_frames=" << s.plc_frames
    <<   "  fec_recovered=" << s.fec_recovered
    <<   "  plc_concealed=" << s.plc_concealed << "\n"
    << "│  MOS (" << method << "): LQ=" << s.mos_lq << "  CQ=" << s.mos_cq << "\n";
    std::cout << "│  Speaker   : "
    << (std::isnan(s.audio_level_dbov) ? "---" : std::to_string(s.audio_level_dbov) + " dBov")
    << "  Mic       : "
    << (std::isnan(s.mic_level_dbov)   ? "---" : std::to_string(s.mic_level_dbov)   + " dBov")
    << "\n";
    std::cout << "└───────────────────────────────────────────────\n";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Audio device listing
 * ═══════════════════════════════════════════════════════════════════════════ */
static void print_devices(Medut::SDK& sdk)
{
    medut_audio_device_t devs[32];
    int n = medut_audio_list_input_devices(devs, 32);
    if (n > 0) {
        std::cout << "Input devices (" << n << "):\n";
        for (int i = 0; i < n; i++)
            std::cout << "  [" << i << "] " << devs[i].name
            << (devs[i].is_default ? "  *default*" : "") << "\n";
    }
    n = medut_audio_list_output_devices(devs, 32);
    if (n > 0) {
        std::cout << "Output devices (" << n << "):\n";
        for (int i = 0; i < n; i++)
            std::cout << "  [" << i << "] " << devs[i].name
            << (devs[i].is_default ? "  *default*" : "") << "\n";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Detect whether the first argument is a JSON file path.
 * Heuristic: ends with ".json" OR starts with '{'.
 * ═══════════════════════════════════════════════════════════════════════════ */
static bool looks_like_json_path(const char* arg) {
    std::string s(arg);
    if (s.size() > 5 && s.substr(s.size() - 5) == ".json") return true;
    /* also allow a raw JSON string passed directly */
    if (!s.empty() && s[0] == '{') return true;
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) {
        std::cerr
        << "Usage:\n"
        << "  " << argv[0] << " account.json [callee-uri]\n"
        << "  " << argv[0] << " <sip-uri> <password> [callee-uri]\n";
        return 1;
    }

    /* ── Parse config ─────────────────────────────────────────────────── */
    AccountConfig cfg;
    const char*   callee = nullptr;

    if (looks_like_json_path(argv[1])) {
        /* JSON mode */
        try {
            JsonValue j;
            std::string arg1(argv[1]);
            if (!arg1.empty() && arg1[0] == '{')
                j = parse_json(arg1);          /* raw JSON string on CLI */
                else
                    j = load_json_file(arg1);      /* file path */
                    cfg.from_json(j);
        } catch (const std::exception& e) {
            std::cerr << "Failed to load account config: " << e.what() << "\n";
            return 1;
        }
        if (argc >= 3) callee = argv[2];
    } else {
        /* Legacy: sip-uri password [callee] */
        if (argc < 3) {
            std::cerr << "usage: " << argv[0]
            << " <sip-uri> <password> [<callee-uri>]\n";
            return 1;
        }
        cfg.from_cli(argv[1], argv[2]);
        if (argc >= 4) callee = argv[3];
    }

    if (!cfg.enabled) {
        std::cerr << "Account is disabled in config. Exiting.\n";
        return 0;
    }
    if (cfg.uri.empty()) {
        std::cerr << "No URI in config.\n";
        return 1;
    }

    cfg.build();
    cfg.dump();

    /* ── State shared between main and event callback ─────────────────── */
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    registered       = false;
    bool                    reg_ok           = false;
    bool                    call_established = false;
    bool                    call_done        = false;
    bool                    incoming_call    = false;
    Medut::Call           active_call;

    /* ── SDK setup ────────────────────────────────────────────────────── */
    Medut::SDK sdk;
    sdk.config().log_level         = 0;  /* errors only; 3 = debug for audio troubleshooting */
    sdk.config().trace_sip         = false;
    sdk.config().prefer_ipv6       = false;
    /* Desktop audio.  WebRTC AEC3 cancels echo while both sides talk; on a
     * build or machine without it, init falls back to the half-duplex
     * suppressor.  A raw laptop mic also wants noise suppression and AGC. */
    sdk.config().aec_mode          = MEDUT_AEC_WEBRTC;
    sdk.config().ns                = true;
    sdk.config().agc               = true;
    sdk.config().audio_codecs[0]   = MEDUT_CODEC_OPUS;
    sdk.config().audio_codecs[1]   = MEDUT_CODEC_PCMU;
    sdk.config().audio_codecs[2]   = MEDUT_CODEC_PCMA;
    sdk.config().audio_codecs[3]   = MEDUT_CODEC_G722;
    sdk.config().audio_codec_count = 4;

    sdk.on_event([&](const medut_event_t& ev) {
        switch (ev.type) {

            case MEDUT_EV_REG_STATE:
                if (ev.u.reg.state == MEDUT_REG_REGISTERED) {
                    std::cout << "Registered OK.\n";
                    std::lock_guard<std::mutex> lk(mtx);
                    registered = true;
                    reg_ok     = true;
                    cv.notify_one();
                } else if (ev.u.reg.state == MEDUT_REG_RECONNECTING) {
                    /* Transient — the SDK is retrying by itself, so report it
                     * and keep waiting rather than unblocking main(). */
                    const char* detail = ev.u.reg.error_str
                        ? ev.u.reg.error_str
                        : medut_strerror(ev.u.reg.error);
                    std::cout << "Reconnecting: " << detail;
                    if (ev.u.reg.retry_attempt)
                        std::cout << " (attempt " << ev.u.reg.retry_attempt
                                  << " in " << ev.u.reg.retry_delay_ms
                                  << " ms)";
                    std::cout << "\n";
                } else if (ev.u.reg.state == MEDUT_REG_FAILED) {
                    const char* detail = ev.u.reg.error_str
                        ? ev.u.reg.error_str
                        : medut_strerror(ev.u.reg.error);
                    std::cerr << "Registration failed: " << detail << "\n";
                    std::lock_guard<std::mutex> lk(mtx);
                    registered = true; /* unblock main */
                    cv.notify_one();
                }
                break;

            case MEDUT_EV_INCOMING_CALL:
                std::cout << "\n=== Incoming call from "
                << (ev.u.incoming.from_uri ? ev.u.incoming.from_uri : "unknown")
                << " ===\n"
                << "Press 'a' + Enter to answer, 'r' + Enter to reject\n";
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    active_call   = Medut::Call(ev.u.incoming.call);
                    incoming_call = true;
                    cv.notify_one();
                }
                break;

            case MEDUT_EV_CALL_STATE: {
                static const char* kStateNames[] = {
                    "CALLING","RINGING","ESTABLISHED","HELD","ENDED","CANCELLED","FAILED"
                };
                int si = ev.u.call_state.state;
                const char* sname = (si >= 0 && si <= 6) ? kStateNames[si] : "?";
                status::say([&](std::ostream& os) {
                    os << "Call state: " << sname << " (" << si << ")";
                    if (ev.u.call_state.reason && ev.u.call_state.reason[0])
                        os << "  reason=\"" << ev.u.call_state.reason << "\"";
                    if (ev.u.call_state.error != MEDUT_OK)
                        os << "  error=" << ev.u.call_state.error;
                    os << "\n";
                });

                if (ev.u.call_state.state == MEDUT_CALL_ESTABLISHED) {
                    /* Talk time starts when the far end answers, not when we
                     * dialled — that is the number a user expects to see. */
                    status::start_timer();
                    std::lock_guard<std::mutex> lk(mtx);
                    call_established = true;
                    cv.notify_one();
                } else if (ev.u.call_state.state == MEDUT_CALL_ENDED  ||
                    ev.u.call_state.state == MEDUT_CALL_FAILED ||
                    ev.u.call_state.state == MEDUT_CALL_CANCELLED) {
                    long long talked = status::stop_timer();
                    if (talked >= 0)
                        status::say_line("Call ended after " +
                                         status::fmt_elapsed(talked) + ".");
                    std::lock_guard<std::mutex> lk(mtx);
                call_done = true;
                cv.notify_one();
                    }
                    break;
            }

            case MEDUT_EV_TRANSFER_REQUEST: {
                std::cout << "=== Transfer request: REFER to "
                << (ev.u.transfer_req.refer_to_uri ? ev.u.transfer_req.refer_to_uri : "?")
                << (ev.u.transfer_req.has_replaces ? "  [attended]" : "  [blind]") << "\n";
                /* Answer it — the far end is waiting for the NOTIFY that says
                 * what happened, and only accept/reject sends one.  Accepting
                 * keeps the new call linked to this one so the SDK reports the
                 * outcome; hanging up and dialling would not. */
                medut_call_handle_t moved = nullptr;
                if (medut_call_transfer_accept(ev.u.transfer_req.call, &moved)
                    == MEDUT_OK) {
                    std::cout << "    following the transfer\n";
                }
                else {
                    medut_call_transfer_reject(ev.u.transfer_req.call,
                                                 603, "Declined");
                }
                break;
            }

            case MEDUT_EV_MEDIA_STATS:
                /* The SDK polls every 2 s (stats_interval_ms) because FEC,
                 * bitrate adaptation and quality alerts all run off that
                 * tick; print every third one to keep the console readable. */
                if (ev.u.stats.is_final || ev.u.stats.stats_tick % 3 == 1)
                    print_stats(ev.u.stats);
                break;

            case MEDUT_EV_LOG:
                if (ev.u.log.message)
                    std::cerr << "[sdk] " << ev.u.log.message;
            break;

            case MEDUT_EV_SIP_TRACE:
                std::cout << (ev.u.sip_trace.dir == MEDUT_MEDIA_DIR_TX ? ">>>\n" : "<<<\n")
                << ev.u.sip_trace.raw_message << "\n---\n";
                break;

            default: break;
        }
    });

    /* ── Register ─────────────────────────────────────────────────────── */
    auto account = sdk.create_account(cfg.acfg);

    /* Optional per-account tuning */
    for (const auto& kv : cfg.extra_headers)
        medut_account_add_header(account.handle(), kv.first.c_str(), kv.second.c_str());

    if (cfg.rel100 != MEDUT_100REL_DISABLED)
        medut_account_set_100rel(account.handle(), cfg.rel100);

    account.register_account();

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(15), [&]{ return registered; });
    }

    print_devices(sdk);

    /* ── Dial or wait ─────────────────────────────────────────────────── */
    if (!reg_ok) return 1;

    if (callee) {
        std::string callee_uri = callee;
        if (callee_uri.rfind("sip:", 0) != 0)
            callee_uri = "sip:" + callee_uri;
        if (callee_uri.find('@') == std::string::npos) {
            std::string domain = cfg.uri;
            if (domain.rfind("sip:", 0) == 0) domain = domain.substr(4);
            size_t at_pos = domain.find('@');
            if (at_pos != std::string::npos) domain = domain.substr(at_pos + 1);
            callee_uri = callee_uri + "@" + domain;
        }
        std::cout << "Dialling " << callee_uri << " ...\n";
        active_call = account.call(callee_uri.c_str());

        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(30),
                    [&]{ return call_established || call_done; });
        if (!call_established) return 0;

        lk.unlock();
        status::say_line("In call \u00b7 0:00   Keys: h=hangup  o=hold  r=resume  "
                         "m=mute  u=unmute  t=transfer");

        std::thread input_thread([&]{
            char c;
            while (std::cin.get(c)) {
                if      (c == 'h' || c == 'H') { active_call.hangup();          break; }
                else if (c == 'o' || c == 'O') { try { active_call.hold();   status::say_line("On hold.");  } catch (...) {} }
                else if (c == 'r' || c == 'R') { try { active_call.resume(); status::say_line("Resumed.");  } catch (...) {} }
                else if (c == 'm' || c == 'M') { try { active_call.mute(true);  status::say_line("Muted.");   } catch (...) {} }
                else if (c == 'u' || c == 'U') { try { active_call.mute(false); status::say_line("Unmuted."); } catch (...) {} }
                else if (c == 't' || c == 'T') {
                    std::string dest;
                    status::say([](std::ostream& os) { os << "Transfer to URI: "; });
                    std::getline(std::cin, dest);
                    if (!dest.empty()) {
                        try { active_call.transfer(dest); status::say_line("Transfer sent."); }
                        catch (const std::exception& e) { status::say_line(std::string("transfer failed: ") + e.what()); }
                    }
                }
            }
        });
        input_thread.detach();

        std::unique_lock<std::mutex> lk2(mtx);
        cv.wait(lk2, [&]{ return call_done; });

    } else {
        std::cout << "Waiting for incoming call (30 s)...\n";
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(30),
                    [&]{ return incoming_call || call_done; });

        if (incoming_call) {
            lk.unlock();
            char choice = '\0';
            while (std::cin.get(choice)) {
                std::cin.ignore();
                if (choice == 'a' || choice == 'A') {
                    try { active_call.answer(); }
                    catch (const std::exception& e) {
                        std::cerr << "answer failed: " << e.what() << "\n";
                    }
                    break;
                } else if (choice == 'r' || choice == 'R') {
                    active_call.hangup();
                    break;
                }
            }
            lk.lock();
            cv.wait_for(lk, std::chrono::seconds(60), [&]{ return call_done; });
        }
    }

    return 0;
}