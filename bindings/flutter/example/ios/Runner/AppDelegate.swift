import Flutter
import PushKit
import UIKit
import medut

/// Starts the SIP stack natively, before any Flutter engine exists, and
/// wires PushKit to it.
///
/// This is what lets a VoIP push bring the phone back online without Dart:
/// the accounts the Flutter UI registered with `remember: true` are
/// re-created and registered here, CallKit rings for an incoming call, and
/// when the Flutter side comes up `Medut.start()` attaches to this running
/// stack (`sdk.reattached`).  The Android example does the same in
/// MedutApplication.kt.
@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate, PKPushRegistryDelegate {
  private var pushRegistry: PKPushRegistry?

  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    var config = MedutConfig()
    config.logLevel = .debug
    config.traceSIP = true
    config.traceSDPDiff = true
    config.statsIntervalMs = 2000
    config.mosAlertThreshold = 3.5
    config.lossAlertThreshold = 5.0
    config.jitterAlertThreshold = 40.0

    var options = StartOptions()
    options.callKit = CallKitOptions()
    do {
      try Medut.shared.start(config, options: options)
    } catch {
      NSLog("Medut: start failed: \(error)")
    }

    let registry = PKPushRegistry(queue: .main)
    registry.delegate = self
    registry.desiredPushTypes = [.voIP]
    pushRegistry = registry

    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)
  }

  // MARK: PKPushRegistryDelegate

  func pushRegistry(_ registry: PKPushRegistry, didUpdate pushCredentials: PKPushCredentials,
                    for type: PKPushType) {
    MedutPush.updatePushToken(pushCredentials.token)
  }

  func pushRegistry(_ registry: PKPushRegistry, didInvalidatePushTokenFor type: PKPushType) {
    MedutPush.updatePushToken(hex: "")
  }

  func pushRegistry(_ registry: PKPushRegistry, didReceiveIncomingPushWith payload: PKPushPayload,
                    for type: PKPushType, completion: @escaping () -> Void) {
    // Reports the call to CallKit at once (iOS requires it), re-registers,
    // and binds the INVITE to that call when it arrives.
    MedutPush.wake(payload: payload.dictionaryPayload, completion: completion)
  }
}
