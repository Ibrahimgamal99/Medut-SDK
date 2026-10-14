# Observability

Medut provides built-in tools for monitoring call quality, tracing SIP traffic, and capturing packets for offline analysis.

---

## Media statistics (RTCP)

Periodic stats are on by default at `cfg.stats_interval_ms = 2000`:

```c
cfg.stats_interval_ms = 2000;  // default; 0 disables
```

You will receive `MEDUT_EV_MEDIA_STATS` events at that interval during every active call.

!!! warning "0 disables more than the events"
    `stats_interval_ms` is also the master switch for RTCP accounting inside
    baresip (`avt.rtp_stats`). With it at 0 the loss, jitter, RTT and MOS fields
    read back as zero from `medut_call_get_stats()` too, and every feature
    derived from them — quality alerts, media-stall detection and adaptive
    bitrate — is inert.

### Key metrics

**Quality scores**

| Metric | Field | Unit | Good | Acceptable | Poor |
|---|---|---|---|---|---|
| TX packet loss | `loss_pct` | % | < 1% | < 5% | ≥ 5% |
| RX packet loss | `loss_pct_rx` | % | < 1% | < 5% | ≥ 5% |
| RX jitter | `jitter_ms` | ms | < 10 | < 30 | ≥ 30 |
| Round-trip time | `rtt_ms` | ms | < 150 | < 300 | ≥ 300 |
| MOS listening quality (TX path) | `mos_lq` | 1.0–4.5 | ≥ 4.0 | ≥ 3.6 | < 3.6 |
| MOS conversational quality (TX path) | `mos_cq` | 1.0–4.5 | ≥ 4.0 | ≥ 3.6 | < 3.6 |
| MOS listening quality (RX path) | `mos_lq_rx` | 1.0–4.5 | ≥ 4.0 | ≥ 3.6 | < 3.6 |
| MOS conversational quality (RX path) | `mos_cq_rx` | 1.0–4.5 | ≥ 4.0 | ≥ 3.6 | < 3.6 |
| Worst / average LQ this call | `mos_lq_min`, `mos_lq_avg` | 1.0–4.5 | — | — | — |

**Rates vs counters.** `loss_pct` and `loss_pct_rx` are rates over the **last
poll window** (`stats_interval_ms`), not lifetime averages — a lifetime rate can
never recover from one early burst. The `packets_lost` / `packets_lost_rx`
counters beside them stay cumulative for the call, per RFC 3550.
`mos_lq_avg` is the session average, taken in the R-factor domain and converted
once, because MOS is non-linear in R.

**Zero is not always a measurement.** `rtt_ms` comes only from the peer's
receiver report (RFC 3550 LSR/DLSR), so it reads `0.0` until the first RR
carrying both arrives — and for the whole call against a peer that sends no
RTCP. `0.0` there means "not measured yet". The only fields that use `NaN` for
"unavailable" are `audio_level_dbov` and `mic_level_dbov`.

**Bandwidth**

| Metric | Field | Unit | Notes |
|---|---|---|---|
| Current TX bitrate | `bandwidth_kbps_tx` | kbps | Instantaneous |
| Current RX bitrate | `bandwidth_kbps_rx` | kbps | Instantaneous |
| Average TX bitrate | `avg_bandwidth_kbps_tx` | kbps | Session average |
| Average RX bitrate | `avg_bandwidth_kbps_rx` | kbps | Session average |

**Jitter buffer**

| Metric | Field | Notes |
|---|---|---|
| Buffer depth | `jitter_buffer_ms` | Current adaptive delay introduced |
| Buffer load | `jitter_buffer_load` | Packets currently buffered |
| Late arrivals | `late_packets` | Arrived after playout deadline |
| Discarded | `discarded_packets` | Overflow or flush |

**Audio level**

| Metric | Field | Notes |
|---|---|---|
| Received level | `audio_level_dbov` | dBov: 0 = max, –127 = silent; `NaN` = unavailable |

**Stream identity** — useful for correlating stats with network captures

`NaN` and `-127` are different answers, and the difference is the whole
diagnostic value. `NaN` means no frame was ever measured — media never
started. `-127` is a measurement: frames arrived and every sample was zero,
which is a dead capture or playback path rather than a quiet room. Read the
mic and speaker levels together with the RTP counters:

| mic | spk | tx/rx | Reading |
|---|---|---|---|
| `NaN` | `NaN` | 0 | No media at all — check ICE/DTLS, not audio |
| `NaN` | `NaN` | non-zero | RTP is flowing but the audio tap never ran |
| `-127` | real | non-zero | Capture is delivering digital silence |
| real | `-127` | non-zero | Playback path is receiving silence |

| Field | Notes |
|---|---|
| `ssrc_tx` | Our SSRC as seen in Wireshark |
| `ssrc_rx` | Remote SSRC (0 until first RTP received) |
| `remote_addr` | Remote RTP endpoint `"ip:port"` |
| `payload_type` | RTP payload type number |

### MOS methods

Set `cfg.mos_method` to choose the scoring algorithm:

| Method | Enum | Formula | Best for |
|---|---|---|---|
| E-Model (ITU-T G.107 narrowband, G.107.1 wideband) | `MEDUT_MOS_EMODEL` | `Ie-eff = Ie + (95−Ie)·Ppl/(Ppl+Bpl)`; `R-LQ = Ro − Ie-eff`; `R-CQ = R-LQ − Id` | Accurate VoIP quality assessment |
| Simplified | `MEDUT_MOS_SIMPLIFIED` | `LQ = 4.5 − 0.09·loss − 0.0009·jitter`; `CQ = LQ − 0.0005·RTT` | Quick dashboard metric |

Both produce MOS in the 1.0–4.5 range.

**LQ vs CQ.** LQ is *listening* quality — the received signal alone, with no
delay term. CQ is *conversational* quality: LQ minus the delay impairment `Id`,
which the E-model computes from a real one-way delay estimate `Ta` (network
`RTT/2` plus the jitter buffer depth actually in use):

```
Id = 0.024·Ta + 0.11·(Ta − 177.3)·[Ta > 177.3]
```

So `CQ ≤ LQ` always, and the penalty grows from the first millisecond of delay
rather than switching on at a threshold. Each direction is scored from its own
loss and its own jitter. The RX scores use **effective** loss — network loss
plus packets the jitter buffer discarded (late, overflow, flush), which is the
single largest source of MOS over-estimation when omitted. The TX scores cannot
do this: the peer's jitter buffer is not observable from here.

### Synchronous query

`medut_call_get_stats()` returns the current stats without waiting for the next timer tick. Packet counters and bandwidth are always populated; RTCP fields are zero until the first RTCP exchange.

```c
medut_ev_media_stats_t stats;
int rc = medut_call_get_stats(call, &stats);
if (rc == MEDUT_OK) {
    printf("MOS-LQ=%.2f  MOS-CQ=%.2f  RTT=%.0f ms  loss=%.1f%%\n",
           stats.mos_lq, stats.mos_cq, stats.rtt_ms, stats.loss_pct);
    printf("TX %u kbps  RX %u kbps  jitter %.1f ms\n",
           stats.bandwidth_kbps_tx, stats.bandwidth_kbps_rx, stats.jitter_ms);
    printf("codec %s  remote %s  SSRC rx=%u\n",
           stats.codec_name, stats.remote_addr, stats.ssrc_rx);
}
```

---

## Quality alerts

Rather than re-deriving thresholds from every stats tick, set them once and get
an edge-triggered event when one is crossed — and another when it is crossed
back. A call that stays bad produces one alert, not one per tick.

```c
cfg.mos_alert_threshold    = 3.5;    /* defaults */
cfg.loss_alert_threshold   = 5.0;
cfg.jitter_alert_threshold = 40.0;
cfg.media_stall_ms         = 4000;
```

| Issue | Fires when | `value` |
|---|---|---|
| `MEDUT_QUALITY_MOS` | `mos_lq` drops below `mos_alert_threshold` | the MOS |
| `MEDUT_QUALITY_LOSS` | `loss_pct` exceeds `loss_alert_threshold` | loss % |
| `MEDUT_QUALITY_JITTER` | `jitter_ms` exceeds `jitter_alert_threshold` | jitter ms |
| `MEDUT_QUALITY_MEDIA_STALL` | no inbound RTP for `media_stall_ms` | stall ms |

`MEDIA_STALL` is not a metric threshold — it is the *absence* of inbound RTP
while the call is neither held nor mid-handover. It is the one condition that no
amount of reading the other metrics will reveal, because when RTP stops the
metrics simply stop changing. Suppressed on held calls and during a handover
migration, where `MEDUT_EV_NETWORK` narrates the same outage in more detail.

```c
case MEDUT_EV_QUALITY_ALERT: {
    const medut_ev_quality_alert_t *a = &ev->u.quality_alert;
    printf("%s %s: %.1f (threshold %.1f)\n",
           a->recovering ? "recovered" : "degraded",
           issue_name(a->issue), a->value, a->threshold);
    break;
}
```

Set any threshold to 0 to disable that alert. See
[Degraded links](../guides/degraded_links.md) for what to do about each one.

---

## SIP trace

Set `cfg.trace_sip = true` to receive `MEDUT_EV_SIP_TRACE` for every SIP message sent or received:

```c
cfg.trace_sip = true;
```

Each event contains:

| Field | Description |
|---|---|
| `dir` | `MEDUT_MEDIA_DIR_TX` (sent) or `MEDUT_MEDIA_DIR_RX` (received) |
| `transport` | `"UDP"`, `"TCP"`, `"TLS"`, `"WS"`, `"WSS"` |
| `remote_addr` | `"ip:port"` of the remote endpoint |
| `raw_message` | Complete SIP message text |
| `timestamp_us` | Microseconds since boot |

Example handler:

```c
case MEDUT_EV_SIP_TRACE: {
    const char *arrow = (ev->u.sip_trace.dir == MEDUT_MEDIA_DIR_TX) ? ">>>" : "<<<";
    printf("[%s %s %s]\n%s\n---\n",
           arrow, ev->u.sip_trace.transport, ev->u.sip_trace.remote_addr,
           ev->u.sip_trace.raw_message);
    break;
}
```

**Warning:** SIP trace generates high event volume. Enable only for debugging; disable in production to reduce CPU and memory overhead.

---

## SDP negotiation trace

Set `cfg.trace_sdp_diff = true` to receive `MEDUT_EV_SDP_NEGOTIATION` after each SDP offer/answer exchange:

```c
cfg.trace_sdp_diff = true;
```

The event payload includes the negotiated codec, media encryption, rejected codecs, and any warnings.

---

## Pcap capture

Write SIP packets to a Wireshark-compatible pcap file. RTP, STUN and DTLS
are not captured (the media sockets have no tap yet); for those, capture on
the network or on the PBX.

```c
// Start capture
medut_pcap_start("/tmp/capture.pcap");

// ... make calls ...

// Stop capture and flush
medut_pcap_stop();
```

The pcap file contains synthetic Ethernet/IP/UDP headers around each SIP message so Wireshark can decode the SIP layer without a network interface capture.

**Workflow:**

1. Start pcap before initiating calls.
2. Reproduce the issue.
3. Stop pcap.
4. Open the file in Wireshark: `wireshark /tmp/capture.pcap`
5. Filter SIP with `sip`, RTP with `rtp`, or RTCP with `rtcp`.

---

## Log levels

Set `cfg.log_level` to control verbosity:

| Level | Value | What you see |
|---|---|---|
| Error | 0 | Failures and crashes only |
| Warning | 1 | Configuration issues, retries |
| Info | 2 | Registration state, call lifecycle |
| Debug | 3 | Internal state machines, packet details |

Log messages are delivered via `MEDUT_EV_LOG` events. To suppress events entirely, set `log_level = -1` (no events emitted — only stats/trace events).

---

## Putting it all together

A typical monitoring setup:

```c
medut_config_t cfg;
medut_config_init(&cfg);

cfg.log_level         = 1;     // warnings
cfg.stats_interval_ms = 5000;  // RTCP stats every 5 s
cfg.trace_sip         = false; // enable on demand
cfg.trace_sdp_diff    = true;  // always track codec negotiation
cfg.mos_method        = MEDUT_MOS_EMODEL;

// In production: no pcap. On bug report: enable pcap + SIP trace.
```
