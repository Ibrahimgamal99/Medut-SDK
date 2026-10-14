package dev.medut.android

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import dev.medut.Medut

/**
 * Registers the accounts after a reboot, when the app has opted in with
 * [MedutAndroid.setStartOnBoot].  Without it nothing registers until a push
 * arrives, and a server that has dropped the binding has nowhere to push.
 *
 * The broadcast starts the process, so `Application.onCreate` runs
 * [MedutAndroid.start], which restores and registers the saved accounts;
 * this then refreshes every registration, as on a push.  Disabled in the
 * SDK's manifest until [MedutAndroid.setStartOnBoot] turns it on.
 */
class MedutBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        if (!Medut.isRunning) {
            Log.w("MedutBoot", "boot: the stack is not running; start it from Application.onCreate")
            return
        }
        MedutPush.reachAccounts()
    }
}
