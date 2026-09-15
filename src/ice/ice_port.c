/**
 * @file ice_port.c  echo-ice: ports (one socket per interface × family)
 *
 * A port is a socket the generation can send and receive on.  The stream's
 * own RTP/RTCP sockets are attached with is_stream_sock=true: they are
 * shared with baresip, their receive helper belongs to the media (see
 * ice_route.c), and sends must go through udp_send_helper so the DTLS/SRTP
 * layers above are bypassed rather than re-entered.  Ports the agent
 * allocates itself (relay in PR1, per-interface in PR3) carry their own
 * receive handler and are sent on with plain udp_send.
 *
 * The bound address is supplied by the creator rather than read back with
 * udp_local_get(): that call needs a real file descriptor, and the loopback
 * harness runs on sockless sockets.
 */

#include <string.h>
#include "ice_internal.h"


static void port_destructor(void *arg)
{
	struct icea_port *port = arg;

	list_unlink(&port->le);

	/* Gathering transaction first: libre's ctrans destructor is silent
	 * (no completion callback), but it must go before the socket it
	 * would otherwise try to send retransmits on. */
	mem_deref(port->ct_gath);
	mem_deref(port->ska);
	mem_deref(port->turnc);
	mem_deref(port->uh);     /* helper before socket: it locks us->lock */
	mem_deref(port->us);
}


int icea_port_attach(struct icea_gen *gen, unsigned compid,
		     struct udp_sock *us, const struct sa *laddr,
		     const char *ifname, int cost, bool is_stream_sock,
		     struct icea_port **portp)
{
	struct icea_port *port;

	if (!gen || !us || !laddr || compid < 1 || compid > gen->ncomp)
		return EINVAL;

	port = mem_zalloc(sizeof(*port), port_destructor);
	if (!port)
		return ENOMEM;

	port->gen            = gen;
	port->compid         = compid;
	port->us             = mem_ref(us);
	port->laddr          = *laddr;
	port->cost           = cost;
	port->is_stream_sock = is_stream_sock;
	if (ifname)
		str_ncpy(port->ifname, ifname, sizeof(port->ifname));

	list_append(&gen->portl, &port->le, port);

	if (portp)
		*portp = port;
	return 0;
}


struct icea_port *icea_port_find(const struct icea_gen *gen,
				 const struct udp_sock *us)
{
	struct le *le;

	if (!gen)
		return NULL;

	LIST_FOREACH(&gen->portl, le) {
		struct icea_port *port = le->data;
		if (port->us == us)
			return port;
	}
	return NULL;
}


/* Bind one socket for `laddr` inside [port_min, port_max].  The RTP
 * component takes an even port and, when RTCP is not multiplexed, the
 * caller asks for port+1 next through the same range (RFC 3550 §11). */
int icea_port_alloc(struct icea_gen *gen, unsigned compid,
		    const struct sa *laddr, const char *ifname, int cost,
		    uint16_t port_min, uint16_t port_max,
		    struct icea_port **portp)
{
	struct icea_port *port;
	struct udp_sock *us = NULL;
	struct sa bind_addr;
	uint16_t span, start, p;
	int err = EADDRINUSE;

	if (!gen || !laddr || compid < 1 || compid > gen->ncomp)
		return EINVAL;
	if (port_min < 1024 || port_max <= port_min) {
		port_min = 1024;
		port_max = 65535;
	}

	port = mem_zalloc(sizeof(*port), port_destructor);
	if (!port)
		return ENOMEM;
	port->gen    = gen;
	port->compid = compid;
	port->cost   = cost;
	if (ifname)
		str_ncpy(port->ifname, ifname, sizeof(port->ifname));

	bind_addr = *laddr;
	span  = (uint16_t)((port_max - port_min) / 2);
	start = (uint16_t)(port_min + 2 * (rand_u16() % (span ? span : 1)));
	start &= 0xfffe;

	for (unsigned i = 0; i < span + 1u; i++) {
		p = (uint16_t)(start + 2 * i);
		if (p > port_max)
			p = (uint16_t)(port_min + (p - port_max - 1));
		p &= 0xfffe;

		sa_set_port(&bind_addr, compid == 2 ? p + 1 : p);
		err = udp_listen(&us, &bind_addr, icea_route_port_recv, port);
		if (!err)
			break;
		if (err != EADDRINUSE && err != EACCES)
			break;
	}
	if (err) {
		mem_deref(port);
		return err;
	}

	port->us    = us;
	port->laddr = bind_addr;
	list_append(&gen->portl, &port->le, port);

	ICEA_DEBUG(gen, "ice[gen %u]: port {%u} %J on %s\n", gen->seq, compid,
		   &port->laddr, ifname ? ifname : "?");

	if (portp)
		*portp = port;
	return 0;
}
