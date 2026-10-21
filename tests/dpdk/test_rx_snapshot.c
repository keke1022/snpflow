/* SPDX-License-Identifier: BSD-3-Clause */
/* Compile the actual PMD with a mutation at its validation boundary. */
#include <stdio.h>
#include <stdlib.h>
#include <mlx5_prm.h>
#include "mlx5_rx_validate.h"

static volatile struct mlx5_cqe *mutate;
static unsigned int mutation;

static enum mlx5_rx_cvm_guard_result
validate_then_mutate(bool enabled, bool mprq, bool compressed,
		uint32_t bytes, uint32_t capacity, bool crc, uint8_t lro)
{
	enum mlx5_rx_cvm_guard_result ret = mlx5_rx_cvm_cqe_validate(
		enabled, mprq, compressed, bytes, capacity, crc, lro);

	if (mutate != NULL && ret == MLX5_RX_CVM_GUARD_OK) {
		mutate->byte_cnt = rte_cpu_to_be_32(96);
		if (mutation == 2) {
			mutate->lro_num_seg = 2;
			mutate->rx_hash_res = rte_cpu_to_be_32(0xdeadbeef);
			mutate->op_own = MLX5_CQE_RESP_ERR << 4;
		}
	}
	return ret;
}

#define mlx5_rx_cvm_cqe_validate validate_then_mutate
#include "mlx5_rx.c"
#undef mlx5_rx_cvm_cqe_validate

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	exit(1); } } while (0)

static struct rte_mempool *pool;

static void
run_case(const char *name, uint32_t bytes, uint8_t op, uint8_t lro,
	 bool crc, unsigned int change, uint32_t ci, bool reject, bool empty)
{
	struct mlx5_rxq_data q = {0};
	struct mlx5_cqe cq[4] = {0};
	struct mlx5_wqe_data_seg wq[4] = {0};
	struct rte_mbuf *elts[4], *out = NULL;
	uint32_t db[2] = {0};
	unsigned int before = rte_mempool_avail_count(pool), i;
	uint16_t n;

	for (i = 0; i < 4; i++) {
		cq[i].op_own = MLX5_CQE_INVALID << 4;
		elts[i] = rte_pktmbuf_alloc(pool);
		CHECK(elts[i] != NULL);
		elts[i]->data_len = 128;
		memset(rte_pktmbuf_mtod(elts[i], void *), 0x5a, 128);
	}
	q.cvm_guard = 1;
	q.cqe_n = q.elts_n = 2;
	q.rxseg[0].length = 128;
	/* The real queue has a sentinel plus its single pre-registered MR. */
	q.mr_ctrl.cache_bh.len = 2;
	q.cqes = &cq;
	q.wqes = wq;
	q.elts = &elts;
	q.cq_db = &db[0];
	q.rq_db = &db[1];
	q.cq_ci = q.rq_ci = ci;
	q.crc_present = crc;
	q.rss_hash = 1;
	cq[ci & 3].op_own = op | !!(ci & 4);
	cq[ci & 3].byte_cnt = rte_cpu_to_be_32(bytes);
	cq[ci & 3].lro_num_seg = lro;
	cq[ci & 3].rx_hash_res = rte_cpu_to_be_32(0x1234);
	mutate = change ? &cq[ci & 3] : NULL;
	mutation = change;
	n = mlx5_rx_burst(&q, &out, 1);
	mutate = NULL;
	if (reject || empty) {
		CHECK(n == 0 && out == NULL);
		CHECK(q.cq_ci == ci && q.rq_ci == ci);
		CHECK(q.cvm_rx_stopped == reject);
		if (reject) {
			CHECK(q.stats.idropped == 1);
			CHECK(mlx5_rx_burst(&q, &out, 1) == 0);
			CHECK(q.stats.idropped == 1);
		}
	} else {
		CHECK(n == 1 && out != NULL);
		CHECK(out->pkt_len == bytes - (crc ? 4 : 0));
		CHECK(out->nb_segs == 1 && !(out->ol_flags & RTE_MBUF_F_RX_LRO));
		CHECK(out->hash.rss == 0x1234);
		CHECK(q.cq_ci == ci + 1 && q.rq_ci == ci + 1);
		CHECK(!q.cvm_rx_stopped);
		rte_pktmbuf_free(out);
	}
	for (i = 0; i < 4; i++)
		rte_pktmbuf_free(elts[i]);
	CHECK(rte_mempool_avail_count(pool) == before);
	printf("PASS rx %s\n", name);
}

int
main(int argc, char **argv)
{
	CHECK(rte_eal_init(argc, argv) >= 0);
	pool = rte_pktmbuf_pool_create("rx_fixture", 127, 0, 0, 2176, SOCKET_ID_ANY);
	CHECK(pool != NULL);
	mlx5_set_ptype_table();
	/* First test fails on the pre-fix PMD without requiring a timing race. */
	run_case("post-validation length mutation", 64, MLX5_CQE_RESP_SEND << 4,
		0, false, 1, 0, false, false);
	run_case("post-validation LRO/hash/opcode mutation", 64,
		MLX5_CQE_RESP_SEND << 4, 0, false, 2, 0, false, false);
	run_case("owner wrap", 64, MLX5_CQE_RESP_SEND << 4, 0, false, 1, 4, false, false);
	run_case("retained CRC", 68, MLX5_CQE_RESP_SEND << 4, 0, true, 1, 0, false, false);
	run_case("capacity boundary", 128, MLX5_CQE_RESP_SEND << 4, 0, false, 0, 0, false, false);
	run_case("oversize", 129, MLX5_CQE_RESP_SEND << 4, 0, false, 0, 0, true, false);
	run_case("zero length", 0, MLX5_CQE_RESP_SEND << 4, 0, false, 0, 0, true, false);
	run_case("CRC only", 4, MLX5_CQE_RESP_SEND << 4, 0, true, 0, 0, true, false);
	run_case("unexpected LRO", 64, MLX5_CQE_RESP_SEND << 4, 2, false, 0, 0, true, false);
	run_case("compressed", 64, (MLX5_CQE_RESP_SEND << 4) | (MLX5_COMPRESSED << 2),
		0, false, 0, 0, true, false);
	run_case("reserved format", 64, (MLX5_CQE_RESP_SEND << 4) | (1 << 2),
		0, false, 0, 0, true, false);
	run_case("error completion", 64, MLX5_CQE_RESP_ERR << 4, 0, false, 0, 0, true, false);
	run_case("wrong opcode", 64, MLX5_CQE_REQ << 4, 0, false, 0, 0, true, false);
	run_case("empty CQ", 64, MLX5_CQE_INVALID << 4, 0, false, 0, 0, false, true);
	rte_mempool_free(pool);
	CHECK(rte_eal_cleanup() == 0);
	return 0;
}
