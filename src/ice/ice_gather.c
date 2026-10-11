/**
 * @file ice_gather.c  echo-ice: candidate gathering
 *
 * RFC 8445 §5.1.1.  For every port the generation owns: a host candidate;
 * one server-reflexive request per configured STUN/TURN server, raced —
 * the first mapped address wins, later ones are redundant unless a NAT
 * maps per destination (then they are all real, and all offered); and
 * one TURN allocation per TURN server on a port of its own, because libre
 * relays every headroom-bearing packet whose destination has a channel,
 * so a relay candidate cannot share a socket with a host one.
 *
 * Gathering is event-driven: the generation is `gathered` when every
 * outstanding request has answered or failed.  The owner keeps the
 * deadline (cfg.ice_gathering_timeout_ms) and may release the offer with
 * whatever exists before that.  Relay-only makes an empty result a
 * failure at the owner, not here.
 */

#include <string.h>
#include "ice_internal.h"


struct gather_req {
	struct icea_port *port;      /* borrowed                                */
	struct icea_server *srv;     /* borrowed                                */
};


static void server_destructor(void *arg)
{
	struct icea_server *srv = arg;

	list_unlink(&srv->le);
	mem_deref(srv->user);
	mem_deref(srv->pass);
}


int icea_server_alloc(struct icea_server **srvp, bool turn, int proto,
		      bool tls, const struct sa *addr, const char *user,
		      const char *pass)
{
	struct icea_server *srv;
	int err = 0;

	if (!srvp || !addr)
		return EINVAL;

	srv = mem_zalloc(sizeof(*srv), server_destructor);
	if (!srv)
		return ENOMEM;

	srv->turn  = turn;
	srv->proto = proto ? proto : IPPROTO_UDP;
	srv->tls   = tls;
	srv->addr  = *addr;
	if (user)
		err |= str_dup(&srv->user, user);
	if (pass)
		err |= str_dup(&srv->pass, pass);

	if (err)
		mem_deref(srv);
	else
		*srvp = srv;
	return err;
}


/* A port on the interface the call signals from has concluded.  That is
 * enough to offer: an extra interface still waiting on its STUN answer
 * (cellular is slow to answer right after an attach) must not hold the
 * primary path's offer back, and a carried-over port that already knows
 * its mapping makes the offer immediate on the interface the call just
 * moved to. */
static bool primary_done(const struct icea_gen *gen)
{
	struct le *le;

	LIST_FOREACH(&gen->portl, le) {
		const struct icea_port *port = le->data;
		if (port->primary && port->gathered && !port->turnc)
			return true;
	}
	return false;
}


static void gather_complete_maybe(struct icea_gen *gen)
{
	if (gen->gathered)
		return;
	if (gen->gather_pending && !primary_done(gen))
		return;

	gen->gathered = true;
	icea_cand_redund_elim(gen);

	ICEA_DEBUG(gen, "ice[gen %u]: gathered %u local candidates\n",
		   gen->seq, list_count(&gen->lcandl));

	if (gen->state == ICEA_STATE_GATHERING || gen->state == ICEA_STATE_NEW)
		icea_gen_set_state(gen, gen->remote_ready ? ICEA_STATE_CHECKING
							  : ICEA_STATE_GATHERING);

	if (gen->cb.gathered)
		gen->cb.gathered(gen, gen->cb.arg);
}


/* ── Server-reflexive ────────────────────────────────────────────────────── */

static void srflx_resp_handler(int err, uint16_t scode, const char *reason,
			       const struct stun_msg *msg, void *arg)
{
	struct icea_port *port = arg;
	struct icea_gen *gen = port->gen;
	struct stun_attr *attr;
	struct icea_cand *host;
	(void)reason;

	port->ct_gath = NULL;    /* libre frees the transaction after this */
	if (gen->gather_pending)
		gen->gather_pending--;

	if (err || scode) {
		ICEA_DEBUG(gen, "ice[gen %u]: srflx on %J failed (%m %u)\n",
			   gen->seq, &port->laddr, err, scode);
		goto out;
	}

	attr = stun_msg_attr(msg, STUN_ATTR_XOR_MAPPED_ADDR);
	if (!attr)
		attr = stun_msg_attr(msg, STUN_ATTR_MAPPED_ADDR);
	if (!attr)
		goto out;

	host = icea_lcand_on_port(gen, port->compid, port);
	if (!host)
		goto out;

	port->srflx = attr->v.sa;

	/* Same address as the host: no NAT on this path — add_local dedupes
	 * by address and returns the host, which is what we want. */
	(void)icea_cand_add_local(gen, ICE_CAND_TYPE_SRFLX, port->compid, port,
				  host, &attr->v.sa, icea_cand_lpref(port));

 out:
	port->gathered = true;
	gather_complete_maybe(gen);
}


static int gather_srflx(struct icea_gen *gen, struct icea_port *port,
			const struct icea_server *srv)
{
	int err;

	if (!gen->stun) {
		struct stun_conf conf = {
			.rto = gen->conf->stun_rto_ms,
			.rc  = gen->conf->stun_rc,
			.rm  = STUN_DEFAULT_RM,
			.ti  = STUN_DEFAULT_TI,
			.tos = 0,
		};
		err = stun_alloc(&gen->stun, &conf, NULL, NULL);
		if (err)
			return err;
	}

	/* One outstanding request per port; a second STUN server races it
	 * on a separate transaction handle only when the first is done. */
	if (port->ct_gath)
		return EBUSY;

	err = stun_request(&port->ct_gath, gen->stun, IPPROTO_UDP, port->us,
			   &srv->addr, 0, STUN_METHOD_BINDING, NULL, 0, false,
			   srflx_resp_handler, port, 1,
			   STUN_ATTR_SOFTWARE, "EchoSDK echo-ice");
	if (err)
		return err;

	gen->gather_pending++;
	return 0;
}


/* ── Relay ───────────────────────────────────────────────────────────────── */

static void turnc_handler(int err, uint16_t scode, const char *reason,
			  const struct sa *relay_addr,
			  const struct sa *mapped_addr,
			  const struct stun_msg *msg, void *arg)
{
	struct icea_port *port = arg;
	struct icea_gen *gen = port->gen;
	struct icea_cand *host, *relay;
	(void)msg;

	if (gen->gather_pending)
		gen->gather_pending--;

	if (err || scode) {
		ICEA_DEBUG(gen, "ice[gen %u]: TURN allocation on %J failed"
			   " (%m %u %s)\n", gen->seq, &port->laddr, err, scode,
			   reason ? reason : "");
		port->turnc = mem_deref(port->turnc);
		goto out;
	}

	host = icea_lcand_on_port(gen, port->compid, port);
	relay = icea_cand_add_local(gen, ICE_CAND_TYPE_RELAY, port->compid,
				    port, host, relay_addr,
				    icea_cand_lpref(port));
	if (relay && mapped_addr && host) {
		relay->rel = *mapped_addr;
		/* the Allocate response doubles as a Binding response */
		(void)icea_cand_add_local(gen, ICE_CAND_TYPE_SRFLX, port->compid,
					  port, host, mapped_addr,
					  icea_cand_lpref(port));
	}

	ICEA_DEBUG(gen, "ice[gen %u]: relay {%u} %J via %J\n", gen->seq,
		   port->compid, relay_addr, &port->laddr);

 out:
	port->gathered = true;
	gather_complete_maybe(gen);
}


static int gather_relay(struct icea_gen *gen, struct icea_port *base_port,
			const struct icea_server *srv,
			uint16_t port_min, uint16_t port_max)
{
	struct icea_port *port = NULL;
	struct stun_conf conf = {
		.rto = gen->conf->stun_rto_ms, .rc = gen->conf->stun_rc,
		.rm = STUN_DEFAULT_RM, .ti = STUN_DEFAULT_TI, .tos = 0,
	};
	int err;

	if (srv->proto != IPPROTO_UDP) {
		/* TURN over TCP/TLS: PR4 (needs a tcp_conn / tls_conn
		 * transport under turnc, as baresip's turn module does). */
		return ENOTSUP;
	}

	/* A socket of its own on the same interface (see the file header). */
	err = icea_port_alloc(gen, base_port->compid, &base_port->laddr,
			      base_port->ifname, base_port->cost,
			      port_min, port_max, &port);
	if (err)
		return err;

	err = turnc_alloc(&port->turnc, &conf, IPPROTO_UDP, port->us,
			  ICEA_LAYER_TURN, &srv->addr, srv->user, srv->pass,
			  TURN_DEFAULT_LIFETIME, turnc_handler, port);
	if (err) {
		mem_deref(port);
		return err;
	}

	/* Host candidate on the relay port too: it is a real path. */
	(void)icea_cand_add_local(gen, ICE_CAND_TYPE_HOST, port->compid, port,
				  NULL, &port->laddr, icea_cand_lpref(port));

	gen->gather_pending++;
	return 0;
}


/* ── Entry ───────────────────────────────────────────────────────────────── */

int icea_gather_start(struct icea_gen *gen, const struct list *serverl,
		      uint16_t port_min, uint16_t port_max)
{
	struct le *ple, *sle;
	unsigned nports = 0;

	if (!gen)
		return EINVAL;

	gen->gathered = false;
	if (gen->state == ICEA_STATE_NEW)
		icea_gen_set_state(gen, ICEA_STATE_GATHERING);

	/* Host candidates for every port that is not a relay port.  A port
	 * carried over from the previous generation with its mapping known
	 * offers that reflexive address at once and is not asked again. */
	LIST_FOREACH(&gen->portl, ple) {
		struct icea_port *port = ple->data;
		struct icea_cand *host;

		if (port->turnc)
			continue;
		nports++;
		host = icea_cand_add_local(gen, ICE_CAND_TYPE_HOST, port->compid,
					   port, NULL, &port->laddr,
					   icea_cand_lpref(port));
		if (port->gathered && host && sa_isset(&port->srflx, SA_ADDR))
			(void)icea_cand_add_local(gen, ICE_CAND_TYPE_SRFLX,
						  port->compid, port, host,
						  &port->srflx,
						  icea_cand_lpref(port));
	}

	/* Reflexive and relayed, per (port × server). */
	LIST_FOREACH(&gen->portl, ple) {
		struct icea_port *port = ple->data;

		if (port->turnc || !serverl || port->gathered)
			continue;

		LIST_FOREACH(serverl, sle) {
			const struct icea_server *srv = sle->data;
			int err;

			if (sa_af(&srv->addr) != sa_af(&port->laddr))
				continue;

			if (srv->turn)
				err = gather_relay(gen, port, srv, port_min,
						   port_max);
			else
				err = gather_srflx(gen, port, srv);

			if (err && err != EBUSY)
				ICEA_DEBUG(gen, "ice[gen %u]: gather via %J:"
					   " %m\n", gen->seq, &srv->addr, err);
		}
	}

	if (!nports)
		return ENOENT;

	gather_complete_maybe(gen);
	return 0;
}


bool icea_gather_done(const struct icea_gen *gen)
{
	return gen && gen->gathered;
}


void icea_gather_port_concluded(struct icea_gen *gen)
{
	if (gen)
		gather_complete_maybe(gen);
}
