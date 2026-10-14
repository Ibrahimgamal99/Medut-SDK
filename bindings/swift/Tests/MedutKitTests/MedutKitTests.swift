import XCTest
import Medut
@testable import MedutKit

/// Needs no SIP server: binding parity with the C header, config marshalling,
/// and the handle-lifetime rules around start and shutdown.
final class MedutKitTests: XCTestCase {

    override func tearDown() {
        Medut.shared.shutdown()
        super.tearDown()
    }

    // ── Enum parity: the Swift raw values must be the C values ───────────

    func testEnumValuesMatchHeader() {
        XCTAssertEqual(Transport.wss.rawValue, Int(MEDUT_TRANSPORT_WSS.rawValue))
        XCTAssertEqual(MediaEncryption.dtlsSRTP.rawValue, Int(MEDUT_MEDIA_ENC_DTLS_SRTP.rawValue))
        XCTAssertEqual(CallState.failed.rawValue, Int(MEDUT_CALL_FAILED.rawValue))
        XCTAssertEqual(RegistrationState.reconnecting.rawValue, Int(MEDUT_REG_RECONNECTING.rawValue))
        XCTAssertEqual(ICEState.closed.rawValue, Int(MEDUT_ICE_CLOSED.rawValue))
        XCTAssertEqual(ICECandidateType.none.rawValue, Int(MEDUT_ICE_CAND_NONE.rawValue))
        XCTAssertEqual(NetworkStage.callICEFailed.rawValue, Int(MEDUT_NET_CALL_ICE_FAILED.rawValue))
        XCTAssertEqual(QualityIssue.mediaStall.rawValue, Int(MEDUT_QUALITY_MEDIA_STALL.rawValue))
        XCTAssertEqual(PushProvider.fcm.rawValue, Int(MEDUT_PUSH_PROVIDER_FCM.rawValue))
        XCTAssertEqual(DTMFMode.auto.rawValue, Int(MEDUT_DTMF_AUTO.rawValue))
        XCTAssertEqual(ReliableProvisionalMode.required.rawValue, Int(MEDUT_100REL_REQUIRED.rawValue))
        XCTAssertEqual(ICEHandoverPolicy.failFast.rawValue, Int(MEDUT_ICE_HANDOVER_FAIL_FAST.rawValue))
        XCTAssertEqual(PresenceStatus.busy.rawValue, Int(MEDUT_PRESENCE_BUSY.rawValue))
        XCTAssertEqual(JitterBufferType.fixed.rawValue, Int(MEDUT_JBUF_FIXED.rawValue))
        XCTAssertEqual(MediaDirection.tx.rawValue, Int(MEDUT_MEDIA_DIR_TX.rawValue))
        XCTAssertEqual(MOSMethod.simplified.rawValue, Int(MEDUT_MOS_SIMPLIFIED.rawValue))
        XCTAssertEqual(ICETransportPolicy.relay.rawValue, Int(MEDUT_ICE_TRANSPORT_RELAY.rawValue))
    }

    // ── Config marshalling ───────────────────────────────────────────────

    func testDefaultConfigRoundTripsToCDefaults() {
        let defaults = MedutConfig.cDefaults
        MedutConfig().withCConfig { c in
            XCTAssertEqual(c.version, defaults.version)
            XCTAssertEqual(c.struct_size, defaults.struct_size)
            XCTAssertEqual(c.reg_expires, defaults.reg_expires)
            XCTAssertEqual(c.keepalive_interval, defaults.keepalive_interval)
            XCTAssertEqual(c.stats_interval_ms, defaults.stats_interval_ms)
            XCTAssertEqual(c.opus.bitrate, defaults.opus.bitrate)
            XCTAssertEqual(c.opus.fec, defaults.opus.fec)
            XCTAssertEqual(c.verify_server, defaults.verify_server)
            XCTAssertEqual(c.platform_audio_activate, defaults.platform_audio_activate)
            XCTAssertEqual(c.aec_mode, defaults.aec_mode)
            XCTAssertEqual(c.net_max_attempts, defaults.net_max_attempts)
            XCTAssertEqual(c.audio_codec_name_count, 0)
            XCTAssertNil(c.ice_servers)
            XCTAssertNil(c.ws_extra_headers)
        }
    }

    func testConfigStringsListsAndCodecs() {
        var config = MedutConfig()
        config.serverURL = "wss://pbx.example.com/ws"
        config.wsExtraHeaders = ["X-A: 1", "X-B: 2"]
        config.iceServers = [ICEServer(url: "turn:t.example.com", username: "u", password: "p")]
        config.audioCodecs = ["pcmu", "opus", String(repeating: "x", count: 40)]

        config.withCConfig { c in
            XCTAssertEqual(cString(c.server_url), "wss://pbx.example.com/ws")
            XCTAssertEqual(cString(c.ws_extra_headers[0]), "X-A: 1")
            XCTAssertEqual(cString(c.ws_extra_headers[1]), "X-B: 2")
            XCTAssertNil(c.ws_extra_headers[2])
            XCTAssertEqual(c.ice_server_count, 1)
            XCTAssertEqual(cString(c.ice_servers[0].url), "turn:t.example.com")
            XCTAssertEqual(cString(c.ice_servers[0].pass), "p")
            XCTAssertEqual(c.audio_codec_name_count, 3)
            XCTAssertEqual(fixedCString(c.audio_codec_names.0), "pcmu")
            XCTAssertEqual(fixedCString(c.audio_codec_names.1), "opus")
            // Truncated to 31 bytes and still NUL-terminated.
            XCTAssertEqual(fixedCString(c.audio_codec_names.2).count, 31)
        }
    }

    func testAccountConfigInheritsRTCPMuxWhenUnset() {
        var config = AccountConfig(uri: "alice@pbx.example.com", password: "secret")
        config.withCConfig { c in XCTAssertFalse(c.pointee.rtcp_mux_set) }
        config.rtcpMux = false
        config.withCConfig { c in
            XCTAssertTrue(c.pointee.rtcp_mux_set)
            XCTAssertFalse(c.pointee.rtcp_mux)
        }
    }

    // ── Lifecycle and handle lifetime ────────────────────────────────────

    private func startQuietly() throws {
        var options = StartOptions()
        options.monitorNetwork = false
        options.audioSession = .manual
        var config = MedutConfig()
        config.platformAudioActivate = false
        try Medut.shared.start(config, options: options)
    }

    func testStartTwiceThrowsAlready() throws {
        try startQuietly()
        XCTAssertTrue(Medut.shared.isRunning)
        XCTAssertThrowsError(try startQuietly()) { error in
            XCTAssertEqual((error as? MedutError)?.code, .already)
        }
    }

    func testAccountLifecycle() throws {
        try startQuietly()
        let account = try Medut.shared.createAccount(
            AccountConfig(uri: "alice@127.0.0.1", password: "secret"))
        XCTAssertEqual(account.aor, "sip:alice@127.0.0.1")
        XCTAssertEqual(Medut.shared.account(aor: "sip:alice@127.0.0.1"), account)
        XCTAssertEqual(account.registrationState, .unregistered)

        account.destroy()
        XCTAssertTrue(Medut.shared.accounts.isEmpty)
        XCTAssertThrowsError(try account.register()) { error in
            XCTAssertEqual((error as? MedutError)?.code, .accountDestroyed)
        }
        account.destroy() // idempotent
    }

    func testHandlesStopWorkingAfterShutdown() throws {
        try startQuietly()
        let account = try Medut.shared.createAccount(
            AccountConfig(uri: "bob@127.0.0.1", password: "secret"))
        Medut.shared.shutdown()

        XCTAssertFalse(Medut.shared.isRunning)
        XCTAssertThrowsError(try account.register()) { error in
            XCTAssertEqual((error as? MedutError)?.code, .notRunning)
        }

        // A restart is a new generation: the old handle must stay dead even
        // though the stack is running again.
        try startQuietly()
        XCTAssertThrowsError(try account.register()) { error in
            XCTAssertEqual((error as? MedutError)?.code, .notRunning)
        }
        account.destroy() // must not touch the freed native account
    }

    func testEventsAreDelivered() throws {
        let gotLog = expectation(description: "log event")
        gotLog.assertForOverFulfill = false
        let subscription = Medut.shared.addEventHandler(queue: .global()) { event in
            if case .log = event { gotLog.fulfill() }
        }
        defer { subscription.cancel() }

        var options = StartOptions()
        options.monitorNetwork = false
        options.audioSession = .manual
        var config = MedutConfig()
        config.platformAudioActivate = false
        config.logLevel = .debug
        try Medut.shared.start(config, options: options)
        wait(for: [gotLog], timeout: 5)
    }
}
