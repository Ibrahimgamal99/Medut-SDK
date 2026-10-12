/**
 * @file ice_gather.c  medut-ice: candidate gathering
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
	mem_deref(srv->host);
	mem_deref(srv->user);
	mem_deref(srv->pass);
}


int icea_server_alloc(struct icea_server **srvp, bool turn, int proto,
		      bool tls, const struct sa *addr, const char *host,
		      const char *user, const char *pass)
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
	if (host)
		err |= str_dup(&srv->host, host);
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
		if (port->primary && port->gathered && !port->turnc &&
		    !port->is_virtual)
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
			   STUN_ATTR_SOFTWARE, "Medut medut-ice");
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
		if (port->is_virtual) {
			port->tlsc = mem_deref(port->tlsc);
			port->tc   = mem_deref(port->tc);
		}
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


/* ── Relay over TCP / TLS ──────────────────────────────────────────────────
 *
 * The TURN client talks to the server over a byte stream; what the peer
 * sends us arrives as Data indications or ChannelData frames on that
 * stream and is handed to the port's normal receive path, and what we
 * send on the port goes back out through turnc_send().  This is the relay
 * that still works where UDP is blocked or shaped, which is the case for
 * a TURN server on tcp/443 and most hostile networks. */

static void relay_tcp_concluded(struct icea_port *port)
{
	struct icea_gen *gen = port->gen;

	if (port->gathered)
		return;
	port->gathered = true;
	if (gen->gather_pending)
		gen->gather_pending--;
	gather_complete_maybe(gen);
}


static int relay_tcp_send_h(const struct sa *dst, struct mbuf *mb,
			    void *arg)
{
	struct icea_port *port = arg;

	return port->turnc ? turnc_send(port->turnc, dst, mb) : ENOTCONN;
}


static void relay_tcp_estab_h(void *arg)
{
	struct icea_port *port = arg;
	struct icea_gen *gen = port->gen;
	struct stun_conf conf = {
		.rto = gen->conf->stun_rto_ms, .rc = gen->conf->stun_rc,
		.rm = STUN_DEFAULT_RM, .ti = STUN_DEFAULT_TI, .tos = 0,
	};
	int err;

	ICEA_DEBUG(gen, "ice[gen %u]: %s to TURN server %J established\n",
		   gen->seq, port->srv_tls ? "TLS" : "TCP", &port->srv_addr);

	err = turnc_alloc(&port->turnc, &conf, IPPROTO_TCP, port->tc, 0,
			  &port->srv_addr, port->srv_user, port->srv_pass,
			  TURN_DEFAULT_LIFETIME, turnc_handler, port);
	if (err) {
		ICEA_DEBUG(gen, "ice[gen %u]: TURN client over TCP failed"
			   " (%m)\n", gen->seq, err);
		relay_tcp_concluded(port);
	}
}


static void relay_tcp_close_h(int err, void *arg)
{
	struct icea_port *port = arg;
	struct icea_gen *gen = port->gen;

	ICEA_DEBUG(gen, "ice[gen %u]: %s to TURN server %J closed (%m)\n",
		   gen->seq, port->srv_tls ? "TLS" : "TCP", &port->srv_addr,
		   err);
	port->turnc = mem_deref(port->turnc);
	relay_tcp_concluded(port);
}


/* RFC 5766 §11.5: STUN messages and ChannelData frames back to back on
 * the stream, ChannelData padded to four bytes. */
int icea_turn_stream_frames(struct mbuf **carryp, struct mbuf *mb_pkt,
			    icea_turn_frame_h *frameh, void *arg)
{
	int err;

	if (!carryp || !mb_pkt || !frameh)
		return EINVAL;

	if (*carryp) {
		struct mbuf *carry = *carryp;
		size_t pos = carry->pos;

		carry->pos = carry->end;
		err = mbuf_write_mem(carry, mbuf_buf(mb_pkt),
				     mbuf_get_left(mb_pkt));
		carry->pos = pos;
		if (err)
			return err;
	}
	else {
		*carryp = mem_ref(mb_pkt);
	}

	for (;;) {
		struct mbuf *mb = *carryp;
		size_t len, framelen, pos, end;
		uint16_t typ;

		if (mbuf_get_left(mb) < 4)
			break;

		typ = ntohs(mbuf_read_u16(mb));
		len = ntohs(mbuf_read_u16(mb));
		mb->pos -= 4;

		if (typ < 0x4000) {
			/* STUN: the header length excludes the header and is
			 * always a multiple of four, so nothing is padded. */
			len += STUN_HEADER_SIZE;
			framelen = len;
		}
		else if (typ < 0x8000) {
			len += 4;
			/* The padding that rounds ChannelData up to four bytes
			 * is part of the frame on a stream, so the frame is
			 * not here until the padding is.  Consuming it early
			 * would resume the next read on those bytes — every
			 * later frame misparsed, and the relay closed. */
			framelen = (len + 3) & ~(size_t)3;
		}
		else {
			return EBADMSG;
		}

		if (mbuf_get_left(mb) < framelen)
			break;

		pos = mb->pos;
		end = mb->end;
		mb->end = pos + len;    /* the frame, without its padding */

		err = frameh(mb, arg);

		mb->pos = pos + framelen;
		mb->end = end;
		if (err)
			return err;

		if (mb->pos >= mb->end) {
			*carryp = mem_deref(*carryp);
			break;
		}
	}

	return 0;
}


static int relay_tcp_frame_h(struct mbuf *mb, void *arg)
{
	struct icea_port *port = arg;
	struct sa src;
	int err;

	if (!port->turnc)
		return 0;

	sa_init(&src, AF_UNSPEC);
	err = turnc_recv(port->turnc, &src, mb);
	if (err)
		return err;

	/* what the peer sent through the relay */
	if (mbuf_get_left(mb) && sa_isset(&src, SA_ADDR))
		udp_recv_packet(port->us, &src, mb);

	return 0;
}


static void relay_tcp_recv_h(struct mbuf *mb_pkt, void *arg)
{
	struct icea_port *port = arg;
	struct icea_gen *gen = port->gen;
	int err;

	err = icea_turn_stream_frames(&port->tcp_mb, mb_pkt,
				      relay_tcp_frame_h, port);
	if (err) {
		ICEA_DEBUG(gen, "ice[gen %u]: TURN stream from %J: %m\n",
			   gen->seq, &port->srv_addr, err);
		port->tcp_mb = mem_deref(port->tcp_mb);
		port->turnc = mem_deref(port->turnc);
		relay_tcp_concluded(port);
	}
}


/* The TLS context the generation's TURN connections share.  libre never
 * calls SSL_CTX_set_default_verify_paths(), so an empty store would make
 * every certificate unverifiable; conf.tls_ca_file is the same bundle SIP
 * is given (ice_mod.c), platform-resolved when the app named none. */
static int relay_tls_ctx(struct icea_gen *gen)
{
	struct tls *tls = NULL;
	int err;

	err = tls_alloc(&tls, TLS_METHOD_TLS, NULL, NULL);
	if (err)
		return err;

	if (!gen->conf->tls_verify_server)
		tls_disable_verify_server(tls);
	else if (!gen->conf->tls_ca_file)
		err = ENOENT;
	else
		err = tls_add_ca(tls, gen->conf->tls_ca_file);

	if (err)
		mem_deref(tls);
	else
		gen->tls = tls;
	return err;
}


static int gather_relay_tcp(struct icea_gen *gen, struct icea_port *base_port,
			    const struct icea_server *srv)
{
	struct icea_port *port = NULL;
	int err;

	err = icea_port_alloc_virtual(gen, base_port->compid, &base_port->laddr,
				      base_port->ifname, base_port->cost,
				      relay_tcp_send_h, &port);
	if (err)
		return err;

	port->srv_addr = srv->addr;
	port->srv_tls  = srv->tls;
	if (srv->user) err |= str_dup(&port->srv_user, srv->user);
	if (srv->pass) err |= str_dup(&port->srv_pass, srv->pass);
	if (err)
		goto fail;

	err = tcp_connect(&port->tc, &srv->addr, relay_tcp_estab_h,
			  relay_tcp_recv_h, relay_tcp_close_h, port);
	if (err)
		goto fail;

	if (srv->tls) {
		char addrbuf[64];
		const char *host = srv->host;

		/* The name checked is the one from the URI (srv->host).
		 * Falling back to the address verifies the chain but not the
		 * name: libre skips SNI and the name check for an IP
		 * literal. */
		if (!host) {
			(void)re_snprintf(addrbuf, sizeof(addrbuf), "%j",
					  &srv->addr);
			host = addrbuf;
		}

		if (!gen->tls) {
			err = relay_tls_ctx(gen);
			if (err) {
				ICEA_DEBUG(gen, "ice[gen %u]: TURN over TLS to"
					   " %s: no usable CA bundle (%m)\n",
					   gen->seq, host, err);
				goto fail;
			}
		}
		err = tls_start_tcp(&port->tlsc, gen->tls, port->tc, 0);
		if (err)
			goto fail;

		/* Without this the handshake checks nothing: libre arms
		 * SSL_VERIFY_PEER here, not in the context, and this is also
		 * what sends SNI — a TURN server behind a TLS load balancer
		 * needs the name to pick a certificate at all.  A failure
		 * fails this relay; the other servers still gather. */
		err = tls_set_verify_server(port->tlsc, host);
		if (err)
			goto fail;
	}

	gen->gather_pending++;
	ICEA_DEBUG(gen, "ice[gen %u]: TURN over %s to %J for {%u}\n", gen->seq,
		   srv->tls ? "TLS" : "TCP", &srv->addr, base_port->compid);
	return 0;

 fail:
	mem_deref(port);
	return err;
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

	if (srv->proto != IPPROTO_UDP)
		return gather_relay_tcp(gen, base_port, srv);

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

		if (port->turnc || port->is_virtual)
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

		if (port->turnc || port->is_virtual || !serverl || port->gathered)
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
