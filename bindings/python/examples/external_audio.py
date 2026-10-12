"""
external_audio.py — take over the microphone and speaker from the SDK.

Demonstrates the app-owned audio device end to end on a desktop, with no audio
hardware involved at all: this script *is* the sound card. It feeds the far end
a synthesised tone instead of a microphone, and writes everything the far end
says to a WAV file instead of playing it.

    app "capture" thread   -> sdk.external_audio_push()  -> far end hears a tone
    app "playback" thread  <- sdk.external_audio_pull()  -> far-end-audio.wav

Everything else stays with the SDK — SIP, ICE, SRTP, codec negotiation, the
jitter buffer. Only the device is yours.

Usage:
    python external_audio.py account.json bob@pbx.example.com
    python external_audio.py account.json                      # receive mode

    TONE_HZ=0 python external_audio.py ...    # push silence instead of a tone
    OUT_WAV=/tmp/rx.wav python external_audio.py ...

The account JSON is the same file quickstart.py takes.

Why this example exists: it is the cheapest way to verify the app-owned device
is really working, because a wrong answer is audible at the far end (no tone)
and visible in the WAV (empty file). On mobile the same two loops live in the
plugin's native layer — Kotlin over JNI, Swift calling the C — never in the
managed language, because a GC pause on the capture path is a dropped frame.
"""

import json
import math
import os
import struct
import sys
import threading
import time
import wave
from typing import Optional

import medut as sdk

TONE_HZ = float(os.environ.get("TONE_HZ", "440"))
OUT_WAV = os.environ.get("OUT_WAV", "far-end-audio.wav")


class AppOwnedAudio:
    """The two realtime loops the app owns once the SDK lets go of the device.

    Both are paced by the call's own clock: one ptime of audio per tick, at the
    rate the codec negotiated. That is deliberately naive — a real app is paced
    by AudioRecord/AudioTrack or AVAudioEngine, which block until the hardware
    is ready. Here there is no hardware, so sleep stands in for it.
    """

    def __init__(self):
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self._phase = 0.0
        self._wav: Optional[wave.Wave_write] = None
        self._fmt: Optional[tuple] = None

    # ── Format ──────────────────────────────────────────────────────────────

    def wait_for_format(self, timeout=10.0) -> Optional[tuple]:
        """Poll until the call has media.

        There is no "media is up" event: call state "established" is a SIP
        state and races the device by a few milliseconds, and a mid-call
        re-INVITE can change the codec with no state change at all. Polling the
        format is how you learn both.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            fmt = sdk.external_audio_format()
            if fmt:
                return fmt
            time.sleep(0.02)
        return None

    # ── Capture: what the far end hears ─────────────────────────────────────

    def _capture_loop(self, srate: int, ch: int, ptime: int):
        nsamp = srate * ch * ptime // 1000
        period = ptime / 1000.0
        step = 2.0 * math.pi * TONE_HZ / srate
        next_tick = time.monotonic()

        while not self._stop.is_set():
            if TONE_HZ > 0:
                samples = []
                for _ in range(nsamp // ch):
                    v = int(12000 * math.sin(self._phase))
                    self._phase += step
                    samples.extend([v] * ch)
                pcm = struct.pack(f"<{nsamp}h", *samples)
            else:
                pcm = b"\x00" * (nsamp * 2)

            err = sdk.external_audio_push(pcm)
            # ENODEV just means no call is capturing right now — the app pushed
            # between calls. Not worth reacting to.
            if err not in (0, 19):
                print(f"push failed: {err}")

            next_tick += period
            time.sleep(max(0.0, next_tick - time.monotonic()))

    # ── Playback: what the local user would hear ────────────────────────────

    def _playback_loop(self, srate: int, ch: int, ptime: int):
        nsamp = srate * ch * ptime // 1000
        period = ptime / 1000.0
        next_tick = time.monotonic()

        while not self._stop.is_set():
            # Always returns a full buffer — silence when no call is up — so
            # there is nothing to branch on before handing it to the "speaker".
            pcm = sdk.external_audio_pull(nsamp)
            if self._wav:
                self._wav.writeframes(pcm)

            next_tick += period
            time.sleep(max(0.0, next_tick - time.monotonic()))

    # ── Lifecycle ───────────────────────────────────────────────────────────

    def start(self) -> bool:
        fmt = self.wait_for_format()
        if not fmt:
            print("No media on the call — nothing to drive.")
            return False

        srate, ch, ptime = fmt
        self._fmt = fmt
        print(f"Negotiated audio: {srate} Hz, {ch} ch, {ptime} ms "
              f"({srate * ch * ptime // 1000} samples/frame)")

        self._wav = wave.open(OUT_WAV, "wb")
        self._wav.setnchannels(ch)
        self._wav.setsampwidth(2)
        self._wav.setframerate(srate)

        self._stop.clear()
        for target in (self._capture_loop, self._playback_loop):
            t = threading.Thread(target=target, args=(srate, ch, ptime),
                                 daemon=True)
            t.start()
            self._threads.append(t)

        tone = f"a {TONE_HZ:g} Hz tone" if TONE_HZ > 0 else "silence"
        print(f"Pushing {tone}; writing the far end to {OUT_WAV}")
        return True

    def stop(self):
        self._stop.set()
        for t in self._threads:
            t.join(timeout=1.0)
        self._threads.clear()
        if self._wav:
            frames = self._wav.getnframes()
            self._wav.close()
            self._wav = None
            secs = frames / self._fmt[0] if self._fmt else 0
            print(f"Wrote {OUT_WAV}: {frames} frames ({secs:.1f}s)")


def _load_account(path: str):
    with open(path) as f:
        cfg = json.load(f)
    uri = cfg.get("uri", "")
    password = cfg.get("password", "")
    kwargs = {k: v for k, v in cfg.items()
              if k not in ("uri", "password", "enabled")}
    return uri, password, kwargs


def main():
    if len(sys.argv) < 2:
        print("usage: external_audio.py account.json [callee-uri]")
        return 1

    uri, password, account_kwargs = _load_account(sys.argv[1])
    callee = sys.argv[2] if len(sys.argv) >= 3 else None

    sdk.configure(log_level=0)

    # Take the device before any call opens one. Doing this after a call is up
    # also works — the SDK re-points live calls — but claiming it first keeps
    # the SDK from touching the platform audio device at all.
    sdk.use_external_audio(True)
    print("App owns the microphone and speaker; the SDK opens no device.")

    audio = AppOwnedAudio()

    acc = sdk.create_account(uri, password, **account_kwargs)
    acc.register()

    active: Optional[sdk.Call] = None

    @sdk.on("registered")
    def _(ev):
        nonlocal active
        print("Registered.")
        if callee:
            active = acc.call(callee)
            print(f"Dialling {callee}")

    @sdk.on("reconnecting")
    def _(ev):
        print(f"Reconnecting: {ev.error_str or ev.error}")

    @sdk.on("reg_failed")
    def _(ev):
        print(f"Registration failed: {ev.error}")
        sdk.stop()

    @sdk.on("incoming_call")
    def _(ev):
        nonlocal active
        active = ev.call
        print(f"Incoming call from {ev.from_uri} — answering")
        ev.call.answer()

    @sdk.on("call_state")
    def _(ev):
        print(f"Call state: {ev.state}")
        if ev.state == "established":
            # Start the loops here, not on the SIP event alone: start() waits
            # for the device to actually open before sizing its buffers.
            audio.start()
        elif ev.state in ("ended", "failed"):
            audio.stop()
            sdk.stop()

    try:
        sdk.run()
    except KeyboardInterrupt:
        pass
    finally:
        audio.stop()
        # Hand the device back before shutting down. Not strictly required —
        # shutdown tears the driver down either way — but it keeps the pairing
        # obvious, and the setting does not survive a restart regardless.
        sdk.use_external_audio(False)

    return 0


if __name__ == "__main__":
    sys.exit(main())
