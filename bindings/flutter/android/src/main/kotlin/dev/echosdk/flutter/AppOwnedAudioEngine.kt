package dev.echosdk.flutter

import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.NoiseSuppressor
import android.os.Process
import android.util.Log
import dev.echosdk.ExternalAudio
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicLong

/**
 * Reference implementation of the app-owned audio device on Android.
 *
 * The SDK stops opening any capture or playback device
 * ([ExternalAudio.nativeUseExternal]); this class becomes the device. It owns
 * the microphone and speaker while SIP, ICE, SRTP, codecs and the jitter buffer
 * stay in the SDK.
 *
 * ## Why an app would want this
 *
 * Not for latency — the SDK's own OpenSL ES driver is fine. It is for control:
 * when the platform and the SDK's driver disagree about routing (Bluetooth SCO
 * that connects after the stream opened, Telecom owning the route, a call that
 * comes up one-way), owning the device turns a race into program order.
 *
 * ## Echo cancellation is not optional here
 *
 * The SDK's mobile AEC *is* the capture preset of the driver this displaces. It
 * follows the device out. So this must capture through
 * [MediaRecorder.AudioSource.VOICE_COMMUNICATION] with the platform in
 * [AudioManager.MODE_IN_COMMUNICATION] — that is the same HAL path
 * `platform/android/sles_vc.c` requests — or the call echoes badly.
 *
 * ## Threads
 *
 * Three, and the third is what makes format changes tractable:
 *
 *  - **watcher** polls the negotiated format every 20 ms while armed. There is
 *    no "media is up" event in the SDK: call state `established` is a SIP state
 *    and races the device, and a mid-call re-INVITE can renegotiate the codec
 *    with no state change at all. One poll covers "open the device now",
 *    "the format changed, reopen" and "the call ended, close".
 *  - **capture** blocks in `AudioRecord.read()` and pushes.
 *  - **playback** pulls and blocks in `AudioTrack.write()`.
 *
 * Two device threads rather than one because capture and playback run on
 * independent hardware clocks; the blocking call *is* the clock in each case,
 * so there is no timer here and no drift correction to write.
 *
 * Note `push()` and `pull()` share one lock inside the SDK and run the codec
 * inside it, so the two threads serialise briefly (~1 ms of a 20 ms budget with
 * Opus). Using 20 ms frames rather than sles_vc's 10 ms halves how often that
 * happens, and `AudioRecord` per-call overhead is far higher than an OpenSL
 * buffer-queue callback anyway.
 */
internal class AppOwnedAudioEngine(
    private val am: AudioManager,
    private val onError: (code: String, message: String) -> Unit,
) {
    private companion object {
        const val TAG = "EchoSDKAppAudio"

        /** App-side frame size. Independent of ptime: push() re-frames. */
        const val FRAME_MS = 20

        const val WATCH_POLL_MS = 20L
    }

    // Guards the stream lifecycle against the watcher, the plugin and Telecom
    // all poking at it. The realtime loops never take it.
    private val lock = Any()

    @Volatile private var armed = false
    @Volatile private var paused = false
    @Volatile private var running = false

    private var watcher: Thread? = null
    private var captureThread: Thread? = null
    private var playbackThread: Thread? = null

    private var record: AudioRecord? = null
    private var track: AudioTrack? = null
    private var aec: AcousticEchoCanceler? = null
    private var ns: NoiseSuppressor? = null
    /** The SDK has been told capture runs through the voice path. */
    private var voicePath = false

    /** Current format epoch: {srate, ch, ptime}. Null when no device is open. */
    private var openFmt: Triple<Int, Int, Int>? = null
    private var lastError: String? = null

    // Frame counters. The only way to tell "the device opened" from "audio is
    // actually moving" — a silent call looks identical to a working one from
    // the outside, which is the failure mode this whole feature exists to fix.
    private val pushFrames = AtomicLong()
    private val pullFrames = AtomicLong()
    private val pushErrors = AtomicLong()

    /// Peak absolute sample seen from the microphone, as a fraction of full
    /// scale (x1e6 so it fits an AtomicLong). Distinguishes "the capture thread
    /// is running" from "the capture thread is running and the microphone is
    /// actually producing sound" — a stereo config the device accepts but
    /// cannot fill, or a permission Android answers with silence rather than an
    /// error, both look healthy by frame count alone.
    private val capturePeakMicro = AtomicLong()

    /// Frames that carried any non-zero sample. The VOICE_COMMUNICATION path
    /// runs the platform noise suppressor, which emits *exact* digital silence
    /// when it hears no speech — so in a quiet room almost every frame is
    /// legitimately all-zero. A ratio here distinguishes that from a capture
    /// path that is genuinely dead, which a peak or a level snapshot cannot.
    private val nonSilentFrames = AtomicLong()

    // ── Public surface ──────────────────────────────────────────────────────

    /**
     * Start watching for media. Devices open when the call actually has some,
     * which is later than "answered" — see the watcher note above.
     */
    fun arm() {
        synchronized(lock) {
            if (!ExternalAudio.available) {
                report("unavailable", "libechosdk.so is not loaded")
                return
            }
            if (armed) return
            armed = true
            paused = false
            lastError = null

            watcher = Thread({ watchLoop() }, "EchoSDK-audio-watch").apply {
                priority = Thread.NORM_PRIORITY
                start()
            }
        }
    }

    /** Stop everything and release the devices. Safe to call when not armed. */
    fun disarm() {
        val w: Thread?
        synchronized(lock) {
            if (!armed) return
            armed = false
            w = watcher
            watcher = null
        }
        w?.join(500)
        synchronized(lock) { closeStreams() }
    }

    /** Audio focus lost: stop touching the device but keep watching. */
    fun pause() {
        synchronized(lock) {
            if (!armed || paused) return
            paused = true
            closeStreams()
        }
    }

    /** Audio focus regained; the watcher reopens on its next tick. */
    fun resume() {
        synchronized(lock) { paused = false }
    }

    /**
     * Recreate both streams against the route that is now in force.
     *
     * This is the pre-31 Bluetooth fix. On API 31+ `setCommunicationDevice()`
     * moves live streams and nothing is needed. On API 24-30 a stream created
     * before SCO reaches CONNECTED is routed to the non-SCO device **for its
     * lifetime**, and `startBluetoothSco()` takes 0.5-2 s — which is exactly
     * the long-standing "pick Bluetooth, audio stays on the earpiece" bug. Now
     * that the app owns the streams, it can simply build new ones.
     */
    fun restartStreams() {
        synchronized(lock) {
            if (!armed || paused || openFmt == null) return
            val fmt = openFmt!!
            closeStreams()
            openStreams(fmt.first, fmt.second, fmt.third)
        }
    }

    fun status(): Map<String, Any?> = synchronized(lock) {
        val fmt = openFmt
        mapOf(
            "armed" to armed,
            "paused" to paused,
            "running" to running,
            "available" to ExternalAudio.available,
            "sampleRate" to fmt?.first,
            "channels" to fmt?.second,
            "ptimeMs" to fmt?.third,
            "pushFrames" to pushFrames.get(),
            "pullFrames" to pullFrames.get(),
            "pushErrors" to pushErrors.get(),
            "capturePeak" to capturePeakMicro.get() / 1.0e6,
            "nonSilentFrames" to nonSilentFrames.get(),
            "lastError" to lastError,
        )
    }

    // ── Watcher ─────────────────────────────────────────────────────────────

    private fun watchLoop() {
        val out = IntArray(3)   // reused: no allocation on the poll path

        while (armed) {
            try {
                val err = ExternalAudio.nativeFormat(out)
                synchronized(lock) {
                    if (!armed) return@synchronized
                    if (paused) {
                        if (openFmt != null) closeStreams()
                        return@synchronized
                    }
                    if (err != 0) {
                        // ENODEV: no call has media. Between calls, or ended.
                        if (openFmt != null) closeStreams()
                    } else {
                        val want = Triple(out[0], out[1], out[2])
                        if (openFmt == null) {
                            openStreams(want.first, want.second, want.third)
                        } else if (openFmt != want) {
                            // Mid-call codec renegotiation. Costs ~150-250 ms
                            // of silence; rare enough to be worth the
                            // simplicity of not resampling.
                            Log.i(TAG, "format changed $openFmt -> $want")
                            closeStreams()
                            openStreams(want.first, want.second, want.third)
                        }
                    }
                }
            } catch (t: Throwable) {
                Log.e(TAG, "watcher", t)
            }
            try {
                Thread.sleep(WATCH_POLL_MS)
            } catch (_: InterruptedException) {
                return
            }
        }
    }

    // ── Stream lifecycle (always under `lock`) ──────────────────────────────

    private fun openStreams(srate: Int, ch: Int, ptime: Int) {
        val frameSamples = srate * ch * FRAME_MS / 1000
        val frameBytes = frameSamples * 2

        // Capture is ALWAYS mono, whatever the codec negotiated. Phones have
        // no stereo voice-communication capture path: asking for
        // CHANNEL_IN_STEREO gets an AudioRecord that initialises, reads
        // without error, and returns near-silence — a working-looking call the
        // far end cannot hear. Opus routinely negotiates 48 kHz stereo, so
        // this is the common case, not an edge one. Mono is up-mixed to the
        // negotiated channel count on the way to push().
        val inMask = AudioFormat.CHANNEL_IN_MONO
        val outMask = if (ch == 2) AudioFormat.CHANNEL_OUT_STEREO
                      else AudioFormat.CHANNEL_OUT_MONO

        val minRec = AudioRecord.getMinBufferSize(
            srate, inMask, AudioFormat.ENCODING_PCM_16BIT)
        val minTrk = AudioTrack.getMinBufferSize(
            srate, outMask, AudioFormat.ENCODING_PCM_16BIT)
        if (minRec <= 0 || minTrk <= 0) {
            // The negotiated rate is not one this device will open. Better to
            // hand the call back to the SDK's driver than to fail it.
            report("unsupported-rate",
                   "device will not open ${srate}Hz ${ch}ch")
            return
        }

        val r: AudioRecord
        val t: AudioTrack
        try {
            r = AudioRecord.Builder()
                // The whole point: this is the HAL path that carries the
                // platform's echo canceller. sles_vc.c asks for the same one.
                .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(srate)
                        .setChannelMask(inMask)
                        .build())
                .setBufferSizeInBytes(maxOf(minRec * 2, frameBytes * 4))
                .build()

            t = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build())
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(srate)
                        .setChannelMask(outMask)
                        .build())
                .setBufferSizeInBytes(maxOf(minTrk * 2, frameBytes * 4))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
        } catch (t2: Throwable) {
            report("device-open", "could not build audio devices: ${t2.message}")
            return
        }

        if (r.state != AudioRecord.STATE_INITIALIZED) {
            // Overwhelmingly this is RECORD_AUDIO not being granted. Worth
            // naming: the same condition on the SDK's own driver produces a
            // call that comes up with the far end hearing silence, and no
            // error anywhere.
            r.release()
            t.release()
            report("mic-permission",
                   "AudioRecord did not initialise — is RECORD_AUDIO granted?")
            return
        }

        // Belt and braces on top of the VOICE_COMMUNICATION preset: some
        // vendors expose these separately. Best-effort; never fatal.
        // AGC is deliberately skipped — it fights Opus's own gain control.
        aec = runCatching {
            if (AcousticEchoCanceler.isAvailable())
                AcousticEchoCanceler.create(r.audioSessionId)?.apply { enabled = true }
            else null
        }.getOrNull()
        ns = runCatching {
            if (NoiseSuppressor.isAvailable())
                NoiseSuppressor.create(r.audioSessionId)?.apply { enabled = true }
            else null
        }.getOrNull()

        record = r
        track = t
        openFmt = Triple(srate, ch, ptime)
        // Per-call, not cumulative: "did the mic work on THIS call" is the
        // question worth being able to answer.
        capturePeakMicro.set(0)
        nonSilentFrames.set(0)
        running = true

        // Prime with one frame of silence before play(), so the first write
        // does not race the track starting. Same reasoning as sles_vc.c.
        val silence = ByteBuffer.allocateDirect(frameBytes)
            .order(ByteOrder.nativeOrder())
        t.write(silence, frameBytes, AudioTrack.WRITE_BLOCKING)

        r.startRecording()
        t.play()

        // The SDK's own NS and AGC would only cost speech on top of the
        // platform's: tell it they are under the microphone now. Read back,
        // not assumed — though the preset taking says nothing about whether
        // this phone's HAL actually cancels echo.
        voicePath = r.audioSource == MediaRecorder.AudioSource.VOICE_COMMUNICATION &&
                    r.recordingState == AudioRecord.RECORDSTATE_RECORDING
        if (voicePath) ExternalAudio.setVoicePath(true)

        captureThread = Thread({ captureLoop(frameSamples / ch, ch) },
                               "EchoSDK-audio-capture").apply { start() }
        playbackThread = Thread({ playbackLoop(frameSamples, frameBytes) },
                                "EchoSDK-audio-playback").apply { start() }

        Log.i(TAG, "app-owned audio open: ${srate}Hz ${ch}ch ptime=${ptime}ms " +
                   "frame=${FRAME_MS}ms aec=${aec != null} ns=${ns != null} " +
                   "voicePath=$voicePath")
    }

    private fun closeStreams() {
        running = false

        val ct = captureThread
        val pt = playbackThread
        captureThread = null
        playbackThread = null

        // Stop the hardware first so the blocking read/write returns and the
        // loops can notice `running` went false.
        runCatching { record?.stop() }
        runCatching { track?.pause() }
        if (voicePath) {
            ExternalAudio.setVoicePath(false)
            voicePath = false
        }

        ct?.join(200)
        pt?.join(200)

        runCatching { aec?.release() }
        runCatching { ns?.release() }
        runCatching { record?.release() }
        runCatching { track?.release() }

        aec = null
        ns = null
        record = null
        track = null
        openFmt = null
    }

    // ── Realtime loops ──────────────────────────────────────────────────────

    /** [monoFrames] mono sample frames per read; [ch] the negotiated channel
     *  count the SDK expects back. */
    private fun captureLoop(monoFrames: Int, ch: Int) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val monoBytes = monoFrames * 2
        val buf = ByteBuffer.allocateDirect(monoBytes).order(ByteOrder.nativeOrder())
        // Up-mix target. Same buffer every frame; only used when ch > 1.
        val outBuf = if (ch > 1)
            ByteBuffer.allocateDirect(monoBytes * ch).order(ByteOrder.nativeOrder())
        else null
        val r = record ?: return
        var consecutiveErrors = 0

        while (running) {
            buf.clear()
            val n = r.read(buf, monoBytes, AudioRecord.READ_BLOCKING)
            if (n <= 0) {
                if (!running) return
                // ERROR_INVALID_OPERATION / ERROR_DEAD_OBJECT: try once more,
                // then give up loudly rather than stream silence forever.
                if (++consecutiveErrors >= 2) {
                    report("capture-dead", "AudioRecord.read returned $n twice")
                    return
                }
                continue
            }
            consecutiveErrors = 0

            // Cheap peak meter: a few hundred shorts per 20 ms frame.
            var peak = 0
            val shorts = buf.asShortBuffer()
            var i = 0
            val limit = n / 2
            while (i < limit) {
                val v = shorts.get(i).toInt()
                val a = if (v < 0) -v else v
                if (a > peak) peak = a
                i++
            }
            val micro = (peak * 1_000_000L) / 32768L
            if (micro > capturePeakMicro.get()) capturePeakMicro.set(micro)
            if (peak > 0) nonSilentFrames.incrementAndGet()

            // Up-mix mono to the negotiated layout: the SDK opened the device
            // at `ch` channels and push() interprets the buffer that way, so a
            // mono buffer would be read as half a frame of garbage.
            val pushBuf: ByteBuffer
            val pushSamples: Int
            if (outBuf != null) {
                val src = buf.asShortBuffer()
                val dst = outBuf.asShortBuffer()
                val frames = n / 2
                var i = 0
                while (i < frames) {
                    val v = src.get(i)
                    var c = 0
                    while (c < ch) { dst.put(i * ch + c, v); c++ }
                    i++
                }
                pushBuf = outBuf
                pushSamples = frames * ch
            } else {
                pushBuf = buf
                pushSamples = n / 2
            }

            pushBuf.position(0)
            val err = ExternalAudio.nativePush(pushBuf, pushSamples)
            // ENODEV just means no call is capturing right now.
            if (err == 0) {
                pushFrames.incrementAndGet()
            } else if (err != ExternalAudio.ENODEV) {
                pushErrors.incrementAndGet()
                Log.w(TAG, "push failed: $err")
            }
        }
    }

    private fun playbackLoop(frameSamples: Int, frameBytes: Int) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val buf = ByteBuffer.allocateDirect(frameBytes).order(ByteOrder.nativeOrder())
        val t = track ?: return

        while (running) {
            buf.clear()
            // Always fills — silence when no call is up — so there is nothing
            // to branch on and the track never underruns between calls.
            if (ExternalAudio.nativePull(buf, frameSamples) == 0) {
                pullFrames.incrementAndGet()
            }
            buf.position(0)
            val n = t.write(buf, frameBytes, AudioTrack.WRITE_BLOCKING)
            if (n < 0) {
                if (!running) return
                report("playback-dead", "AudioTrack.write returned $n")
                return
            }
        }
    }

    private fun report(code: String, message: String) {
        Log.e(TAG, "$code: $message")
        lastError = "$code: $message"
        onError(code, message)
    }
}
