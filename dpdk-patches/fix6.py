"""
修复审查第 6 条 + 我自己发现的向量化路径缺口。

两个问题：

(a) 标量路径：非法 mini-CQE 计数时我写的是 `return 0` —— 不消费坏 CQE
    就返回，下次调用还是同一个，**RX 队列永久卡死**。这比不加校验更糟。
    正确做法是交给 mlx5 现成的错误恢复机制：
    mlx5_rx_err_handle() + MLX5_ERROR_CQE_MASK，它会把队列标记为
    需要复位并走完整恢复流程。

(b) 向量化路径完全没有校验，而实测 `port 0 selected vectorized SPRQ
    Rx function` —— **真正在跑的就是它**。标量路径的校验是死代码。
    mlx5_rxtx_vec_sse.h 里 mcqe_n 同样来自 cq->byte_cnt，
    且被用作 elts 数组索引（elts = &elts[mcqe_n]）。

另外本补丁给 mlx5_rx_len_valid() 补上实际调用 —— 原先只定义未使用。
"""
import os

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/net/mlx5')
os.chdir(BASE)

# ================================================================== #
# (a) 标量路径：改为走现成的错误恢复机制
# ================================================================== #
p = 'mlx5_rx.c'
s = open(p).read()

old = '''					if (unlikely(!mlx5_rx_cqe_cnt_valid(cnt, 1u << rxq->cqe_n))) {
						rte_atomic_fetch_add_explicit(&rxq->stats.idropped,
							1, rte_memory_order_relaxed);
						return 0;
					}
					zip->cqe_cnt = cnt;'''
new = '''					if (unlikely(!mlx5_rx_cqe_cnt_valid(cnt, 1u << rxq->cqe_n))) {
						/*
						 * 坏 CQE 必须被消费或隔离，否则下次
						 * 调用还会读到同一个，队列永久卡死。
						 * 交给 mlx5 现成的错误恢复机制：它会
						 * 把队列标记为需要复位并走完整流程。
						 */
						DRV_LOG(ERR,
							"rx: 压缩 CQE 声明的 mini-CQE 数 %u 非法"
							"（CQ 容量 %u），按 CQ 错误处理", cnt,
							1u << rxq->cqe_n);
						rte_atomic_fetch_add_explicit(&rxq->stats.idropped,
							1, rte_memory_order_relaxed);
						rxq->err_state = MLX5_RXQ_ERR_STATE_NEED_RESET;
						ret = mlx5_rx_err_handle(rxq, 0, 1, skip_cnt);
						if (ret == MLX5_CQE_STATUS_HW_OWN)
							return MLX5_ERROR_CQE_MASK;
						return 0;
					}
					zip->cqe_cnt = cnt;'''
assert old in s, '(a) 标量路径 return 0 未匹配'
s = s.replace(old, new, 1)

# 给 len 补上校验（原先 mlx5_rx_len_valid 定义了却没调用）
old_len = '''				++rxq->cq_ci;
				len = rte_be_to_cpu_32(cqe->byte_cnt);'''
new_len = '''				++rxq->cq_ci;
				len = rte_be_to_cpu_32(cqe->byte_cnt);
				/*
				 * 包长同样来自共享页。上层会用它设置 mbuf 的
				 * data_len/pkt_len 并据此做分段处理，
				 * 超范围的值会转化为越界写。
				 */
				if (unlikely(!mlx5_rx_len_valid(len))) {
					DRV_LOG(ERR, "rx: CQE 声明的包长 %u 非法", len);
					rte_atomic_fetch_add_explicit(&rxq->stats.idropped,
						1, rte_memory_order_relaxed);
					rxq->err_state = MLX5_RXQ_ERR_STATE_NEED_RESET;
					return 0;
				}'''
assert old_len in s, '(a) 非压缩 len 未匹配'
s = s.replace(old_len, new_len, 1)
open(p, 'w').write(s)
print('(a) mlx5_rx.c: 坏 CQE 交给错误恢复机制；len 补上校验')

# ================================================================== #
# (b) 向量化路径（SSE）：mcqe_n 加校验
# ================================================================== #
p = 'mlx5_rxtx_vec_sse.h'
s = open(p).read()

old_v1 = '''	/* Restore the compressed count. Must be 16 bits. */
	uint16_t mcqe_n = (rxq->cqe_comp_layout) ?
		(MLX5_CQE_NUM_MINIS(cq->op_own) + 1U) : rte_be_to_cpu_32(cq->byte_cnt);'''
new_v1 = '''	/* Restore the compressed count. Must be 16 bits. */
	/*
	 * 该值来自 CQ 缓冲区。机密虚拟机里 CQ 位于共享页、宿主机可写，
	 * 而 mcqe_n 会被用作 elts 数组索引（elts = &elts[mcqe_n]）并作为
	 * 循环上界，越界后果是内存破坏。此处单次读入后立即钳制到
	 * CQ 容量之内 —— 向量化路径无法像标量路径那样走错误恢复流程，
	 * 钳制是这里能做到的最小代价的防护。
	 */
	uint16_t mcqe_n = (rxq->cqe_comp_layout) ?
		(MLX5_CQE_NUM_MINIS(cq->op_own) + 1U) : rte_be_to_cpu_32(cq->byte_cnt);

	if (unlikely(mcqe_n == 0 || mcqe_n > (1u << rxq->cqe_n)))
		mcqe_n = 1;'''
assert old_v1 in s, '(b) 向量化 mcqe_n 第一处未匹配'
s = s.replace(old_v1, new_v1, 1)

old_v2 = '''			mcqe_n = MLX5_CQE_NUM_MINIS(cq->op_own) + 1;
			pkts_n += mcqe_n;'''
new_v2 = '''			mcqe_n = MLX5_CQE_NUM_MINIS(cq->op_own) + 1;
			/* 同上：钳制到 CQ 容量之内 */
			if (unlikely(mcqe_n > (1u << rxq->cqe_n)))
				mcqe_n = 1;
			pkts_n += mcqe_n;'''
assert old_v2 in s, '(b) 向量化 mcqe_n 第二处未匹配'
s = s.replace(old_v2, new_v2, 1)
open(p, 'w').write(s)
print('(b) mlx5_rxtx_vec_sse.h: mcqe_n 两处加钳制')

# 其它架构的向量化实现同样处理（本机不编译，但保持一致）
for f, pat in (('mlx5_rxtx_vec_neon.h', None), ('mlx5_rxtx_vec_altivec.h', None)):
    if not os.path.exists(f):
        continue
    s = open(f).read()
    tgt = '''(MLX5_CQE_NUM_MINIS(cq->op_own) + 1U) : rte_be_to_cpu_32(cq->byte_cnt);'''
    if tgt in s and 'mcqe_n = 1;' not in s:
        s = s.replace(tgt, tgt + '''

	/* 同 SSE 版本：该值来自共享页，用作数组索引前必须钳制 */
	if (unlikely(mcqe_n == 0 || mcqe_n > (1u << rxq->cqe_n)))
		mcqe_n = 1;''', 1)
        open(f, 'w').write(s)
        print('(b) %s: 同步加钳制' % f)
