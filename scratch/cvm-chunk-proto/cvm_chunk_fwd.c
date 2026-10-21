/* SPDX-License-Identifier: BSD-3-Clause */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include "cvm_chunk_receiver.h"
#include "cvm_mbuf_bridge.h"
#include "testpmd.h"

#define CVM_CHUNK_UDP_PORT 9000U
#define CVM_CHUNK_DEFAULT_MAX_OBJECT (UINT64_C(64) * 1024 * 1024)
#define CVM_CHUNK_HARD_MAX_OBJECT (UINT64_C(1) * 1024 * 1024 * 1024)

struct cvm_chunk_endpoint {
	struct rte_ether_addr peer_mac;
	struct rte_ether_addr local_mac;
	uint32_t peer_ipv4;
	uint32_t local_ipv4;
	uint16_t peer_udp_port;
};

struct cvm_chunk_fwd_state {
	struct cvm_chunk_receiver receiver;
	struct cvm_chunk_endpoint endpoint;
	uint64_t accepted;
	uint64_t duplicates;
	uint64_t rejected;
	uint64_t acks;
	bool started;
	bool completion_reported;
};

static struct cvm_chunk_fwd_state cvm_state;

static int
parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	if (text == NULL || text[0] == '\0')
		return -EINVAL;
	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno != 0 || *end != '\0')
		return -EINVAL;
	*value = (uint64_t)parsed;
	return 0;
}

static int
read_endpoint(portid_t port, struct cvm_chunk_endpoint *endpoint)
{
	const char *peer_ip = getenv("CVM_CHUNK_PEER_IPV4");
	const char *local_ip = getenv("CVM_CHUNK_LOCAL_IPV4");
	const char *peer_mac = getenv("CVM_CHUNK_PEER_MAC");
	const char *peer_port = getenv("CVM_CHUNK_PEER_UDP_PORT");
	uint64_t parsed_port;

	if (peer_ip == NULL || local_ip == NULL || peer_mac == NULL ||
	    peer_port == NULL ||
	    inet_pton(AF_INET, peer_ip, &endpoint->peer_ipv4) != 1 ||
	    inet_pton(AF_INET, local_ip, &endpoint->local_ipv4) != 1 ||
	    rte_ether_unformat_addr(peer_mac, &endpoint->peer_mac) != 0 ||
	    parse_u64(peer_port, &parsed_port) != 0 || parsed_port == 0 ||
	    parsed_port > UINT16_MAX ||
	    rte_eth_macaddr_get(port, &endpoint->local_mac) != 0)
		return -EINVAL;
	endpoint->peer_udp_port = (uint16_t)parsed_port;
	return 0;
}

static int
cvm_chunk_begin(portid_t port)
{
	const char *key_hex = getenv("CVM_CHUNK_KEY_HEX");
	const char *max_text = getenv("CVM_CHUNK_MAX_OBJECT");
	uint64_t max_object = CVM_CHUNK_DEFAULT_MAX_OBJECT;
	uint8_t key[CVM_CHUNK_KEY_SIZE];
	int ret;

	if (cvm_state.started)
		return -EALREADY;
	if (!cvm_mbuf_bridge_enabled() || cur_fwd_config.nb_fwd_ports != 1 ||
	    cur_fwd_config.nb_fwd_streams != 1 ||
	    cur_fwd_config.nb_fwd_lcores != 1 || nb_rxq != 1 || nb_txq != 1) {
		fprintf(stderr, "cvmchunk requires the CVM bridge and exactly one "
			"port, RX queue, TX queue, stream, and forwarding lcore\n");
		return -EINVAL;
	}
	if (max_text != NULL && parse_u64(max_text, &max_object) != 0)
		return -EINVAL;
	if (max_object == 0 || max_object > CVM_CHUNK_HARD_MAX_OBJECT) {
		fprintf(stderr, "CVM_CHUNK_MAX_OBJECT must be in [1, 1 GiB]\n");
		return -EINVAL;
	}
	if (cvm_chunk_key_parse(key, key_hex) != 0) {
		fprintf(stderr, "CVM_CHUNK_KEY_HEX must contain 64 hex digits\n");
		return -EINVAL;
	}
	memset(&cvm_state, 0, sizeof(cvm_state));
	ret = read_endpoint(port, &cvm_state.endpoint);
	if (ret == 0)
		ret = cvm_chunk_receiver_init(&cvm_state.receiver, key,
					      max_object);
	OPENSSL_cleanse(key, sizeof(key));
	if (ret != 0) {
		fprintf(stderr, "invalid CVM chunk endpoint configuration\n");
		return ret;
	}
	cvm_state.started = true;
	printf("cvmchunk: private authenticated receiver ready; max_object=%" PRIu64
	       " bytes\n", max_object);
	return 0;
}

static void
cvm_chunk_end(portid_t port __rte_unused)
{
	if (!cvm_state.started)
		return;
	printf("cvmchunk: accepted=%" PRIu64 " duplicates=%" PRIu64
	       " rejected=%" PRIu64 " acks=%" PRIu64 "\n",
	       cvm_state.accepted, cvm_state.duplicates, cvm_state.rejected,
	       cvm_state.acks);
	cvm_chunk_receiver_destroy(&cvm_state.receiver);
	memset(&cvm_state, 0, sizeof(cvm_state));
}

static int
parse_data_packet(const struct rte_mbuf *m, uint8_t frame[RTE_ETHER_MAX_LEN],
		const uint8_t **payload, size_t *payload_len)
{
	struct rte_ether_hdr eth;
	struct rte_ipv4_hdr ip;
	struct rte_udp_hdr udp;
	const uint8_t *data;
	uint16_t ip_len;
	uint16_t udp_len;
	uint16_t fragment;
	size_t udp_offset = sizeof(eth) + sizeof(ip);

	if (m->pkt_len < udp_offset + sizeof(udp) ||
	    m->pkt_len > RTE_ETHER_MAX_LEN)
		return -EMSGSIZE;
	data = rte_pktmbuf_read(m, 0, m->pkt_len, frame);
	if (data == NULL)
		return -EMSGSIZE;
	memcpy(&eth, data, sizeof(eth));
	memcpy(&ip, data + sizeof(eth), sizeof(ip));
	memcpy(&udp, data + udp_offset, sizeof(udp));
	if (eth.ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
	    !rte_is_same_ether_addr(&eth.src_addr,
				    &cvm_state.endpoint.peer_mac) ||
	    !rte_is_same_ether_addr(&eth.dst_addr,
				    &cvm_state.endpoint.local_mac) ||
	    ip.version_ihl != RTE_IPV4_VHL_DEF || ip.next_proto_id != IPPROTO_UDP ||
	    ip.src_addr != cvm_state.endpoint.peer_ipv4 ||
	    ip.dst_addr != cvm_state.endpoint.local_ipv4 ||
	    udp.src_port != rte_cpu_to_be_16(cvm_state.endpoint.peer_udp_port) ||
	    udp.dst_port != rte_cpu_to_be_16(CVM_CHUNK_UDP_PORT))
		return -EPROTO;
	fragment = rte_be_to_cpu_16(ip.fragment_offset);
	if ((fragment & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0)
		return -EPROTO;
	ip_len = rte_be_to_cpu_16(ip.total_length);
	udp_len = rte_be_to_cpu_16(udp.dgram_len);
	if (ip_len < sizeof(ip) + sizeof(udp) ||
	    udp_len != ip_len - sizeof(ip) ||
	    m->pkt_len < sizeof(eth) + ip_len)
		return -EMSGSIZE;
	*payload = data + udp_offset + sizeof(udp);
	*payload_len = udp_len - sizeof(udp);
	return 0;
}

static struct rte_mbuf *
make_ack(struct rte_mempool *pool)
{
	uint8_t ack[CVM_CHUNK_MAX_UDP_PAYLOAD];
	struct rte_ether_hdr eth = {0};
	struct rte_ipv4_hdr ip = {0};
	struct rte_udp_hdr udp = {0};
	struct rte_mbuf *m;
	uint8_t *data;
	size_t ack_len;
	size_t packet_len;
	int ret;

	ret = cvm_chunk_receiver_build_ack(&cvm_state.receiver, ack,
					   sizeof(ack), &ack_len);
	if (ret != 0)
		return NULL;
	packet_len = sizeof(eth) + sizeof(ip) + sizeof(udp) + ack_len;
	m = rte_pktmbuf_alloc(pool);
	if (m == NULL)
		return NULL;
	data = (uint8_t *)rte_pktmbuf_append(m, (uint16_t)packet_len);
	if (data == NULL) {
		rte_pktmbuf_free(m);
		return NULL;
	}
	eth.src_addr = cvm_state.endpoint.local_mac;
	eth.dst_addr = cvm_state.endpoint.peer_mac;
	eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	ip.version_ihl = RTE_IPV4_VHL_DEF;
	ip.total_length = rte_cpu_to_be_16((uint16_t)(sizeof(ip) + sizeof(udp) +
						       ack_len));
	ip.time_to_live = 64;
	ip.next_proto_id = IPPROTO_UDP;
	ip.src_addr = cvm_state.endpoint.local_ipv4;
	ip.dst_addr = cvm_state.endpoint.peer_ipv4;
	ip.hdr_checksum = rte_ipv4_cksum(&ip);
	udp.src_port = rte_cpu_to_be_16(CVM_CHUNK_UDP_PORT);
	udp.dst_port = rte_cpu_to_be_16(cvm_state.endpoint.peer_udp_port);
	udp.dgram_len = rte_cpu_to_be_16((uint16_t)(sizeof(udp) + ack_len));
	/* A zero UDP checksum is valid for IPv4; AEAD protects the payload. */
	memcpy(data, &eth, sizeof(eth));
	memcpy(data + sizeof(eth), &ip, sizeof(ip));
	memcpy(data + sizeof(eth) + sizeof(ip), &udp, sizeof(udp));
	memcpy(data + sizeof(eth) + sizeof(ip) + sizeof(udp), ack, ack_len);
	OPENSSL_cleanse(ack, sizeof(ack));
	return m;
}

static void
report_complete(void)
{
	uint8_t digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	unsigned int i;

	if (cvm_state.completion_reported)
		return;
	if (EVP_Digest(cvm_state.receiver.object,
		       (size_t)cvm_state.receiver.object_len, digest, &digest_len,
		       EVP_sha256(), NULL) != 1)
		return;
	printf("cvmchunk_complete=PASS object_id=%016" PRIx64
	       " bytes=%" PRIu64 " sha256=",
	       cvm_state.receiver.object_id, cvm_state.receiver.object_len);
	for (i = 0; i < digest_len; i++)
		printf("%02x", digest[i]);
	printf("\n");
	OPENSSL_cleanse(digest, sizeof(digest));
	cvm_state.completion_reported = true;
}

static bool
pkt_burst_cvm_chunk(struct fwd_stream *fs)
{
	struct rte_mbuf *rx[MAX_PKT_BURST];
	struct rte_mbuf *tx[MAX_PKT_BURST];
	uint16_t nb_rx;
	uint16_t nb_tx = 0;
	uint16_t i;

	nb_rx = common_fwd_stream_receive(fs, rx, nb_pkt_per_burst);
	if (unlikely(nb_rx == 0))
		return false;
	for (i = 0; i < nb_rx; i++) {
		uint8_t frame[RTE_ETHER_MAX_LEN];
		const uint8_t *payload;
		size_t payload_len;
		enum cvm_chunk_accept_result result;
		struct rte_mbuf *ack;
		int ret;

		ret = parse_data_packet(rx[i], frame, &payload, &payload_len);
		if (ret == 0)
			ret = cvm_chunk_receiver_accept(&cvm_state.receiver, payload,
						payload_len, &result);
		if (ret != 0) {
			cvm_state.rejected++;
			fs->fwd_dropped++;
			rte_pktmbuf_free(rx[i]);
			continue;
		}
		if (result == CVM_CHUNK_ACCEPT_DUPLICATE)
			cvm_state.duplicates++;
		else
			cvm_state.accepted++;
		if (result == CVM_CHUNK_ACCEPT_COMPLETE)
			report_complete();
		ack = make_ack(rx[i]->pool);
		rte_pktmbuf_free(rx[i]);
		if (ack == NULL) {
			fs->fwd_dropped++;
			continue;
		}
		tx[nb_tx++] = ack;
		cvm_state.acks++;
	}
	if (nb_tx != 0)
		common_fwd_stream_transmit(fs, tx, nb_tx);
	return true;
}

struct fwd_engine cvm_chunk_engine = {
	.fwd_mode_name = "cvmchunk",
	.port_fwd_begin = cvm_chunk_begin,
	.port_fwd_end = cvm_chunk_end,
	.stream_init = common_fwd_stream_init,
	.packet_fwd = pkt_burst_cvm_chunk,
};
