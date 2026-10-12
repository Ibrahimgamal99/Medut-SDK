"""
quickstart.py — register an account and make or receive one call.

Usage:
    python quickstart.py account.json                          # receive mode
    python quickstart.py account.json bob@pbx.example.com      # dial
    python quickstart.py alice@pbx.example.com secret          # legacy CLI mode (receive)
    python quickstart.py alice@pbx.example.com secret bob@...  # legacy CLI mode (dial)

Debug:
    MEDUT_DEBUG_INIT=1 python quickstart.py account.json     # verbose init/shutdown trace

Minimal JSON account config (account.json):
{
  "enabled":      true,
  "uri":          "120@pbx.example.com",
  "password":     "secret",
  "display_name": "Extension 120",
  "transport":    "wss",
  "media_enc":    "dtls_srtp",
  "ice_enabled":  true,
  "rtcp_mux":     true,
  "stun_server":  "stun:stun.l.google.com:19302",
  "verify_tls":   true,
  "audio_codec":  "opus"
}

Optional overrides (add when needed):
  "server_url":     "wss://pbx.example.com:443/ws"
  "outbound_proxy": "sip:proxy.example.com:5060;transport=udp"
  "auth_user":      "alice"
  "extra_headers":  {"X-Tenant-Id": "42"}
  "audio_codecs":   ["opus", "pcmu"]
  "rel100":         "enabled"

Media stats:
  Stats are printed every STATS_INTERVAL_S seconds while a call is active.
  Change STATS_INTERVAL_S below to any value (e.g. 1, 2, 5).
  Press 's' during a call for an immediate refresh.
"""

import json
import sys
import threading
import time
from typing import Optional

import medut as sdk

STATS_INTERVAL_S = 5.0


# ── Live call timer ──────────────────────────────────────────────────────────
#
# The elapsed time is kept on a wall clock here rather than read from
# CallStats.call_duration_ms, for the same reason a GUI would: that figure only
# advances once per stats tick (STATS_INTERVAL_S, and nothing at all if stats
# are disabled), so a clock built on it moves in five-second jumps.
#
# The ticker owns one terminal line and rewrites it in place with \r.  Anything
# else printed while a call is up has to clear that line first, or it lands on
# top of the timer — which is what say() is for.

_call_started_at: Optional[float] = None
_ticker_stop = threading.Event()
_ticker_lock = threading.Lock()
_status_shown = False
# Bumped by every start_call_timer().  A ticker exits as soon as its own
# generation is stale, so a second call in the same session cannot end up with
# two threads drawing the status line: without it, a ticker asleep in its
# one-second wait when the previous call ended finds the event cleared again by
# the time it wakes, and carries on alongside the new one.
_ticker_gen = 0


def _fmt_elapsed(seconds: float) -> str:
    s = int(seconds)
    if s >= 3600:
        return f"{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}"
    return f"{s // 60}:{s % 60:02d}"


def say(*args, **kwargs) -> None:
    """print() that does not collide with the in-call status line."""
    global _status_shown
    with _ticker_lock:
        if _status_shown:
            sys.stdout.write("\r\033[K")
            _status_shown = False
        print(*args, **kwargs)


def _tick(gen: int) -> None:
    global _status_shown
    while not _ticker_stop.wait(1.0):
        started = _call_started_at
        if started is None or gen != _ticker_gen:
            return
        with _ticker_lock:
            # Re-check under the lock: stop_call_timer() sets the event and then
            # clears the line, and without this a tick that had already passed
            # the wait() could redraw over the cleared line afterwards.
            if _ticker_stop.is_set() or gen != _ticker_gen:
                return
            sys.stdout.write(f"\rIn call · {_fmt_elapsed(time.monotonic() - started)}")
            sys.stdout.flush()
            _status_shown = True


def start_call_timer() -> None:
    """Begin counting talk time.  Idempotent, so hold/resume does not restart it."""
    global _call_started_at, _ticker_gen
    if _call_started_at is not None:
        return
    _call_started_at = time.monotonic()
    _ticker_gen += 1
    _ticker_stop.clear()
    threading.Thread(target=_tick, args=(_ticker_gen,), daemon=True).start()


def stop_call_timer() -> Optional[float]:
    """Stop the ticker and return the total talk time, or None if never started."""
    global _call_started_at
    _ticker_stop.set()
    started, _call_started_at = _call_started_at, None
    say("", end="")   # clear the status line
    return None if started is None else time.monotonic() - started


def _config_from_json(path_or_str: str):
    j = json.loads(path_or_str) if path_or_str.startswith("{") else json.load(open(path_or_str))

    uri      = j.pop("uri", "")
    password = j.pop("password", "")
    enabled  = j.pop("enabled", True)

    # audio_codec (singular) alias
    if "audio_codec" in j and "audio_codecs" not in j:
        v = j.pop("audio_codec")
        if v:
            j["audio_codecs"] = [v] if isinstance(v, str) else v
    elif "audio_codec" in j:
        j.pop("audio_codec")

    if isinstance(j.get("audio_codecs"), str):
        j["audio_codecs"] = [j["audio_codecs"]] if j["audio_codecs"] else []

    # verify_tls → global SDK setting
    verify_tls = j.pop("verify_tls", True)

    sdk_kwargs = {}
    if not verify_tls:
        sdk_kwargs["verify_server"] = False

    account_kwargs = {k: v for k, v in j.items() if v is not None}
    return uri, password, enabled, account_kwargs, sdk_kwargs


def _config_from_cli(sip_uri: str, password: str):
    u    = sip_uri[4:] if sip_uri.startswith("sip:") else sip_uri
    at   = u.find("@")
    host = u[at + 1:] if at != -1 else u
    host = host[:host.find(":")] if ":" in host else host
    account_kwargs = {
        "transport":   "wss",
        "server_url":  f"wss://{host}:443/",
        "media_enc":   "dtls_srtp",
        "ice_enabled": True,
        "stun_server": "stun:stun.l.google.com:19302",
    }
    return sip_uri, password, True, account_kwargs, {}


def print_devices():
    for label, fn in (("Input", sdk.list_input_devices), ("Output", sdk.list_output_devices)):
        devices = fn()
        if devices:
            print(f"{label} devices ({len(devices)}):")
            for i, d in enumerate(devices):
                print(f"  [{i}] {d['name']}{'  *default*' if d['is_default'] else ''}")


def main():
    if len(sys.argv) < 2:
        print("Usage:\n"
              "  quickstart.py account.json [callee-uri]\n"
              "  quickstart.py <sip-uri> <password> [<callee-uri>]")
        return 1

    arg1   = sys.argv[1]
    callee: Optional[str] = None

    if arg1.endswith(".json") or arg1.startswith("{"):
        try:
            uri, password, enabled, account_kwargs, sdk_kwargs = _config_from_json(arg1)
        except Exception as e:
            print(f"Failed to load config: {e}")
            return 1
        if len(sys.argv) >= 3:
            callee = sys.argv[2]
    else:
        if len(sys.argv) < 3:
            print(f"usage: {sys.argv[0]} <sip-uri> <password> [<callee-uri>]")
            return 1
        uri, password, enabled, account_kwargs, sdk_kwargs = _config_from_cli(arg1, sys.argv[2])
        if len(sys.argv) >= 4:
            callee = sys.argv[3]

    if not enabled:
        print("Account is disabled in config.")
        return 0
    if not uri:
        print("No URI in config.")
        return 1

    # Global SDK config.  stats_interval_ms stays at its 2 s default: FEC,
    # bitrate adaptation and quality alerts run off that tick (the printing
    # cadence is STATS_INTERVAL_S, separately).  aec_mode=2 is WebRTC AEC3,
    # full duplex; init falls back to the half-duplex suppressor on a build or
    # machine without it.  A raw desktop mic also wants NS and AGC.
    sdk.configure(log_level=0, aec_mode=2, ns=True, agc=True, **sdk_kwargs)

    acc = sdk.create_account(uri, password, **account_kwargs)
    acc.register()

    active_call: Optional[sdk.Call] = None
    call_lock = threading.Lock()

    # ── Event handlers ────────────────────────────────────────────────────────

    @sdk.on("registered")
    def _(ev):
        nonlocal active_call
        say("Registered OK.")
        print_devices()
        if callee:
            say(f"Dialling {callee} ...")
            with call_lock:
                active_call = sdk.call(callee)
        else:
            say("Waiting for incoming call...")

    @sdk.on("reconnecting")
    def _(ev):
        # The SDK is recovering this by itself — say so and keep waiting.
        detail = ev.error_str or sdk.strerror(ev.error)
        if ev.retry_attempt:
            say(f"Reconnecting: {detail} "
                f"(attempt {ev.retry_attempt} in {ev.retry_delay_ms} ms)")
        else:
            say(f"Reconnecting: {detail}")

    @sdk.on("reg_failed")
    def _(ev):
        # Terminal: bad credentials, or the retry budget is spent.
        detail = ev.error_str or sdk.strerror(ev.error)
        say(f"Registration failed: {detail}")
        sdk.stop()

    @sdk.on("incoming_call")
    def _(ev):
        nonlocal active_call
        say(f"\n=== Incoming call from {ev.from_uri} ===")
        say("Press 'a' + Enter to answer, 'r' + Enter to reject")
        with call_lock:
            active_call = ev.call

        def _answer_or_reject():
            for line in sys.stdin:
                ch = line.strip().lower()
                if ch == "a":
                    with call_lock:
                        if active_call:
                            try:
                                active_call.answer()
                            except Exception as e:
                                say(f"answer failed: {e}")
                    break
                elif ch == "r":
                    with call_lock:
                        if active_call:
                            active_call.hangup()
                    break

        threading.Thread(target=_answer_or_reject, daemon=True).start()

    @sdk.on("call_state")
    def _(ev):
        nonlocal active_call
        msg = f"Call state: {ev.state}"
        if ev.reason:
            msg += f"  reason={ev.reason!r}"
        if ev.error:
            msg += f"  error={ev.error}"
        say(msg)

        if ev.state == "established":
            # Talk time starts when the far end answers, not when we dialled.
            start_call_timer()
            say(f"In call · 0:00   (stats every {STATS_INTERVAL_S}s). "
                f"Keys: h=hangup  o=hold  r=resume  m=mute  u=unmute  t=transfer  s=stats-now")
            with call_lock:
                c = active_call
            if c:
                # say("") first: s.print() writes a multi-line block, and the
                # in-call status line has to be cleared out of its way.
                def _show(st):
                    say("", end="")
                    st.print()

                c.poll_stats(interval=STATS_INTERVAL_S, on_update=_show)

            def _interactive():
                for line in sys.stdin:
                    ch = line.strip().lower()
                    with call_lock:
                        c = active_call
                    if not c:
                        break
                    if ch == "h":
                        c.hangup(); break
                    elif ch == "o":
                        try: c.hold();         say("On hold.")
                        except Exception as e: say(f"hold: {e}")
                    elif ch == "r":
                        try: c.resume();       say("Resumed.")
                        except Exception as e: say(f"resume: {e}")
                    elif ch == "m":
                        try: c.mute(True);     say("Muted.")
                        except Exception as e: say(f"mute: {e}")
                    elif ch == "u":
                        try: c.mute(False);    say("Unmuted.")
                        except Exception as e: say(f"unmute: {e}")
                    elif ch == "s":
                        say("", end="")
                        c.stats().print()
                    elif ch == "t":
                        say("", end="")
                        dest = input("Transfer to URI: ").strip()
                        if dest:
                            try: c.transfer(dest); say("Transfer sent.")
                            except Exception as e: say(f"transfer: {e}")

            threading.Thread(target=_interactive, daemon=True).start()

        if ev.state in ("ended", "failed", "cancelled"):
            talked = stop_call_timer()
            if talked is not None:
                say(f"Call ended after {_fmt_elapsed(talked)}.")
            with call_lock:
                active_call = None
            sdk.stop()

    @sdk.on("transfer_request")
    def _(ev):
        kind = "attended" if ev.has_replaces else "blind"
        say(f"=== Transfer request ({kind}): REFER to {ev.refer_to_uri}")
        # Answer it: the far end is waiting for the NOTIFY that says what
        # happened, and only accept/reject sends one. Accepting keeps the new
        # call linked to this one so the SDK reports the outcome for us —
        # hanging up and dialling the URI would not.
        try:
            ev.call.transfer_accept()
            say("    following the transfer")
        except Exception as exc:
            say(f"    could not follow ({exc}); declining")
            ev.call.transfer_reject(603, "Declined")

    @sdk.on("sip_trace")
    def _(ev):
        say(f"{'>>>' if ev.direction == 'tx' else '<<<'}\n{ev.raw_message}\n---")

    @sdk.on("log")
    def _(ev):
        say(f"[sdk] {ev.message}")

    # ── Run ───────────────────────────────────────────────────────────────────

    sdk.run()
    say("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
