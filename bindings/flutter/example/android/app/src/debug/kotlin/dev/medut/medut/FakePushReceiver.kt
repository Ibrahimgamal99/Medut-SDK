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
 *   adb shell am broadcast -n dev.medut.medut/.FakePushReceiver \
 *       --es type call --es caller_name Alice --es caller_uri sip:alice@pbx
 *
 * The broadcast starts the process (so [MedutApplication.onCreate] runs and
 * registers the remembered accounts), then this does what a
 * FirebaseMessagingService would: every string extra becomes a field of the
 * data message, and `MedutPush.wake` reads it (docs/guides/push.md).  With
 * Telecom on, the phone rings at once with the caller given; call the
 * extension within the wait and the INVITE takes the ring over.  Add
 * `--es call_id <Call-ID>` to match one INVITE; send `--es type cancel` to
 * end a ring whose INVITE never came.  No extras is a plain wake-up.
 */
class FakePushReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val data = intent.extras?.let { extras ->
            extras.keySet().mapNotNull { key -> extras.getString(key)?.let { key to it } }.toMap()
        } ?: emptyMap()
        val pending = goAsync()
        Thread {
            try {
                val call = MedutPush.wake(context, data, waitForCallMs = 20_000)
                Log.i("MedutFakePush", "wake $data returned ${call?.remoteUri ?: "no call"}")
            } catch (e: Exception) {
                Log.w("MedutFakePush", "wake failed: $e")
            } finally {
                pending.finish()
            }
        }.start()
    }
}
