package dev.medut.android.telecom

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Person
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import android.media.AudioAttributes
import android.media.RingtoneManager
import android.os.Build
import androidx.annotation.RequiresApi
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger

/**
 * The built-in incoming-call UI for apps that have none of their own up: a
 * ringing, high-priority call notification whose full-screen intent opens
 * the app's launch activity, with Answer and Decline actions.  On API 31+ it
 * is a `CallStyle` notification, which the system shows as a call.
 *
 * The app gets the call id back as [MedutTelecom.EXTRA_CALL_ID] on the launch
 * intent ([MedutTelecom.callFor]); for a call a push announced, the ring's key,
 * which [MedutTelecom.callFor] resolves once the INVITE has arrived.  Needs POST_NOTIFICATIONS on API 33+.
 */
@RequiresApi(Build.VERSION_CODES.O)
internal class IncomingCallNotifier(private val context: Context, private val options: TelecomOptions) {
    private val nm = context.getSystemService(NotificationManager::class.java)
    private val ids = ConcurrentHashMap<String, Int>()
    private val next = AtomicInteger(0x4D45_0000)

    /** Show the ringing notification for [callId], the key the actions and
     *  the launch intent carry ([MedutTelecom.EXTRA_CALL_ID]). */
    fun show(callId: String, title: String) {
        ensureChannel()
        val id = ids.getOrPut(callId) { next.incrementAndGet() }

        val launch = context.packageManager.getLaunchIntentForPackage(context.packageName)?.apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            putExtra(MedutTelecom.EXTRA_CALL_ID, callId)
        }
        val open = launch?.let {
            PendingIntent.getActivity(context, id, it,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        }
        val answer = action(MedutTelecom.ACTION_ANSWER, callId, id + 1)
        val decline = action(MedutTelecom.ACTION_DECLINE, callId, id + 2)

        val builder = Notification.Builder(context, options.notificationChannelId)
            .setSmallIcon(smallIcon())
            .setContentTitle(title)
            .setContentText("Incoming call")
            .setCategory(Notification.CATEGORY_CALL)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setAutoCancel(false)
        if (open != null) {
            builder.setContentIntent(open)
            builder.setFullScreenIntent(open, true)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val caller = Person.Builder().setName(title).setImportant(true).build()
            builder.style = Notification.CallStyle.forIncomingCall(caller, decline, answer)
        } else {
            builder.addAction(Notification.Action.Builder(null as Icon?, "Decline", decline).build())
            builder.addAction(Notification.Action.Builder(null as Icon?, "Answer", answer).build())
            @Suppress("DEPRECATION")
            builder.setPriority(Notification.PRIORITY_MAX)
        }
        nm.notify(id, builder.build())
    }

    fun cancel(callId: String) {
        ids.remove(callId)?.let { nm.cancel(it) }
    }

    fun cancelAll() {
        for (id in ids.values) nm.cancel(id)
        ids.clear()
    }

    private fun action(action: String, callId: String, requestCode: Int): PendingIntent {
        val intent = Intent(context, MedutCallActionReceiver::class.java)
            .setAction(action)
            .putExtra(MedutTelecom.EXTRA_CALL_ID, callId)
        return PendingIntent.getBroadcast(context, requestCode, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
    }

    private fun smallIcon(): Int =
        context.applicationInfo.icon.takeIf { it != 0 } ?: android.R.drawable.sym_call_incoming

    private fun ensureChannel() {
        if (nm.getNotificationChannel(options.notificationChannelId) != null) return
        val channel = NotificationChannel(options.notificationChannelId,
            options.notificationChannelName, NotificationManager.IMPORTANCE_HIGH).apply {
            setSound(RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE),
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build())
            enableVibration(true)
            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
        }
        nm.createNotificationChannel(channel)
    }
}
