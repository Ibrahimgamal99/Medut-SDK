package dev.medut.android.telecom

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
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
        val call = bridge.takePending(request.extras?.getString(MedutTelecom.EXTRA_CALL_ID))
            ?: return failed()
        return MedutConnection(call, bridge).also { bridge.attach(call, it) }
    }

    override fun onCreateIncomingConnectionFailed(
        from: PhoneAccountHandle?, request: ConnectionRequest,
    ) {
        val bridge = MedutTelecom.bridge ?: return
        bridge.takePending(request.extras?.getString(MedutTelecom.EXTRA_CALL_ID))
            ?.let { bridge.incomingRefused(it) }
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

/** Answer / Decline from the built-in incoming-call notification. */
class MedutCallActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        MedutTelecom.handleAction(context, intent)
    }
}
