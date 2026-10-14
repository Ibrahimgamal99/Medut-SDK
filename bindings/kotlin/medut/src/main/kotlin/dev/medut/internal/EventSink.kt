package dev.medut.internal

import dev.medut.CallState
import dev.medut.IceCandidateType
import dev.medut.IceState
import dev.medut.MediaDirection
import dev.medut.MediaStats
import dev.medut.Medut
import dev.medut.MedutError
import dev.medut.MedutEvent
import dev.medut.NetworkStage
import dev.medut.PresenceStatus
import dev.medut.QualityIssue
import dev.medut.RegistrationState

/**
 * Entry point for the SDK's event thread (medut_jni.c, on_event).  Decodes,
 * updates the call registry, and hands the event to [Medut] for delivery.
 *
 * Must not throw (C treats an exception as "not kept" and releases the event)
 * and must not take [Gate] or make blocking SDK calls: shutdown holds the
 * gate while it joins this thread.
 */
internal object EventSink {
    /** Returns true when an ended Call took ownership of [ev]. */
    @JvmStatic
    fun onEvent(raw: RawEvent, ev: Long): Boolean {
        var retained = false
        try {
            val event = decode(raw, ev) { retained = it }
            Medut.publish(event)
        } catch (t: Throwable) {
            // A decoding bug must not take the event thread down with it.
            System.err.println("Medut: event ${raw.type} not delivered: $t")
        }
        return retained
    }

    private inline fun decode(r: RawEvent, ev: Long, retain: (Boolean) -> Unit): MedutEvent {
        val registry = Medut.registry
        return when (r.type) {
            EventType.LOG -> MedutEvent.Log(r.message.orEmpty())

            EventType.REGISTRAR_WARNING -> MedutEvent.RegistrarWarning(r.message.orEmpty())

            EventType.REG_STATE -> MedutEvent.Registration(
                account = registry.account(r.account),
                state = RegistrationState.of(r.state),
                error = MedutError.fromRaw(r.error),
                errorText = r.error_str,
                retryAttempt = r.retry_attempt,
                retryDelayMs = r.retry_delay_ms,
            )

            EventType.INCOMING_CALL -> {
                if (r.call == 0L) return MedutEvent.Unknown(r.type)
                val account = registry.account(r.account)
                val from = r.from_uri.orEmpty()
                val call = registry.trackCall(r.call, account, CallState.RINGING,
                    isOutgoing = false, remoteUri = from, remoteDisplayName = r.display_name)
                MedutEvent.IncomingCall(account, call, from, r.display_name)
            }

            EventType.CALL_STATE -> {
                if (r.call == 0L) return MedutEvent.Unknown(r.type)
                val state = CallState.of(r.state)
                val error = MedutError.fromRaw(r.error)
                val call = registry.trackCall(r.call)
                if (state.isTerminal) {
                    retain(call.takeTerminalEvent(ev, state, error, r.reason))
                    val noneLeft = registry.untrackCall(r.call)
                    Medut.platform?.onCallEnded(call, noneLeft)
                } else {
                    call.update(state)
                    Medut.platform?.onCallLive(call)
                }
                MedutEvent.CallStateChanged(call.account ?: registry.account(r.account),
                    call, state, error, r.reason,
                    endedLocally = state.isTerminal && call.endRequestedLocally)
            }

            EventType.CALL_DTMF -> MedutEvent.Dtmf(registry.trackCallOrNull(r.call), r.digit.toChar())

            EventType.SDP_NEGOTIATION -> MedutEvent.SdpNegotiation(
                call = registry.trackCallOrNull(r.call),
                localSdp = r.local_sdp,
                remoteSdp = r.remote_sdp,
                negotiatedCodec = r.negotiated_codec,
                negotiatedCrypto = r.negotiated_crypto,
                rejectedCodecs = r.rejected_codecs.orEmpty().filterNotNull(),
                warnings = r.warnings.orEmpty().filterNotNull(),
            )

            EventType.SIP_TRACE -> MedutEvent.SipTrace(
                direction = MediaDirection.of(r.dir),
                transport = r.transport.orEmpty(),
                remoteAddress = r.remote_addr.orEmpty(),
                message = r.raw_message.orEmpty(),
                timestampMicros = r.timestamp_us,
            )

            EventType.MEDIA_STATS -> {
                val stats = r.stats ?: return MedutEvent.Unknown(r.type)
                MedutEvent.MediaStatsUpdate(registry.trackCallOrNull(r.call), MediaStats.from(stats))
            }

            EventType.TRANSFER_REQUEST -> MedutEvent.TransferRequest(
                account = registry.account(r.account),
                call = registry.trackCallOrNull(r.call),
                referToUri = r.refer_to_uri.orEmpty(),
                isAttended = r.has_replaces,
                autoFollowed = r.auto_followed,
            )

            EventType.TRANSFER_FAILED -> MedutEvent.TransferFailed(
                account = registry.account(r.account),
                call = registry.trackCallOrNull(r.call),
                reason = r.reason,
            )

            EventType.MWI -> MedutEvent.MessageWaiting(
                account = registry.account(r.account),
                messagesWaiting = r.messages_waiting,
                newVoice = r.new_voice, oldVoice = r.old_voice,
                newUrgent = r.new_urgent, oldUrgent = r.old_urgent,
                rawBody = r.raw_body,
            )

            EventType.MESSAGE -> MedutEvent.Message(
                account = registry.account(r.account),
                fromUri = r.from_uri.orEmpty(),
                body = r.body.orEmpty(),
                contentType = r.content_type,
            )

            EventType.PRESENCE_STATE -> MedutEvent.Presence(
                account = registry.account(r.account),
                targetUri = r.target_uri.orEmpty(),
                status = PresenceStatus.of(r.status),
            )

            EventType.QUALITY_ALERT -> MedutEvent.QualityAlert(
                call = registry.trackCallOrNull(r.call),
                issue = QualityIssue.of(r.issue),
                value = r.value,
                threshold = r.threshold,
                recovering = r.recovering,
            )

            EventType.NETWORK -> MedutEvent.Network(
                stage = NetworkStage.of(r.event),
                call = registry.trackCallOrNull(r.call),
                accountHandle = registry.account(r.account),
                localAddress = r.local_addr.orEmpty(),
                attempt = r.attempt,
                maxAttempts = r.max_attempts,
                elapsedMs = r.elapsed_ms,
                ice = r.ice,
                error = MedutError.fromRaw(r.error),
            )

            EventType.ICE_STATE -> MedutEvent.IceStateChanged(
                call = registry.trackCallOrNull(r.call),
                state = IceState.of(r.state),
                previous = IceState.of(r.prev),
                generation = r.generation,
                localType = IceCandidateType.of(r.local_type),
                remoteType = IceCandidateType.of(r.remote_type),
                localAddress = r.local_addr.orEmpty(),
                remoteAddress = r.remote_addr.orEmpty(),
                rttMs = r.rtt_ms,
                error = MedutError.fromRaw(r.error),
            )

            else -> MedutEvent.Unknown(r.type)
        }
    }
}
