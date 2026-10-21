/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <rte_eth_ring.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_icmp.h>
#include <rte_gso.h>
#include "testpmd.h"
#include "cvm_mbuf_bridge.h"
#ifdef CVM_CHUNK_OPENSSL
#include "cvm_chunk_packet.h"
#endif

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	exit(1); } } while (0)
#define COUNT 255
#define ROOM 4096

static struct rte_mempool *dma, *cpu, *ordinary;
static struct rte_ring *rx, *tx;
static portid_t port;

static struct rte_mbuf *
packet(struct rte_mempool *pool, uint16_t len)
{
	struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip;
	struct rte_udp_hdr *udp;

	CHECK(m != NULL);
	eth = (void *)rte_pktmbuf_append(m, len);
	CHECK(eth != NULL);
	memset(eth, 0x5a, len);
	eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	ip = (void *)(eth + 1);
	memset(ip, 0, sizeof(*ip));
	ip->version_ihl = RTE_IPV4_VHL_DEF;
	ip->total_length = rte_cpu_to_be_16(len - sizeof(*eth));
	ip->next_proto_id = IPPROTO_UDP;
	ip->time_to_live = 64;
	ip->src_addr = rte_cpu_to_be_32(RTE_IPV4(10, 0, 0, 1));
	ip->dst_addr = rte_cpu_to_be_32(RTE_IPV4(10, 0, 0, 2));
	ip->hdr_checksum = rte_ipv4_cksum(ip);
	udp = (void *)(ip + 1);
	udp->src_port = rte_cpu_to_be_16(1000);
	udp->dst_port = rte_cpu_to_be_16(1001);
	udp->dgram_len = rte_cpu_to_be_16(len - sizeof(*eth) - sizeof(*ip));
	udp->dgram_cksum = 0;
	return m;
}

static void
balanced(const char *name)
{
	CHECK(rte_mempool_avail_count(dma) == COUNT);
	CHECK(rte_mempool_avail_count(cpu) == COUNT);
	CHECK(rte_mempool_avail_count(ordinary) == COUNT);
	printf("PASS bridge %s\n", name);
}

static void
copy_cases(void)
{
	struct rte_mbuf *m[2], *saved[2], *held[COUNT];
	uint8_t *shared, original;
	uint16_t dropped;
	unsigned int i;

	m[0] = packet(dma, 64);
	shared = rte_pktmbuf_mtod(m[0], uint8_t *);
	original = shared[63];
	m[0]->ol_flags |= RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_RSS_HASH;
	m[0]->hash.rss = 123;
	m[0]->packet_type = RTE_PTYPE_L3_IPV4;
	CHECK(cvm_mbuf_bridge_rx_copy(m, 1, &dropped) == 1 && dropped == 0);
	CHECK(m[0]->pool == cpu && m[0]->ol_flags == 0 && m[0]->hash.rss == 0);
	CHECK(m[0]->packet_type == 0);
	shared[63] ^= 0xff;
	CHECK(rte_pktmbuf_mtod(m[0], uint8_t *)[63] == original);
	m[0]->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
	m[0]->l2_len = 14;
	m[0]->l3_len = 20;
	CHECK(cvm_mbuf_bridge_tx_copy(m, 1) == 0);
	CHECK(m[0]->pool == dma && (m[0]->ol_flags & RTE_MBUF_F_EXTERNAL));
	CHECK(m[0]->ol_flags & RTE_MBUF_F_TX_IP_CKSUM);
	CHECK(m[0]->l2_len == 14 && m[0]->l3_len == 20);
	CHECK(rte_pktmbuf_mtod(m[0], uint8_t *)[63] == original);
	rte_pktmbuf_free(m[0]);
	balanced("snapshot isolation, metadata sanitization and TX offloads");

	m[0] = packet(cpu, 64);
	CHECK(rte_pktmbuf_chain(m[0], packet(cpu, 64)) == 0);
	CHECK(cvm_mbuf_bridge_tx_copy(m, 1) == 0);
	CHECK(m[0]->pkt_len == 128 && m[0]->pool == dma);
	CHECK(cvm_mbuf_bridge_rx_copy(m, 1, &dropped) == 1);
	CHECK(m[0]->pkt_len == 128 && m[0]->pool == cpu);
	rte_pktmbuf_free(m[0]);
	balanced("segmented packet round trip");

	m[0] = packet(dma, 64);
	CHECK(cvm_mbuf_bridge_tx_copy(m, 1) == -EINVAL);
	rte_pktmbuf_free(m[0]);
	m[0] = packet(ordinary, 64);
	CHECK(cvm_mbuf_bridge_tx_copy(m, 1) == -EINVAL);
	CHECK(cvm_mbuf_bridge_rx_copy(m, 1, &dropped) == 0 && dropped == 1);
	CHECK(cvm_mbuf_bridge_cpu_pool(ordinary) == NULL);
	m[0] = packet(cpu, 64);
	CHECK(rte_pktmbuf_chain(m[0], packet(dma, 64)) == 0);
	CHECK(cvm_mbuf_bridge_tx_copy(m, 1) == -EINVAL);
	rte_pktmbuf_free(m[0]);
	balanced("reject shared TX, unknown pools and mixed-domain chains");
	m[1] = packet(dma, 64);
	m[0] = rte_pktmbuf_clone(m[1], cpu);
	CHECK(m[0] != NULL && m[0]->pool == cpu);
	CHECK(cvm_mbuf_bridge_tx_copy(m, 1) == -EINVAL);
	rte_pktmbuf_free_bulk(m, 2);
	balanced("private descriptor cannot disguise shared external payload");

	for (i = 0; i < COUNT - 1; i++)
		CHECK((held[i] = rte_pktmbuf_alloc(dma)) != NULL);
	m[0] = saved[0] = packet(cpu, 64);
	m[1] = saved[1] = packet(cpu, 64);
	CHECK(cvm_mbuf_bridge_tx_copy(m, 2) == -ENOMEM);
	CHECK(m[0] == saved[0] && m[1] == saved[1]);
	CHECK(rte_mempool_avail_count(dma) == 1);
	rte_pktmbuf_free_bulk(m, 2);
	rte_pktmbuf_free_bulk(held, COUNT - 1);
	balanced("TX allocation failure is atomic and returns temporary copies");

	for (i = 0; i < COUNT; i++)
		CHECK((held[i] = rte_pktmbuf_alloc(cpu)) != NULL);
	m[0] = packet(dma, 64);
	CHECK(cvm_mbuf_bridge_rx_copy(m, 1, &dropped) == 0 && dropped == 1);
	rte_pktmbuf_free_bulk(held, COUNT);
	balanced("RX allocation failure frees the DMA source");

	m[0] = packet(cpu, 64);
	m[0]->pkt_len++;
	CHECK(cvm_mbuf_bridge_tx_copy(m, 1) == -EINVAL);
	m[0]->pkt_len--;
	rte_pktmbuf_free(m[0]);
	balanced("reject inconsistent packet length");
}

static void
partial_tx_case(void)
{
	struct fwd_stream fs = {.tx_port = port, .retry_enabled = 1};
	struct rte_mbuf *m;
	unsigned int i, count = 0;

	for (i = 0; i < 6; i++) {
		m = packet(dma, 64);
		CHECK(rte_ring_enqueue(tx, m) == 0);
	}
	CHECK(tx_only_engine.port_fwd_begin(port) == 0);
	CHECK(tx_only_engine.packet_fwd(&fs));
	CHECK(fs.tx_packets == 1 && fs.fwd_dropped == 3);
	while (rte_ring_dequeue(tx, (void **)&m) == 0) {
		rte_pktmbuf_free(m);
		count++;
	}
	CHECK(count == 7);
	balanced("partial TX and retry release unsent shared copies");
}

static void
engine_case(struct fwd_engine *engine, bool receive, bool transmit)
{
	struct fwd_stream fs = {.rx_port = port, .tx_port = port};
	struct rte_mbuf *m;
	unsigned int count = 0;

	CHECK(cvm_mbuf_bridge_check_mode(engine->fwd_mode_name) == 0);
	if (engine->port_fwd_begin != NULL)
		CHECK(engine->port_fwd_begin(port) == 0);
	if (receive) {
		m = packet(dma, 64);
		if (engine == &icmp_echo_engine) {
			struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m,
				struct rte_ipv4_hdr *, 14);
			struct rte_icmp_hdr *icmp = (void *)(ip + 1);

			ip->next_proto_id = IPPROTO_ICMP;
			ip->hdr_checksum = 0;
			ip->hdr_checksum = rte_ipv4_cksum(ip);
			icmp->icmp_type = RTE_ICMP_TYPE_ECHO_REQUEST;
			icmp->icmp_code = 0;
			icmp->icmp_cksum = 0;
			icmp->icmp_cksum = (uint16_t)~rte_raw_cksum(icmp, 30);
		}
		CHECK(rte_ring_enqueue(rx, m) == 0);
	}
	CHECK(engine->packet_fwd(&fs));
	while (rte_ring_dequeue(tx, (void **)&m) == 0) {
		CHECK(m->pool == dma && (m->ol_flags & RTE_MBUF_F_EXTERNAL));
		CHECK(m->pkt_len >= 60);
		rte_pktmbuf_free(m);
		count++;
	}
	CHECK(fs.rx_packets == (receive ? 1 : 0));
	CHECK(fs.fwd_dropped == 0);
	CHECK(fs.tx_packets == count && (transmit ? count > 0 : count == 0));
	if (engine->port_fwd_end != NULL)
		engine->port_fwd_end(port);
	balanced(engine->fwd_mode_name);
}

#ifdef CVM_CHUNK_OPENSSL
static void
authenticated_case(void)
{
	struct fwd_stream fs = {.rx_port = port, .tx_port = port};
	struct cvm_chunk_header h = {.flags = CVM_CHUNK_F_DATA, .key_epoch = 1,
		.session_id = 7, .object_id = 9, .total_chunks = 1,
		.object_len = 32, .payload_len = 32};
	uint8_t key[CVM_CHUNK_KEY_SIZE] = {0}, plain[32] = {0x42};
	uint8_t wire[CVM_CHUNK_MAX_UDP_PAYLOAD], opened[CVM_CHUNK_MAX_UDP_PAYLOAD];
	struct rte_ether_addr local;
	size_t len;
	unsigned int i;

	CHECK(setenv("CVM_CHUNK_KEY_HEX",
		"0000000000000000000000000000000000000000000000000000000000000000", 1) == 0);
	CHECK(setenv("CVM_CHUNK_PEER_IPV4", "10.0.0.1", 1) == 0);
	CHECK(setenv("CVM_CHUNK_LOCAL_IPV4", "10.0.0.2", 1) == 0);
	CHECK(setenv("CVM_CHUNK_PEER_MAC", "5a:5a:5a:5a:5a:5a", 1) == 0);
	CHECK(setenv("CVM_CHUNK_PEER_UDP_PORT", "1000", 1) == 0);
	cur_fwd_config.nb_fwd_ports = cur_fwd_config.nb_fwd_streams = 1;
	nb_rxq = nb_txq = 1;
	CHECK(cvm_chunk_engine.port_fwd_begin(port) == 0);
	CHECK(rte_eth_macaddr_get(port, &local) == 0);
	CHECK(cvm_chunk_packet_build(key, &h, plain, wire, sizeof(wire), &len) == 0);
	/* Corrupt first packet, then valid retry and authentic duplicate. */
	for (i = 0; i < 3; i++) {
		struct rte_mbuf *m = packet(dma, 42 + len), *ack;
		struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		struct rte_udp_hdr *udp = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *, 34);
		struct cvm_chunk_header ack_header;

		eth->dst_addr = local;
		udp->dst_port = rte_cpu_to_be_16(9000);
		memcpy(udp + 1, wire, len);
		if (i == 0)
			((uint8_t *)(udp + 1))[len - 1] ^= 1;
		CHECK(rte_ring_enqueue(rx, m) == 0);
		CHECK(cvm_chunk_engine.packet_fwd(&fs));
		if (i == 0) {
			CHECK(rte_ring_empty(tx));
			CHECK(fs.fwd_dropped == 1);
			continue;
		}
		CHECK(rte_ring_dequeue(tx, (void **)&ack) == 0);
		CHECK(ack->pool == dma && (ack->ol_flags & RTE_MBUF_F_EXTERNAL));
		CHECK(cvm_chunk_packet_open(key, rte_pktmbuf_mtod_offset(ack, uint8_t *, 42),
			ack->pkt_len - 42, &ack_header, opened, sizeof(opened)) == 0);
		CHECK(cvm_chunk_ack_header_validate(&ack_header, 7, 9, 1, 32, 1) == 0);
		CHECK(ack_header.chunk_id == 1); /* contiguous authenticated prefix */
		rte_pktmbuf_free(ack);
		CHECK(rte_ring_empty(tx));
	}
	CHECK(fs.rx_packets == 3 && fs.tx_packets == 2 && fs.fwd_dropped == 1);
	cvm_chunk_engine.port_fwd_end(port);
	balanced("cvmchunk tamper rejection, authenticated retry and replay ACK");
}
#endif

static void
gso_case(void)
{
	struct rte_mbuf *m = packet(cpu, 3000), *segments[16];
	struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *, 14);
	struct rte_tcp_hdr *tcp = (void *)(ip + 1);
	struct rte_gso_ctx ctx = {.direct_pool = cpu, .indirect_pool = cpu,
		.gso_types = RTE_ETH_TX_OFFLOAD_TCP_TSO, .gso_size = 600};
	int n, i;
	uint32_t payload = 0;

	ip->next_proto_id = IPPROTO_TCP;
	memset(tcp, 0, sizeof(*tcp));
	tcp->data_off = 5 << 4;
	tcp->tcp_flags = RTE_TCP_ACK_FLAG;
	m->l2_len = 14;
	m->l3_len = 20;
	m->l4_len = 20;
	m->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_TCP_SEG;
	n = rte_gso_segment(m, &ctx, segments, RTE_DIM(segments));
	CHECK(n > 1);
	rte_pktmbuf_free(m);
	for (i = 0; i < n; i++) {
		CHECK(segments[i]->pool == cpu && segments[i]->nb_segs > 1);
		payload += segments[i]->pkt_len - 54;
	}
	CHECK(payload == 3000 - 54);
	CHECK(cvm_mbuf_bridge_tx_copy(segments, n) == 0);
	for (i = 0; i < n; i++)
		CHECK(segments[i]->pool == dma && segments[i]->pkt_len <= 600);
	rte_pktmbuf_free_bulk(segments, n);
	balanced("GSO indirect private segments copied to DMA");
}

int
main(int argc, char **argv)
{
	const struct rte_memzone *mz;
	struct rte_pktmbuf_extmem ext;
	struct rte_eth_conf conf = {0};
	struct fwd_lcore lc = {0}, *lcs[] = {&lc};
	struct rte_mbuf *m;
	uint16_t dropped = 9;
	int id;

	CHECK(rte_eal_init(argc, argv) >= 0);
	ordinary = rte_pktmbuf_pool_create("ordinary", COUNT, 0, 0, ROOM, SOCKET_ID_ANY);
	CHECK(ordinary != NULL);
	m = packet(ordinary, 64);
	CHECK(cvm_mbuf_bridge_cpu_pool(ordinary) == ordinary);
	CHECK(cvm_mbuf_bridge_rx_copy(&m, 1, &dropped) == 1 && dropped == 0);
	CHECK(cvm_mbuf_bridge_tx_copy(&m, 1) == 0 && m->pool == ordinary);
	CHECK(cvm_mbuf_bridge_check_mode("ieee1588") == 0);
	rte_pktmbuf_free(m);
	puts("PASS bridge disabled preserves ordinary operation");
	mz = rte_memzone_reserve("fake_dma", (COUNT + 1) * ROOM, SOCKET_ID_ANY, 0);
	CHECK(mz != NULL);
	ext = (struct rte_pktmbuf_extmem){.buf_ptr = mz->addr, .buf_iova = mz->iova,
		.buf_len = mz->len, .elt_size = ROOM};
	dma = rte_pktmbuf_pool_create_extbuf("fake_dma_pool", COUNT, 0, 0, ROOM,
		SOCKET_ID_ANY, &ext, 1);
	CHECK(dma != NULL);
	CHECK(cvm_mbuf_bridge_register_pool(dma, COUNT, 0, ROOM, 0, 0) == 0);
	cpu = cvm_mbuf_bridge_cpu_pool(dma);
	CHECK(cpu != NULL && cpu != dma && (cpu->flags & RTE_MEMPOOL_F_NON_IO));
	copy_cases();
	gso_case();
	CHECK(cvm_mbuf_bridge_check_mode("ieee1588") == -ENOTSUP);
	CHECK(cvm_mbuf_bridge_check_mode("recycle_mbufs") == -ENOTSUP);
	CHECK(cvm_mbuf_bridge_check_mode("shared_rxq") == -ENOTSUP);
	balanced("unsupported modes rejected explicitly");

	rx = rte_ring_create("fixture_rx", 512, SOCKET_ID_ANY, 0);
	tx = rte_ring_create("fixture_tx", 8, SOCKET_ID_ANY, 0);
	CHECK(rx != NULL && tx != NULL);
	id = rte_eth_from_rings("fixture", &rx, 1, &tx, 1, SOCKET_ID_ANY);
	CHECK(id >= 0);
	port = id;
	CHECK(rte_eth_dev_configure(port, 1, 1, &conf) == 0);
	CHECK(rte_eth_rx_queue_setup(port, 0, 128, SOCKET_ID_ANY, NULL, dma) == 0);
	CHECK(rte_eth_tx_queue_setup(port, 0, 128, SOCKET_ID_ANY, NULL) == 0);
	CHECK(rte_eth_dev_start(port) == 0);
	ports = calloc(RTE_MAX_ETHPORTS, sizeof(*ports));
	CHECK(ports != NULL);
	fwd_lcores = lcs;
	fwd_lcores_cpuids[0] = rte_lcore_id();
	cur_fwd_config.nb_fwd_lcores = 1;
	lc.mbp = cvm_mbuf_bridge_cpu_pool(dma);
	nb_pkt_per_burst = 4;
	tx_pkt_length = tx_pkt_seg_lengths[0] = 64;
	tx_pkt_nb_segs = 1;
	engine_case(&io_fwd_engine, true, true);
	engine_case(&mac_fwd_engine, true, true);
	engine_case(&mac_swap_engine, true, true);
	engine_case(&five_tuple_swap_fwd_engine, true, true);
	engine_case(&rx_only_engine, true, false);
	engine_case(&tx_only_engine, false, true);
	engine_case(&flow_gen_engine, true, true);
	nb_pkt_flowgen_clones = 2;
	engine_case(&flow_gen_engine, true, true);
	nb_pkt_flowgen_clones = 0;
	engine_case(&csum_fwd_engine, true, true);
	tx_pkt_split = TX_PKT_SPLIT_ON;
	tx_pkt_nb_segs = 2;
	tx_pkt_seg_lengths[0] = tx_pkt_seg_lengths[1] = 32;
	engine_case(&csum_fwd_engine, true, true);
	engine_case(&tx_only_engine, false, true);
	tx_pkt_split = TX_PKT_SPLIT_OFF;
	tx_pkt_nb_segs = 1;
	tx_pkt_seg_lengths[0] = 64;
	engine_case(&icmp_echo_engine, true, true);
	noisy_tx_sw_bufsz = 0;
	engine_case(&noisy_vnf_engine, true, true);
	partial_tx_case();
#ifdef CVM_CHUNK_OPENSSL
	authenticated_case();
#endif
	CHECK(rte_eth_dev_stop(port) == 0);
	CHECK(rte_eth_dev_close(port) == 0);
	rte_ring_free(rx);
	rte_ring_free(tx);
	free(ports);
	rte_mempool_free(cpu);
	rte_mempool_free(dma);
	rte_mempool_free(ordinary);
	CHECK(rte_memzone_free(mz) == 0);
	CHECK(rte_eal_cleanup() == 0);
	return 0;
}
