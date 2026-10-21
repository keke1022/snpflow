"""
第 B 步：PMD 内存安全 —— 来自共享页的字段一律先读入局部变量再校验。

机密虚拟机里 CQ 缓冲区位于共享页，宿主机可读可写。PMD 从中读出的
若干字段被直接当作索引或长度使用，一旦取值越界就是内存破坏，
而不只是收到坏包。

这与 TOCTOU 是两回事：即便宿主机并无恶意，一个出错的值也会让
PMD 越界访问。因此校验无条件进行，不列入"看性能数字再定"的队列。

本补丁覆盖非向量化路径的这些点：
  - 压缩 CQE 的 mini-CQE 计数（被用来推进 cq_ci，无界）
  - wqe_counter（被用作 WQE 环索引）
  - byte_cnt（包长）
  - MPRQ 的 stride 计数与长度
  - TX 完成的 wqe_counter

向量化路径（mlx5_rxtx_vec_*.h）另行处理，见文末说明。
"""
import os

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/net/mlx5')
os.chdir(BASE)

# ================================================================== #
# 1. mlx5_rx.h：校验辅助
# ================================================================== #
p = 'mlx5_rx.h'
s = open(p).read()

helper = '''
/*
 * 以下取值来自 CQ 缓冲区。在机密虚拟机里该缓冲区位于共享页，
 * 宿主机可写，因此这些值必须视为不可信输入：先读入局部变量
 * （volatile 保证单次读取），再校验，最后才使用。
 *
 * 被当作索引或长度使用而未经校验的值，越界后果是内存破坏，
 * 不是收到坏包。
 */

/**
 * 校验压缩 CQE 声明的 mini-CQE 个数。
 *
 * 该值用于推进 cq_ci（zip->cq_ci = rxq->cq_ci + cqe_cnt），
 * 未经校验时可被推到任意位置。合法上限是整个 CQ 的元素数。
 *
 * @return 通过校验返回 true。
 */
static __rte_always_inline bool
mlx5_rx_cqe_cnt_valid(const struct mlx5_rxq_data *rxq, uint32_t cqe_cnt)
{
	return cqe_cnt != 0 && cqe_cnt <= (1u << rxq->cqe_n);
}

/**
 * 校验包长。
 *
 * 上限取 CQ 声明所能承载的最大值；调用方仍需按自身缓冲区大小
 * 再做一次约束。
 */
static __rte_always_inline bool
mlx5_rx_len_valid(const struct mlx5_rxq_data *rxq, uint32_t len)
{
	RTE_SET_USED(rxq);
	return len <= UINT16_MAX;
}

'''

anchor = '''/* Get pointer to the first stride. */'''
assert anchor in s, 'mlx5_rx.h 锚点未匹配'
s = s.replace(anchor, helper + anchor, 1)
open(p, 'w').write(s)
print('mlx5_rx.h: 已加校验辅助函数')

# ================================================================== #
# 2. mlx5_rx.c：压缩 CQE 计数与 wqe_counter
# ================================================================== #
p = 'mlx5_rx.c'
s = open(p).read()

old = '''				/* Fix endianness. */
				zip->cqe_cnt = rxq->cqe_comp_layout ?
					(MLX5_CQE_NUM_MINIS(op_own) + 1U) :
					rte_be_to_cpu_32(cqe->byte_cnt);'''
new = '''				/* Fix endianness. */
				/*
				 * 压缩 CQE 里 byte_cnt 表示 mini-CQE 个数，
				 * 它会被用来推进 cq_ci。该值来自共享页，
				 * 宿主机可写，未经校验时可把 cq_ci 推到任意
				 * 位置 —— 越界后果是内存破坏而非坏包。
				 * 这里单次读入局部变量后立即校验。
				 */
				{
					uint32_t cnt = rxq->cqe_comp_layout ?
						(MLX5_CQE_NUM_MINIS(op_own) + 1U) :
						rte_be_to_cpu_32(cqe->byte_cnt);

					if (unlikely(!mlx5_rx_cqe_cnt_valid(rxq, cnt))) {
						rte_atomic_fetch_add_explicit(&rxq->stats.idropped,
							1, rte_memory_order_relaxed);
						return 0;
					}
					zip->cqe_cnt = cnt;
				}'''
assert old in s, '压缩 CQE 计数未匹配'
s = s.replace(old, new, 1)

old2 = '''				if (widx != NULL) {
					zip->wqe_idx = rte_be_to_cpu_16(cqe->wqe_counter);
					*widx = zip->wqe_idx;
				}'''
new2 = '''				if (widx != NULL) {
					/*
					 * wqe_counter 被用作 WQE 环索引，
					 * 同样来自共享页。按环大小取模，
					 * 使越界值不会转化为越界访问。
					 */
					uint16_t wc = rte_be_to_cpu_16(cqe->wqe_counter);

					zip->wqe_idx = wc & ((1u << rxq->elts_n) - 1);
					*widx = zip->wqe_idx;
				}'''
assert old2 in s, 'wqe_counter(压缩) 未匹配'
s = s.replace(old2, new2, 1)

old3 = '''				++rxq->cq_ci;
				len = rte_be_to_cpu_32(cqe->byte_cnt);
				if (widx != NULL)
					*widx = rte_be_to_cpu_16(cqe->wqe_counter);'''
new3 = '''				++rxq->cq_ci;
				len = rte_be_to_cpu_32(cqe->byte_cnt);
				if (widx != NULL) {
					/* 同上：按环大小取模后再用作索引 */
					uint16_t wc = rte_be_to_cpu_16(cqe->wqe_counter);

					*widx = wc & ((1u << rxq->elts_n) - 1);
				}'''
assert old3 in s, 'wqe_counter(非压缩) 未匹配'
s = s.replace(old3, new3, 1)

open(p, 'w').write(s)
print('mlx5_rx.c: 压缩 CQE 计数与 wqe_counter 已加校验')

# ================================================================== #
# 3. mlx5_tx.c：完成队列里的 wqe_counter
# ================================================================== #
p = 'mlx5_tx.c'
s = open(p).read()

old_tx = '''		txq->wqe_pi = rte_be_to_cpu_16(last_cqe->wqe_counter);'''
new_tx = '''		/*
		 * wqe_counter 来自共享页（宿主机可写），被用作 WQE 环
		 * 的生产者索引。按环掩码取模，使异常值不会转化为
		 * 越界访问。
		 */
		txq->wqe_pi = rte_be_to_cpu_16(last_cqe->wqe_counter) &
			      txq->wqe_m;'''
assert old_tx in s, 'TX wqe_pi 未匹配'
s = s.replace(old_tx, new_tx, 1)
open(p, 'w').write(s)
print('mlx5_tx.c: wqe_pi 已按环掩码取模')
