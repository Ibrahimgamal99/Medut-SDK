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
import android.util.Log
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
 * which [MedutTelecom.callFor] resolves once the INVITE has arrived.  Needs POST_NOTIFICATIONS on API 33+
 * (see [dev.medut.android.MedutAndroid.notificationsEnabled]).
 *
 * Degrades instead of failing: without notification permission nothing is
 * posted (logged); without a call screen to open, or on API 34+ without
 * USE_FULL_SCREEN_INTENT, there is no full-screen intent, and since a
 * `CallStyle` notification needs one (or a foreground service, which the SDK
 * does not run), it is then a plain high-priority notification with the same
 * Answer / Decline actions.
 */
@RequiresApi(Build.VERSION_CODES.O)
internal class IncomingCallNotifier(private val context: Context, private val options: TelecomOptions) {
    private val nm = context.getSystemService(NotificationManager::class.java)
    private val ids = ConcurrentHashMap<String, Int>()
    private val next = AtomicInteger(0x4D45_0000)

    /** Show the ringing notification for [callId], the key the actions and
     *  the launch intent carry ([MedutTelecom.EXTRA_CALL_ID]). */
    fun show(callId: String, title: String) {
        if (!nm.areNotificationsEnabled()) {
            Log.w(TAG, "incoming call from $title: notifications are disabled for this app " +
                "(POST_NOTIFICATIONS not granted on API 33+, or turned off by the user), " +
                "so the incoming-call notification cannot be shown")
            return
        }
        ensureChannel()
        if (nm.getNotificationChannel(options.notificationChannelId)?.importance ==
            NotificationManager.IMPORTANCE_NONE) {
            Log.w(TAG, "incoming call from $title: the notification channel " +
                "'${options.notificationChannelId}' is blocked by the user")
        }
        val id = ids.getOrPut(callId) { next.incrementAndGet() }

        val open = MedutTelecom.callScreenIntent(context, callId, null)?.let {
            PendingIntent.getActivity(context, id, it,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        }
        // Answer opens an activity directly: Android 12+ blocks one started
        // from a receiver a notification triggered.
        val answer = PendingIntent.getActivity(context, id + 1,
            Intent(context, MedutAnswerActivity::class.java)
                .setAction(MedutTelecom.ACTION_ANSWER)
                .putExtra(MedutTelecom.EXTRA_CALL_ID, callId)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val decline = action(MedutTelecom.ACTION_DECLINE, callId, id + 2)
        val fullScreen = open != null && fullScreenIntentAllowed()

        try {
            nm.notify(id, build(title, open, fullScreen, answer, decline, callStyle = fullScreen))
        } catch (e: RuntimeException) {
            // A CallStyle the system refused after all (IllegalArgumentException:
            // "must either be for a foreground Service or use a fullScreenIntent"),
            // or a SecurityException from a vendor build: retry plain.
            Log.w(TAG, "incoming-call notification refused, retrying without CallStyle: $e")
            try {
                nm.notify(id, build(title, open, fullScreen, answer, decline, callStyle = false))
            } catch (e2: RuntimeException) {
                Log.e(TAG, "incoming-call notification could not be posted: $e2")
                ids.remove(callId)
            }
        }
    }

    private fun build(title: String, open: PendingIntent?, fullScreen: Boolean,
                      answer: PendingIntent, decline: PendingIntent,
                      callStyle: Boolean): Notification {
        val builder = Notification.Builder(context, options.notificationChannelId)
            .setSmallIcon(smallIcon())
            .setContentTitle(title)
            .setContentText("Incoming call")
            .setCategory(Notification.CATEGORY_CALL)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setAutoCancel(false)
        if (open != null) builder.setContentIntent(open)
        if (open != null && fullScreen) builder.setFullScreenIntent(open, true)
        if (callStyle && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val caller = Person.Builder().setName(title).setImportant(true).build()
            builder.style = Notification.CallStyle.forIncomingCall(caller, decline, answer)
        } else {
            builder.addAction(Notification.Action.Builder(null as Icon?, "Decline", decline).build())
            builder.addAction(Notification.Action.Builder(null as Icon?, "Answer", answer).build())
            @Suppress("DEPRECATION")
            builder.setPriority(Notification.PRIORITY_MAX)
        }
        val notification = builder.build()
        // Ring until answered, declined or gone, not once.
        notification.flags = notification.flags or Notification.FLAG_INSISTENT
        return notification
    }

    /** API 34+: USE_FULL_SCREEN_INTENT can be denied (by the user, or by
     *  Play policy for apps that are not calling apps). */
    private fun fullScreenIntentAllowed(): Boolean {
        if (Build.VERSION.SDK_INT < 34) return true
        val allowed = runCatching { nm.canUseFullScreenIntent() }.getOrDefault(false)
        if (!allowed) {
            Log.w(TAG, "USE_FULL_SCREEN_INTENT is not granted: the incoming call is shown " +
                "as a heads-up notification, not full screen")
        }
        return allowed
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

    private companion object {
        const val TAG = "MedutTelecom"
    }
}
