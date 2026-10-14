import Foundation
import Medut

/// Stack-wide audio controls: `Medut.shared.audio`.
///
/// The processing toggles reach calls that are already up, from their next
/// frame.  On iOS the VoiceProcessingIO unit already cancels echo and runs its
/// own NS/AGC, so the SDK keeps its own NS/AGC off there (see medut.h).
public final class MedutAudio: @unchecked Sendable {
    init() {}

    private var gate: StackGate { Medut.shared.gate }

    // ── Processing ───────────────────────────────────────────────────────

    /// Turn echo control off, or back to the mode chosen at start.
    public func setEchoCancellation(_ enabled: Bool) throws {
        try gate.read(nil, "medut_set_aec") { medut_set_aec(enabled) }
    }

    /// Only off <-> the start-time mode is valid at runtime; `.webRTC` is
    /// unavailable on iOS.
    public func setAECMode(_ mode: AECMode) throws {
        try gate.read(nil, "medut_set_aec_mode") {
            try check(medut_set_aec_mode(mode.cValue()), "medut_set_aec_mode")
        }
    }

    /// Built-in suppressor strength, 0...1.
    public func setAECSuppressionLevel(_ level: Float) throws {
        try gate.read(nil, "medut_set_aec_suppression_level") {
            medut_set_aec_suppression_level(level)
        }
    }

    public func setNoiseSuppression(_ enabled: Bool) throws {
        try gate.read(nil, "medut_set_ns") { medut_set_ns(enabled) }
    }

    public func setAutoGainControl(_ enabled: Bool) throws {
        try gate.read(nil, "medut_set_agc") { medut_set_agc(enabled) }
    }

    /// Manual microphone gain in dB, clamped to -20...+20.
    public func setMicGain(dB: Float) throws {
        try gate.read(nil, "medut_set_mic_gain_db") { medut_set_mic_gain_db(dB) }
    }

    /// Manual speaker gain in dB, clamped to -20...+20.
    public func setSpeakerGain(dB: Float) throws {
        try gate.read(nil, "medut_set_speaker_gain_db") { medut_set_speaker_gain_db(dB) }
    }

    /// Jitter buffer bounds for calls set up after this; 0 restores a default.
    public func setJitterBuffer(minMs: UInt32, maxMs: UInt32) throws {
        try gate.read(nil, "medut_set_jitter_buffer") { medut_set_jitter_buffer(minMs, maxMs) }
    }

    public func setJitterBufferType(_ type: JitterBufferType) throws {
        try gate.read(nil, "medut_set_jitter_buffer_type") {
            medut_set_jitter_buffer_type(type.cValue())
        }
    }

    /// Link-adaptive bit rate.  0 for a bound keeps the configured value.
    public func setAdaptiveBitrate(_ enabled: Bool, minBps: UInt32 = 0, maxBps: UInt32 = 0) throws {
        try gate.read(nil, "medut_set_adaptive_bitrate") {
            medut_set_adaptive_bitrate(enabled, minBps, maxBps)
        }
    }

    // ── Devices ──────────────────────────────────────────────────────────

    public func inputDevices() throws -> [AudioDevice] {
        try gate.read(nil, "medut_audio_list_input_devices") {
            listDevices(medut_audio_list_input_devices)
        }
    }

    public func outputDevices() throws -> [AudioDevice] {
        try gate.read(nil, "medut_audio_list_output_devices") {
            listDevices(medut_audio_list_output_devices)
        }
    }

    /// nil selects the platform default.  On iOS prefer
    /// `MedutAudioSession.selectRoute(_:)`, which goes through AVAudioSession.
    public func setInputDevice(_ name: String?) throws {
        try gate.read(nil, "medut_audio_set_input_device") {
            try check(medut_audio_set_input_device(name), "medut_audio_set_input_device")
        }
    }

    public func setOutputDevice(_ name: String?) throws {
        try gate.read(nil, "medut_audio_set_output_device") {
            try check(medut_audio_set_output_device(name), "medut_audio_set_output_device")
        }
    }

    private func listDevices(_ fn: (UnsafeMutablePointer<medut_audio_device_t>?, Int32) -> Int32)
        -> [AudioDevice] {
        var buf = [medut_audio_device_t](repeating: medut_audio_device_t(), count: 32)
        let n = buf.withUnsafeMutableBufferPointer { fn($0.baseAddress, Int32($0.count)) }
        return buf.prefix(Int(max(0, n))).map(AudioDevice.init)
    }

    // ── App-owned audio device ───────────────────────────────────────────
    //
    // With `useExternalDevice(true)` the SDK opens no AudioUnit; the app
    // captures and plays PCM itself and moves it with push/pull.  The app
    // then owns echo cancellation too (capture through VoiceProcessingIO and
    // say so with `setExternalVoicePath(true)`).  push, pull, format and
    // isActive are safe from real-time audio threads.

    public func useExternalDevice(_ enabled: Bool) throws {
        try gate.read(nil, "medut_audio_use_external") {
            try check(medut_audio_use_external(enabled), "medut_audio_use_external")
        }
    }

    /// Give the stack captured microphone audio (S16 interleaved, any size).
    /// Returns false when no call is capturing.
    @discardableResult
    public func pushExternal(_ pcm: UnsafeBufferPointer<Int16>) -> Bool {
        guard let base = pcm.baseAddress else { return false }
        return medut_audio_external_push(base, pcm.count) == 0
    }

    /// Fill `pcm` with audio to play; always fully written, silence when no
    /// call is playing.  Returns false in that case.
    @discardableResult
    public func pullExternal(_ pcm: UnsafeMutableBufferPointer<Int16>) -> Bool {
        guard let base = pcm.baseAddress else { return false }
        return medut_audio_external_pull(base, pcm.count) == 0
    }

    /// The current call's negotiated format, or nil before media is up.  Poll
    /// it: a re-INVITE can change the codec with no state change.
    public var externalFormat: ExternalAudioFormat? {
        var rate: UInt32 = 0
        var ch: UInt8 = 0
        var ptime: UInt32 = 0
        guard medut_audio_external_format(&rate, &ch, &ptime) == 0 else { return nil }
        return ExternalAudioFormat(sampleRate: rate, channels: ch, ptimeMs: ptime)
    }

    public var isExternalActive: Bool { medut_audio_external_is_active() }

    /// Report that the app's device captures through the platform voice path
    /// (VoiceProcessingIO), so the SDK keeps its own NS/AGC off.  Call from
    /// the device's start and stop, never from the audio callback.
    public func setExternalVoicePath(_ active: Bool) {
        medut_audio_external_set_voice_path(active)
    }
}
