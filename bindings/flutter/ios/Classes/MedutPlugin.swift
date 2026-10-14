import Flutter
import UIKit

/// Flutter side of MedutKit (bindings/swift): every Pigeon host call delegates
/// to `Medut.shared` / `MedutAudioSession.shared`, and SDK events are
/// forwarded to Dart.
///
/// The stack belongs to native code for the process lifetime.  Dart's start
/// attaches to it when the app already started it (at launch, or from a
/// PushKit wake), and detaching the engine leaves it running.
///
/// Host calls and event forwarding both run on the main thread, so the maps
/// below need no lock.
public class MedutPlugin: NSObject, FlutterPlugin, MedutHostApi {
    private var events: MedutEventsApi?
    private var subscription: MedutSubscription?
    private var routesObserver: NSObjectProtocol?

    // Objects Dart refers to by id.  Ended calls stay until Dart releases
    // them (or the cap below evicts them), so methods on an ended Dart Call
    // keep resolving.
    private var accounts: [String: Account] = [:]
    private var calls: [String: Call] = [:]
    private var endedCalls: [String] = []
    private static let maxEndedCalls = 32

    private static let audioRoutesChanged: Int64 = 100
    private static let appOwnedAudioError: Int64 = 101

    public static func register(with registrar: FlutterPluginRegistrar) {
        let plugin = MedutPlugin()
        MedutHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: plugin)
        plugin.events = MedutEventsApi(binaryMessenger: registrar.messenger())
        plugin.subscription = Medut.shared.addEventHandler(queue: .main) { [weak plugin] event in
            plugin?.forward(event)
        }
        MedutAudioSession.shared.onAppOwnedAudioError = { [weak plugin] code, message in
            DispatchQueue.main.async {
                plugin?.send(EventMessage(type: MedutPlugin.appOwnedAudioError, message: message,
                                          errorCode: code))
            }
        }
        plugin.routesObserver = NotificationCenter.default.addObserver(
            forName: MedutAudioSession.routesDidChangeNotification, object: nil, queue: .main
        ) { [weak plugin] _ in
            guard let plugin = plugin else { return }
            plugin.send(EventMessage(type: MedutPlugin.audioRoutesChanged, routes: plugin.routes()))
        }
        // Keeps the instance alive and gets it detachFromEngine.
        registrar.publish(plugin)
    }

    public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
        subscription?.cancel()
        subscription = nil
        if let observer = routesObserver {
            NotificationCenter.default.removeObserver(observer)
            routesObserver = nil
        }
        MedutAudioSession.shared.onAppOwnedAudioError = nil
        MedutHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: nil)
        events = nil
    }

    // ── Events ───────────────────────────────────────────────────────────

    private func send(_ event: EventMessage) {
        guard let events = events else { return }
        // Main-actor tasks created on the main thread run in creation order.
        Task { @MainActor in try? await events.onEvent(event: event) }
    }

    private func forward(_ event: MedutEvent) {
        if let account = event.account { accounts[account.id.uuidString] = account }
        if let call = event.call { _ = track(call) }
        send(event.message)
    }

    @discardableResult
    private func track(_ call: Call) -> CallMessage {
        let id = call.id.uuidString
        calls[id] = call
        if call.isEnded && !endedCalls.contains(id) {
            endedCalls.append(id)
            while endedCalls.count > MedutPlugin.maxEndedCalls {
                calls.removeValue(forKey: endedCalls.removeFirst())
            }
        }
        return call.message
    }

    // ── Lifecycle ────────────────────────────────────────────────────────

    func start(config: ConfigMessage, options: StartOptionsMessage) throws -> StartResultMessage {
        try sdk {
            let reattached = Medut.shared.isRunning
            if !reattached {
                var startOptions = StartOptions()
                startOptions.monitorNetwork = options.monitorNetwork
                switch options.audioMode {
                case 1: startOptions.audioSession = .callKit
                case 2: startOptions.audioSession = .manual
                default: startOptions.audioSession = .automatic
                }
                // Dart's `telecom` is the SDK's own call-framework
                // integration: CallKit here.
                if options.telecom { startOptions.callKit = CallKitOptions() }
                try Medut.shared.start(try config.sdk(), options: startOptions)
            }
            if options.appOwnedAudio { try MedutAudioSession.shared.useAppOwnedAudio(true) }
            for account in Medut.shared.accounts { accounts[account.id.uuidString] = account }
            return StartResultMessage(
                reattached: reattached,
                version: Medut.version,
                accounts: Medut.shared.accounts.map { $0.message },
                calls: Medut.shared.calls.map { track($0) })
        }
    }

    func shutdown() throws {
        Medut.shared.shutdown()
        accounts.removeAll()
        calls.removeAll()
        endedCalls.removeAll()
    }

    func releaseCall(callId: String) throws {
        guard let call = calls[callId], call.isEnded else { return }
        calls.removeValue(forKey: callId)
        endedCalls.removeAll { $0 == callId }
    }

    // ── Accounts ─────────────────────────────────────────────────────────

    func createAccount(config: AccountConfigMessage, remember: Bool) throws -> AccountMessage {
        try sdk {
            let cfg = try config.sdk()
            let account = try Medut.shared.createAccount(cfg)
            accounts[account.id.uuidString] = account
            if remember { Medut.shared.accountStore.save(cfg) }
            return account.message
        }
    }

    func accountDestroy(accountId: String, forget: Bool) throws {
        guard let account = accounts.removeValue(forKey: accountId) else { return }
        if forget { Medut.shared.accountStore.remove(account.aor) }
        account.destroy()
    }

    func rememberedAccounts() throws -> [String] {
        Medut.shared.accountStore.list().map { $0.uri }
    }

    func forgetAccount(uri: String) throws {
        Medut.shared.accountStore.remove(uri)
    }

    func accountRegister(accountId: String) throws { try sdk { try account(accountId).register() } }
    func accountUnregister(accountId: String) throws { try sdk { try account(accountId).unregister() } }

    func accountRegistrationState(accountId: String) throws -> Int64 {
        try sdk { Int64(try account(accountId).registrationState.rawValue) }
    }

    func accountSetRetryPolicy(accountId: String, initialMs: Int64, maxMs: Int64, backoff: Double,
                               maxAttempts: Int64) throws {
        try sdk {
            try account(accountId).setRetryPolicy(initialMs: UInt32(clamping: initialMs),
                                                  maxMs: UInt32(clamping: maxMs),
                                                  backoff: Float(backoff),
                                                  maxAttempts: UInt32(clamping: maxAttempts))
        }
    }

    func accountCancelRetry(accountId: String) throws { try sdk { try account(accountId).cancelRetry() } }
    func accountRetryNow(accountId: String) throws { try sdk { try account(accountId).retryNow() } }
    func accountKeepaliveNow(accountId: String) throws { try sdk { try account(accountId).keepaliveNow() } }

    func accountSetPushToken(accountId: String, token: String?) throws {
        try sdk {
            let target = try account(accountId)
            try target.setPushToken(token)
            // A remembered account must come back from a push with this token.
            Medut.shared.accountStore.updatePushToken(token, uri: target.aor)
        }
    }

    func accountSetIceServers(accountId: String, servers: [IceServerMessage], policy: Int64) throws {
        try sdk {
            try account(accountId).setICEServers(servers.map { $0.sdk }, policy: try enumValue(policy))
        }
    }

    func accountAddHeader(accountId: String, name: String, value: String) throws {
        try sdk { try account(accountId).addHeader(name: name, value: value) }
    }

    func accountAddRegisterHeader(accountId: String, name: String, value: String) throws {
        try sdk { try account(accountId).addRegisterHeader(name: name, value: value) }
    }

    func accountSetReliableProvisional(accountId: String, mode: Int64) throws {
        try sdk { try account(accountId).setReliableProvisional(try enumValue(mode)) }
    }

    func accountSendMessage(accountId: String, to: String, body: String, contentType: String?) throws {
        try sdk { try account(accountId).sendMessage(to: to, body: body, contentType: contentType) }
    }

    func accountPublishPresence(accountId: String, status: Int64) throws {
        try sdk { try account(accountId).publishPresence(try enumValue(status)) }
    }

    func accountSubscribePresence(accountId: String, uri: String) throws {
        try sdk { try account(accountId).subscribePresence(uri) }
    }

    func accountUnsubscribePresence(accountId: String, uri: String) throws {
        try sdk { try account(accountId).unsubscribePresence(uri) }
    }

    func accountCall(accountId: String, uri: String) throws -> CallMessage {
        try sdk { track(try account(accountId).call(uri)) }
    }

    // ── Calls ────────────────────────────────────────────────────────────

    func callAnswer(callId: String) throws { try sdk { try call(callId).answer() } }
    func callHangup(callId: String) throws { try sdk { try call(callId).hangup() } }

    func callReject(callId: String, statusCode: Int64, reason: String?) throws {
        try sdk { try call(callId).reject(statusCode: UInt16(clamping: statusCode), reason: reason) }
    }

    func callHold(callId: String) throws { try sdk { try call(callId).hold() } }
    func callResume(callId: String) throws { try sdk { try call(callId).resume() } }
    func callIsHeld(callId: String) throws -> Bool { try sdk { try call(callId).isHeld } }

    func callSendDtmf(callId: String, digit: String) throws {
        guard digit.count == 1, let c = digit.first else {
            throw PigeonError(code: "INVALID_ARGUMENT", message: "sendDtmf takes one digit, got \"\(digit)\"",
                              details: -1)
        }
        try sdk { try call(callId).sendDTMF(c) }
    }

    func callTransfer(callId: String, uri: String) throws { try sdk { try call(callId).transfer(to: uri) } }

    func callAttendedTransfer(callId: String, consultationCallId: String) throws {
        try sdk { try call(callId).attendedTransfer(to: try call(consultationCallId)) }
    }

    func callAcceptTransfer(callId: String) throws -> CallMessage {
        try sdk { track(try call(callId).acceptTransfer()) }
    }

    func callRejectTransfer(callId: String, statusCode: Int64, reason: String?) throws {
        try sdk { try call(callId).rejectTransfer(statusCode: UInt16(clamping: statusCode), reason: reason) }
    }

    func callAddHeader(callId: String, name: String, value: String) throws {
        try sdk { try call(callId).addHeader(name: name, value: value) }
    }

    func callSetMuted(callId: String, muted: Bool) throws { try sdk { try call(callId).setMuted(muted) } }
    func callIsMuted(callId: String) throws -> Bool { try sdk { try call(callId).isMuted } }

    func callSetReceiveMuted(callId: String, muted: Bool) throws {
        try sdk { try call(callId).setReceiveMuted(muted) }
    }

    func callStartRecording(callId: String, path: String) throws {
        try sdk { try call(callId).startRecording(to: path) }
    }

    func callStopRecording(callId: String) throws { try sdk { try call(callId).stopRecording() } }

    func callSetDscp(callId: String, dscp: Int64) throws {
        try sdk { try call(callId).setDSCP(UInt8(clamping: dscp)) }
    }

    func callSetRtpTimeout(callId: String, seconds: Int64) throws {
        try sdk { try call(callId).setRTPTimeout(seconds: UInt32(clamping: seconds)) }
    }

    func callSetBitrate(callId: String, bps: Int64) throws {
        try sdk { try call(callId).setBitrate(UInt32(clamping: bps)) }
    }

    func callIceState(callId: String) throws -> Int64 {
        try sdk { Int64(try call(callId).iceState.rawValue) }
    }

    func callInfo(callId: String) throws -> CallInfoMessage { try sdk { try call(callId).info().message } }
    func callStats(callId: String) throws -> MediaStatsMessage { try sdk { try call(callId).stats().message } }

    // ── Audio processing and devices ─────────────────────────────────────

    private var audio: MedutAudio { Medut.shared.audio }

    func setEchoCancellation(enabled: Bool) throws { try sdk { try audio.setEchoCancellation(enabled) } }
    func setAecMode(mode: Int64) throws { try sdk { try audio.setAECMode(try enumValue(mode)) } }

    func setAecSuppressionLevel(level: Double) throws {
        try sdk { try audio.setAECSuppressionLevel(Float(level)) }
    }

    func setNoiseSuppression(enabled: Bool) throws { try sdk { try audio.setNoiseSuppression(enabled) } }
    func setAutoGainControl(enabled: Bool) throws { try sdk { try audio.setAutoGainControl(enabled) } }
    func setMicGainDb(db: Double) throws { try sdk { try audio.setMicGain(dB: Float(db)) } }
    func setSpeakerGainDb(db: Double) throws { try sdk { try audio.setSpeakerGain(dB: Float(db)) } }

    func setJitterBuffer(minMs: Int64, maxMs: Int64) throws {
        try sdk { try audio.setJitterBuffer(minMs: UInt32(clamping: minMs), maxMs: UInt32(clamping: maxMs)) }
    }

    func setJitterBufferType(type: Int64) throws {
        try sdk { try audio.setJitterBufferType(try enumValue(type)) }
    }

    func setAdaptiveBitrate(enabled: Bool, minBps: Int64, maxBps: Int64) throws {
        try sdk {
            try audio.setAdaptiveBitrate(enabled, minBps: UInt32(clamping: minBps),
                                         maxBps: UInt32(clamping: maxBps))
        }
    }

    func inputDevices() throws -> [AudioDeviceMessage] {
        try sdk { try audio.inputDevices().map { $0.message } }
    }

    func outputDevices() throws -> [AudioDeviceMessage] {
        try sdk { try audio.outputDevices().map { $0.message } }
    }

    func setInputDevice(name: String?) throws { try sdk { try audio.setInputDevice(name) } }
    func setOutputDevice(name: String?) throws { try sdk { try audio.setOutputDevice(name) } }

    // ── Call-audio routes ────────────────────────────────────────────────

    private func routes() -> [AudioRouteMessage] {
        MedutAudioSession.shared.availableRoutes().map { $0.message }
    }

    func audioRoutes() throws -> [AudioRouteMessage] { routes() }

    func selectAudioRoute(id: String) throws -> [AudioRouteMessage] {
        try MedutAudioSession.shared.selectRoute(id)
        return routes()
    }

    func setSpeakerphone(on: Bool) throws { try MedutAudioSession.shared.setSpeakerphone(on) }
    func setExternalRouting(on: Bool) throws { MedutAudioSession.shared.routingIsExternal = on }

    // ── App-owned audio ──────────────────────────────────────────────────

    func useAppOwnedAudio(enable: Bool) throws {
        try sdk { try MedutAudioSession.shared.useAppOwnedAudio(enable) }
    }

    func appOwnedAudioStatus() throws -> [String: Any?] {
        MedutAudioSession.shared.appOwnedAudioStatus.mapValues { $0 }
    }

    func appOwnedAudioActive() throws -> Bool { Medut.shared.audio.isExternalActive }

    func appOwnedAudioFormat() throws -> ExternalAudioFormatMessage? {
        Medut.shared.audio.externalFormat.map {
            ExternalAudioFormatMessage(sampleRate: Int64($0.sampleRate), channels: Int64($0.channels),
                                       ptimeMs: Int64($0.ptimeMs))
        }
    }

    func platformCallAudioActive(active: Bool) throws {
        if active {
            MedutAudioSession.shared.callKitDidActivate()
        } else {
            MedutAudioSession.shared.callKitDidDeactivate()
        }
    }

    // ── Network and capture ──────────────────────────────────────────────

    func networkChanged() throws { try sdk { try Medut.shared.network.changed() } }

    func networkSetMonitorInterval(seconds: Int64) throws {
        try sdk { try Medut.shared.network.setMonitorInterval(seconds: UInt32(clamping: seconds)) }
    }

    func networkSetHandoverPolicy(reinviteCalls: Bool, hangupOnFailure: Bool) throws {
        try sdk {
            try Medut.shared.network.setHandoverPolicy(reinviteCalls: reinviteCalls,
                                                       hangupOnFailure: hangupOnFailure)
        }
    }

    func networkLocalAddress() throws -> String { try sdk { try Medut.shared.network.localAddress() } }
    func networkIsUp() throws -> Bool { Medut.shared.network.isUp }
    func startPcap(path: String) throws { try sdk { try Medut.shared.startPcap(path: path) } }
    func stopPcap() throws { try sdk { try Medut.shared.stopPcap() } }

    // ── Helpers ──────────────────────────────────────────────────────────

    private func account(_ id: String) throws -> Account {
        guard let account = accounts[id] else {
            throw PigeonError(code: "ACCOUNT_DESTROYED", message: "no account \(id)", details: -3)
        }
        return account
    }

    private func call(_ id: String) throws -> Call {
        guard let call = calls[id] else {
            throw PigeonError(code: "CALL_ENDED", message: "no call \(id) (ended and released)", details: -3)
        }
        return call
    }

    /// Run an SDK call, turning its failures into the errors Dart decodes.
    private func sdk<T>(_ body: () throws -> T) throws -> T {
        do {
            return try body()
        } catch let error as MedutError {
            throw PigeonError(code: error.code.name, message: error.description,
                              details: Int(error.rawValue))
        }
    }
}

// ── Pigeon <-> SDK conversions ───────────────────────────────────────────

private func enumValue<E: RawRepresentable>(_ value: Int64) throws -> E where E.RawValue == Int {
    guard let e = E(rawValue: Int(value)) else {
        throw PigeonError(code: "INVALID_ARGUMENT", message: "unknown enum value \(value)", details: -1)
    }
    return e
}

extension MedutError.Code {
    /// The code names the Kotlin SDK uses, which the Dart side decodes.
    var name: String {
        switch self {
        case .invalidArgument: return "INVALID_ARGUMENT"
        case .outOfMemory: return "OUT_OF_MEMORY"
        case .invalidState: return "INVALID_STATE"
        case .dns: return "DNS"
        case .transport: return "TRANSPORT"
        case .authentication: return "AUTHENTICATION"
        case .serverError: return "SERVER_ERROR"
        case .webSocketRejected: return "WEBSOCKET_REJECTED"
        case .timeout: return "TIMEOUT"
        case .already: return "ALREADY"
        case .forbidden: return "FORBIDDEN"
        case .notRunning: return "NOT_RUNNING"
        case .callEnded: return "CALL_ENDED"
        case .accountDestroyed: return "ACCOUNT_DESTROYED"
        case .system: return "SYSTEM"
        case .unknown: return "UNKNOWN"
        }
    }
}

private extension IceServerMessage {
    var sdk: ICEServer { ICEServer(url: url, username: username, password: password) }
}

private extension ConfigMessage {
    func sdk() throws -> MedutConfig {
        var c = MedutConfig()
        if let v = transport { c.transport = try enumValue(v) }
        if let v = localIp { c.localIP = v }
        if let v = localPort { c.localPort = UInt16(clamping: v) }
        if let v = bindInterface { c.bindInterface = v }
        if let v = preferIpv6 { c.preferIPv6 = v }
        if let v = sipDomain { c.sipDomain = v }
        if let v = serverUrl { c.serverURL = v }
        if let v = serverHost { c.serverHost = v }
        if let v = serverPort { c.serverPort = UInt16(clamping: v) }
        if let v = outboundProxy { c.outboundProxy = v }
        if let v = caCertPath { c.caCertPath = v }
        if let v = clientCert { c.clientCert = v }
        if let v = clientKey { c.clientKey = v }
        if let v = verifyServer { c.verifyServer = v }
        if let v = sniHostname { c.sniHostname = v }
        if let v = userAgent { c.userAgent = v }
        if let v = wsOrigin { c.wsOrigin = v }
        if let v = wsExtraHeaders { c.wsExtraHeaders = v }
        if let v = wsKeepaliveMs { c.wsKeepaliveMs = UInt32(clamping: v) }
        if let v = stunServer { c.stunServer = v }
        if let v = turnServer { c.turnServer = v }
        if let v = turnUser { c.turnUser = v }
        if let v = turnPass { c.turnPass = v }
        if let v = iceEnabled { c.iceEnabled = v }
        if let v = rtcpMux { c.rtcpMux = v }
        if let v = iceServers { c.iceServers = v.map { $0.sdk } }
        if let v = iceTransportPolicy { c.iceTransportPolicy = try enumValue(v) }
        if let v = iceGatheringTimeoutMs { c.iceGatheringTimeoutMs = UInt32(clamping: v) }
        if let v = iceMultipath { c.iceMultipath = v }
        if let v = mediaEncryption { c.mediaEncryption = try enumValue(v) }
        if let v = audioCodecs { c.audioCodecs = v }
        if let v = dscpSip { c.dscpSIP = UInt8(clamping: v) }
        if let v = dscpRtp { c.dscpRTP = UInt8(clamping: v) }
        if let v = aecMode { c.aecMode = try enumValue(v) }
        if let v = noiseSuppression { c.noiseSuppression = v }
        if let v = autoGainControl { c.autoGainControl = v }
        if let v = aecSuppressionLevel { c.aecSuppressionLevel = Float(v) }
        if let v = micGainDb { c.micGainDB = Float(v) }
        if let v = speakerGainDb { c.speakerGainDB = Float(v) }
        if let v = opusBitrate { c.opus.bitrate = Int32(clamping: v) }
        if let v = opusComplexity { c.opus.complexity = Int32(clamping: v) }
        if let v = opusConstantBitrate { c.opus.constantBitrate = v }
        if let v = opusDtx { c.opus.discontinuousTransmission = v }
        if let v = opusFec { c.opus.forwardErrorCorrection = v }
        if let v = opusStereo { c.opus.stereo = v }
        if let v = platformAudioActivate { c.platformAudioActivate = v }
        if let v = jitterBufferType { c.jitterBufferType = try enumValue(v) }
        if let v = jitterBufferMinMs { c.jitterBufferMinMs = UInt32(clamping: v) }
        if let v = jitterBufferMaxMs { c.jitterBufferMaxMs = UInt32(clamping: v) }
        if let v = regExpires { c.regExpires = UInt32(clamping: v) }
        if let v = regRefreshPct { c.regRefreshPct = UInt32(clamping: v) }
        if let v = keepaliveIntervalMs { c.keepaliveIntervalMs = UInt32(clamping: v) }
        if let v = keepaliveReregister { c.keepaliveReregister = v }
        if let v = regRetryInitialMs { c.regRetryInitialMs = UInt32(clamping: v) }
        if let v = regRetryMaxMs { c.regRetryMaxMs = UInt32(clamping: v) }
        if let v = regRetryBackoff { c.regRetryBackoff = Float(v) }
        if let v = regRetryMaxAttempts { c.regRetryMaxAttempts = UInt32(clamping: v) }
        if let v = regRetryJitter { c.regRetryJitter = Float(v) }
        if let v = dnsSrvFailover { c.dnsSRVFailover = v }
        if let v = sipTimerBMs { c.sipTimerBMs = UInt32(clamping: v) }
        if let v = sipTimerFMs { c.sipTimerFMs = UInt32(clamping: v) }
        if let v = sessionTimerEnabled { c.sessionTimerEnabled = v }
        if let v = sessionExpiresSeconds { c.sessionExpiresSeconds = UInt32(clamping: v) }
        if let v = sessionMinSeSeconds { c.sessionMinSESeconds = UInt32(clamping: v) }
        if let v = statsIntervalMs { c.statsIntervalMs = UInt32(clamping: v) }
        if let v = mosMethod { c.mosMethod = try enumValue(v) }
        if let v = mosAlertThreshold { c.mosAlertThreshold = Float(v) }
        if let v = lossAlertThreshold { c.lossAlertThreshold = Float(v) }
        if let v = jitterAlertThreshold { c.jitterAlertThreshold = Float(v) }
        if let v = rtpTimeoutSeconds { c.rtpTimeoutSeconds = UInt32(clamping: v) }
        if let v = mediaStallMs { c.mediaStallMs = UInt32(clamping: v) }
        if let v = adaptiveBitrate { c.adaptiveBitrate = v }
        if let v = adaptMinBitrate { c.adaptMinBitrate = UInt32(clamping: v) }
        if let v = adaptMaxBitrate { c.adaptMaxBitrate = UInt32(clamping: v) }
        if let v = adaptLossDownPct { c.adaptLossDownPct = Float(v) }
        if let v = adaptLossUpPct { c.adaptLossUpPct = Float(v) }
        if let v = adaptRecoverTicks { c.adaptRecoverTicks = UInt32(clamping: v) }
        if let v = opusExpectedLossPct { c.opusExpectedLossPct = UInt32(clamping: v) }
        if let v = netMonitorIntervalSeconds { c.netMonitorIntervalSeconds = UInt32(clamping: v) }
        if let v = netSettleMs { c.netSettleMs = UInt32(clamping: v) }
        if let v = netReinviteCalls { c.netReinviteCalls = v }
        if let v = netHangupOnMigrationFailure { c.netHangupOnMigrationFailure = v }
        if let v = netVerifyMs { c.netVerifyMs = UInt32(clamping: v) }
        if let v = netMaxAttempts { c.netMaxAttempts = UInt32(clamping: v) }
        if let v = netIceHandover { c.netICEHandover = try enumValue(v) }
        if let v = tmpDir { c.tmpDir = v }
        if let v = traceSip { c.traceSIP = v }
        if let v = traceSdpDiff { c.traceSDPDiff = v }
        if let v = pcapPath { c.pcapPath = v }
        if let v = logLevel { c.logLevel = try enumValue(v) }
        return c
    }
}

private extension AccountConfigMessage {
    func sdk() throws -> AccountConfig {
        var c = AccountConfig(uri: uri, password: password)
        if let v = transport { c.transport = try enumValue(v) }
        if let v = serverUrl { c.serverURL = v }
        if let v = serverHost { c.serverHost = v }
        if let v = serverPort { c.serverPort = UInt16(clamping: v) }
        if let v = outboundProxy { c.outboundProxy = v }
        if let v = authUser { c.authUser = v }
        if let v = displayName { c.displayName = v }
        if let v = mediaEncryption { c.mediaEncryption = try enumValue(v) }
        if let v = iceEnabled { c.iceEnabled = v }
        if let v = rtcpMux { c.rtcpMux = v }
        if let v = stunServer { c.stunServer = v }
        if let v = turnServer { c.turnServer = v }
        if let v = turnUser { c.turnUser = v }
        if let v = turnPass { c.turnPass = v }
        if let v = iceServers { c.iceServers = v.map { $0.sdk } }
        if let v = iceTransportPolicy { c.iceTransportPolicy = try enumValue(v) }
        if let v = verifyTls { c.verifyTLS = v }
        if let v = pushProvider { c.pushProvider = try enumValue(v) }
        if let v = pushToken { c.pushToken = v }
        if let v = pushParam { c.pushParam = v }
        if let v = audioCodecs { c.audioCodecs = v }
        if let v = dtmfMode { c.dtmfMode = try enumValue(v) }
        return c
    }
}

private extension Account {
    var message: AccountMessage {
        AccountMessage(id: id.uuidString, aor: aor,
                       registrationState: Int64(registrationState.rawValue))
    }
}

private extension Call {
    var message: CallMessage {
        CallMessage(id: id.uuidString, accountId: account?.id.uuidString,
                    state: Int64(state.rawValue), isOutgoing: isOutgoing,
                    remoteUri: remoteURI, remoteDisplayName: remoteDisplayName)
    }
}

private extension AudioRoute {
    var message: AudioRouteMessage {
        let kindName: String
        switch kind {
        case .earpiece: kindName = "earpiece"
        case .speaker: kindName = "speaker"
        case .bluetooth: kindName = "bluetooth"
        case .wiredHeadset: kindName = "wired-headset"
        }
        return AudioRouteMessage(id: id, name: name, kind: kindName, isActive: isActive)
    }
}

private extension AudioDevice {
    var message: AudioDeviceMessage {
        AudioDeviceMessage(name: name, label: description, isDefault: isDefault)
    }
}

private extension CallInfo {
    var message: CallInfoMessage {
        CallInfoMessage(
            peerUri: peerURI, peerDisplayName: peerDisplayName, localUri: localURI,
            contactUri: contactURI, callId: callID, diverterUri: diverterURI,
            isOutgoing: isOutgoing, isRemoteHold: isRemoteHold,
            sipStatus: Int64(sipStatus), durationMs: Int64(clamping: durationMs),
            setupDurationMs: Int64(setupDurationMs), lineNumber: Int64(lineNumber),
            transport: Int64(transport.rawValue), state: Int64(state.rawValue))
    }
}

private extension MediaStats {
    var message: MediaStatsMessage {
        MediaStatsMessage(
            packetsSent: Int64(packetsSent), packetsReceived: Int64(packetsReceived),
            packetsLost: Int64(packetsLost), packetsLostRx: Int64(packetsLostRx),
            bytesSent: Int64(bytesSent), bytesReceived: Int64(bytesReceived),
            txErrors: Int64(txErrors), rxErrors: Int64(rxErrors),
            lossPct: Double(lossPct), lossPctRx: Double(lossPctRx),
            jitterMs: Double(jitterMs), txJitterMs: Double(txJitterMs), rttMs: Double(rttMs),
            jitterBufferMs: Int64(jitterBufferMs), jitterBufferLoad: Int64(jitterBufferLoad),
            latePackets: Int64(latePackets), discardedPackets: Int64(discardedPackets),
            jitterBufferTargetMs: Int64(jitterBufferTargetMs),
            jitterBufferAdaptive: jitterBufferAdaptive,
            plcFrames: Int64(plcFrames), plcRatio: Double(plcRatio),
            fecRecovered: Int64(fecRecovered), plcConcealed: Int64(plcConcealed),
            fecActive: fecActive, fecLevelPct: Int64(fecLevelPct),
            bandwidthKbpsTx: Int64(bandwidthKbpsTx), bandwidthKbpsRx: Int64(bandwidthKbpsRx),
            avgBandwidthKbpsTx: Int64(avgBandwidthKbpsTx), avgBandwidthKbpsRx: Int64(avgBandwidthKbpsRx),
            mosLq: Double(mosLQ), mosCq: Double(mosCQ), mosLqRx: Double(mosLQRx), mosCqRx: Double(mosCQRx),
            mosLqMin: Double(mosLQMin), mosLqAvg: Double(mosLQAvg),
            mosMethod: Int64(mosMethod.rawValue),
            codecName: codecName, codecClockRate: Int64(codecClockRate),
            codecSampleRate: Int64(codecSampleRate), codecChannels: Int64(codecChannels),
            payloadType: Int64(payloadType),
            audioLevelDbov: Double(audioLevelDBov), micLevelDbov: Double(micLevelDBov),
            ssrcTx: Int64(ssrcTx), ssrcRx: Int64(ssrcRx), remoteAddress: remoteAddress,
            statsTick: Int64(statsTick), callDurationMs: Int64(clamping: callDurationMs),
            isFinal: isFinal,
            iceState: Int64(iceState.rawValue), iceLocalType: Int64(iceLocalType.rawValue),
            iceRemoteType: Int64(iceRemoteType.rawValue), iceRttMs: Double(iceRttMs),
            iceGeneration: Int64(iceGeneration),
            iceLocalAddress: iceLocalAddress, iceRemoteAddress: iceRemoteAddress)
    }
}

private extension MedutEvent {
    var message: EventMessage {
        let accountId = account?.id.uuidString
        let callMessage = call?.message
        func raw(_ e: MedutError?) -> Int64? { e.map { Int64($0.rawValue) } }

        switch self {
        case .log(let text):
            return EventMessage(type: 0, message: text)
        case .registration(let e):
            return EventMessage(type: 1, accountId: accountId, state: Int64(e.state.rawValue),
                                error: raw(e.error), retryAttempt: Int64(e.retryAttempt),
                                retryDelayMs: Int64(e.retryDelayMs), errorText: e.errorText)
        case .incomingCall(let e):
            return EventMessage(type: 2, accountId: accountId, call: callMessage,
                                fromUri: e.fromURI, displayName: e.displayName)
        case .callState(let e):
            return EventMessage(type: 3, accountId: accountId, call: callMessage,
                                state: Int64(e.state.rawValue), error: raw(e.error), reason: e.reason)
        case .dtmf(let e):
            return EventMessage(type: 4, call: callMessage, digit: String(e.digit))
        case .sdpNegotiation(let e):
            return EventMessage(type: 5, call: callMessage, localSdp: e.localSDP, remoteSdp: e.remoteSDP,
                                negotiatedCodec: e.negotiatedCodec, negotiatedCrypto: e.negotiatedCrypto,
                                rejectedCodecs: e.rejectedCodecs, warnings: e.warnings)
        case .sipTrace(let e):
            return EventMessage(type: 6, direction: Int64(e.direction.rawValue), transport: e.transport,
                                remoteAddress: e.remoteAddress, rawMessage: e.message,
                                timestampUs: Int64(clamping: e.timestampMicros))
        case .mediaStats(let e):
            return EventMessage(type: 7, call: callMessage, stats: e.stats.message)
        case .registrarWarning(let text):
            return EventMessage(type: 8, message: text)
        case .transferRequest(let e):
            return EventMessage(type: 9, accountId: accountId, call: callMessage,
                                referToUri: e.referToURI, isAttended: e.isAttended,
                                autoFollowed: e.autoFollowed)
        case .messageWaiting(let e):
            return EventMessage(type: 10, accountId: accountId, messagesWaiting: e.messagesWaiting,
                                newVoice: Int64(e.newVoice), oldVoice: Int64(e.oldVoice),
                                newUrgent: Int64(e.newUrgent), oldUrgent: Int64(e.oldUrgent),
                                rawBody: e.rawBody)
        case .message(let e):
            return EventMessage(type: 11, accountId: accountId, fromUri: e.fromURI, body: e.body,
                                contentType: e.contentType)
        case .presence(let e):
            return EventMessage(type: 12, accountId: accountId, targetUri: e.targetURI,
                                status: Int64(e.status.rawValue))
        case .qualityAlert(let e):
            return EventMessage(type: 13, call: callMessage, issue: Int64(e.issue.rawValue),
                                value: Double(e.value), threshold: Double(e.threshold),
                                recovering: e.recovering)
        case .network(let e):
            return EventMessage(type: 14, accountId: accountId, call: callMessage, error: raw(e.error),
                                stage: Int64(e.stage.rawValue), localAddress: e.localAddress,
                                attempt: Int64(e.attempt), maxAttempts: Int64(e.maxAttempts),
                                elapsedMs: Int64(e.elapsedMs), ice: e.ice)
        case .transferFailed(let e):
            return EventMessage(type: 15, accountId: accountId, call: callMessage, reason: e.reason)
        case .iceState(let e):
            return EventMessage(type: 16, call: callMessage, state: Int64(e.state.rawValue),
                                previousState: Int64(e.previous.rawValue), error: raw(e.error),
                                remoteAddress: e.remoteAddress, localAddress: e.localAddress,
                                generation: Int64(e.generation), localType: Int64(e.localType.rawValue),
                                remoteType: Int64(e.remoteType.rawValue), rttMs: Double(e.rttMs))
        case .unknown(let type):
            return EventMessage(type: Int64(type))
        }
    }
}
