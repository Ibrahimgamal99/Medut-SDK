// Desktop smoke test for the Kotlin binding against the real Linux build.
//
// Exercises the JNI layer and the core API on a plain JVM: config defaults
// read from C by field name, event delivery from the SDK thread, an ended
// call's handle staying valid, and dead handles refusing to run.  Needs no
// SIP server: the INVITE goes to TEST-NET-1 and the Timer B watchdog fails it.
//
// Run: bindings/kotlin/desktop-test/run.sh (after scripts/build-linux.sh)

import dev.medut.AccountConfig
import dev.medut.AecMode
import dev.medut.IceState
import dev.medut.LogLevel
import dev.medut.Medut
import dev.medut.MedutConfig
import dev.medut.MedutError
import dev.medut.MedutEvent
import dev.medut.MedutException
import dev.medut.RegistrationState
import dev.medut.Transport
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.system.exitProcess

private var failures = 0

private fun expect(cond: Boolean, what: String) {
    if (cond) println("  ok   $what") else { println("  FAIL $what"); failures++ }
}

private inline fun expectThrows(code: MedutError.Code, what: String, body: () -> Unit) {
    try {
        body()
        println("  FAIL $what (no exception)"); failures++
    } catch (e: MedutException) {
        expect(e.code == code, "$what -> ${e.code}")
    }
}

fun main() {
    println("Medut ${Medut.version}")

    println("defaults come from medut_config_init()")
    val d = MedutConfig()
    expect(d.regExpires == 3600, "regExpires 3600 (${d.regExpires})")
    expect(d.keepaliveIntervalMs == 30000, "keepaliveIntervalMs 30000 (${d.keepaliveIntervalMs})")
    expect(d.statsIntervalMs == 2000, "statsIntervalMs 2000 (${d.statsIntervalMs})")
    expect(d.netMaxAttempts == 6, "netMaxAttempts 6 (${d.netMaxAttempts})")
    expect(d.opus.bitrate == 32000, "opus.bitrate 32000 (${d.opus.bitrate})")
    expect(d.opus.forwardErrorCorrection, "opus.forwardErrorCorrection true")
    expect(d.aecMode == AecMode.SUPPRESSOR, "aecMode SUPPRESSOR (${d.aecMode})")
    expect(d.verifyServer, "verifyServer true")
    expect(d.regRetryBackoff == 2.0f, "regRetryBackoff 2.0 (${d.regRetryBackoff})")
    expect(d.logLevel == LogLevel.WARNING, "logLevel WARNING (${d.logLevel})")

    val events = CopyOnWriteArrayList<MedutEvent>()
    val ended = CountDownLatch(1)
    Medut.addListener({ it.run() }) { ev ->
        events.add(ev)
        if (ev is MedutEvent.CallStateChanged && ev.state.isTerminal) ended.countDown()
    }

    println("lifecycle")
    val config = MedutConfig(
        tmpDir = System.getProperty("java.io.tmpdir"),
        netMonitorIntervalSeconds = 0,
        logLevel = LogLevel.DEBUG,
        sipTimerBMs = 1500,
        audioCodecs = listOf("pcmu", "opus"),
        userAgent = "medut-kotlin-test ☃",
    )
    Medut.start(config)
    expect(Medut.isRunning, "running after start")
    expect(!Medut.reattached, "fresh start, not reattached")
    expectThrows(MedutError.Code.ALREADY, "second start") { Medut.start(config) }

    println("accounts")
    val account = Medut.createAccount(
        AccountConfig(uri = "alice@192.0.2.1:5060", password = "secret", transport = Transport.UDP,
                      displayName = "Alïce 😀", rtcpMux = false))
    expect(account.aor == "sip:alice@192.0.2.1", "aor (${account.aor})")
    expect(Medut.account("sip:alice@192.0.2.1") === account, "lookup by aor")
    expect(account.registrationState == RegistrationState.UNREGISTERED, "unregistered")

    println("an ended call stays valid")
    val call = account.call("sip:bob@192.0.2.1:5060")
    expect(Medut.calls.contains(call), "tracked while live")
    expect(ended.await(20, TimeUnit.SECONDS), "terminal state within 20 s")
    val terminal = events.filterIsInstance<MedutEvent.CallStateChanged>().last()
    expect(terminal.call === call, "event carries the same Call object")
    expect(call.isEnded, "isEnded (${call.state})")
    expect(Medut.calls.isEmpty(), "untracked once ended")
    println("       ended: ${call.state} error=${call.endError} reason=${call.endReason}")
    expect(!terminal.endedLocally, "a timeout is not ended locally")
    val info = call.info()
    expect(info.state == call.state, "info() after end (${info.state})")
    call.hangup() // no-op
    expect(!call.isHeld, "isHeld false")
    expectThrows(MedutError.Code.CALL_ENDED, "hold() on an ended call") { call.hold() }

    println("a hangup is reported as ended locally")
    val hungUp = CountDownLatch(1)
    val hangupListener = Medut.addListener({ it.run() }) { ev ->
        if (ev is MedutEvent.CallStateChanged && ev.state.isTerminal) hungUp.countDown()
    }
    val local = account.call("sip:carol@192.0.2.1:5060")
    local.hangup()
    expect(hungUp.await(20, TimeUnit.SECONDS), "terminal state within 20 s")
    hangupListener.cancel()
    val localEnd = events.filterIsInstance<MedutEvent.CallStateChanged>().last { it.call === local }
    expect(localEnd.endedLocally, "endedLocally after hangup() (${localEnd.state})")

    println("events")
    expect(events.any { it is MedutEvent.Log }, "log events decoded")
    expect(events.filterIsInstance<MedutEvent.Log>().all { it.message.isNotEmpty() }, "log text intact")
    val states = events.filterIsInstance<MedutEvent.CallStateChanged>().map { it.state }
    expect(states.last().isTerminal, "call states end terminal ($states)")

    println("destroyed account")
    account.destroy()
    expect(Medut.accounts.isEmpty(), "untracked")
    expectThrows(MedutError.Code.ACCOUNT_DESTROYED, "register() after destroy") { account.register() }
    account.destroy() // idempotent
    expect(account.registrationState == RegistrationState.UNREGISTERED, "state reads UNREGISTERED")

    println("shutdown and restart")
    val stale = Medut.createAccount(AccountConfig(uri = "bob@192.0.2.1", password = "x"))
    Medut.shutdown()
    expect(!Medut.isRunning, "stopped")
    expectThrows(MedutError.Code.NOT_RUNNING, "call.info() after shutdown") { call.info() }
    expectThrows(MedutError.Code.NOT_RUNNING, "account.register() after shutdown") { stale.register() }
    expect(call.iceState == IceState.CLOSED, "iceState CLOSED after shutdown")
    Medut.start(config)
    expectThrows(MedutError.Code.NOT_RUNNING, "old handle after restart") { stale.register() }
    stale.destroy() // must not touch the freed native account
    Medut.shutdown()

    // The ended call's terminal event is released once the Call is
    // unreachable; give the releaser a chance to run.
    @Suppress("UNUSED_VALUE")
    run { System.gc(); Thread.sleep(200) }

    println(if (failures == 0) "PASS" else "$failures FAILED")
    exitProcess(if (failures == 0) 0 else 1)
}
