package dev.medut.flutter

import android.content.Context
import android.os.Handler
import android.os.Looper
import dev.medut.Account
import dev.medut.AccountConfig
import dev.medut.AecMode
import dev.medut.Call
import dev.medut.CallInfo
import dev.medut.DtmfMode
import dev.medut.IceHandoverPolicy
import dev.medut.IceServer
import dev.medut.IceTransportPolicy
import dev.medut.JitterBufferType
import dev.medut.LogLevel
import dev.medut.MediaEncryption
import dev.medut.MediaStats
import dev.medut.Medut
import dev.medut.MedutConfig
import dev.medut.MedutEvent
import dev.medut.MedutException
import dev.medut.MedutSubscription
import dev.medut.MosMethod
import dev.medut.OpusConfig
import dev.medut.PresenceStatus
import dev.medut.PushProvider
import dev.medut.ReliableProvisionalMode
import dev.medut.Transport
import dev.medut.android.AndroidOptions
import dev.medut.android.AudioModePolicy
import dev.medut.android.AudioRoute
import dev.medut.android.MedutAndroid
import dev.medut.android.telecom.TelecomOptions
import io.flutter.embedding.engine.plugins.FlutterPlugin
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executor
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Flutter side of the Kotlin SDK (bindings/kotlin): every Pigeon host call
 * delegates to [Medut] / [MedutAndroid], and SDK events are forwarded to Dart.
 *
 * The stack belongs to native code for the process lifetime.  Dart's start
 * attaches to it when the app already started it (from Application.onCreate
 * or a push), and detaching the engine leaves it running.
 */
class MedutPlugin : FlutterPlugin, MedutHostApi {
    private lateinit var context: Context
    private var events: MedutEventsApi? = null
    private var subscription: MedutSubscription? = null
    private var scope: CoroutineScope? = null
    private val main = Handler(Looper.getMainLooper())
    private val mainExecutor = Executor { main.post(it) }

    // Objects Dart refers to by id.  Ended calls stay until Dart releases
    // them (or the cap below evicts them), so methods on an ended Dart Call
    // keep resolving.
    private val accounts = ConcurrentHashMap<String, Account>()
    private val calls = ConcurrentHashMap<String, Call>()
    private val endedCalls = ArrayDeque<String>()
    private var routesListener: (() -> Unit)? = null
    private var audioErrorHandler: ((String, String) -> Unit)? = null
    private var hostAudioErrorHandler: ((String, String) -> Unit)? = null

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        context = binding.applicationContext
        MedutHostApi.setUp(binding.binaryMessenger, this)
        events = MedutEventsApi(binding.binaryMessenger)
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
        // Nothing is forwarded, and no SDK hook is taken, until Dart starts:
        // an app that drives the SDK natively and never calls Dart's start()
        // keeps its own handlers and pays for no events.
    }

    /** Dart's start(): forward events from here on.  Before the stack starts,
     *  so its first events reach Dart. */
    private fun attach() {
        if (subscription != null) return
        subscription = Medut.addListener(mainExecutor) { forward(it) }
        // Chain to a handler the host app set rather than replace it.
        hostAudioErrorHandler = MedutAndroid.onAppOwnedAudioError
        val handler: (String, String) -> Unit = { code, message ->
            hostAudioErrorHandler?.invoke(code, message)
            send(EventMessage(type = APP_OWNED_AUDIO_ERROR, errorCode = code, message = message))
        }
        audioErrorHandler = handler
        MedutAndroid.onAppOwnedAudioError = handler
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        subscription?.cancel()
        subscription = null
        routesListener?.let { l -> runCatching { MedutAndroid.audioRoutes.removeListener(l) } }
        routesListener = null
        if (audioErrorHandler != null && MedutAndroid.onAppOwnedAudioError === audioErrorHandler) {
            MedutAndroid.onAppOwnedAudioError = hostAudioErrorHandler
        }
        audioErrorHandler = null
        hostAudioErrorHandler = null
        MedutHostApi.setUp(binding.binaryMessenger, null)
        events = null
        scope?.cancel()
        scope = null
    }

    // ── Events ───────────────────────────────────────────────────────────

    private fun send(event: EventMessage) {
        val api = events ?: return
        // UNDISPATCHED: the message is handed to the channel in the caller's
        // (main-thread) order, before the coroutine first suspends.
        scope?.launch(start = CoroutineStart.UNDISPATCHED) {
            runCatching { api.onEvent(event) }
        }
    }

    private fun forward(event: MedutEvent) {
        event.account?.let { accounts[it.id.toString()] = it }
        event.call?.let { track(it) }
        send(event.toMessage())
    }

    private fun track(call: Call): CallMessage {
        val id = call.id.toString()
        calls[id] = call
        if (call.isEnded) synchronized(endedCalls) {
            if (id !in endedCalls) {
                endedCalls.addLast(id)
                while (endedCalls.size > MAX_ENDED_CALLS) calls.remove(endedCalls.removeFirst())
            }
        }
        return call.toMessage()
    }

    // ── Lifecycle ────────────────────────────────────────────────────────

    override fun start(config: ConfigMessage, options: StartOptionsMessage): StartResultMessage = sdk {
        attach()
        val reattached = Medut.isRunning
        if (!reattached) {
            MedutAndroid.start(context, config.toSdk(), AndroidOptions(
                monitorNetwork = options.monitorNetwork,
                audioMode = when (options.audioMode) {
                    1L -> AudioModePolicy.TELECOM
                    2L -> AudioModePolicy.MANUAL
                    else -> AudioModePolicy.AUTOMATIC
                },
                telecom = if (options.telecom) TelecomOptions() else null,
            ))
        }
        if (options.appOwnedAudio) MedutAndroid.useAppOwnedAudio(true)
        if (routesListener == null) {
            val l = { send(EventMessage(type = AUDIO_ROUTES_CHANGED, routes = routes())) }
            // Absent when the app started the stack without MedutAndroid.
            if (runCatching { MedutAndroid.audioRoutes.addListener(l) }.isSuccess) routesListener = l
        }
        Medut.accounts.forEach { accounts[it.id.toString()] = it }
        StartResultMessage(
            reattached = reattached,
            version = Medut.version,
            accounts = Medut.accounts.map { it.toMessage() },
            calls = Medut.calls.map { track(it) },
        )
    }

    override fun shutdown() = sdk {
        MedutAndroid.shutdown()
        accounts.clear()
        calls.clear()
        synchronized(endedCalls) { endedCalls.clear() }
    }

    override fun releaseCall(callId: String) {
        val call = calls[callId] ?: return
        if (call.isEnded) {
            calls.remove(callId)
            synchronized(endedCalls) { endedCalls.remove(callId) }
        }
    }

    // ── Accounts ─────────────────────────────────────────────────────────

    override fun createAccount(config: AccountConfigMessage, remember: Boolean): AccountMessage = sdk {
        val cfg = config.toSdk()
        val account = Medut.createAccount(cfg)
        accounts[account.id.toString()] = account
        if (remember) MedutAndroid.accountStore.save(cfg)
        account.toMessage()
    }

    override fun accountDestroy(accountId: String, forget: Boolean) = sdk {
        accounts.remove(accountId)?.let { account ->
            if (forget) MedutAndroid.accountStore.remove(account.aor)
            account.destroy()
        }
        Unit
    }

    override fun rememberedAccounts(): List<String> =
        sdk { MedutAndroid.accountStore.list().map { it.uri } }

    override fun forgetAccount(uri: String) = sdk {
        MedutAndroid.accountStore.remove(uri)
        Unit
    }

    override fun accountRegister(accountId: String) = sdk { account(accountId).register() }
    override fun accountUnregister(accountId: String) = sdk { account(accountId).unregister() }
    override fun accountRegistrationState(accountId: String): Long =
        sdk { account(accountId).registrationState.value.toLong() }

    override fun accountSetRetryPolicy(accountId: String, initialMs: Long, maxMs: Long,
                                       backoff: Double, maxAttempts: Long) = sdk {
        account(accountId).setRetryPolicy(initialMs.toInt(), maxMs.toInt(), backoff.toFloat(), maxAttempts.toInt())
    }

    override fun accountCancelRetry(accountId: String) = sdk { account(accountId).cancelRetry() }
    override fun accountRetryNow(accountId: String) = sdk { account(accountId).retryNow() }
    override fun accountKeepaliveNow(accountId: String) = sdk { account(accountId).keepaliveNow() }
    override fun accountSetPushToken(accountId: String, token: String?) = sdk {
        val account = account(accountId)
        account.setPushToken(token)
        // A remembered account must come back from a push with this token.
        MedutAndroid.accountStore.updatePushToken(token, account.aor)
        Unit
    }

    override fun accountSetIceServers(accountId: String, servers: List<IceServerMessage>, policy: Long) = sdk {
        account(accountId).setIceServers(servers.map { it.toSdk() },
            enumOf(policy, IceTransportPolicy.entries) { it.value })
    }

    override fun accountAddHeader(accountId: String, name: String, value: String) =
        sdk { account(accountId).addHeader(name, value) }

    override fun accountAddRegisterHeader(accountId: String, name: String, value: String) =
        sdk { account(accountId).addRegisterHeader(name, value) }

    override fun accountSetReliableProvisional(accountId: String, mode: Long) = sdk {
        account(accountId).setReliableProvisional(enumOf(mode, ReliableProvisionalMode.entries) { it.value })
    }

    override fun accountSendMessage(accountId: String, to: String, body: String, contentType: String?) =
        sdk { account(accountId).sendMessage(to, body, contentType) }

    override fun accountPublishPresence(accountId: String, status: Long) = sdk {
        account(accountId).publishPresence(enumOf(status, PresenceStatus.entries) { it.value })
    }

    override fun accountSubscribePresence(accountId: String, uri: String) =
        sdk { account(accountId).subscribePresence(uri) }

    override fun accountUnsubscribePresence(accountId: String, uri: String) =
        sdk { account(accountId).unsubscribePresence(uri) }

    override fun accountCall(accountId: String, uri: String): CallMessage =
        sdk { track(account(accountId).call(uri)) }

    // ── Calls ────────────────────────────────────────────────────────────

    override fun callAnswer(callId: String) = sdk { call(callId).answer() }
    override fun callHangup(callId: String) = sdk { call(callId).hangup() }
    override fun callReject(callId: String, statusCode: Long, reason: String?) =
        sdk { call(callId).reject(statusCode.toInt(), reason) }
    override fun callHold(callId: String) = sdk { call(callId).hold() }
    override fun callResume(callId: String) = sdk { call(callId).resume() }
    override fun callIsHeld(callId: String): Boolean = sdk { call(callId).isHeld }

    override fun callSendDtmf(callId: String, digit: String) = sdk {
        val c = digit.singleOrNull()
            ?: throw FlutterError("INVALID_ARGUMENT", "sendDtmf takes one digit, got \"$digit\"", -1)
        call(callId).sendDtmf(c)
    }

    override fun callTransfer(callId: String, uri: String) = sdk { call(callId).transfer(uri) }
    override fun callAttendedTransfer(callId: String, consultationCallId: String) =
        sdk { call(callId).attendedTransfer(call(consultationCallId)) }
    override fun callAcceptTransfer(callId: String): CallMessage =
        sdk { track(call(callId).acceptTransfer()) }
    override fun callRejectTransfer(callId: String, statusCode: Long, reason: String?) =
        sdk { call(callId).rejectTransfer(statusCode.toInt(), reason) }
    override fun callAddHeader(callId: String, name: String, value: String) =
        sdk { call(callId).addHeader(name, value) }
    override fun callSetMuted(callId: String, muted: Boolean) = sdk { call(callId).setMuted(muted) }
    override fun callIsMuted(callId: String): Boolean = sdk { call(callId).isMuted }
    override fun callSetReceiveMuted(callId: String, muted: Boolean) =
        sdk { call(callId).setReceiveMuted(muted) }
    override fun callStartRecording(callId: String, path: String) =
        sdk { call(callId).startRecording(path) }
    override fun callStopRecording(callId: String) = sdk { call(callId).stopRecording() }
    override fun callSetDscp(callId: String, dscp: Long) = sdk { call(callId).setDscp(dscp.toInt()) }
    override fun callSetRtpTimeout(callId: String, seconds: Long) =
        sdk { call(callId).setRtpTimeout(seconds.toInt()) }
    override fun callSetBitrate(callId: String, bps: Long) = sdk { call(callId).setBitrate(bps.toInt()) }
    override fun callIceState(callId: String): Long = sdk { call(callId).iceState.value.toLong() }
    override fun callInfo(callId: String): CallInfoMessage = sdk { call(callId).info().toMessage() }
    override fun callStats(callId: String): MediaStatsMessage = sdk { call(callId).stats().toMessage() }

    // ── Audio processing and devices ─────────────────────────────────────

    override fun setEchoCancellation(enabled: Boolean) = sdk { Medut.audio.setEchoCancellation(enabled) }
    override fun setAecMode(mode: Long) = sdk { Medut.audio.setAecMode(enumOf(mode, AecMode.entries) { it.value }) }
    override fun setAecSuppressionLevel(level: Double) = sdk { Medut.audio.setAecSuppressionLevel(level.toFloat()) }
    override fun setNoiseSuppression(enabled: Boolean) = sdk { Medut.audio.setNoiseSuppression(enabled) }
    override fun setAutoGainControl(enabled: Boolean) = sdk { Medut.audio.setAutoGainControl(enabled) }
    override fun setMicGainDb(db: Double) = sdk { Medut.audio.setMicGainDb(db.toFloat()) }
    override fun setSpeakerGainDb(db: Double) = sdk { Medut.audio.setSpeakerGainDb(db.toFloat()) }
    override fun setJitterBuffer(minMs: Long, maxMs: Long) =
        sdk { Medut.audio.setJitterBuffer(minMs.toInt(), maxMs.toInt()) }
    override fun setJitterBufferType(type: Long) =
        sdk { Medut.audio.setJitterBufferType(enumOf(type, JitterBufferType.entries) { it.value }) }
    override fun setAdaptiveBitrate(enabled: Boolean, minBps: Long, maxBps: Long) =
        sdk { Medut.audio.setAdaptiveBitrate(enabled, minBps.toInt(), maxBps.toInt()) }

    override fun inputDevices(): List<AudioDeviceMessage> =
        sdk { Medut.audio.inputDevices().map { AudioDeviceMessage(it.name, it.description, it.isDefault) } }

    override fun outputDevices(): List<AudioDeviceMessage> =
        sdk { Medut.audio.outputDevices().map { AudioDeviceMessage(it.name, it.description, it.isDefault) } }

    override fun setInputDevice(name: String?) = sdk { Medut.audio.setInputDevice(name) }
    override fun setOutputDevice(name: String?) = sdk { Medut.audio.setOutputDevice(name) }

    // ── Call-audio routes ────────────────────────────────────────────────

    private fun routes(): List<AudioRouteMessage> = MedutAndroid.audioRoutes.list().map { it.toMessage() }

    override fun audioRoutes(): List<AudioRouteMessage> = sdk { routes() }
    override fun selectAudioRoute(id: String): List<AudioRouteMessage> =
        sdk { MedutAndroid.audioRoutes.select(id).map { it.toMessage() } }
    override fun setSpeakerphone(on: Boolean) = sdk { MedutAndroid.audioRoutes.setSpeakerphone(on) }
    override fun setExternalRouting(on: Boolean) = sdk { MedutAndroid.audioRoutes.externalRouting = on }

    // ── App-owned audio ──────────────────────────────────────────────────

    override fun useAppOwnedAudio(enable: Boolean) = sdk { MedutAndroid.useAppOwnedAudio(enable) }
    override fun appOwnedAudioStatus(): Map<String, Any?> = MedutAndroid.appOwnedAudioStatus
    override fun appOwnedAudioActive(): Boolean = Medut.audio.isExternalActive
    override fun appOwnedAudioFormat(): ExternalAudioFormatMessage? = Medut.audio.externalFormat?.let {
        ExternalAudioFormatMessage(it.sampleRate.toLong(), it.channels.toLong(), it.ptimeMs.toLong())
    }
    override fun platformCallAudioActive(active: Boolean) = MedutAndroid.setCallAudioActive(active)

    // ── Network and capture ──────────────────────────────────────────────

    override fun networkChanged() = sdk { Medut.network.changed() }
    override fun networkSetMonitorInterval(seconds: Long) =
        sdk { Medut.network.setMonitorInterval(seconds.toInt()) }
    override fun networkSetHandoverPolicy(reinviteCalls: Boolean, hangupOnFailure: Boolean) =
        sdk { Medut.network.setHandoverPolicy(reinviteCalls, hangupOnFailure) }
    override fun networkLocalAddress(): String = sdk { Medut.network.localAddress }
    override fun networkIsUp(): Boolean = Medut.network.isUp
    override fun startPcap(path: String) = sdk { Medut.startPcap(path) }
    override fun stopPcap() = sdk { Medut.stopPcap() }

    // ── Helpers ──────────────────────────────────────────────────────────

    private fun account(id: String): Account = accounts[id]
        ?: throw FlutterError("ACCOUNT_DESTROYED", "no account $id", -3)

    private fun call(id: String): Call = calls[id]
        ?: throw FlutterError("CALL_ENDED", "no call $id (ended and released)", -3)

    /** Run an SDK call, turning its failures into the errors Dart decodes. */
    private inline fun <T> sdk(body: () -> T): T = try {
        body()
    } catch (e: MedutException) {
        throw FlutterError(e.code.name, e.message, e.error.rawValue)
    } catch (e: IllegalStateException) {
        throw FlutterError("NOT_RUNNING", e.message, -3)
    }

    private companion object {
        const val AUDIO_ROUTES_CHANGED = 100L
        const val APP_OWNED_AUDIO_ERROR = 101L

        /** Ended calls kept for Dart before the oldest is dropped. */
        const val MAX_ENDED_CALLS = 32
    }
}

// ── Pigeon <-> SDK conversions ───────────────────────────────────────────

private inline fun <E> enumOf(value: Long, entries: List<E>, raw: (E) -> Int): E =
    entries.firstOrNull { raw(it) == value.toInt() }
        ?: throw FlutterError("INVALID_ARGUMENT", "unknown enum value $value", -1)

private fun IceServerMessage.toSdk() = IceServer(url, username, password)

private fun ConfigMessage.toSdk(): MedutConfig {
    val d = MedutConfig()
    return MedutConfig(
        transport = transport?.let { enumOf(it, Transport.entries) { e -> e.value } } ?: d.transport,
        localIp = localIp ?: d.localIp,
        localPort = localPort?.toInt() ?: d.localPort,
        bindInterface = bindInterface ?: d.bindInterface,
        preferIpv6 = preferIpv6 ?: d.preferIpv6,
        sipDomain = sipDomain ?: d.sipDomain,
        serverUrl = serverUrl ?: d.serverUrl,
        serverHost = serverHost ?: d.serverHost,
        serverPort = serverPort?.toInt() ?: d.serverPort,
        outboundProxy = outboundProxy ?: d.outboundProxy,
        caCertPath = caCertPath ?: d.caCertPath,
        clientCert = clientCert ?: d.clientCert,
        clientKey = clientKey ?: d.clientKey,
        verifyServer = verifyServer ?: d.verifyServer,
        sniHostname = sniHostname ?: d.sniHostname,
        userAgent = userAgent ?: d.userAgent,
        wsOrigin = wsOrigin ?: d.wsOrigin,
        wsExtraHeaders = wsExtraHeaders ?: d.wsExtraHeaders,
        wsKeepaliveMs = wsKeepaliveMs?.toInt() ?: d.wsKeepaliveMs,
        stunServer = stunServer ?: d.stunServer,
        turnServer = turnServer ?: d.turnServer,
        turnUser = turnUser ?: d.turnUser,
        turnPass = turnPass ?: d.turnPass,
        iceEnabled = iceEnabled ?: d.iceEnabled,
        rtcpMux = rtcpMux ?: d.rtcpMux,
        iceServers = iceServers?.map { it.toSdk() } ?: d.iceServers,
        iceTransportPolicy = iceTransportPolicy?.let { enumOf(it, IceTransportPolicy.entries) { e -> e.value } }
            ?: d.iceTransportPolicy,
        iceGatheringTimeoutMs = iceGatheringTimeoutMs?.toInt() ?: d.iceGatheringTimeoutMs,
        iceMultipath = iceMultipath ?: d.iceMultipath,
        mediaEncryption = mediaEncryption?.let { enumOf(it, MediaEncryption.entries) { e -> e.value } }
            ?: d.mediaEncryption,
        audioCodecs = audioCodecs ?: d.audioCodecs,
        dscpSip = dscpSip?.toInt() ?: d.dscpSip,
        dscpRtp = dscpRtp?.toInt() ?: d.dscpRtp,
        aecMode = aecMode?.let { enumOf(it, AecMode.entries) { e -> e.value } } ?: d.aecMode,
        noiseSuppression = noiseSuppression ?: d.noiseSuppression,
        autoGainControl = autoGainControl ?: d.autoGainControl,
        aecSuppressionLevel = aecSuppressionLevel?.toFloat() ?: d.aecSuppressionLevel,
        micGainDb = micGainDb?.toFloat() ?: d.micGainDb,
        speakerGainDb = speakerGainDb?.toFloat() ?: d.speakerGainDb,
        opus = OpusConfig(
            bitrate = opusBitrate?.toInt() ?: d.opus.bitrate,
            complexity = opusComplexity?.toInt() ?: d.opus.complexity,
            constantBitrate = opusConstantBitrate ?: d.opus.constantBitrate,
            discontinuousTransmission = opusDtx ?: d.opus.discontinuousTransmission,
            forwardErrorCorrection = opusFec ?: d.opus.forwardErrorCorrection,
            stereo = opusStereo ?: d.opus.stereo,
        ),
        platformAudioActivate = platformAudioActivate ?: d.platformAudioActivate,
        jitterBufferType = jitterBufferType?.let { enumOf(it, JitterBufferType.entries) { e -> e.value } }
            ?: d.jitterBufferType,
        jitterBufferMinMs = jitterBufferMinMs?.toInt() ?: d.jitterBufferMinMs,
        jitterBufferMaxMs = jitterBufferMaxMs?.toInt() ?: d.jitterBufferMaxMs,
        regExpires = regExpires?.toInt() ?: d.regExpires,
        regRefreshPct = regRefreshPct?.toInt() ?: d.regRefreshPct,
        keepaliveIntervalMs = keepaliveIntervalMs?.toInt() ?: d.keepaliveIntervalMs,
        keepaliveReregister = keepaliveReregister ?: d.keepaliveReregister,
        regRetryInitialMs = regRetryInitialMs?.toInt() ?: d.regRetryInitialMs,
        regRetryMaxMs = regRetryMaxMs?.toInt() ?: d.regRetryMaxMs,
        regRetryBackoff = regRetryBackoff?.toFloat() ?: d.regRetryBackoff,
        regRetryMaxAttempts = regRetryMaxAttempts?.toInt() ?: d.regRetryMaxAttempts,
        regRetryJitter = regRetryJitter?.toFloat() ?: d.regRetryJitter,
        dnsSrvFailover = dnsSrvFailover ?: d.dnsSrvFailover,
        sipTimerBMs = sipTimerBMs?.toInt() ?: d.sipTimerBMs,
        sipTimerFMs = sipTimerFMs?.toInt() ?: d.sipTimerFMs,
        sessionTimerEnabled = sessionTimerEnabled ?: d.sessionTimerEnabled,
        sessionExpiresSeconds = sessionExpiresSeconds?.toInt() ?: d.sessionExpiresSeconds,
        sessionMinSeSeconds = sessionMinSeSeconds?.toInt() ?: d.sessionMinSeSeconds,
        statsIntervalMs = statsIntervalMs?.toInt() ?: d.statsIntervalMs,
        mosMethod = mosMethod?.let { enumOf(it, MosMethod.entries) { e -> e.value } } ?: d.mosMethod,
        mosAlertThreshold = mosAlertThreshold?.toFloat() ?: d.mosAlertThreshold,
        lossAlertThreshold = lossAlertThreshold?.toFloat() ?: d.lossAlertThreshold,
        jitterAlertThreshold = jitterAlertThreshold?.toFloat() ?: d.jitterAlertThreshold,
        rtpTimeoutSeconds = rtpTimeoutSeconds?.toInt() ?: d.rtpTimeoutSeconds,
        mediaStallMs = mediaStallMs?.toInt() ?: d.mediaStallMs,
        adaptiveBitrate = adaptiveBitrate ?: d.adaptiveBitrate,
        adaptMinBitrate = adaptMinBitrate?.toInt() ?: d.adaptMinBitrate,
        adaptMaxBitrate = adaptMaxBitrate?.toInt() ?: d.adaptMaxBitrate,
        adaptLossDownPct = adaptLossDownPct?.toFloat() ?: d.adaptLossDownPct,
        adaptLossUpPct = adaptLossUpPct?.toFloat() ?: d.adaptLossUpPct,
        adaptRecoverTicks = adaptRecoverTicks?.toInt() ?: d.adaptRecoverTicks,
        opusExpectedLossPct = opusExpectedLossPct?.toInt() ?: d.opusExpectedLossPct,
        netMonitorIntervalSeconds = netMonitorIntervalSeconds?.toInt() ?: d.netMonitorIntervalSeconds,
        netSettleMs = netSettleMs?.toInt() ?: d.netSettleMs,
        netReinviteCalls = netReinviteCalls ?: d.netReinviteCalls,
        netHangupOnMigrationFailure = netHangupOnMigrationFailure ?: d.netHangupOnMigrationFailure,
        netVerifyMs = netVerifyMs?.toInt() ?: d.netVerifyMs,
        netMaxAttempts = netMaxAttempts?.toInt() ?: d.netMaxAttempts,
        netIceHandover = netIceHandover?.let { enumOf(it, IceHandoverPolicy.entries) { e -> e.value } }
            ?: d.netIceHandover,
        tmpDir = tmpDir ?: d.tmpDir,
        traceSip = traceSip ?: d.traceSip,
        traceSdpDiff = traceSdpDiff ?: d.traceSdpDiff,
        pcapPath = pcapPath ?: d.pcapPath,
        logLevel = logLevel?.let { enumOf(it, LogLevel.entries) { e -> e.value } } ?: d.logLevel,
    )
}

private fun AccountConfigMessage.toSdk(): AccountConfig {
    val d = AccountConfig(uri, password)
    return d.copy(
        transport = transport?.let { enumOf(it, Transport.entries) { e -> e.value } } ?: d.transport,
        serverUrl = serverUrl ?: d.serverUrl,
        serverHost = serverHost ?: d.serverHost,
        serverPort = serverPort?.toInt() ?: d.serverPort,
        outboundProxy = outboundProxy ?: d.outboundProxy,
        authUser = authUser ?: d.authUser,
        displayName = displayName ?: d.displayName,
        mediaEncryption = mediaEncryption?.let { enumOf(it, MediaEncryption.entries) { e -> e.value } }
            ?: d.mediaEncryption,
        iceEnabled = iceEnabled ?: d.iceEnabled,
        rtcpMux = rtcpMux ?: d.rtcpMux,
        stunServer = stunServer ?: d.stunServer,
        turnServer = turnServer ?: d.turnServer,
        turnUser = turnUser ?: d.turnUser,
        turnPass = turnPass ?: d.turnPass,
        iceServers = iceServers?.map { it.toSdk() } ?: d.iceServers,
        iceTransportPolicy = iceTransportPolicy?.let { enumOf(it, IceTransportPolicy.entries) { e -> e.value } }
            ?: d.iceTransportPolicy,
        verifyTls = verifyTls ?: d.verifyTls,
        pushProvider = pushProvider?.let { enumOf(it, PushProvider.entries) { e -> e.value } } ?: d.pushProvider,
        pushToken = pushToken ?: d.pushToken,
        pushParam = pushParam ?: d.pushParam,
        audioCodecs = audioCodecs ?: d.audioCodecs,
        dtmfMode = dtmfMode?.let { enumOf(it, DtmfMode.entries) { e -> e.value } } ?: d.dtmfMode,
    )
}

private fun Account.toMessage() =
    AccountMessage(id.toString(), aor, registrationState.value.toLong())

private fun Call.toMessage() = CallMessage(
    id = id.toString(),
    accountId = account?.id?.toString(),
    state = state.value.toLong(),
    isOutgoing = isOutgoing,
    remoteUri = remoteUri,
    remoteDisplayName = remoteDisplayName,
)

private fun AudioRoute.toMessage() = AudioRouteMessage(
    id = id,
    name = name,
    kind = when (kind) {
        AudioRoute.Kind.EARPIECE -> "earpiece"
        AudioRoute.Kind.SPEAKER -> "speaker"
        AudioRoute.Kind.BLUETOOTH -> "bluetooth"
        AudioRoute.Kind.WIRED_HEADSET -> "wired-headset"
    },
    isActive = isActive,
)

private fun CallInfo.toMessage() = CallInfoMessage(
    peerUri = peerUri,
    peerDisplayName = peerDisplayName,
    localUri = localUri,
    contactUri = contactUri,
    callId = callId,
    diverterUri = diverterUri,
    isOutgoing = isOutgoing,
    isRemoteHold = isRemoteHold,
    sipStatus = sipStatus.toLong(),
    durationMs = durationMs,
    setupDurationMs = setupDurationMs,
    lineNumber = lineNumber,
    transport = transport.value.toLong(),
    state = state.value.toLong(),
)

private fun MediaStats.toMessage() = MediaStatsMessage(
    packetsSent = packetsSent,
    packetsReceived = packetsReceived,
    packetsLost = packetsLost,
    packetsLostRx = packetsLostRx,
    bytesSent = bytesSent,
    bytesReceived = bytesReceived,
    txErrors = txErrors,
    rxErrors = rxErrors,
    lossPct = lossPct.toDouble(),
    lossPctRx = lossPctRx.toDouble(),
    jitterMs = jitterMs.toDouble(),
    txJitterMs = txJitterMs.toDouble(),
    rttMs = rttMs.toDouble(),
    jitterBufferMs = jitterBufferMs,
    jitterBufferLoad = jitterBufferLoad,
    latePackets = latePackets,
    discardedPackets = discardedPackets,
    jitterBufferTargetMs = jitterBufferTargetMs,
    jitterBufferAdaptive = jitterBufferAdaptive,
    plcFrames = plcFrames,
    plcRatio = plcRatio.toDouble(),
    fecRecovered = fecRecovered,
    plcConcealed = plcConcealed,
    fecActive = fecActive,
    fecLevelPct = fecLevelPct,
    bandwidthKbpsTx = bandwidthKbpsTx,
    bandwidthKbpsRx = bandwidthKbpsRx,
    avgBandwidthKbpsTx = avgBandwidthKbpsTx,
    avgBandwidthKbpsRx = avgBandwidthKbpsRx,
    mosLq = mosLq.toDouble(),
    mosCq = mosCq.toDouble(),
    mosLqRx = mosLqRx.toDouble(),
    mosCqRx = mosCqRx.toDouble(),
    mosLqMin = mosLqMin.toDouble(),
    mosLqAvg = mosLqAvg.toDouble(),
    mosMethod = mosMethod.value.toLong(),
    codecName = codecName,
    codecClockRate = codecClockRate,
    codecSampleRate = codecSampleRate,
    codecChannels = codecChannels.toLong(),
    payloadType = payloadType.toLong(),
    audioLevelDbov = audioLevelDbov.toDouble(),
    micLevelDbov = micLevelDbov.toDouble(),
    ssrcTx = ssrcTx,
    ssrcRx = ssrcRx,
    remoteAddress = remoteAddress,
    statsTick = statsTick,
    callDurationMs = callDurationMs,
    isFinal = isFinal,
    iceState = iceState.value.toLong(),
    iceLocalType = iceLocalType.value.toLong(),
    iceRemoteType = iceRemoteType.value.toLong(),
    iceRttMs = iceRttMs.toDouble(),
    iceGeneration = iceGeneration,
    iceLocalAddress = iceLocalAddress,
    iceRemoteAddress = iceRemoteAddress,
)

private fun MedutEvent.toMessage(): EventMessage {
    val acct = account?.id?.toString()
    val callMsg = call?.toMessage()
    return when (this) {
        is MedutEvent.Log -> EventMessage(type = 0, message = message)
        is MedutEvent.Registration -> EventMessage(type = 1, accountId = acct,
            state = state.value.toLong(), error = error?.rawValue?.toLong(), errorText = errorText,
            retryAttempt = retryAttempt, retryDelayMs = retryDelayMs)
        is MedutEvent.IncomingCall -> EventMessage(type = 2, accountId = acct, call = callMsg,
            fromUri = fromUri, displayName = displayName)
        is MedutEvent.CallStateChanged -> EventMessage(type = 3, accountId = acct, call = callMsg,
            state = state.value.toLong(), error = error?.rawValue?.toLong(), reason = reason)
        is MedutEvent.Dtmf -> EventMessage(type = 4, call = callMsg, digit = digit.toString())
        is MedutEvent.SdpNegotiation -> EventMessage(type = 5, call = callMsg, localSdp = localSdp,
            remoteSdp = remoteSdp, negotiatedCodec = negotiatedCodec,
            negotiatedCrypto = negotiatedCrypto, rejectedCodecs = rejectedCodecs, warnings = warnings)
        is MedutEvent.SipTrace -> EventMessage(type = 6, direction = direction.value.toLong(),
            transport = transport, remoteAddress = remoteAddress, rawMessage = message,
            timestampUs = timestampMicros)
        is MedutEvent.MediaStatsUpdate -> EventMessage(type = 7, call = callMsg, stats = stats.toMessage())
        is MedutEvent.RegistrarWarning -> EventMessage(type = 8, message = message)
        is MedutEvent.TransferRequest -> EventMessage(type = 9, accountId = acct, call = callMsg,
            referToUri = referToUri, isAttended = isAttended, autoFollowed = autoFollowed)
        is MedutEvent.MessageWaiting -> EventMessage(type = 10, accountId = acct,
            messagesWaiting = messagesWaiting, newVoice = newVoice, oldVoice = oldVoice,
            newUrgent = newUrgent, oldUrgent = oldUrgent, rawBody = rawBody)
        is MedutEvent.Message -> EventMessage(type = 11, accountId = acct, fromUri = fromUri,
            body = body, contentType = contentType)
        is MedutEvent.Presence -> EventMessage(type = 12, accountId = acct, targetUri = targetUri,
            status = status.value.toLong())
        is MedutEvent.QualityAlert -> EventMessage(type = 13, call = callMsg, issue = issue.value.toLong(),
            value = value.toDouble(), threshold = threshold.toDouble(), recovering = recovering)
        is MedutEvent.Network -> EventMessage(type = 14, accountId = acct, call = callMsg,
            stage = stage.value.toLong(), localAddress = localAddress, attempt = attempt,
            maxAttempts = maxAttempts, elapsedMs = elapsedMs, ice = ice,
            error = error?.rawValue?.toLong())
        is MedutEvent.TransferFailed -> EventMessage(type = 15, accountId = acct, call = callMsg,
            reason = reason)
        is MedutEvent.IceStateChanged -> EventMessage(type = 16, call = callMsg,
            state = state.value.toLong(), previousState = previous.value.toLong(),
            generation = generation, localType = localType.value.toLong(),
            remoteType = remoteType.value.toLong(), localAddress = localAddress,
            remoteAddress = remoteAddress, rttMs = rttMs.toDouble(), error = error?.rawValue?.toLong())
        is MedutEvent.Unknown -> EventMessage(type = type.toLong())
    }
}
