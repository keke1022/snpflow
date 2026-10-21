/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <mlx5_prm.h>
#include "mlx5_tx_validate.h"

static volatile struct mlx5_cqe *mutate;

static enum mlx5_tx_cvm_guard_result
validate_then_mutate(bool enabled, uint16_t ci, uint16_t pi,
		uint32_t fcq, uint16_t counter)
{
	enum mlx5_tx_cvm_guard_result ret = mlx5_tx_cvm_cqe_validate(
		enabled, ci, pi, fcq, counter);

	if (mutate != NULL && ret == MLX5_TX_CVM_GUARD_OK)
		mutate->wqe_counter = rte_cpu_to_be_16(0xbeef);
	return ret;
}

#define mlx5_tx_cvm_cqe_validate validate_then_mutate
#include "mlx5_tx.c"
#undef mlx5_tx_cvm_cqe_validate

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	exit(1); } } while (0)

static struct rte_mempool *pool;

static void
run_case(const char *name, uint16_t ci, uint16_t pending, uint16_t counter,
	 uint8_t op, bool change, bool reject, bool empty)
{
	struct mlx5_txq_data *q = calloc(1, sizeof(*q) +
					   4 * sizeof(struct rte_mbuf *));
	struct mlx5_cqe cq[4] = {0};
	uint32_t fcqs[4] = {0}, db = 0;
	unsigned int before = rte_mempool_avail_count(pool), i;

	for (i = 0; i < 4; i++)
		cq[i].op_own = MLX5_CQE_INVALID << 4;
	CHECK(q != NULL);
	q->elts[0] = rte_pktmbuf_alloc(pool);
	CHECK(q->elts[0] != NULL);
	q->cvm_guard = 1;
	q->cqe_s = q->elts_s = 4;
	q->cqe_m = q->elts_m = 3;
	q->cqe_n = q->elts_n = 2;
	q->cq_ci = ci;
	q->cq_pi = ci + pending;
	q->cqes = cq;
	q->fcqs = fcqs;
	q->elts_head = 1;
	q->cq_db = &db;
	q->wqe_pi = 3;
	fcqs[ci & 3] = mlx5_tx_fcq_make(1, rte_cpu_to_be_16(7));
	cq[ci & 3].op_own = op | !!(ci & 4);
	cq[ci & 3].wqe_counter = rte_cpu_to_be_16(counter);
	mutate = change ? &cq[ci & 3] : NULL;
	mlx5_tx_handle_completion(q, 0);
	mutate = NULL;
	if (reject || empty) {
		CHECK(q->cq_ci == ci && q->elts_tail == 0 && q->wqe_pi == 3);
		CHECK(q->cvm_tx_stopped == reject);
		CHECK(rte_mempool_avail_count(pool) == before - 1);
		if (reject) {
			mlx5_tx_handle_completion(q, 0);
			CHECK(q->cq_ci == ci && q->elts_tail == 0);
		}
		rte_pktmbuf_free(q->elts[0]);
	} else {
		CHECK(q->wqe_pi == 7);
		CHECK(q->cq_ci == (uint16_t)(ci + 1) && q->elts_tail == 1);
		CHECK(!q->cvm_tx_stopped);
	}
	CHECK(rte_mempool_avail_count(pool) == before);
	free(q);
	printf("PASS tx %s\n", name);
}

int
main(int argc, char **argv)
{
	CHECK(rte_eal_init(argc, argv) >= 0);
	pool = rte_pktmbuf_pool_create("tx_fixture", 127, 0, 0, 2176, SOCKET_ID_ANY);
	CHECK(pool != NULL);
	run_case("post-validation counter mutation", 0, 1, 7, MLX5_CQE_REQ << 4, true, false, false);
	run_case("owner wrap", 4, 1, 7, MLX5_CQE_REQ << 4, true, false, false);
	run_case("consumer wrap", UINT16_MAX, 1, 7, MLX5_CQE_REQ << 4, true, false, false);
	run_case("no pending request", 0, 0, 7, MLX5_CQE_REQ << 4, false, true, false);
	run_case("wrong counter", 0, 1, 8, MLX5_CQE_REQ << 4, false, true, false);
	run_case("error completion", 0, 1, 7, MLX5_CQE_REQ_ERR << 4, false, true, false);
	run_case("wrong opcode", 0, 1, 7, MLX5_CQE_RESP_SEND << 4, false, true, false);
	run_case("compressed", 0, 1, 7, (MLX5_CQE_REQ << 4) | (MLX5_COMPRESSED << 2),
		false, true, false);
	run_case("empty CQ", 0, 1, 7, MLX5_CQE_INVALID << 4, false, false, true);
	rte_mempool_free(pool);
	CHECK(rte_eal_cleanup() == 0);
	return 0;
}
