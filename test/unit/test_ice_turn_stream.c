/**
 * @file test_ice_turn_stream.c  medut-ice: TURN over TCP/TLS stream framing
 *
 * icea_turn_stream_frames() reassembles RFC 5766 §11.5 framing off a byte
 * stream: STUN messages and ChannelData frames back to back, ChannelData
 * padded to four bytes.  The stream is one buffer split at every offset
 * the network could split it at, which is where the padding-boundary bug
 * lived: the frame was consumed while its padding was still in flight.
 *
 * Build: make -C test/unit test_ice_turn_stream && ./test/unit/test_ice_turn_stream
 */

#include <stdio.h>
#include <string.h>
#include <re.h>
#include "../../src/ice/ice_internal.h"

static int g_pass, g_fail;

#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)


/* What the handler saw, in order. */
#define MAX_FRAMES 16
struct seen {
	unsigned n;
	uint16_t typ[MAX_FRAMES];
	size_t   len[MAX_FRAMES];     /* bytes handed over, padding excluded */
	uint8_t  first[MAX_FRAMES];   /* first payload byte, 0 when empty    */
	int      ret;                 /* what frame_h returns                */
};


static int frame_h(struct mbuf *mb, void *arg)
{
	struct seen *s = arg;
	size_t pos = mb->pos;
	uint16_t typ;

	if (s->n >= MAX_FRAMES)
		return ENOSPC;

	typ = ntohs(mbuf_read_u16(mb));
	(void)mbuf_read_u16(mb);

	s->typ[s->n]   = typ;
	s->len[s->n]   = mb->end - pos;
	s->first[s->n] = mbuf_get_left(mb) ? mbuf_buf(mb)[0] : 0;
	s->n++;

	mb->pos = pos;
	return s->ret;
}


static void push_chan(struct mbuf *mb, uint16_t nr, const uint8_t *pld,
		      size_t len)
{
	mbuf_write_u16(mb, htons(nr));
	mbuf_write_u16(mb, htons((uint16_t)len));
	mbuf_write_mem(mb, pld, len);
	while (mb->end & 0x03)
		mbuf_write_u8(mb, 0);
}


/* A STUN Data indication with no attributes: header only, length 0. */
static void push_stun(struct mbuf *mb)
{
	static const uint8_t tid[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 };

	mbuf_write_u16(mb, htons(0x0017));            /* indication, Data   */
	mbuf_write_u16(mb, htons(0));
	mbuf_write_u32(mb, htonl(0x2112a442));         /* magic cookie      */
	mbuf_write_mem(mb, tid, sizeof(tid));
}


/* Feed `stream` to the parser in chunks of `chunk` bytes. */
static int feed(const struct mbuf *stream, size_t chunk, struct seen *s)
{
	struct mbuf *carry = NULL;
	size_t off = 0;
	int err = 0;

	while (off < stream->end) {
		size_t n = min(chunk, stream->end - off);
		struct mbuf *pkt = mbuf_alloc(n);

		mbuf_write_mem(pkt, stream->buf + off, n);
		pkt->pos = 0;

		err = icea_turn_stream_frames(&carry, pkt, frame_h, s);
		mem_deref(pkt);
		if (err)
			break;
		off += n;
	}

	mem_deref(carry);
	return err;
}


int main(void)
{
	static const uint8_t p3[3]  = { 0xa1, 0xa2, 0xa3 };   /* pads by 1 */
	static const uint8_t p5[5]  = { 0xb1, 0xb2, 0xb3, 0xb4, 0xb5 }; /* 3 */
	static const uint8_t p4[4]  = { 0xc1, 0xc2, 0xc3, 0xc4 };   /* none */
	struct mbuf *stream = mbuf_alloc(256);
	struct seen s;
	size_t chunk;

	libre_init();

	/* padded, padded, unpadded, then a STUN message behind them */
	push_chan(stream, 0x4001, p3, sizeof(p3));
	push_chan(stream, 0x4002, p5, sizeof(p5));
	push_chan(stream, 0x4003, p4, sizeof(p4));
	push_stun(stream);
	stream->pos = 0;

	/* Every split the network could hand us, one byte at a time
	 * included: each must yield the same four frames.  A split that
	 * lands between a frame and its padding is the regression. */
	for (chunk = 1; chunk <= stream->end; chunk++) {
		memset(&s, 0, sizeof(s));

		CHECK(0 == feed(stream, chunk, &s));
		CHECK(4 == s.n);
		if (s.n != 4) {
			fprintf(stderr, "  (chunk size %zu)\n", chunk);
			continue;
		}
		CHECK(s.typ[0] == 0x4001 && s.len[0] == 4 + 3 &&
		      s.first[0] == 0xa1);
		CHECK(s.typ[1] == 0x4002 && s.len[1] == 4 + 5 &&
		      s.first[1] == 0xb1);
		CHECK(s.typ[2] == 0x4003 && s.len[2] == 4 + 4 &&
		      s.first[2] == 0xc1);
		CHECK(s.typ[3] == 0x0017 && s.len[3] == STUN_HEADER_SIZE);
	}

	/* A frame that never completes yields nothing and no error: the
	 * parser holds it until the rest arrives. */
	{
		struct mbuf *pkt = mbuf_alloc(8);
		struct mbuf *carry = NULL;

		memset(&s, 0, sizeof(s));
		mbuf_write_mem(pkt, stream->buf, 6);   /* half of frame one */
		pkt->pos = 0;
		CHECK(0 == icea_turn_stream_frames(&carry, pkt, frame_h, &s));
		CHECK(0 == s.n);
		CHECK(carry != NULL);
		mem_deref(carry);
		mem_deref(pkt);
	}

	/* 0x8000 and above is neither STUN nor ChannelData. */
	{
		struct mbuf *pkt = mbuf_alloc(8);
		struct mbuf *carry = NULL;

		memset(&s, 0, sizeof(s));
		mbuf_write_u16(pkt, htons(0x8001));
		mbuf_write_u16(pkt, htons(0));
		pkt->pos = 0;
		CHECK(EBADMSG == icea_turn_stream_frames(&carry, pkt, frame_h,
							 &s));
		mem_deref(carry);
		mem_deref(pkt);
	}

	/* The handler stops the loop, and the error reaches the caller. */
	{
		memset(&s, 0, sizeof(s));
		s.ret = EPROTO;
		CHECK(EPROTO == feed(stream, stream->end, &s));
		CHECK(1 == s.n);
	}

	mem_deref(stream);
	libre_close();

	printf("test_ice_turn_stream: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
