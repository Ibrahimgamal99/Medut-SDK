package dev.medut.medut

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import dev.medut.android.MedutPush

/**
 * Debug builds only: stands in for an FCM message so the push wake-up path
 * can be exercised without a Firebase project.
 *
 *   adb shell am force-stop dev.medut.medut
 *   adb shell am broadcast -n dev.medut.medut/.FakePushReceiver
 *
 * The broadcast starts the process (so [MedutApplication.onCreate] runs and
 * registers the remembered accounts), then this does what a
 * FirebaseMessagingService would: `MedutPush.wake`, which makes the
 * accounts reachable and waits for the INVITE.  Call the extension within
 * the wait to see Telecom ring with no UI up.
 */
class FakePushReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        Thread {
            try {
                val call = MedutPush.wake(context, waitForCallMs = 20_000)
                Log.i("MedutFakePush", "wake returned ${call?.remoteUri ?: "no call"}")
            } catch (e: Exception) {
                Log.w("MedutFakePush", "wake failed: $e")
            } finally {
                pending.finish()
            }
        }.start()
    }
}
