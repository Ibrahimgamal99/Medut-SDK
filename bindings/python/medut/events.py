"""
events.py — Python dataclasses mirroring Medut event payloads.

The raw C event pointer is decoded into one of these objects and put on
the per-account queue.  All string fields are decoded from bytes to str.
State fields use human-readable strings instead of integer constants.
"""

import math
import threading
from dataclasses import dataclass, field
from typing import Optional, List


@dataclass
class RegStateEvent:
    type: str = field(init=False, default="reg_state")
    # "reconnecting" is a transient loss the SDK is recovering from by itself
    # (retry armed, dead keepalive path, network handover) — show it, do not
    # act on it.  "failed" is terminal: bad credentials, the retry budget is
    # spent, or the app cancelled the retry.
    state: str           # "unregistered" | "registering" | "registered" | "failed" | "unregistering" | "reconnecting"
    error: int           # MEDUT_ERR_* or MEDUT_OK
    error_str: Optional[str]
    retry_attempt: int
    retry_delay_ms: int


@dataclass
class IncomingCallEvent:
    type: str = field(init=False, default="incoming_call")
    call: object         # Medut.Call (set by Account)
    from_uri: str
    display_name: Optional[str]


@dataclass
class CallStateEvent:
    type: str = field(init=False, default="call_state")
    call: object         # Medut.Call
    state: str           # "calling" | "ringing" | "established" | "held" | "ended" | "cancelled" | "failed"
    error: int
    reason: Optional[str]


@dataclass
class CallDtmfEvent:
    type: str = field(init=False, default="dtmf")
    call: object
    digit: str           # single character


@dataclass
class SdpNegotiationEvent:
    type: str = field(init=False, default="sdp_negotiation")
    call: object
    local_sdp: str
    remote_sdp: str
    negotiated_codec: Optional[str]
    negotiated_crypto: Optional[str]
    rejected_codecs: List[str]
    warnings: List[str]


@dataclass
class SipTraceEvent:
    type: str = field(init=False, default="sip_trace")
    direction: str       # "tx" | "rx"
    transport: str
    remote_addr: str
    raw_message: str
    timestamp_us: int


@dataclass
class MediaStatsEvent:
    type: str = field(init=False, default="media_stats")
    call: object
    # Packet counters
    packets_sent: int
    packets_received: int
    packets_lost: int        # TX-side
    packets_lost_rx: int     # RX-side
    bytes_sent: int
    bytes_received: int
    tx_errors: int
    rx_errors: int
    # Loss
    loss_pct: float          # TX-side %
    loss_pct_rx: float       # RX-side %
    # Delay / jitter
    jitter_ms: float
    tx_jitter_ms: float
    rtt_ms: float
    # Jitter buffer
    jitter_buffer_ms: int
    jitter_buffer_load: int
    late_packets: int
    discarded_packets: int
    jitter_buffer_target_ms: int
    jitter_buffer_adaptive: bool
    # PLC
    plc_frames: int
    plc_ratio: float
    # Bandwidth
    bandwidth_kbps_tx: int
    bandwidth_kbps_rx: int
    avg_bandwidth_kbps_tx: int
    avg_bandwidth_kbps_rx: int
    # MOS
    mos_lq: float
    mos_cq: float
    mos_lq_rx: float
    mos_cq_rx: float
    mos_method: int
    # Codec
    codec_name: str
    codec_clock_rate: int
    codec_sample_rate: int
    codec_channels: int
    payload_type: int
    # Audio level
    audio_level_dbov: float  # speaker (RX) dBov; NaN when unavailable
    mic_level_dbov: float    # microphone (TX) dBov; NaN when unavailable
    # Stream identity
    ssrc_tx: int
    ssrc_rx: int
    remote_addr: str
    # Session history
    mos_lq_min: float
    mos_lq_avg: float
    stats_tick: int
    call_duration_ms: int
    is_final: bool
    # ICE -- the media path as the agent sees it.  ice_state is "new" and the
    # rest zero on a call without ICE.  remote_addr above is the same address
    # as ice_remote_addr on an ICE call.
    ice_state: str = "new"
    ice_local_type: str = "none"
    ice_remote_type: str = "none"
    ice_rtt_ms: float = 0.0
    ice_generation: int = 0
    ice_local_addr: str = ""
    ice_remote_addr: str = ""
    # Opus FEC we send: on at 4% peer-reported loss (2% twice running), off
    # after 10 s under 1%.
    # False / 0 for G.711 or a peer that did not ask for FEC.
    fec_active: bool = False
    fec_level_pct: int = 0
    # Opus FEC we receive: how each frame the jitter buffer reported lost
    # was filled in, cumulative — from the next packet's redundant copy, or
    # by concealment alone.  0 for a codec other than Opus.
    fec_recovered: int = 0
    plc_concealed: int = 0


@dataclass
class LogEvent:
    type: str = field(init=False, default="log")
    message: str


@dataclass
class RegistrarWarningEvent:
    type: str = field(init=False, default="registrar_warning")
    message: str


@dataclass
class TransferRequestEvent:
    """The peer asked us to transfer this call somewhere else (SIP REFER).

    Answer it with ``ev.call.transfer_accept()`` or
    ``ev.call.transfer_reject()`` — exactly one. The SDK has already sent the
    provisional replies, and the transferor is now waiting for the final NOTIFY
    that only one of those two produces; ignoring the event leaves it waiting
    out its subscription.
    """
    type: str = field(init=False, default="transfer_request")
    call: object
    refer_to_uri: str
    has_replaces: bool
    #: True when the SDK already followed the transfer itself. Always False
    #: today — placing the call is the app's decision — but check it before
    #: calling transfer_accept() so future auto-follow policy cannot make you
    #: place a second call on top of the SDK's.
    auto_followed: bool = False


@dataclass
class TransferFailedEvent:
    """An outgoing REFER was refused — the transfer did not happen.

    `call` is still established.  A refused transfer is not a call failure:
    the far end would not take the call and the user is still on the line,
    usually on hold.  Resume them and report `reason`; do not hang up.

    There is no matching success event.  An accepted REFER hands the call to
    the transfer target and closes our leg, which arrives as a call-state
    event with state "ended".
    """
    type: str = field(init=False, default="transfer_failed")
    call: object
    reason: str


@dataclass
class MwiEvent:
    type: str = field(init=False, default="mwi")
    messages_waiting: bool
    new_voice: int
    old_voice: int
    new_urgent: int
    old_urgent: int
    raw_body: Optional[str]


@dataclass
class MessageEvent:
    type: str = field(init=False, default="message")
    from_uri: str
    body: str
    content_type: str


@dataclass
class PresenceStateEvent:
    type: str = field(init=False, default="presence_state")
    target_uri: str
    status: str          # "unknown" | "open" | "closed" | "busy"


@dataclass
class QualityAlertEvent:
    type: str = field(init=False, default="quality_alert")
    call: object
    # "mos" | "loss" | "jitter" | "rtt" | "media_stall"
    #
    # "media_stall" means no inbound RTP for configure(media_stall_ms=...) —
    # the link is up and the call is nominally fine, but no audio is arriving.
    # `value` is the stall duration in ms.  Non-fatal; it fires again with
    # recovering=True when packets resume.  Use configure(rtp_timeout_s=...)
    # to end such a call instead of just reporting it.
    issue: str
    value: float
    threshold: float
    recovering: bool   # True = the value came back across the threshold


@dataclass
class NetworkEvent:
    """Progress of a network handover (Wi-Fi <-> 4G/5G, VPN, dock/undock).

    stage is one of:
      change_detected, down, up, transport_reset, reregistering,
      call_migrating, call_migrate_accepted, call_migrated,
      call_migration_failed, call_deferred, handover_failed,
      call_ice_stale, call_ice_failed

    An ICE call is migrated with a full RFC 8445 §9 ICE restart: new
    credentials and a fresh gather on the new interface, so the re-INVITE
    carries candidates for the network the device is on now.  That is
    automatic and shows up as the ordinary call_migrating → call_migrated
    sequence, with the re-INVITE arriving up to ice_gathering_timeout_ms later.

    call_ice_stale marks the exception: a call with ICE that could not be
    re-gathered, whose re-INVITE therefore carries the pre-handover
    candidates.  That recovers a direct or TURN-relayed path; if it fails the
    remedy is to re-place the call.  configure(net_ice_handover=1) makes the
    SDK give up after one attempt rather than retrying such a call.

    call_ice_failed is the other ICE outcome, and a different one: the
    candidates *were* gathered and offered and the peer answered, but every
    pair in the check list failed, so nothing was nominated and the media path
    was never validated.  The usual causes are a peer that rejected the
    connectivity checks -- Asterisk answers STUN 401 for a while after it
    rebuilds its own ICE session across a restart offer -- or a path that
    stopped carrying UDP.

    The call is not ended.  Where an earlier ICE generation had validated a
    pair the SDK holds the stream on it, so audio usually survives; the event
    says the *new* path was never proven, not that the call is silent.  Check
    whether audio is actually flowing (the media_stall quality alert answers
    that) rather than hanging up on it.

    Typical logging:
        if ev.stage == "call_migrating":
            log(f"link settled - rebuilding media path "
                f"{ev.attempt}/{ev.max_attempts}")
        elif ev.stage == "call_migrate_accepted":
            log("peer accepted the new path - waiting for audio to resume")
        elif ev.stage == "call_migrated":
            log(f"media recovered after {ev.elapsed_ms / 1000:.1f}s")
    """
    type: str = field(init=False, default="network")
    stage: str
    call: object
    local_addr: str
    attempt: int
    max_attempts: int
    elapsed_ms: int
    ice: bool            # True when the call uses ICE (recovery is best-effort)
    error: int


@dataclass
class IceStateEvent:
    """The ICE liveness of a call's media path changed.

    state / prev are one of:
      new, gathering, checking, connected, completed, disconnected, failed,
      closed
    -- the agent's own verdict from its connectivity checks (RFC 8445) and
    consent freshness (RFC 7675), mirroring the browser's
    RTCIceConnectionState.  `connected`: a pair is writable; `completed`: it
    is also nominated; `disconnected`: the selected pair stopped answering or
    receiving while checks continue; `failed`: every pair timed out.  The SDK
    never ends a call on `failed` -- redialling is the app's decision.

    local_addr / remote_addr are the selected pair as "ip:port" ("" until one
    is selected), generation counts ICE restarts (1 = initial), rtt_ms is the
    STUN round trip on the selected pair.
    """
    type: str = field(init=False, default="ice_state")
    state: str
    prev: str
    call: object
    generation: int
    local_type: str
    remote_type: str
    local_addr: str
    remote_addr: str
    rtt_ms: float
    error: int


class CallStats:
    """
    Live call statistics that update in-place as media_stats events arrive.

    Obtain via account.stats_stream() which yields this object each time
    it refreshes, or read .current on a Call after the call is established.

    All fields mirror MediaStatsEvent. Before the first update every numeric
    field is 0 / NaN and string fields are empty; check .available first.

        for stats in account.stats_stream():
            print(stats.mos_lq, stats.rtt_ms)
            if stats.is_final:
                break
    """

    __slots__ = (
        "_lock",
        "available",
        # packet counters
        "packets_sent", "packets_received",
        "packets_lost", "packets_lost_rx",
        "bytes_sent", "bytes_received",
        "tx_errors", "rx_errors",
        # loss
        "loss_pct", "loss_pct_rx",
        # delay / jitter
        "jitter_ms", "tx_jitter_ms", "rtt_ms",
        # jitter buffer
        "jitter_buffer_ms", "jitter_buffer_load",
        "late_packets", "discarded_packets",
        "jitter_buffer_target_ms", "jitter_buffer_adaptive",
        # plc
        "plc_frames", "plc_ratio",
        # bandwidth
        "bandwidth_kbps_tx", "bandwidth_kbps_rx",
        "avg_bandwidth_kbps_tx", "avg_bandwidth_kbps_rx",
        # mos
        "mos_lq", "mos_cq", "mos_lq_rx", "mos_cq_rx", "mos_method",
        # codec
        "codec_name", "codec_clock_rate", "codec_sample_rate",
        "codec_channels", "payload_type",
        # audio levels
        "audio_level_dbov", "mic_level_dbov",
        # stream identity
        "ssrc_tx", "ssrc_rx", "remote_addr",
        # session history
        "mos_lq_min", "mos_lq_avg", "stats_tick",
        "call_duration_ms", "is_final",
        # ICE
        "ice_state", "ice_local_type", "ice_remote_type", "ice_rtt_ms",
        "ice_generation", "ice_local_addr", "ice_remote_addr",
        # Opus FEC
        "fec_active", "fec_level_pct", "fec_recovered", "plc_concealed",
    )

    def __init__(self):
        self._lock = threading.Lock()
        self.available = False
        _nan = math.nan
        self.packets_sent = 0;        self.packets_received = 0
        self.packets_lost = 0;        self.packets_lost_rx = 0
        self.bytes_sent = 0;          self.bytes_received = 0
        self.tx_errors = 0;           self.rx_errors = 0
        self.loss_pct = 0.0;          self.loss_pct_rx = 0.0
        self.jitter_ms = 0.0;         self.tx_jitter_ms = 0.0;  self.rtt_ms = 0.0
        self.jitter_buffer_ms = 0;    self.jitter_buffer_load = 0
        self.late_packets = 0;        self.discarded_packets = 0
        self.jitter_buffer_target_ms = 0; self.jitter_buffer_adaptive = False
        self.plc_frames = 0;          self.plc_ratio = 0.0
        self.bandwidth_kbps_tx = 0;   self.bandwidth_kbps_rx = 0
        self.avg_bandwidth_kbps_tx = 0; self.avg_bandwidth_kbps_rx = 0
        self.mos_lq = _nan;           self.mos_cq = _nan
        self.mos_lq_rx = _nan;        self.mos_cq_rx = _nan;  self.mos_method = 0
        self.codec_name = "";         self.codec_clock_rate = 0
        self.codec_sample_rate = 0;   self.codec_channels = 0;  self.payload_type = 0
        self.audio_level_dbov = _nan; self.mic_level_dbov = _nan
        self.ssrc_tx = 0;             self.ssrc_rx = 0;  self.remote_addr = ""
        self.mos_lq_min = _nan;       self.mos_lq_avg = _nan
        self.stats_tick = 0;          self.call_duration_ms = 0;  self.is_final = False
        self.ice_state = "new";       self.ice_local_type = "none"
        self.ice_remote_type = "none"; self.ice_rtt_ms = 0.0
        self.ice_generation = 0
        self.ice_local_addr = "";     self.ice_remote_addr = ""
        self.fec_active = False;      self.fec_level_pct = 0

    def _update(self, ev: "MediaStatsEvent") -> None:
        with self._lock:
            self.available              = True
            self.packets_sent           = ev.packets_sent
            self.packets_received       = ev.packets_received
            self.packets_lost           = ev.packets_lost
            self.packets_lost_rx        = ev.packets_lost_rx
            self.bytes_sent             = ev.bytes_sent
            self.bytes_received         = ev.bytes_received
            self.tx_errors              = ev.tx_errors
            self.rx_errors              = ev.rx_errors
            self.loss_pct               = ev.loss_pct
            self.loss_pct_rx            = ev.loss_pct_rx
            self.jitter_ms              = ev.jitter_ms
            self.tx_jitter_ms           = ev.tx_jitter_ms
            self.rtt_ms                 = ev.rtt_ms
            self.jitter_buffer_ms       = ev.jitter_buffer_ms
            self.jitter_buffer_load     = ev.jitter_buffer_load
            self.late_packets           = ev.late_packets
            self.discarded_packets      = ev.discarded_packets
            self.jitter_buffer_target_ms = ev.jitter_buffer_target_ms
            self.jitter_buffer_adaptive = ev.jitter_buffer_adaptive
            self.plc_frames             = ev.plc_frames
            self.plc_ratio              = ev.plc_ratio
            self.bandwidth_kbps_tx      = ev.bandwidth_kbps_tx
            self.bandwidth_kbps_rx      = ev.bandwidth_kbps_rx
            self.avg_bandwidth_kbps_tx  = ev.avg_bandwidth_kbps_tx
            self.avg_bandwidth_kbps_rx  = ev.avg_bandwidth_kbps_rx
            self.mos_lq                 = ev.mos_lq
            self.mos_cq                 = ev.mos_cq
            self.mos_lq_rx              = ev.mos_lq_rx
            self.mos_cq_rx              = ev.mos_cq_rx
            self.mos_method             = ev.mos_method
            self.codec_name             = ev.codec_name
            self.codec_clock_rate       = ev.codec_clock_rate
            self.codec_sample_rate      = ev.codec_sample_rate
            self.codec_channels         = ev.codec_channels
            self.payload_type           = ev.payload_type
            self.audio_level_dbov       = ev.audio_level_dbov
            self.mic_level_dbov         = ev.mic_level_dbov
            self.ssrc_tx                = ev.ssrc_tx
            self.ssrc_rx                = ev.ssrc_rx
            self.remote_addr            = ev.remote_addr
            self.mos_lq_min             = ev.mos_lq_min
            self.mos_lq_avg             = ev.mos_lq_avg
            self.stats_tick             = ev.stats_tick
            self.call_duration_ms       = ev.call_duration_ms
            self.is_final               = ev.is_final
            self.ice_state              = ev.ice_state
            self.ice_local_type         = ev.ice_local_type
            self.ice_remote_type        = ev.ice_remote_type
            self.ice_rtt_ms             = ev.ice_rtt_ms
            self.ice_generation         = ev.ice_generation
            self.ice_local_addr         = ev.ice_local_addr
            self.ice_remote_addr        = ev.ice_remote_addr
            self.fec_active             = ev.fec_active
            self.fec_level_pct          = ev.fec_level_pct

    def print(self) -> None:
        """Print a formatted stats block to stdout."""
        if not self.available:
            print("CallStats: no data yet")
            return
        method = "E-model" if self.mos_method == 0 else "simplified"
        spk = f"{self.audio_level_dbov:.4f} dBov" if not math.isnan(self.audio_level_dbov) else "n/a"
        mic = f"{self.mic_level_dbov:.4f} dBov"   if not math.isnan(self.mic_level_dbov)   else "n/a"
        dur = self.call_duration_ms // 1000
        elapsed = (f"{dur // 3600}:{dur // 60 % 60:02d}:{dur % 60:02d}"
                   if dur >= 3600 else f"{dur // 60}:{dur % 60:02d}")
        print(
            f"┌─ Media Stats (tick={self.stats_tick}, in call {elapsed}) ──────\n"
            f"│  Codec     : {self.codec_name}  {self.codec_clock_rate // 1000} kHz"
            f"  ch={self.codec_channels}  PT={self.payload_type}\n"
            f"│  Remote    : {self.remote_addr}"
            f"  SSRC rx={self.ssrc_rx}  tx={self.ssrc_tx}\n"
            f"│  Packets   : tx={self.packets_sent}  rx={self.packets_received}"
            f"  lost_tx={self.packets_lost} ({self.loss_pct:.1f}%)"
            f"  lost_rx={self.packets_lost_rx} ({self.loss_pct_rx:.1f}%)\n"
            f"│  Bandwidth : tx={self.bandwidth_kbps_tx} kbps  rx={self.bandwidth_kbps_rx} kbps"
            f"  (avg tx={self.avg_bandwidth_kbps_tx}  rx={self.avg_bandwidth_kbps_rx})\n"
            f"│  Delay     : RTT={self.rtt_ms:.1f} ms"
            f"  jitter={self.jitter_ms:.1f} ms"
            f"  tx_jitter={self.tx_jitter_ms:.1f} ms\n"
            f"│  Jitter buf: depth={self.jitter_buffer_ms} ms"
            f"  load={self.jitter_buffer_load}"
            f"  late={self.late_packets}"
            f"  discarded={self.discarded_packets}\n"
            f"│  MOS ({method}): LQ={self.mos_lq:.3f}  CQ={self.mos_cq:.3f}\n"
            f"│  Speaker   : {spk}\n"
            f"│  Mic       : {mic}\n"
            f"└───────────────────────────────────────────────",
            flush=True,
        )

    def __repr__(self) -> str:
        if not self.available:
            return "CallStats(no data yet)"
        return (
            f"CallStats(tick={self.stats_tick}"
            f" mos_lq={self.mos_lq:.3f}"
            f" rtt={self.rtt_ms:.1f}ms"
            f" loss={self.loss_pct:.1f}%"
            f" jitter={self.jitter_ms:.1f}ms"
            f" codec={self.codec_name}"
            f" final={self.is_final})"
        )
