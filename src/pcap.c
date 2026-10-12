/**
 * @file pcap.c  pcap file writer (LINKTYPE_RAW=101)
 *
 * Writes a pcap file with synthetic IP/UDP framing for each SIP message.
 * Produces files readable by Wireshark and tcpdump for SIP analysis.
 *
 * pcap global header: magic 0xa1b2c3d4, version 2.4, link_type=RAW(101)
 * Per-packet: pcap_pkthdr + IPv4 header (proto=UDP) + UDP header + SIP payload
 *
 * All writes are serialized under g_bsdk.pcap_lock (called from re_main thread
 * only in practice, but the lock guards against medut_pcap_start/stop races).
 */

#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* Windows implementation of gettimeofday */
static int gettimeofday(struct timeval *tv, void *tz)
{
	FILETIME ft;
	uint64_t tmpres = 0;

	GetSystemTimeAsFileTime(&ft);

	tmpres |= ft.dwHighDateTime;
	tmpres <<= 32;
	tmpres |= ft.dwLowDateTime;

	/* Convert file time to unix epoch */
	tmpres /= 10;  /* Convert to microseconds */
	tmpres -= 11644473600000000ULL; /* Convert from 1601 to 1970 epoch */

	tv->tv_sec = (long)(tmpres / 1000000UL);
	tv->tv_usec = (long)(tmpres % 1000000UL);

	return 0;
}
#else
#include <sys/time.h>
#include <arpa/inet.h>
#endif
#include "medut_internal.h"

/* Packing must apply ONLY to the on-wire pcap/IP/UDP/TCP structs below.
 * Pushing pack(1) before medut_internal.h would change the layout of
 * struct bsdk_ctx in this TU only, breaking access to g_bsdk fields. */
#ifdef _WIN32
#pragma pack(push, 1)
#define PACKED_STRUCT struct
#else
#define PACKED_STRUCT struct __attribute__((packed))
#endif

/* PCAP format constants */
#define PCAP_MAGIC        0xa1b2c3d4
#define PCAP_VERSION_MAJ  2
#define PCAP_VERSION_MIN  4
#define PCAP_LINKTYPE_RAW 101

/* IP constants */
#define IP_HDR_LEN     20
#define IP_PROTO_UDP   17
#define IP_PROTO_TCP   6

/* Transport layer header sizes */
#define UDP_HDR_LEN    8
#define TCP_HDR_LEN    20

typedef PACKED_STRUCT {
	uint32_t magic;
	uint16_t version_major;
	uint16_t version_minor;
	int32_t  thiszone;
	uint32_t sigfigs;
	uint32_t snaplen;
	uint32_t network;
} pcap_global_hdr_t;

typedef PACKED_STRUCT {
	uint32_t ts_sec;
	uint32_t ts_usec;
	uint32_t incl_len;
	uint32_t orig_len;
} pcap_pkt_hdr_t;

typedef PACKED_STRUCT {
	uint8_t  ihl_ver;
	uint8_t  tos;
	uint16_t total_len;
	uint16_t id;
	uint16_t frag_off;
	uint8_t  ttl;
	uint8_t  proto;
	uint16_t checksum;
	uint8_t  src[4];
	uint8_t  dst[4];
} ip_hdr_t;

typedef PACKED_STRUCT {
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t length;
	uint16_t checksum;
} udp_hdr_t;

typedef PACKED_STRUCT {
	uint16_t src_port;
	uint16_t dst_port;
	uint32_t seq;
	uint32_t ack;
	uint16_t data_offset_flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgent_ptr;
} tcp_hdr_t;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void write_global_header(FILE *f)
{
	pcap_global_hdr_t h = {
		.magic         = PCAP_MAGIC,
		.version_major = PCAP_VERSION_MAJ,
		.version_minor = PCAP_VERSION_MIN,
		.thiszone      = 0,
		.sigfigs       = 0,
		.snaplen       = 65535,
		.network       = PCAP_LINKTYPE_RAW,
	};
	fwrite(&h, sizeof(h), 1, f);
}

static void sa_to_bytes(const struct sa *addr, uint8_t out[4], uint16_t *port)
{
	if (!addr || !sa_isset(addr, SA_ADDR)) {
		memset(out, 0, 4);
		*port = 0;
		return;
	}
	/* sa_in() returns host-byte-order IPv4; we need network byte order */
	uint32_t ip_nbo = htonl(sa_in(addr));
	memcpy(out, &ip_nbo, 4);
	*port = sa_port(addr);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int bsdk_pcap_open(const char *path)
{
	if (!path)
		return 0;

	mtx_lock(&g_bsdk.pcap_lock);
	if (g_bsdk.pcap_file) {
		mtx_unlock(&g_bsdk.pcap_lock);
		return EALREADY;
	}
	g_bsdk.pcap_file = fopen(path, "wb");
	if (!g_bsdk.pcap_file) {
		mtx_unlock(&g_bsdk.pcap_lock);
		return errno ? errno : EIO;
	}
	write_global_header(g_bsdk.pcap_file);
	fflush(g_bsdk.pcap_file);
	mtx_unlock(&g_bsdk.pcap_lock);
	return 0;
}

void bsdk_pcap_close(void)
{
	{
		const uint64_t *p = (const uint64_t*)&g_bsdk.pcap_lock;
		BSDK_TRACE("[bsdk] pcap_close: addr=%p bytes=%016llx %016llx %016llx %016llx %016llx\n",
		       (void*)p, p[0], p[1], p[2], p[3], p[4]);
	}
	mtx_lock(&g_bsdk.pcap_lock);
	BSDK_TRACE("[bsdk] pcap_close: locked\n");
	if (g_bsdk.pcap_file) {
		fclose(g_bsdk.pcap_file);
		g_bsdk.pcap_file = NULL;
	}
	mtx_unlock(&g_bsdk.pcap_lock);
	BSDK_TRACE("[bsdk] pcap_close: done\n");
}

void bsdk_pcap_write_sip(const char *data, size_t len,
                           const struct sa *src, const struct sa *dst,
                           bool is_udp)
{
	if (!data || !len)
		return;

	mtx_lock(&g_bsdk.pcap_lock);
	if (!g_bsdk.pcap_file) {
		mtx_unlock(&g_bsdk.pcap_lock);
		return;
	}

	uint8_t src_ip[4], dst_ip[4];
	uint16_t src_port, dst_port;
	sa_to_bytes(src, src_ip, &src_port);
	sa_to_bytes(dst, dst_ip, &dst_port);
	if (!src_port) src_port = 5060;
	if (!dst_port) dst_port = 5060;

	struct timeval tv;
	gettimeofday(&tv, NULL);

	uint32_t pkt_len;
	uint8_t ip_proto;
	uint16_t l4_total;

	if (is_udp) {
		ip_proto = IP_PROTO_UDP;
		uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + len);
		l4_total = udp_len;
		pkt_len  = IP_HDR_LEN + udp_len;

		pcap_pkt_hdr_t ph = {
			.ts_sec   = (uint32_t)tv.tv_sec,
			.ts_usec  = (uint32_t)tv.tv_usec,
			.incl_len = pkt_len,
			.orig_len = pkt_len,
		};

		ip_hdr_t ip = {
			.ihl_ver   = 0x45,
			.tos       = 0,
			.total_len = htons((uint16_t)(IP_HDR_LEN + l4_total)),
			.id        = 0,
			.frag_off  = 0,
			.ttl       = 64,
			.proto     = ip_proto,
			.checksum  = 0,
		};
		memcpy(ip.src, src_ip, 4);
		memcpy(ip.dst, dst_ip, 4);

		udp_hdr_t udp = {
			.src_port = htons(src_port),
			.dst_port = htons(dst_port),
			.length   = htons(udp_len),
			.checksum = 0,
		};

		fwrite(&ph,  sizeof(ph),  1, g_bsdk.pcap_file);
		fwrite(&ip,  sizeof(ip),  1, g_bsdk.pcap_file);
		fwrite(&udp, sizeof(udp), 1, g_bsdk.pcap_file);
		fwrite(data, 1, len,         g_bsdk.pcap_file);
	} else {
		ip_proto = IP_PROTO_TCP;
		l4_total = (uint16_t)(TCP_HDR_LEN + len);
		pkt_len  = IP_HDR_LEN + l4_total;

		pcap_pkt_hdr_t ph = {
			.ts_sec   = (uint32_t)tv.tv_sec,
			.ts_usec  = (uint32_t)tv.tv_usec,
			.incl_len = pkt_len,
			.orig_len = pkt_len,
		};

		ip_hdr_t ip = {
			.ihl_ver   = 0x45,
			.tos       = 0,
			.total_len = htons((uint16_t)(IP_HDR_LEN + l4_total)),
			.id        = 0,
			.frag_off  = 0,
			.ttl       = 64,
			.proto     = ip_proto,
			.checksum  = 0,
		};
		memcpy(ip.src, src_ip, 4);
		memcpy(ip.dst, dst_ip, 4);

		tcp_hdr_t tcp = {
			.src_port          = htons(src_port),
			.dst_port          = htons(dst_port),
			.seq               = 0,
			.ack               = 0,
			.data_offset_flags = htons(0x5010),
			.window            = htons(65535),
			.checksum          = 0,
			.urgent_ptr        = 0,
		};

		fwrite(&ph,  sizeof(ph),  1, g_bsdk.pcap_file);
		fwrite(&ip,  sizeof(ip),  1, g_bsdk.pcap_file);
		fwrite(&tcp, sizeof(tcp), 1, g_bsdk.pcap_file);
		fwrite(data, 1, len,          g_bsdk.pcap_file);
	}

	fflush(g_bsdk.pcap_file);
	mtx_unlock(&g_bsdk.pcap_lock);
}

/* ── Public control API ──────────────────────────────────────────────────── */

int medut_pcap_start(const char *path)
{
	if (!path) return MEDUT_ERR_INVAL;
	return bsdk_pcap_open(path);
}

int medut_pcap_stop(void)
{
	bsdk_pcap_close();
	return MEDUT_OK;
}

#ifdef _WIN32
#pragma pack(pop)
#endif
