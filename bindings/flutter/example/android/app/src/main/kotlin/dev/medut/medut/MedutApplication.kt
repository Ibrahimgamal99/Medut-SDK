package dev.medut.medut

import android.app.Application
import dev.medut.LogLevel
import dev.medut.MedutConfig
import dev.medut.android.AndroidOptions
import dev.medut.android.MedutAndroid
import dev.medut.android.telecom.TelecomOptions

/**
 * Starts the SIP stack natively, before any Flutter engine exists.
 *
 * This is what lets a push (or just a launch) bring the phone back online
 * without Dart: the accounts the Flutter UI registered with `remember: true`
 * are re-created and registered right here, and Telecom rings for an
 * incoming call.  When the Flutter side comes up, `Medut.start()` attaches
 * to this running stack (`sdk.reattached`) and finds the account in
 * `sdk.accounts`.
 */
class MedutApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        MedutAndroid.start(
            this,
            MedutConfig(
                logLevel = LogLevel.DEBUG,
                traceSip = true,
                traceSdpDiff = true,
                statsIntervalMs = 2000,
                mosAlertThreshold = 3.5f,
                lossAlertThreshold = 5.0f,
                jitterAlertThreshold = 40.0f,
                // Relative: lands in the app's cache dir.
                pcapPath = "medut.pcap",
            ),
            AndroidOptions(telecom = TelecomOptions()),
        )
    }
}
