/**
 * @file ice_route.c  medut-ice: receive path and STUN demultiplexing
 *
 * Every datagram that reaches one of the agent's own ports comes here.
 * STUN is told apart from data by the RFC 5389 rules (first two bits zero,
 * magic cookie), decoded once, and routed: Binding Requests to the server
 * side, responses to the ping table (then to libre's gathering
 * transactions), indications to the pair's receive clock.  Data stamps the
 * pair it arrived on and is handed to the owner, who injects it into the
 * stream socket above the ICE layer with the source rewritten to the
 * virtual peer.
 *
 * The stream sockets (baresip's own RTP/RTCP) are served by a helper in
 * ice_mod.c that calls icea_route_stun_demux() for the STUN half and does
 * the source rewrite itself, so the demux logic lives in one place.
 */

#include <string.h>
#include "ice_internal.h"


bool icea_is_stun(const struct mbuf *mb)
{
	const uint8_t *p;

	if (!mb || mbuf_get_left(mb) < 20)
		return false;

	p = mbuf_buf(mb);
	if ((p[0] & 0xc0) != 0)
		return false;

	return p[4] == 0x21 && p[5] == 0x12 && p[6] == 0xa4 && p[7] == 0x42;
}


bool icea_route_stun_demux(struct icea_gen *gen, struct icea_port *port,
			   const struct sa *src, struct mbuf *mb)
{
	struct stun_unknown_attr ua;
	struct stun_msg *msg = NULL;
	size_t pos;

	if (!gen || !port || !mb || !icea_is_stun(mb))
		return false;

	pos = mb->pos;
	if (stun_msg_decode(&msg, mb, &ua)) {
		mb->pos = pos;         /* not STUN after all: hand it back */
		return false;
	}

	if (stun_msg_method(msg) == STUN_METHOD_BINDING) {

		switch (stun_msg_class(msg)) {

		case STUN_CLASS_REQUEST:
			icea_stund_recv(gen, port, src, msg);
			break;

		case STUN_CLASS_SUCCESS_RESP:
		case STUN_CLASS_ERROR_RESP:
			if (icea_check_recv_response(gen, port, src, msg))
				break;
			/* Not one of ours: a gathering transaction, then. */
			if (gen->stun)
				(void)stun_ctrans_recv(gen->stun, msg, &ua);
			break;

		case STUN_CLASS_INDICATION:
			/* pjnath / libre keepalives — proof of life only */
			icea_pair_note_ping_recv(gen, port->compid, port, src);
			break;

		default:
			break;
		}
	}
	else if (gen->stun) {
		/* TURN and friends on a gathering socket */
		(void)stun_ctrans_recv(gen->stun, msg, &ua);
	}

	mem_deref(msg);
	return true;
}


void icea_route_port_recv(const struct sa *src, struct mbuf *mb, void *arg)
{
	struct icea_port *port = arg;
	struct icea_gen *gen;

	if (!port || !port->gen || !src || !mb)
		return;
	gen = port->gen;

	if (icea_route_stun_demux(gen, port, src, mb))
		return;

	/* Application data.  Only a known remote counts as receiving; an
	 * unknown source is either dropped (policy) or passed up and logged. */
	if (icea_pair_by_src(gen, port->compid, port, src)) {
		icea_pair_note_data(gen, port->compid, port, src);
	}
	else if (gen->conf->drop_unknown_src) {
		ICEA_DEBUG(gen, "ice[gen %u]: dropping data from unknown %J\n",
			   gen->seq, src);
		return;
	}

	if (gen->cb.data)
		gen->cb.data(gen, port->compid, src, mb, gen->cb.arg);
}
