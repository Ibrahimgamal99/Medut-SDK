"""
EchoSDK — simple, flat Python SDK for SIP/RTP.

    import echo_sdk as sdk

    sdk.configure(transport="wss", media_enc="dtls_srtp")

    alice = sdk.create_account("alice@pbx.example.com", "secret")
    alice.register()

    @sdk.on("registered")
    def _(ev):
        sdk.call("120")

    @sdk.on("incoming_call")
    def _(ev):
        ev.call.answer()

    @sdk.on("ended")
    def _(ev):
        sdk.stop()

    sdk.run()
"""

import logging
import queue
import signal
import sys
import threading
from typing import Callable, Optional

from ._loader import ffi, lib
from .events import (
    IceStateEvent,
    RegStateEvent, IncomingCallEvent, CallStateEvent, CallDtmfEvent,
    SdpNegotiationEvent, SipTraceEvent, MediaStatsEvent, LogEvent,
    RegistrarWarningEvent, TransferRequestEvent, TransferFailedEvent, MwiEvent,
    MessageEvent, PresenceStateEvent, QualityAlertEvent, NetworkEvent,
    CallStats,
)

_log = logging.getLogger("echo_sdk")

# ── Push provider constants (no string equivalent in the C API) ───────────────
PUSH_PROVIDER_NONE         = 0
PUSH_PROVIDER_APNS         = 1
PUSH_PROVIDER_APNS_SANDBOX = 2
PUSH_PROVIDER_FCM          = 3

# ── Valid names for @sdk.on() ─────────────────────────────────────────────────
_VALID_NAMES = frozenset({
    # umbrella
    "reg_state", "call_state",
    # reg sub-states
    "registering", "registered", "unregistered", "reg_failed", "reconnecting",
    # call sub-states
    "calling", "ringing", "established", "held", "ended", "cancelled", "call_failed",
    # direct events
    "incoming_call", "dtmf", "sdp_negotiation", "sip_trace", "media_stats",
    "log", "registrar_warning", "transfer_request", "transfer_failed",
    "mwi", "message",
    "presence_state", "quality_alert", "network",
    # wildcard
    "*",
})

# ── String → int maps ─────────────────────────────────────────────────────────
_STR_TRANSPORT = {"udp": 0, "tcp": 1, "tls": 2, "ws": 3, "wss": 4}
_STR_MEDIA_ENC = {"none": 0, "sdes": 1, "dtls_srtp": 2}
_STR_REL100    = {"disabled": 0, "enabled": 1, "required": 2}
_STR_DTMF      = {"rfc4733": 0, "sip_info": 1, "auto": 2}

# ── Module-level state ────────────────────────────────────────────────────────
_config: dict        = {}
_config_locked       = False
_init_done           = False
_init_lock           = threading.Lock()
_handlers: dict      = {}            # name → list[Callable]
_accounts: list      = []
_accounts_by_handle: dict = {}       # int(cdata handle) → Account
_accounts_lock       = threading.Lock()
_event_q             = queue.SimpleQueue()
_dispatcher: Optional[threading.Thread] = None
_stop_evt            = threading.Event()
_cdata_keepalive: list = []          # keep C char[] / struct refs alive


# ── Helpers ───────────────────────────────────────────────────────────────────

def _s(cptr) -> Optional[str]:
    if cptr == ffi.NULL:
        return None
    return ffi.string(cptr).decode("utf-8", errors="replace")


def _str_array(arr) -> list:
    result, i = [], 0
    while arr[i] != ffi.NULL:
        result.append(_s(arr[i]))
        i += 1
    return result


def _check(rc, what):
    if rc != 0:
        raise RuntimeError(f"EchoSDK.{what} failed (code {rc}): {strerror(rc)}")


def _ensure_init():
    global _init_done, _config_locked
    if _init_done:
        return
    with _init_lock:
        if _init_done:
            return
        _config_locked = True
        try:
            sys.stdout.reconfigure(line_buffering=True)
        except AttributeError:
            pass
        cfg = ffi.new("echosdk_config_t *")
        lib.echosdk_config_init(cfg)
        # configure() documents the enum options in their string form, and
        # create_account() already translates the global "transport" that way.
        # Without the same translation here the string reaches setattr() as a
        # char[] destined for an int field, which cffi rejects outright — so
        # configure(transport="udp") raised a TypeError from inside
        # _ensure_init() rather than doing what it says.
        _config_enums = {"transport": _STR_TRANSPORT, "media_enc": _STR_MEDIA_ENC}
        for key, table in _config_enums.items():
            if isinstance(_config.get(key), str):
                _config[key] = table[_config[key]]
        for key, val in _config.items():
            if isinstance(val, str):
                ref = ffi.new("char[]", val.encode())
                _cdata_keepalive.append(ref)
                setattr(cfg, key, ref)
            elif isinstance(val, bool):
                setattr(cfg, key, int(val))
            else:
                setattr(cfg, key, val)
        cfg.event_cb       = _global_event_cb
        cfg.event_userdata = ffi.NULL
        _cdata_keepalive.append(cfg)  # C may hold pointers into this struct
        _check(lib.echosdk_init(cfg), "init")
        _init_done = True


def _raw_stats_to_event(s) -> MediaStatsEvent:
    return MediaStatsEvent(
        call=None,
        packets_sent            = s.packets_sent,
        packets_received        = s.packets_received,
        packets_lost            = s.packets_lost,
        packets_lost_rx         = s.packets_lost_rx,
        bytes_sent              = s.bytes_sent,
        bytes_received          = s.bytes_received,
        tx_errors               = s.tx_errors,
        rx_errors               = s.rx_errors,
        loss_pct                = s.loss_pct,
        loss_pct_rx             = s.loss_pct_rx,
        jitter_ms               = s.jitter_ms,
        tx_jitter_ms            = s.tx_jitter_ms,
        rtt_ms                  = s.rtt_ms,
        jitter_buffer_ms        = s.jitter_buffer_ms,
        jitter_buffer_load      = s.jitter_buffer_load,
        late_packets            = s.late_packets,
        discarded_packets       = s.discarded_packets,
        jitter_buffer_target_ms = s.jitter_buffer_target_ms,
        jitter_buffer_adaptive  = bool(s.jitter_buffer_adaptive),
        plc_frames              = s.plc_frames,
        plc_ratio               = s.plc_ratio,
        bandwidth_kbps_tx       = s.bandwidth_kbps_tx,
        bandwidth_kbps_rx       = s.bandwidth_kbps_rx,
        avg_bandwidth_kbps_tx   = s.avg_bandwidth_kbps_tx,
        avg_bandwidth_kbps_rx   = s.avg_bandwidth_kbps_rx,
        mos_lq                  = s.mos_lq,
        mos_cq                  = s.mos_cq,
        mos_lq_rx               = s.mos_lq_rx,
        mos_cq_rx               = s.mos_cq_rx,
        mos_method              = s.mos_method,
        codec_name              = _s(s.codec_name) or "",
        codec_clock_rate        = s.codec_clock_rate,
        codec_sample_rate       = s.codec_sample_rate,
        codec_channels          = s.codec_channels,
        payload_type            = s.payload_type,
        audio_level_dbov        = s.audio_level_dbov,
        mic_level_dbov          = s.mic_level_dbov,
        ssrc_tx                 = s.ssrc_tx,
        ssrc_rx                 = s.ssrc_rx,
        remote_addr             = ffi.string(s.remote_addr).decode("utf-8", errors="replace"),
        mos_lq_min              = s.mos_lq_min,
        mos_lq_avg              = s.mos_lq_avg,
        stats_tick              = s.stats_tick,
        call_duration_ms        = s.call_duration_ms,
        is_final                = bool(s.is_final),
        ice_state               = _ICE_STATE[s.ice_state] if 0 <= s.ice_state < len(_ICE_STATE) else "new",
        ice_local_type          = _ICE_CAND[s.ice_local_type] if 0 <= s.ice_local_type < len(_ICE_CAND) else "none",
        ice_remote_type         = _ICE_CAND[s.ice_remote_type] if 0 <= s.ice_remote_type < len(_ICE_CAND) else "none",
        ice_rtt_ms              = s.ice_rtt_ms,
        ice_generation          = s.ice_generation,
        ice_local_addr          = _s(s.ice_local_addr) or "",
        ice_remote_addr         = _s(s.ice_remote_addr) or "",
        fec_active              = bool(s.fec_active),
        fec_level_pct           = s.fec_level_pct,
    )


def _list_audio_devices(fn) -> list:
    buf = ffi.new("echosdk_audio_device_t[32]")
    n = fn(buf, 32)
    out = []
    for i in range(max(n, 0)):
        out.append({
            "name":        ffi.string(buf[i].name).decode("utf-8", errors="replace"),
            "description": ffi.string(buf[i].description).decode("utf-8", errors="replace"),
            "is_default":  bool(buf[i].is_default),
        })
    return out


# ── C event callback → global queue ──────────────────────────────────────────

@ffi.callback("void(const echosdk_event_t *, void *)")
def _global_event_cb(ev_ptr, _userdata):
    ev  = ev_ptr[0]
    typ = ev.type

    try:
        # Index by the C enum value, so "reconnecting" sits at 5 where the
        # header appends it — not next to "failed", where it reads better.
        _REG_STATES  = ("unregistered", "registering", "registered",
                        "failed", "unregistering", "reconnecting")
        _CALL_STATES = ("calling", "ringing", "established", "held",
                        "ended", "cancelled", "failed")
        _PRESENCE    = ("unknown", "open", "closed", "busy")
        _QUALITY     = ("mos", "loss", "jitter", "rtt", "media_stall")
        _NET_STAGE   = ("change_detected", "down", "up", "transport_reset",
                        "reregistering", "call_migrating",
                        "call_migrate_accepted", "call_migrated",
                        "call_migration_failed", "call_deferred",
                        "handover_failed", "call_ice_stale",
                        "call_ice_failed")

        acct_handle = ffi.NULL
        call_handle = ffi.NULL

        if typ == 0:    # LOG
            obj = LogEvent(message=_s(ev.u.log.message) or "")

        elif typ == 1:  # REG_STATE
            acct_handle = ev.u.reg.account
            obj = RegStateEvent(
                state         = _REG_STATES[ev.u.reg.state],
                error         = ev.u.reg.error,
                error_str     = _s(ev.u.reg.error_str),
                retry_attempt = ev.u.reg.retry_attempt,
                retry_delay_ms= ev.u.reg.retry_delay_ms,
            )

        elif typ == 2:  # INCOMING_CALL
            acct_handle = ev.u.incoming.account
            call_handle = ev.u.incoming.call
            obj = IncomingCallEvent(
                call=None,
                from_uri    = _s(ev.u.incoming.from_uri) or "",
                display_name= _s(ev.u.incoming.display_name),
            )

        elif typ == 3:  # CALL_STATE
            acct_handle = ev.u.call_state.account
            call_handle = ev.u.call_state.call
            obj = CallStateEvent(
                call=None,
                state  = _CALL_STATES[ev.u.call_state.state],
                error  = ev.u.call_state.error,
                reason = _s(ev.u.call_state.reason),
            )

        elif typ == 4:  # DTMF
            call_handle = ev.u.dtmf.call
            obj = CallDtmfEvent(call=None, digit=chr(ev.u.dtmf.digit))

        elif typ == 5:  # SDP_NEGOTIATION
            call_handle = ev.u.sdp.call
            obj = SdpNegotiationEvent(
                call=None,
                local_sdp         = _s(ev.u.sdp.local_sdp) or "",
                remote_sdp        = _s(ev.u.sdp.remote_sdp) or "",
                negotiated_codec  = _s(ev.u.sdp.negotiated_codec),
                negotiated_crypto = _s(ev.u.sdp.negotiated_crypto),
                rejected_codecs   = _str_array(ev.u.sdp.rejected_codecs),
                warnings          = _str_array(ev.u.sdp.warnings),
            )

        elif typ == 6:  # SIP_TRACE
            obj = SipTraceEvent(
                direction  = "tx" if ev.u.sip_trace.dir == 1 else "rx",
                transport  = _s(ev.u.sip_trace.transport) or "",
                remote_addr= _s(ev.u.sip_trace.remote_addr) or "",
                raw_message= _s(ev.u.sip_trace.raw_message) or "",
                timestamp_us=ev.u.sip_trace.timestamp_us,
            )

        elif typ == 7:  # MEDIA_STATS
            call_handle = ev.u.stats.call
            obj = _raw_stats_to_event(ev.u.stats)

        elif typ == 8:  # REGISTRAR_WARNING
            obj = RegistrarWarningEvent(message=_s(ev.u.reg_warn.message) or "")

        elif typ == 9:  # TRANSFER_REQUEST
            acct_handle = ev.u.transfer_req.account
            call_handle = ev.u.transfer_req.call
            obj = TransferRequestEvent(
                call=None,
                refer_to_uri = _s(ev.u.transfer_req.refer_to_uri) or "",
                has_replaces = bool(ev.u.transfer_req.has_replaces),
                auto_followed = bool(ev.u.transfer_req.auto_followed),
            )

        elif typ == 10: # MWI
            acct_handle = ev.u.mwi.account
            obj = MwiEvent(
                messages_waiting=bool(ev.u.mwi.messages_waiting),
                new_voice  = ev.u.mwi.new_voice,
                old_voice  = ev.u.mwi.old_voice,
                new_urgent = ev.u.mwi.new_urgent,
                old_urgent = ev.u.mwi.old_urgent,
                raw_body   = _s(ev.u.mwi.raw_body),
            )

        elif typ == 11: # MESSAGE
            acct_handle = ev.u.msg.account
            obj = MessageEvent(
                from_uri    = _s(ev.u.msg.from_uri) or "",
                body        = _s(ev.u.msg.body) or "",
                content_type= _s(ev.u.msg.content_type) or "text/plain",
            )

        elif typ == 12: # PRESENCE_STATE
            acct_handle = ev.u.presence.account
            obj = PresenceStateEvent(
                target_uri = _s(ev.u.presence.target_uri) or "",
                status     = _PRESENCE[ev.u.presence.status],
            )

        elif typ == 13: # QUALITY_ALERT
            call_handle = ev.u.quality_alert.call
            q = ev.u.quality_alert
            obj = QualityAlertEvent(
                call       = None,
                issue      = _QUALITY[q.issue],
                value      = q.value,
                threshold  = q.threshold,
                recovering = bool(q.recovering),
            )

        elif typ == 14: # NETWORK
            n = ev.u.network
            call_handle = n.call
            acct_handle = n.account
            obj = NetworkEvent(
                stage        = _NET_STAGE[n.event],
                call         = None,
                local_addr   = _s(n.local_addr) or "",
                attempt      = n.attempt,
                max_attempts = n.max_attempts,
                elapsed_ms   = n.elapsed_ms,
                ice          = bool(n.ice),
                error        = n.error,
            )

        elif typ == 16: # ICE_STATE
            i = ev.u.ice
            call_handle = i.call
            obj = IceStateEvent(
                state       = _ICE_STATE[i.state] if 0 <= i.state < len(_ICE_STATE) else "new",
                prev        = _ICE_STATE[i.prev] if 0 <= i.prev < len(_ICE_STATE) else "new",
                call        = None,
                generation  = i.generation,
                local_type  = _ICE_CAND[i.local_type] if 0 <= i.local_type < len(_ICE_CAND) else "none",
                remote_type = _ICE_CAND[i.remote_type] if 0 <= i.remote_type < len(_ICE_CAND) else "none",
                local_addr  = _s(i.local_addr) or "",
                remote_addr = _s(i.remote_addr) or "",
                rtt_ms      = float(i.rtt_ms),
                error       = i.error,
            )

        elif typ == 15: # TRANSFER_FAILED
            acct_handle = ev.u.transfer_failed.account
            call_handle = ev.u.transfer_failed.call
            obj = TransferFailedEvent(
                call   = None,
                reason = _s(ev.u.transfer_failed.reason) or "",
            )

        else:
            return

        # Attach Call wrapper
        if call_handle != ffi.NULL and hasattr(obj, "call"):
            obj.call = Call(call_handle)

        # Attach account reference (dynamic attribute — dataclasses allow it)
        if acct_handle != ffi.NULL:
            acct = _accounts_by_handle.get(int(ffi.cast("uintptr_t", acct_handle)))
            if acct is not None:
                obj.account = acct

        _event_q.put(obj)

    except Exception:
        pass  # never let Python exceptions escape into C


# ── Event dispatcher ──────────────────────────────────────────────────────────

def _sub_event_name(obj) -> Optional[str]:
    if isinstance(obj, RegStateEvent):
        return "reg_failed" if obj.state == "failed" else obj.state
    if isinstance(obj, CallStateEvent):
        return "call_failed" if obj.state == "failed" else obj.state
    return None


def _dispatch_event(obj):
    base = obj.type
    sub  = _sub_event_name(obj)
    fired = set()
    for name in ([base, sub, "*"] if sub else [base, "*"]):
        if name is None:
            continue
        for h in list(_handlers.get(name, ())):
            hid = id(h)
            if hid in fired:
                continue
            fired.add(hid)
            try:
                h(obj)
            except Exception:
                _log.exception("EchoSDK handler %r raised", h)


def _dispatcher_loop():
    while not _stop_evt.is_set():
        try:
            obj = _event_q.get(timeout=0.1)
        except queue.Empty:
            continue
        if obj is None:
            break
        _dispatch_event(obj)


# ── Public API ────────────────────────────────────────────────────────────────

def configure(**kwargs):
    """Set global SDK options. Must be called before the first create_account().

    Common options:
      log_level         — int (0=off, 1=error, 2=warning, 3=info, 4=debug)
      stats_interval_ms — how often media_stats events fire (default 2000;
                          0 disables RTCP accounting entirely, which also
                          disables quality alerts, media-stall detection and
                          adaptive bitrate)
      verify_server     — bool, validate TLS certificates (default True)
      transport         — "udp" | "tcp" | "tls" | "ws" | "wss"

    Network handover (Wi-Fi <-> 4G/5G) — see network_changed():
      net_monitor_interval_s — interface poll period, 0 = off (default 10)
      net_settle_ms          — debounce before acting (default 1500)
      net_reinvite_calls     — re-INVITE active calls (default True)
      net_verify_ms          — wait for RTP before retrying (default 4000)
      net_max_attempts       — retry ceiling (default 6)
      net_hangup_on_migration_failure — default False
      net_ice_handover       — 0 best-effort (default), 1 fail fast.  An ICE
                               call cannot re-gather candidates mid-call, so
                               1 reports failure after one attempt instead of
                               retrying an offer that cannot succeed.

    Degraded links — the address stays put and the link goes bad.  Handover
    cannot see this; these can:
      media_stall_ms         — warn after this long with no inbound RTP,
                               as a quality_alert with issue "media_stall"
                               (default 4000; 0 = off).  Non-fatal.
      rtp_timeout_s          — END the call after this long with no inbound
                               RTP (default 0 = never).  The fatal version.
      keepalive_interval     — SIP OPTIONS probe period in ms (default 30000).
                               Refreshes the NAT binding and detects a
                               black-holed path; see keepalive_now().
      keepalive_reregister   — re-REGISTER when a probe fails (default True)
      dns_srv_failover       — walk the SRV target list on retry (default True)
      adaptive_bitrate       — lower the Opus bitrate under loss and raise it
                               on recovery, with no re-INVITE (default True)
      adapt_min_bitrate      — bps floor (default 12000)
      adapt_max_bitrate      — bps ceiling (default 32000)
      adapt_loss_down_pct    — step down above this loss %% (default 5.0)
      adapt_loss_up_pct      — step up below this loss %% (default 1.0)
      adapt_recover_ticks    — clean stats ticks before a step up (default 5)
      opus_expected_loss_pct — Opus in-band FEC redundancy %% (default 0 =
                               switched with the link: on at 4%% reported
                               loss or 2%% twice running, off after 10 s
                               under 1%%, 5-25%% while on; see fec_active in
                               stats).  Non-zero pins it.
      reg_retry_jitter       — randomise retry delays by this fraction
                               (default 0.2), so a fleet coming back from an
                               outage does not hit the registrar in one burst
      sip_timer_b_ms         — fail an unanswered outgoing INVITE after this
                               long (default 32000; 8000-12000 to fail fast)
      sip_timer_f_ms         — fail an unanswered REGISTER after this long
                               (default 32000)
    """
    global _config_locked
    if _config_locked:
        raise RuntimeError("echo_sdk.configure() must be called before create_account()")
    _config.update(kwargs)


def on(name: str):
    """Decorator — register a handler for a named event.

    Valid names:
      reg_state, call_state
      registering, registered, unregistered, reg_failed, reconnecting
      calling, ringing, established, held, ended, cancelled, call_failed
      incoming_call, dtmf, sdp_negotiation, sip_trace, media_stats,
      log, registrar_warning, transfer_request, transfer_failed, mwi, message,
      presence_state, quality_alert, network
      * (wildcard — every event)

    Each handler receives a single event object whose fields depend on the
    event type (see events.py).  Call objects are at ev.call; account at ev.account.

        @sdk.on("incoming_call")
        def _(ev):
            print(f"call from {ev.from_uri}")
            ev.call.answer()

        @sdk.on("established")
        def _(ev):
            ev.call.poll_stats(interval=2.0, on_update=lambda s: s.print())
    """
    if name not in _VALID_NAMES:
        raise ValueError(
            f"echo_sdk.on({name!r}): unknown event name. "
            f"Valid names: {sorted(_VALID_NAMES)}"
        )
    def decorator(fn: Callable) -> Callable:
        _handlers.setdefault(name, []).append(fn)
        return fn
    return decorator


def run():
    """Start the event dispatcher and block until stop() is called or Ctrl-C.

    Installs a SIGINT handler that calls stop() unless one is already set.
    On exit: drains remaining events, destroys accounts, shuts down the C SDK.
    """
    global _dispatcher

    if not _handlers:
        raise RuntimeError("echo_sdk.run() called with no @sdk.on() handlers registered")

    _stop_evt.clear()
    _dispatcher = threading.Thread(
        target=_dispatcher_loop, name="EchoSDK-dispatcher", daemon=True)
    _dispatcher.start()

    old_sigint = signal.getsignal(signal.SIGINT)
    installed  = False
    if old_sigint in (signal.SIG_DFL, None):
        def _on_sigint(sig, frame):
            stop()
        signal.signal(signal.SIGINT, _on_sigint)
        installed = True

    try:
        _stop_evt.wait()
    finally:
        if installed:
            signal.signal(signal.SIGINT, old_sigint)

        # Wake the dispatcher so it can exit the get() block
        _event_q.put(None)
        if _dispatcher.is_alive():
            _dispatcher.join(timeout=0.3)

        with _accounts_lock:
            accs = list(_accounts)
        for acc in accs:
            try:
                acc.destroy()
            except Exception:
                pass

        if _init_done:
            lib.echosdk_shutdown()


def stop():
    """Signal run() to finish cleanly. Safe to call from any thread or handler."""
    _stop_evt.set()


def create_account(uri: str, password: str,
                   transport=None,
                   push_provider: int = PUSH_PROVIDER_NONE,
                   push_token: Optional[str] = None,
                   push_param: Optional[str] = None,
                   audio_codecs: Optional[list] = None,
                   **kwargs) -> "Account":
    """Create and return a SIP account. Initializes the SDK on first call.

    Common kwargs:
      transport     — "udp" | "tcp" | "tls" | "ws" | "wss"
      media_enc     — "none" | "sdes" | "dtls_srtp"
      ice_enabled   — bool
      stun_server   — "stun:host:port"
      turn_server / turn_user / turn_pass
      display_name, auth_user, server_url, server_host, server_port
      rtcp_mux      — bool
      rel100        — "disabled" | "enabled" | "required"
      dtmf_mode     — "rfc4733" | "sip_info" | "auto"
      audio_codecs  — list of codec name strings, e.g. ["opus", "pcmu"]
      extra_headers — dict of SIP headers added to all requests
    """
    _ensure_init()

    if transport is None:
        transport = _config.get("transport", 0)
    if isinstance(transport, str):
        transport = _STR_TRANSPORT[transport]

    if isinstance(kwargs.get("media_enc"), str):
        kwargs["media_enc"] = _STR_MEDIA_ENC[kwargs["media_enc"]]
    if "rtcp_mux" in kwargs:
        kwargs["rtcp_mux_set"] = True
    rel100_val = kwargs.pop("rel100", None)
    if isinstance(rel100_val, str):
        rel100_val = _STR_REL100[rel100_val]
    dtmf_val = kwargs.pop("dtmf_mode", None)
    if isinstance(dtmf_val, str):
        dtmf_val = _STR_DTMF[dtmf_val]
    if dtmf_val is not None:
        kwargs["dtmf_mode"] = dtmf_val
    extra_headers = kwargs.pop("extra_headers", {})

    cfg = ffi.new("echosdk_account_config_t *")
    keepalive = [cfg]
    uri_ref  = ffi.new("char[]", uri.encode())
    pass_ref = ffi.new("char[]", password.encode())
    keepalive += [uri_ref, pass_ref]
    cfg.uri           = uri_ref
    cfg.password      = pass_ref
    cfg.transport     = transport
    cfg.push_provider = push_provider

    if push_token is not None:
        ref = ffi.new("char[]", push_token.encode())
        cfg.push_token = ref; keepalive.append(ref)
    if push_param is not None:
        ref = ffi.new("char[]", push_param.encode())
        cfg.push_param = ref; keepalive.append(ref)

    if audio_codecs:
        str_codecs = [c for c in audio_codecs if isinstance(c, str)]
        int_codecs = [c for c in audio_codecs if isinstance(c, int)]
        if str_codecs:
            count = min(len(str_codecs), 8)
            for i, name in enumerate(str_codecs[:count]):
                encoded = name.encode()[:31]
                for j, b in enumerate(encoded):
                    cfg.audio_codec_names[i][j] = bytes([b])
                cfg.audio_codec_names[i][len(encoded)] = b'\x00'
            cfg.audio_codec_name_count = count
        elif int_codecs:
            count = min(len(int_codecs), 8)
            for i, c in enumerate(int_codecs[:count]):
                cfg.audio_codecs[i] = c
            cfg.audio_codec_count = count

    for key, val in kwargs.items():
        if isinstance(val, str):
            ref = ffi.new("char[]", val.encode())
            keepalive.append(ref)
            setattr(cfg, key, ref)
        elif isinstance(val, bool):
            setattr(cfg, key, int(val))
        else:
            setattr(cfg, key, val)

    h = ffi.new("echosdk_account_handle_t *")
    _check(lib.echosdk_account_create(cfg, h), "account_create")

    account = Account(h[0], uri, keepalive)
    if rel100_val is not None:
        lib.echosdk_account_set_100rel(account._h, rel100_val)
    for k, v in extra_headers.items():
        account.add_header(k, v)

    with _accounts_lock:
        _accounts.append(account)
        _accounts_by_handle[int(ffi.cast("uintptr_t", h[0]))] = account

    return account


def _normalize_target(target: str, account: "Account") -> str:
    """Auto-prefix sip: and auto-append @domain from the account's URI."""
    if not target.startswith("sip:"):
        target = "sip:" + target
    if "@" not in target:
        acct_uri = account._uri
        if acct_uri.startswith("sip:"):
            acct_uri = acct_uri[4:]
        if "@" in acct_uri:
            domain = acct_uri.split("@", 1)[1].split(":")[0]
            target = f"{target}@{domain}"
    return target


def call(target: str, account: Optional["Account"] = None) -> "Call":
    """Place an outbound call.

    target  — full SIP URI, bare extension ("120"), or phone number ("+15551234")
    account — which account to dial from; required when multiple accounts exist

        call = sdk.call("120")                    # single-account shorthand
        call = sdk.call("+15551234", account=acc) # explicit
    """
    if account is None:
        with _accounts_lock:
            n = len(_accounts)
            if n == 0:
                raise RuntimeError(
                    "echo_sdk.call() requires an account; none have been created")
            if n > 1:
                raise RuntimeError(
                    f"echo_sdk.call() is ambiguous: {n} accounts registered, "
                    "pass account=...")
            account = _accounts[0]
    return account.call(_normalize_target(target, account))


def version() -> str:
    """Return the EchoSDK library version string."""
    _ensure_init()
    return ffi.string(lib.echosdk_version()).decode()


def strerror(err: int) -> str:
    """Return a human-readable string for a ECHOSDK_ERR_* code."""
    return ffi.string(lib.echosdk_strerror(err)).decode("utf-8", errors="replace")


# ── Audio (module-level) ─────────────────────────────────────────────────────

def list_input_devices() -> list:
    """Return a list of dicts with keys: name, description, is_default."""
    return _list_audio_devices(lib.echosdk_audio_list_input_devices)


def list_output_devices() -> list:
    """Return a list of dicts with keys: name, description, is_default."""
    return _list_audio_devices(lib.echosdk_audio_list_output_devices)


def set_input_device(name: str):
    lib.echosdk_audio_set_input_device(name.encode())


def set_output_device(name: str):
    lib.echosdk_audio_set_output_device(name.encode())


def set_aec(enable: bool):
    """Enable or disable acoustic echo cancellation."""
    lib.echosdk_set_aec(int(enable))


def set_aec_mode(mode: int):
    """0=off, 1=suppressor, 2=webrtc. AEC_WEBRTC requires a desktop build."""
    _check(lib.echosdk_set_aec_mode(mode), "set_aec_mode")


def set_aec_suppression_level(level: float):
    """Suppressor aggressiveness: 0.0=none, 1.0=maximum (default)."""
    lib.echosdk_set_aec_suppression_level(float(level))


def set_ns(enable: bool):
    """Enable or disable noise suppression."""
    lib.echosdk_set_ns(int(enable))


def set_agc(enable: bool):
    """Enable or disable automatic gain control."""
    lib.echosdk_set_agc(int(enable))


def set_mic_gain(db: float):
    """Microphone (TX) gain in dB. Range: -20 to +20. 0 = unity."""
    lib.echosdk_set_mic_gain_db(float(db))


def set_speaker_gain(db: float):
    """Speaker (RX) gain in dB. Range: -20 to +20. 0 = unity."""
    lib.echosdk_set_speaker_gain_db(float(db))


# ── App-owned audio device ───────────────────────────────────────────────────


def use_external_audio(enable: bool):
    """Hand the microphone and speaker to the app.

    The SDK then opens no capture or playback device of its own; you supply and
    consume PCM with external_audio_push/pull. SIP, ICE, SRTP, codecs and the
    jitter buffer stay with the SDK. Takes effect immediately, including on a
    call that is already up.

    Not sticky across shutdown/configure: the device is re-derived from the
    platform on init, so ask again after a restart.

    Echo cancellation follows the device — the platform cancellers the SDK
    relies on belong to the drivers being displaced, so you own AEC too.
    """
    _ensure_init()
    _check(lib.echosdk_audio_use_external(bool(enable)), "use_external_audio")


def external_audio_format() -> Optional[tuple]:
    """(sample_rate, channels, ptime_ms) once the call has media, else None.

    There is no "media is up" event to wait on, so poll this: it reports both
    that the device opened and that a re-INVITE changed the codec.
    """
    srate = ffi.new("uint32_t *")
    ch = ffi.new("uint8_t *")
    ptime = ffi.new("uint32_t *")
    if lib.echosdk_audio_external_format(srate, ch, ptime) != 0:
        return None
    return srate[0], ch[0], ptime[0]


def external_audio_active() -> bool:
    """True while a call is capturing or playing through the app-owned device."""
    return bool(lib.echosdk_audio_external_is_active())


def external_audio_push(pcm) -> int:
    """Push captured microphone audio. This is what the far end hears.

    `pcm` is any S16LE buffer — bytes, bytearray, memoryview, array('h'),
    numpy int16 — at the rate and channel count from external_audio_format().
    Any size is accepted; the stack re-frames internally.

    Returns 0, or errno.ENODEV between calls, which is not worth acting on.
    """
    buf = ffi.from_buffer("int16_t[]", pcm)
    return lib.echosdk_audio_external_push(buf, len(buf))


def external_audio_pull(nsamp: int) -> bytes:
    """Take decoded audio to play. This is what the local user hears.

    Always returns exactly `nsamp` S16LE samples — silence when no call is up —
    so the result can go straight to the speaker without checking.
    """
    buf = ffi.new("int16_t[]", nsamp)
    lib.echosdk_audio_external_pull(buf, nsamp)
    return bytes(ffi.buffer(buf))


def network_changed():
    """Tell the SDK the network may have changed (Wi-Fi <-> 4G/5G, VPN, dock).

    Call this from the platform's connectivity callback. The SDK re-binds its
    SIP transports, re-REGISTERs, and re-INVITEs active calls onto the new
    local address, reporting progress as "network" events.
    """
    _ensure_init()
    return lib.echosdk_network_changed()


def network_set_monitor_interval(seconds: int):
    """Interface poll period in seconds; 0 disables polling entirely."""
    _ensure_init()
    return lib.echosdk_network_set_monitor_interval(int(seconds))


def network_set_handover_policy(reinvite_calls: bool = True,
                                 hangup_on_failure: bool = False):
    """Re-INVITE active calls on handover; optionally hang up on failure."""
    _ensure_init()
    return lib.echosdk_network_set_handover_policy(int(reinvite_calls),
                                                   int(hangup_on_failure))


def network_local_addr() -> str:
    """Local IP the SDK is currently using, or "" when there is none."""
    _ensure_init()
    buf = ffi.new("char[64]")
    if lib.echosdk_network_local_addr(buf, 64) != 0:
        return ""
    return ffi.string(buf).decode()


def network_is_up() -> bool:
    """False while the device has no usable (non-loopback) local address."""
    _ensure_init()
    return bool(lib.echosdk_network_is_up())


def set_jitter_buffer(min_ms: int, max_ms: int):
    lib.echosdk_set_jitter_buffer(min_ms, max_ms)


def set_adaptive_bitrate(enabled: bool, min_bps: int = 0, max_bps: int = 0):
    """Turn link-adaptive bitrate on or off at runtime, with optional bounds.

    Pass 0 for either bound to keep the configured value.  Disabling leaves
    every call at its current rate; use call.set_bitrate(0) to restore the
    negotiated one.
    """
    _ensure_init()
    lib.echosdk_set_adaptive_bitrate(enabled, min_bps, max_bps)


def pcap_start(path: str):
    """Start capturing SIP/RTP to a pcap file."""
    _check(lib.echosdk_pcap_start(path.encode()), "pcap_start")


def pcap_stop():
    """Stop pcap capture and finalize the file."""
    lib.echosdk_pcap_stop()


# ── Call ──────────────────────────────────────────────────────────────────────

class Call:
    """Wrapper around a echosdk_call_handle_t.

    Obtained from sdk.call(...), account.call(...), or ev.call in a handler.
    """

    def __init__(self, handle):
        self._h          = handle
        self._poll_stop  = threading.Event()
        self._poll_thread: Optional[threading.Thread] = None

    def answer(self):
        _check(lib.echosdk_call_answer(self._h), "answer")

    def hangup(self):
        lib.echosdk_call_hangup(self._h)

    def hold(self):
        _check(lib.echosdk_call_hold(self._h), "hold")

    def resume(self):
        _check(lib.echosdk_call_resume(self._h), "resume")

    def is_held(self) -> bool:
        return bool(lib.echosdk_call_is_held(self._h))

    def send_dtmf(self, digit: str):
        lib.echosdk_call_send_dtmf(self._h, ord(digit))

    def transfer(self, uri: str):
        _check(lib.echosdk_call_transfer(self._h, uri.encode()), "transfer")

    def transfer_accept(self) -> "Call":
        """Follow an incoming REFER; returns the new Call to the target.

        Answer a ``transfer_request`` event with this or :meth:`transfer_reject`
        — exactly one. Until you do, the transferor is still waiting on the
        NOTIFY that says what happened, and will sit there until its
        subscription expires.

        Do not implement a transfer by hanging up and dialling: that breaks the
        REFER subscription, and the far end never learns it worked. This keeps
        the two calls linked so the SDK reports the outcome for you.

        The original call stays up; end it when the new one connects.
        """
        out = ffi.new("echosdk_call_handle_t *")
        _check(lib.echosdk_call_transfer_accept(self._h, out),
               "transfer_accept")
        return Call(out[0])

    def transfer_reject(self, scode: int = 603, reason: str = "Declined"):
        """Refuse an incoming REFER, leaving this call up.

        ``scode`` is the SIP status the transferor is told, 400-699; 603
        Decline is the usual "the user said no", 486 for busy.
        """
        _check(lib.echosdk_call_transfer_reject(self._h, scode,
                                                reason.encode()),
               "transfer_reject")

    def info(self) -> dict:
        """Return the call's metadata: peer, URIs, direction, duration.

        Complements :meth:`stats`, which is the per-tick media numbers. Safe
        to call at any point in the call's life, including after it ends.
        """
        i = ffi.new("echosdk_call_info_t *")
        _check(lib.echosdk_call_get_info(self._h, i), "get_info")
        _TRANSPORTS = ("udp", "tcp", "tls", "ws", "wss")
        _STATES     = ("calling", "ringing", "established", "held",
                       "ended", "cancelled", "failed")
        def _fixed(arr) -> str:
            return ffi.string(arr).decode("utf-8", errors="replace")
        return {
            "peer_uri":          _fixed(i.peer_uri),
            "peer_display_name": _fixed(i.peer_display_name),
            "local_uri":         _fixed(i.local_uri),
            "contact_uri":       _fixed(i.contact_uri),
            "call_id":           _fixed(i.call_id),
            "diverter_uri":      _fixed(i.diverter_uri),
            "is_outgoing":       bool(i.is_outgoing),
            "is_remote_hold":    bool(i.is_remote_hold),
            "sip_status":        int(i.sip_status),
            "duration_ms":       int(i.duration_ms),
            "setup_duration_ms": int(i.setup_duration_ms),
            "line_number":       int(i.line_number),
            "transport":         _TRANSPORTS[i.transport]
                                 if 0 <= i.transport < len(_TRANSPORTS)
                                 else "unknown",
            "state":             _STATES[i.state]
                                 if 0 <= i.state < len(_STATES) else "unknown",
        }

    def mute(self, muted: bool = True):
        lib.echosdk_audio_mute(self._h, muted)

    def is_muted(self) -> bool:
        return bool(lib.echosdk_audio_is_muted(self._h))

    def mute_rx(self, muted: bool = True):
        lib.echosdk_audio_mute_rx(self._h, muted)

    def set_dscp_rtp(self, dscp: int):
        _check(lib.echosdk_call_set_dscp_rtp(self._h, dscp), "set_dscp_rtp")

    def set_rtp_timeout(self, seconds: int):
        """End this call after *seconds* with no inbound RTP; 0 = never.

        Per-call override of configure(rtp_timeout_s=...).  Only sendrecv
        streams are checked, so a held call is never torn down by it.
        """
        _check(lib.echosdk_call_set_rtp_timeout(self._h, seconds),
               "set_rtp_timeout")

    def set_bitrate(self, bitrate_bps: int):
        """Set the audio encoder bitrate; 0 restores the negotiated rate.

        Applied through the codec's encoder-update path — no re-INVITE and no
        audio gap — so it does nothing for a fixed-rate codec like G.711.
        With configure(adaptive_bitrate=True) the controller will override
        this on its next decision.
        """
        _check(lib.echosdk_call_set_bitrate(self._h, bitrate_bps),
               "set_bitrate")

    def stats(self) -> CallStats:
        """Return a fresh CallStats snapshot (synchronous, one-shot)."""
        s = ffi.new("echosdk_ev_media_stats_t *")
        lib.echosdk_call_get_stats(self._h, s)
        cs = CallStats()
        cs._update(_raw_stats_to_event(s[0]))
        return cs

    def fetch_stats(self, target: CallStats) -> CallStats:
        """Update *target* in-place with the current stats and return it."""
        s = ffi.new("echosdk_ev_media_stats_t *")
        lib.echosdk_call_get_stats(self._h, s)
        target._update(_raw_stats_to_event(s[0]))
        return target

    def poll_stats(self, interval: float, on_update: Callable):
        """Poll stats at a fixed rate, calling on_update(stats) each tick.

        Runs in a background daemon thread; call stop_polling() to cancel,
        or it stops automatically when is_final is set on the stats.

            call.poll_stats(interval=2.0, on_update=lambda s: s.print())
        """
        self._poll_stop.clear()
        stats = CallStats()

        def _loop():
            while not self._poll_stop.wait(interval):
                self.fetch_stats(stats)
                try:
                    on_update(stats)
                except Exception:
                    _log.exception("EchoSDK poll_stats on_update raised")
                if stats.is_final:
                    break

        self._poll_thread = threading.Thread(target=_loop, daemon=True)
        self._poll_thread.start()

    def stop_polling(self):
        """Stop a poll_stats() loop started on this call."""
        self._poll_stop.set()

    def record_start(self, path: str):
        """Record mixed call audio (RX+TX) to a WAV file."""
        _check(lib.echosdk_call_record_start(self._h, path.encode()), "record_start")

    def record_stop(self):
        """Stop recording and finalize WAV headers."""
        lib.echosdk_call_record_stop(self._h)

    @property
    def handle(self):
        return self._h


# ── Account ───────────────────────────────────────────────────────────────────

class Account:
    """A SIP account. Create via sdk.create_account()."""

    def __init__(self, handle, uri: str, keepalive: list):
        self._h         = handle
        self._uri       = uri
        self._keepalive = keepalive   # C cdata refs that must not be GC'd

    def register(self):
        """Send a SIP REGISTER to activate this account."""
        _check(lib.echosdk_account_register(self._h), "register")

    def unregister(self):
        """Send an unregistration (Expires: 0)."""
        lib.echosdk_account_unregister(self._h)

    def call(self, uri: str) -> Call:
        """Place an outbound call to a full SIP URI."""
        ch = ffi.new("echosdk_call_handle_t *")
        _check(lib.echosdk_call_invite(self._h, uri.encode(), ch), "call_invite")
        return Call(ch[0])

    def send_message(self, to: str, body: str, content_type: str = "text/plain"):
        _check(
            lib.echosdk_message_send(self._h, to.encode(), body.encode(),
                                     content_type.encode()),
            "message_send")

    def publish_presence(self, status: int):
        lib.echosdk_account_publish_presence(self._h, status)

    def subscribe_presence(self, target_uri: str):
        _check(lib.echosdk_account_subscribe_presence(self._h, target_uri.encode()),
               "subscribe_presence")

    def add_header(self, name: str, value: str):
        _check(lib.echosdk_account_add_header(self._h, name.encode(), value.encode()),
               "add_header")

    def add_register_header(self, name: str, value: str):
        """Add a header sent only on REGISTER (not on INVITE/BYE)."""
        _check(lib.echosdk_account_add_register_header(
            self._h, name.encode(), value.encode()), "add_register_header")

    def set_push_token(self, push_token: Optional[str]) -> int:
        """Update the push notification token at runtime. Pass None to clear."""
        tok = push_token.encode() if push_token is not None else ffi.NULL
        return lib.echosdk_account_set_push_token(self._h, tok)

    def set_retry_policy(self, initial_ms: int, max_ms: int,
                         backoff: float, max_attempts: int = 0):
        _check(lib.echosdk_account_set_retry_policy(
            self._h, initial_ms, max_ms, backoff, max_attempts),
            "set_retry_policy")

    def cancel_retry(self):
        lib.echosdk_account_cancel_retry(self._h)

    def retry_now(self):
        lib.echosdk_account_retry_now(self._h)

    def keepalive_now(self):
        """Send a reachability probe (SIP OPTIONS) for this account now.

        Worth doing from an app-foreground or push-wake handler: it answers
        "is my registration still reachable?" before the user tries to place a
        call.  Nothing is reported on success; on failure the account goes to
        reg_failed and, with configure(keepalive_reregister=True), re-registers.
        """
        _check(lib.echosdk_account_keepalive_now(self._h), "keepalive_now")

    def set_100rel(self, mode: int):
        lib.echosdk_account_set_100rel(self._h, mode)

    def destroy(self):
        if self._h is None:
            return
        with _accounts_lock:
            try:
                _accounts.remove(self)
            except ValueError:
                pass
            _accounts_by_handle.pop(int(ffi.cast("uintptr_t", self._h)), None)
        lib.echosdk_account_destroy(self._h)
        self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.destroy()

    def __repr__(self):
        return f"Account({self._uri!r})"


# ── Public surface ────────────────────────────────────────────────────────────

__all__ = [
    # Core API
    "configure", "create_account", "call", "on", "run", "stop",
    "version", "strerror",
    # Audio
    "list_input_devices", "list_output_devices",
    "set_input_device", "set_output_device",
    "set_aec", "set_aec_mode", "set_aec_suppression_level",
    "set_ns", "set_agc", "set_mic_gain", "set_speaker_gain",
    "set_jitter_buffer", "pcap_start", "pcap_stop",
    # App-owned audio device
    "use_external_audio", "external_audio_format", "external_audio_active",
    "external_audio_push", "external_audio_pull",
    # Network handover
    "network_changed", "network_set_monitor_interval",
    "network_set_handover_policy", "network_local_addr", "network_is_up",
    # Classes
    "Account", "Call", "CallStats",
    # Event dataclasses
    "RegStateEvent", "IncomingCallEvent", "CallStateEvent", "CallDtmfEvent",
    "SdpNegotiationEvent", "SipTraceEvent", "MediaStatsEvent", "LogEvent",
    "RegistrarWarningEvent", "TransferRequestEvent", "TransferFailedEvent",
    "MwiEvent",
    "MessageEvent", "PresenceStateEvent", "QualityAlertEvent", "NetworkEvent",
    # Push provider constants (no string form in the C API)
    "PUSH_PROVIDER_NONE", "PUSH_PROVIDER_APNS",
    "PUSH_PROVIDER_APNS_SANDBOX", "PUSH_PROVIDER_FCM",
]
