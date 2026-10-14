package dev.medut.android.telecom

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.telecom.Connection
import android.telecom.ConnectionRequest
import android.telecom.ConnectionService
import android.telecom.DisconnectCause
import android.telecom.PhoneAccountHandle
import androidx.annotation.RequiresApi

/**
 * The self-managed ConnectionService the SDK manifest declares.  Telecom
 * binds it to create a connection for each call [TelecomBridge] announces.
 */
@RequiresApi(Build.VERSION_CODES.O)
class MedutConnectionService : ConnectionService() {
    override fun onCreateIncomingConnection(
        from: PhoneAccountHandle?, request: ConnectionRequest,
    ): Connection {
        val bridge = MedutTelecom.bridge ?: return failed()
        val id = request.extras?.getString(MedutTelecom.EXTRA_CALL_ID)
        bridge.takeRing(id)?.let { ring ->
            return MedutConnection(ring, bridge).also { bridge.attachRing(ring, it) }
        }
        val call = bridge.takePending(id) ?: return failed()
        return MedutConnection(call, bridge).also { bridge.attach(call, it) }
    }

    override fun onCreateIncomingConnectionFailed(
        from: PhoneAccountHandle?, request: ConnectionRequest,
    ) {
        val bridge = MedutTelecom.bridge ?: return
        val id = request.extras?.getString(MedutTelecom.EXTRA_CALL_ID)
        // A push ring Telecom refused: its INVITE rings (or is refused)
        // the ordinary way when it comes.
        bridge.takeRing(id)?.let { bridge.ringRefused(it); return }
        bridge.takePending(id)?.let { bridge.incomingRefused(it) }
    }

    override fun onCreateOutgoingConnection(
        from: PhoneAccountHandle?, request: ConnectionRequest,
    ): Connection {
        val bridge = MedutTelecom.bridge ?: return failed()
        val call = bridge.takePending(request.extras?.getString(MedutTelecom.EXTRA_CALL_ID))
            ?: return failed()
        return MedutConnection(call, bridge).also { bridge.attach(call, it) }
    }

    override fun onCreateOutgoingConnectionFailed(
        from: PhoneAccountHandle?, request: ConnectionRequest,
    ) {
        val bridge = MedutTelecom.bridge ?: return
        bridge.takePending(request.extras?.getString(MedutTelecom.EXTRA_CALL_ID))
            ?.let { bridge.outgoingRefused(it) }
    }

    private fun failed(): Connection =
        Connection.createFailedConnection(DisconnectCause(DisconnectCause.ERROR))
}

/** Decline from the built-in incoming-call notification. */
class MedutCallActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        MedutTelecom.handleAction(intent)
    }
}

/**
 * Answer from the built-in incoming-call notification.  An activity, not a
 * receiver: Android 12 stops a receiver a notification started from opening
 * an activity (a notification trampoline).  Invisible: it answers, opens the
 * app's call screen ([TelecomOptions.callActivity], else the launch activity)
 * with [MedutTelecom.ACTION_ANSWER] and the call's key, and finishes.
 */
class MedutAnswerActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) setShowWhenLocked(true)
        val key = intent?.getStringExtra(MedutTelecom.EXTRA_CALL_ID)
        if (key != null) {
            MedutTelecom.handleAction(intent)
            MedutTelecom.callScreenIntent(this, key, MedutTelecom.ACTION_ANSWER)
                ?.let { runCatching { startActivity(it) } }
        }
        finish()
    }
}
