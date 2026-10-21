"""
待修项 1：向量路径的剩余容量、链式累计、错误返回外层。

审查指出的五个具体问题：
  1. mcqe_n 只与整个 CQ 容量比较，但 cq/elts 指针已偏移 nocmp_n，
     环尾处伪造值能通过检查却越过数组末尾
  2. 链式压缩只查每段，不限制累计 pkts_n
  3. 非法值钳成 1 后仍被解码并返回给应用
  4. byte_cnt 是 32 位，先赋给 uint16_t 再校验 —— 校验的是截断后的值
  5. SSE 查了链式段，NEON/AltiVec 没查，三种实现不一致

做法：
  - 解压函数新增 cap 参数（入口总预算），内部用 acc + raw <= cap
    —— cap 语义固定为「总预算」，不在函数内重新计算剩余量，
    避免审查第 3 条指出的重复扣减
  - 原始 32 位值在截断前校验
  - 违规时返回 0 并置 err_state = NEED_RESET，由外层现有的
    `if (unlikely(err | rxq->err_state))` 接住去做恢复，
    不在 SIMD 循环里做 reset
  - 三种实现改法一致

预算取三者最小：CQ 连续剩余项、elts 连续剩余项、已投递可消费的 mbuf 数。
两个调用点的容量模型不同，分别计算。
"""
import os
import re

BASE = os.path.expanduser('~/dpdk-stable-25.11.3/drivers/net/mlx5')
os.chdir(BASE)

# ================================================================== #
# 1. mlx5_rx.h：植入已通过 bounds-test 的谓词
# ================================================================== #
p = 'mlx5_rx.h'
s = open(p).read()

old = '''static __rte_always_inline bool
mlx5_rx_cqe_cnt_valid(uint32_t cqe_cnt, uint32_t cq_entries)
{
	return cqe_cnt != 0 && cqe_cnt <= cq_entries;
}'''
new = '''static __rte_always_inline bool
mlx5_rx_cqe_cnt_valid(uint32_t cqe_cnt, uint32_t cq_entries)
{
	return cqe_cnt != 0 && cqe_cnt <= cq_entries;
}

/**
 * 校验一段压缩 CQE 声明的 mini-CQE 数量。
 *
 * 逻辑已由 snp-dpdk/bounds-test.c 的用例覆盖（环尾、32 位截断、
 * 链式累计、容量为零）。
 *
 * @param raw
 *   从 CQE 读出的**原始 32 位值**。必须在截断成 16 位之前传入 ——
 *   先赋给 uint16_t 再校验，校验的是截断后的值，等于没校验。
 * @param cap
 *   本轮批处理的**入口总预算**，取三者最小值：CQ 连续剩余项、
 *   elts 连续剩余项、已投递且可消费的 mbuf 数。
 *   语义固定为总预算，函数内不再重新计算剩余量 —— 既用递减的
 *   remaining 又加累计值会重复扣减。
 * @param acc
 *   本轮已累计的 mini-CQE 数。链式压缩下每段单独合法，
 *   累计仍可能走出数组。
 * @param[out] n
 *   通过校验时输出实际数量。
 *
 * @return
 *   true 表示合法。
 */
static __rte_always_inline bool
mlx5_rx_mcqe_n_valid(uint32_t raw, uint16_t cap, uint16_t acc, uint16_t *n)
{
	/* raw > cap 同时覆盖 32 位超范围：cap 最大也只有 65535 */
	if (unlikely(raw == 0 || raw > (uint32_t)cap))
		return false;
	if (unlikely((uint32_t)acc + raw > (uint32_t)cap))
		return false;
	*n = (uint16_t)raw;
	return true;
}'''
assert old in s, 'mlx5_rx.h 谓词锚点未匹配'
s = s.replace(old, new, 1)

# 旧的 len_valid 只适用 SPRQ 单段，改名并写明适用范围
old_len = '''static __rte_always_inline bool
mlx5_rx_len_valid(uint32_t len)
{
	return len <= UINT16_MAX;
}'''
new_len = '''/**
 * 校验包长是否落在单个 mbuf 的可写空间内。
 *
 * **只适用于 SPRQ 单段路径。** 标量 scatter 的 len 是整包长度、
 * 可跨多个 mbuf；MPRQ 的 byte_cnt 是打包字段（位 0-15 长度、
 * 位 16-29 stride 数、位 31 filler）。那两种各需自己的校验。
 */
static __rte_always_inline bool
mlx5_rx_len_valid_sprq(uint32_t len, uint16_t buf_len, uint16_t data_off)
{
	if (unlikely(data_off > buf_len))
		return false;
	return len != 0 && len <= (uint32_t)(buf_len - data_off);
}'''
assert old_len in s, 'mlx5_rx.h len_valid 锚点未匹配'
s = s.replace(old_len, new_len, 1)
open(p, 'w').write(s)
print('mlx5_rx.h: 植入 mcqe_n_valid；len_valid 改名并限定 SPRQ 适用范围')

# ================================================================== #
# 2. 三个向量实现：加 cap 参数、原始值校验、累计检查、错误返回
# ================================================================== #
for f, arch in (('mlx5_rxtx_vec_sse.h', 'SSE'),
                ('mlx5_rxtx_vec_neon.h', 'NEON'),
                ('mlx5_rxtx_vec_altivec.h', 'AltiVec')):
    if not os.path.exists(f):
        print('  [跳过] %s 不存在' % f)
        continue
    s = open(f).read()

    # 2a. 函数签名加 cap
    sig_old = '''rxq_cq_decompress_v(struct mlx5_rxq_data *rxq, volatile struct mlx5_cqe *cq,
		    struct rte_mbuf **elts, bool keep)
{'''
    sig_new = '''rxq_cq_decompress_v(struct mlx5_rxq_data *rxq, volatile struct mlx5_cqe *cq,
		    struct rte_mbuf **elts, bool keep, uint16_t cap)
{'''
    if sig_old not in s:
        print('  [跳过] %s 函数签名未匹配' % f)
        continue
    s = s.replace(sig_old, sig_new, 1)

    # 2b. 第一处 mcqe_n：原始 32 位值校验
    v1_old = re.search(
        r'\tuint16_t mcqe_n = \(rxq->cqe_comp_layout\) \?\n'
        r'\t\t\(MLX5_CQE_NUM_MINIS\(cq->op_own\) \+ 1U\) : rte_be_to_cpu_32\(cq->byte_cnt\);\n'
        r'(?:\n\tif \(unlikely\(mcqe_n == 0 \|\| mcqe_n > \(1u << rxq->cqe_n\)\)\)\n\t\tmcqe_n = 1;\n)?',
        s)
    if v1_old is None:
        print('  [跳过] %s mcqe_n 第一处未匹配' % f)
        continue
    v1_new = '''	/*
	 * 原始值是 32 位，必须在截断成 uint16_t **之前**校验 ——
	 * 先赋值再检查，检查的是截断后的值（0x10001 会变成合法的 1）。
	 *
	 * cap 是入口总预算，已按 CQ / elts 的连续剩余项和可消费 mbuf 数
	 * 取过最小值；不能拿整个队列容量来比，因为 cq/elts 指针已偏移。
	 */
	uint32_t mcqe_raw = (rxq->cqe_comp_layout) ?
		(uint32_t)(MLX5_CQE_NUM_MINIS(cq->op_own) + 1U) :
		rte_be_to_cpu_32(cq->byte_cnt);
	uint16_t mcqe_n;

	if (unlikely(!mlx5_rx_mcqe_n_valid(mcqe_raw, cap, 0, &mcqe_n))) {
		/*
		 * 交给外层恢复：SIMD 循环里没法安全地做队列复位。
		 * 返回 0 表示一个都没解压，外层现有的
		 * `if (unlikely(err | rxq->err_state))` 会接住。
		 */
		rxq->err_state = MLX5_RXQ_ERR_STATE_NEED_RESET;
		return 0;
	}
'''
    s = s[:v1_old.start()] + v1_new + s[v1_old.end():]

    # 2c. 链式那一处：累计校验（三种实现统一处理）
    v2_old_variants = [
        '''			mcqe_n = MLX5_CQE_NUM_MINIS(cq->op_own) + 1;
			/* 同上：钳制到 CQ 容量之内 */
			if (unlikely(mcqe_n > (1u << rxq->cqe_n)))
				mcqe_n = 1;
			pkts_n += mcqe_n;''',
        '''			mcqe_n = MLX5_CQE_NUM_MINIS(cq->op_own) + 1;
			pkts_n += mcqe_n;''',
    ]
    v2_new = '''			/*
			 * 链式压缩：每段单独合法，累计仍可能走出数组。
			 * 用已累计的 pkts_n 作为 acc 一并校验。
			 */
			{
				uint32_t raw = MLX5_CQE_NUM_MINIS(cq->op_own) + 1U;

				if (unlikely(!mlx5_rx_mcqe_n_valid(raw, cap,
								   pkts_n,
								   &mcqe_n))) {
					rxq->err_state =
						MLX5_RXQ_ERR_STATE_NEED_RESET;
					return 0;
				}
			}
			pkts_n += mcqe_n;'''
    hit = False
    for v2_old in v2_old_variants:
        if v2_old in s:
            s = s.replace(v2_old, v2_new, 1)
            hit = True
            break
    open(f, 'w').write(s)
    print('  %-24s 已改（链式校验%s）' % (f, '已加' if hit else '未找到对应段落'))

# ================================================================== #
# 3. 两个调用点：算出入口总预算
# ================================================================== #
p = 'mlx5_rxtx_vec.c'
s = open(p).read()

c1_old = '''		rxq->decompressed = rxq_cq_decompress_v(rxq, &cq[nocmp_n],
							&elts[nocmp_n], true);'''
c1_new = '''		/*
		 * 入口总预算：cq 与 elts 指针都已偏移 nocmp_n，
		 * 因此剩余连续容量要扣掉它；再取可消费 mbuf 数的最小值。
		 */
		{
			uint16_t cap_cq = (uint16_t)(q_n - cq_idx - nocmp_n);
			uint16_t cap_elts = (uint16_t)(e_n - elts_idx - nocmp_n);
			uint16_t cap_mbuf = (uint16_t)(rxq->rq_ci - rxq->rq_pi -
						       nocmp_n);
			uint16_t cap = RTE_MIN(cap_cq,
					       RTE_MIN(cap_elts, cap_mbuf));

			rxq->decompressed = rxq_cq_decompress_v(rxq,
								&cq[nocmp_n],
								&elts[nocmp_n],
								true, cap);
		}'''
assert c1_old in s, '调用点 1 未匹配'
s = s.replace(c1_old, c1_new, 1)

c2_old = '''		rxq->decompressed = rxq_cq_decompress_v(rxq, &cq[nocmp_n],
							&elts[nocmp_n], false);'''
c2_new = '''		/*
		 * MPRQ 的 elts 容量模型不同：elts_n = wqe_n * strd_n，
		 * 掩码是 elts_mask，可消费数用 elts_ci - rq_pi。
		 */
		{
			uint16_t cap_cq = (uint16_t)(q_n - cq_idx - nocmp_n);
			uint16_t cap_elts = (uint16_t)(elts_n - elts_idx -
						       nocmp_n);
			uint16_t cap_mbuf = (uint16_t)(rxq->elts_ci -
						       rxq->rq_pi - nocmp_n);
			uint16_t cap = RTE_MIN(cap_cq,
					       RTE_MIN(cap_elts, cap_mbuf));

			rxq->decompressed = rxq_cq_decompress_v(rxq,
								&cq[nocmp_n],
								&elts[nocmp_n],
								false, cap);
		}'''
assert c2_old in s, '调用点 2 未匹配'
s = s.replace(c2_old, c2_new, 1)
open(p, 'w').write(s)
print('mlx5_rxtx_vec.c: 两个调用点已按各自容量模型算出总预算')
