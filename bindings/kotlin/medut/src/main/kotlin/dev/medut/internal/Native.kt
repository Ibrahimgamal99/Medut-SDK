package dev.medut.internal

import dev.medut.AccountConfig
import dev.medut.MedutConfig
import java.nio.ByteBuffer

/**
 * One native method per medut.h function (medut_jni.c).  Handles cross as
 * `Long`; out-parameters as one-element arrays or Raw* holders.  Nothing
 * here checks lifetimes: callers go through [Gate].
 */
internal object Native {
    init {
        NativeLoader.load()
    }

    // Lifecycle
    @JvmStatic external fun version(): String
    @JvmStatic external fun strerror(err: Int): String
    @JvmStatic external fun configDefaults(out: RawConfig)
    @JvmStatic external fun stackInit(config: MedutConfig): Int
    @JvmStatic external fun setEventHandler(): Int
    @JvmStatic external fun isInitialized(): Boolean
    @JvmStatic external fun shutdown()
    @JvmStatic external fun eventRelease(ev: Long)

    // Accounts
    @JvmStatic external fun accountCreate(config: AccountConfig, out: LongArray): Int
    @JvmStatic external fun accountDestroy(account: Long)
    @JvmStatic external fun accountRegister(account: Long): Int
    @JvmStatic external fun accountUnregister(account: Long): Int
    @JvmStatic external fun accountCancelRetry(account: Long): Int
    @JvmStatic external fun accountRetryNow(account: Long): Int
    @JvmStatic external fun accountKeepaliveNow(account: Long): Int
    @JvmStatic external fun accountRefreshRegistration(account: Long): Int
    @JvmStatic external fun accountSetRetryPolicy(
        account: Long, initialMs: Int, maxMs: Int, backoff: Float, maxAttempts: Int): Int
    @JvmStatic external fun accountSetIceServers(account: Long, servers: RawIceServers, policy: Int): Int
    @JvmStatic external fun accountSetPushToken(account: Long, token: String?): Int
    @JvmStatic external fun accountAddHeader(account: Long, name: String, value: String): Int
    @JvmStatic external fun accountAddRegisterHeader(account: Long, name: String, value: String): Int
    @JvmStatic external fun accountSubscribePresence(account: Long, uri: String): Int
    @JvmStatic external fun accountUnsubscribePresence(account: Long, uri: String): Int
    @JvmStatic external fun accountPublishPresence(account: Long, status: Int): Int
    @JvmStatic external fun accountSet100rel(account: Long, mode: Int): Int
    @JvmStatic external fun accountForeach(): LongArray
    @JvmStatic external fun accountGetAor(account: Long): String?
    @JvmStatic external fun accountGetRegState(account: Long): Int
    @JvmStatic external fun messageSend(account: Long, to: String, body: String, contentType: String?): Int

    // Calls
    @JvmStatic external fun callInvite(account: Long, uri: String, out: LongArray): Int
    @JvmStatic external fun callAnswer(call: Long): Int
    @JvmStatic external fun callHangup(call: Long): Int
    @JvmStatic external fun callReject(call: Long, statusCode: Int, reason: String?): Int
    @JvmStatic external fun callHold(call: Long): Int
    @JvmStatic external fun callResume(call: Long): Int
    @JvmStatic external fun callIsHeld(call: Long): Boolean
    @JvmStatic external fun callSendDtmf(call: Long, digit: Char): Int
    @JvmStatic external fun callTransfer(call: Long, uri: String): Int
    @JvmStatic external fun callAddHeader(call: Long, name: String, value: String): Int
    @JvmStatic external fun callTransferAccept(call: Long, out: LongArray): Int
    @JvmStatic external fun callTransferReject(call: Long, statusCode: Int, reason: String?): Int
    @JvmStatic external fun callAttendedTransfer(call: Long, consultation: Long): Int
    @JvmStatic external fun callForeach(): LongArray
    @JvmStatic external fun callGetAccount(call: Long): Long
    @JvmStatic external fun callGetState(call: Long): Int
    @JvmStatic external fun callIceState(call: Long): Int
    @JvmStatic external fun callGetInfo(call: Long, out: RawCallInfo): Int
    @JvmStatic external fun callGetStats(call: Long, out: RawStats): Int
    @JvmStatic external fun callSetMediaTap(call: Long, on: Boolean): Int
    @JvmStatic external fun callSetDscpRtp(call: Long, dscp: Int): Int
    @JvmStatic external fun callSetRtpTimeout(call: Long, seconds: Int): Int
    @JvmStatic external fun callSetBitrate(call: Long, bps: Int): Int
    @JvmStatic external fun callRecordStart(call: Long, path: String): Int
    @JvmStatic external fun callRecordStop(call: Long): Int
    @JvmStatic external fun audioMute(call: Long, on: Boolean): Int
    @JvmStatic external fun audioIsMuted(call: Long): Boolean
    @JvmStatic external fun audioMuteRx(call: Long, on: Boolean): Int

    // Audio, stack-wide
    @JvmStatic external fun setAec(on: Boolean)
    @JvmStatic external fun setAecMode(mode: Int): Int
    @JvmStatic external fun setAecSuppressionLevel(level: Float)
    @JvmStatic external fun setNs(on: Boolean)
    @JvmStatic external fun setAgc(on: Boolean)
    @JvmStatic external fun setMicGainDb(db: Float)
    @JvmStatic external fun setSpeakerGainDb(db: Float)
    @JvmStatic external fun setJitterBuffer(minMs: Int, maxMs: Int)
    @JvmStatic external fun setJitterBufferType(type: Int)
    @JvmStatic external fun setAdaptiveBitrate(on: Boolean, minBps: Int, maxBps: Int)
    @JvmStatic external fun audioListInputDevices(): Array<String?>
    @JvmStatic external fun audioListOutputDevices(): Array<String?>
    @JvmStatic external fun audioSetInputDevice(name: String?): Int
    @JvmStatic external fun audioSetOutputDevice(name: String?): Int
    @JvmStatic external fun audioUseExternal(on: Boolean): Int
    @JvmStatic external fun audioExternalPush(buf: ByteBuffer, samples: Int): Int
    @JvmStatic external fun audioExternalPull(buf: ByteBuffer, samples: Int): Int
    @JvmStatic external fun audioExternalFormat(out: IntArray): Int
    @JvmStatic external fun audioExternalIsActive(): Boolean
    @JvmStatic external fun audioExternalSetVoicePath(active: Boolean)

    // Network, pcap
    @JvmStatic external fun networkChanged(): Int
    @JvmStatic external fun networkSetInterfaceHandle(ifname: String, handle: Long): Int
    @JvmStatic external fun networkClearInterfaceHandle(ifname: String): Int
    @JvmStatic external fun networkSetMonitorInterval(seconds: Int): Int
    @JvmStatic external fun networkSetHandoverPolicy(reinviteCalls: Boolean, hangupOnFailure: Boolean): Int
    @JvmStatic external fun networkLocalAddr(): String?
    @JvmStatic external fun networkIsUp(): Boolean
    @JvmStatic external fun pcapStart(path: String): Int
    @JvmStatic external fun pcapStop(): Int
}

/**
 * Loads libmedut + libmedut_jni.  On Android they ship in the AAR.  Elsewhere
 * (desktop tests against the Linux build) set the system property
 * `dev.medut.jniLibrary` to the absolute path of the JNI library; it finds
 * the core next to it.
 */
internal object NativeLoader {
    @Volatile private var loaded = false

    @Synchronized
    fun load() {
        if (loaded) return
        val path = System.getProperty("dev.medut.jniLibrary")
        if (path != null) {
            System.load(path)
        } else {
            System.loadLibrary("medut")
            System.loadLibrary("medut_jni")
        }
        loaded = true
    }
}
